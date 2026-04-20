/* Windows ConPTY backend for rbterm.
 *
 * ConPTY (Windows 10 1809+) gives us a pair of pipes + a pseudoconsole
 * handle. The shell runs as a child process configured via STARTUPINFOEX
 * to use the pseudoconsole for its stdin/out/err.
 *
 * Because anonymous pipes on Windows only support synchronous I/O, we
 * spin up a reader thread per PTY that pumps ReadFile into a ring
 * buffer under a critical section. pty_read() just drains whatever the
 * ring currently holds, matching the semantics the Unix backend gets
 * "for free" from O_NONBLOCK.
 *
 * Known limitations (call outs in pty.h):
 *   - pty_cwd() returns false: NtQueryInformationProcess on another
 *     process is fragile/undocumented. Use OSC 0/2 from the shell.
 *   - Colour emoji is unavailable (emoji_stub.c) — DirectWrite port
 *     would be a separate piece of work.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pty.h"

#define RING_CAP (1u << 18)  /* 256 KB per tab is plenty */

struct Pty {
    HPCON  hpc;
    HANDLE hInW;      /* parent->child: we write here */
    HANDLE hOutR;     /* child->parent: we read here */
    HANDLE hProc;
    HANDLE hReader;
    STARTUPINFOEXW siex;
    PROCESS_INFORMATION pi;

    CRITICAL_SECTION lock;
    unsigned char *ring;
    size_t ring_head;   /* write index */
    size_t ring_tail;   /* read index */
    volatile LONG exited;
    volatile LONG stop;
};

static DWORD WINAPI reader_thread(LPVOID arg) {
    Pty *p = (Pty *)arg;
    unsigned char tmp[4096];
    while (!InterlockedCompareExchange(&p->stop, 0, 0)) {
        DWORD got = 0;
        BOOL ok = ReadFile(p->hOutR, tmp, sizeof(tmp), &got, NULL);
        if (!ok || got == 0) break;
        EnterCriticalSection(&p->lock);
        for (DWORD i = 0; i < got; i++) {
            p->ring[p->ring_head] = tmp[i];
            p->ring_head = (p->ring_head + 1) & (RING_CAP - 1);
            if (p->ring_head == p->ring_tail) {
                /* Overrun: drop oldest byte */
                p->ring_tail = (p->ring_tail + 1) & (RING_CAP - 1);
            }
        }
        LeaveCriticalSection(&p->lock);
    }
    InterlockedExchange(&p->exited, 1);
    return 0;
}

static void pick_shell(wchar_t *cmdline, size_t cap) {
    /* Prefer pwsh if on PATH, then powershell, then cmd. We don't probe
       the filesystem — CreateProcessW's PATH search does that for us and
       we fall back below if it fails. */
    const wchar_t *comspec = _wgetenv(L"COMSPEC");
    const wchar_t *override = _wgetenv(L"SHELL");
    if (override && *override) {
        _snwprintf(cmdline, cap, L"%s", override);
        return;
    }
    (void)comspec;
    _snwprintf(cmdline, cap, L"%s", L"powershell.exe");
}

Pty *pty_open(int cols, int rows) {
    Pty *p = (Pty *)calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->ring = (unsigned char *)malloc(RING_CAP);
    if (!p->ring) { free(p); return NULL; }
    InitializeCriticalSection(&p->lock);

    HANDLE hInR = NULL, hOutW = NULL;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    if (!CreatePipe(&hInR,  &p->hInW, &sa, 0)) goto fail;
    if (!CreatePipe(&p->hOutR, &hOutW, &sa, 0)) goto fail;

    COORD size = { (SHORT)cols, (SHORT)rows };
    HRESULT hr = CreatePseudoConsole(size, hInR, hOutW, 0, &p->hpc);
    CloseHandle(hInR);  hInR  = NULL;
    CloseHandle(hOutW); hOutW = NULL;
    if (FAILED(hr) || !p->hpc) {
        fprintf(stderr, "CreatePseudoConsole failed: 0x%08lx\n", (unsigned long)hr);
        goto fail;
    }

    /* Build STARTUPINFOEX with PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE_HANDLE. */
    p->siex.StartupInfo.cb = sizeof(STARTUPINFOEXW);
    SIZE_T needed = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &needed);
    p->siex.lpAttributeList =
        (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0, needed);
    if (!p->siex.lpAttributeList) goto fail;
    if (!InitializeProcThreadAttributeList(p->siex.lpAttributeList, 1, 0, &needed))
        goto fail;
    if (!UpdateProcThreadAttribute(p->siex.lpAttributeList, 0,
            PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE_HANDLE,
            p->hpc, sizeof(p->hpc), NULL, NULL))
        goto fail;

    wchar_t cmdline[512];
    pick_shell(cmdline, sizeof(cmdline)/sizeof(cmdline[0]));

    /* Start the child inside USERPROFILE for a sensible default CWD. */
    const wchar_t *start_dir = _wgetenv(L"USERPROFILE");
    if (!start_dir || !*start_dir) start_dir = NULL;

    BOOL ok = CreateProcessW(
        NULL, cmdline, NULL, NULL, FALSE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
        NULL, start_dir,
        &p->siex.StartupInfo, &p->pi);
    if (!ok) {
        /* Fall back to cmd.exe if the preferred shell isn't available. */
        wcscpy_s(cmdline, sizeof(cmdline)/sizeof(cmdline[0]), L"cmd.exe");
        ok = CreateProcessW(
            NULL, cmdline, NULL, NULL, FALSE,
            EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
            NULL, start_dir,
            &p->siex.StartupInfo, &p->pi);
    }
    if (!ok) {
        fprintf(stderr, "CreateProcess failed: %lu\n", GetLastError());
        goto fail;
    }
    p->hProc = p->pi.hProcess;

    p->hReader = CreateThread(NULL, 0, reader_thread, p, 0, NULL);
    if (!p->hReader) goto fail;
    return p;

fail:
    if (hInR)  CloseHandle(hInR);
    if (hOutW) CloseHandle(hOutW);
    pty_close(p);
    return NULL;
}

void pty_close(Pty *p) {
    if (!p) return;
    InterlockedExchange(&p->stop, 1);
    if (p->hpc) { ClosePseudoConsole(p->hpc); p->hpc = NULL; }
    if (p->hInW)  { CloseHandle(p->hInW);  p->hInW  = NULL; }
    if (p->hOutR) { CloseHandle(p->hOutR); p->hOutR = NULL; }
    if (p->hReader) {
        WaitForSingleObject(p->hReader, 1000);
        CloseHandle(p->hReader);
    }
    if (p->pi.hThread)  CloseHandle(p->pi.hThread);
    if (p->pi.hProcess) {
        WaitForSingleObject(p->pi.hProcess, 1000);
        CloseHandle(p->pi.hProcess);
    }
    if (p->siex.lpAttributeList) {
        DeleteProcThreadAttributeList(p->siex.lpAttributeList);
        HeapFree(GetProcessHeap(), 0, p->siex.lpAttributeList);
    }
    DeleteCriticalSection(&p->lock);
    free(p->ring);
    free(p);
}

bool pty_alive(Pty *p) {
    if (!p || !p->hProc) return false;
    DWORD ws = WaitForSingleObject(p->hProc, 0);
    return ws != WAIT_OBJECT_0;
}

int pty_read(Pty *p, uint8_t *buf, size_t cap) {
    if (!p) return -1;
    size_t n = 0;
    EnterCriticalSection(&p->lock);
    while (n < cap && p->ring_tail != p->ring_head) {
        buf[n++] = p->ring[p->ring_tail];
        p->ring_tail = (p->ring_tail + 1) & (RING_CAP - 1);
    }
    LeaveCriticalSection(&p->lock);
    if (n > 0) return (int)n;
    if (InterlockedCompareExchange(&p->exited, 0, 0) && !pty_alive(p)) return -1;
    return 0;
}

void pty_write(Pty *p, const uint8_t *buf, size_t n) {
    if (!p || !p->hInW) return;
    DWORD off = 0;
    while (off < (DWORD)n) {
        DWORD written = 0;
        if (!WriteFile(p->hInW, buf + off, (DWORD)n - off, &written, NULL)) return;
        if (written == 0) return;
        off += written;
    }
}

void pty_resize(Pty *p, int cols, int rows) {
    if (!p || !p->hpc) return;
    COORD size = { (SHORT)cols, (SHORT)rows };
    ResizePseudoConsole(p->hpc, size);
}

bool pty_cwd(Pty *p, char *out, size_t cap) {
    (void)p; (void)out; (void)cap;
    /* Reading another process's CWD on Windows requires poking the PEB
       via NtQueryInformationProcess + ReadProcessMemory, which is both
       undocumented and permissions-gated. Tab label falls back to the
       shell's OSC 0/2 title. */
    return false;
}
