/* Windows ConPTY backend for rbterm. See pty_internal.h for the contract.
 *
 * ConPTY (Windows 10 1809+) gives us a pair of anonymous pipes plus a
 * pseudoconsole handle. Since anonymous pipes on Windows are synchronous,
 * we spin up one reader thread per PTY feeding a 256 KB ring buffer
 * under a critical section. local_read_impl drains the ring without
 * blocking, matching the semantics O_NONBLOCK gives Unix for free. */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pty_internal.h"

#define RING_CAP (1u << 18)

typedef struct {
    HPCON  hpc;
    HANDLE hInW;
    HANDLE hOutR;
    HANDLE hProc;
    HANDLE hReader;
    STARTUPINFOEXW siex;
    PROCESS_INFORMATION pi;
    CRITICAL_SECTION lock;
    unsigned char *ring;
    size_t ring_head;
    size_t ring_tail;
    volatile LONG exited;
    volatile LONG stop;
} LocalPty;

static DWORD WINAPI reader_thread(LPVOID arg) {
    LocalPty *p = (LocalPty *)arg;
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
                p->ring_tail = (p->ring_tail + 1) & (RING_CAP - 1);
            }
        }
        LeaveCriticalSection(&p->lock);
    }
    InterlockedExchange(&p->exited, 1);
    return 0;
}

static void pick_shell(wchar_t *cmdline, size_t cap) {
    const wchar_t *override = _wgetenv(L"SHELL");
    if (override && *override) {
        _snwprintf(cmdline, cap, L"%s", override);
        return;
    }
    _snwprintf(cmdline, cap, L"%s", L"powershell.exe");
}

void *local_open_impl(int cols, int rows) {
    LocalPty *p = (LocalPty *)calloc(1, sizeof(*p));
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

    const wchar_t *start_dir = _wgetenv(L"USERPROFILE");
    if (!start_dir || !*start_dir) start_dir = NULL;

    BOOL ok = CreateProcessW(
        NULL, cmdline, NULL, NULL, FALSE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
        NULL, start_dir,
        &p->siex.StartupInfo, &p->pi);
    if (!ok) {
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
    local_close_impl(p);
    return NULL;
}

void local_close_impl(void *impl) {
    LocalPty *p = impl;
    if (!p) return;
    InterlockedExchange(&p->stop, 1);
    if (p->hpc) { ClosePseudoConsole(p->hpc); p->hpc = NULL; }
    if (p->hInW)  { CloseHandle(p->hInW);  p->hInW  = NULL; }
    if (p->hOutR) { CloseHandle(p->hOutR); p->hOutR = NULL; }
    if (p->hReader) { WaitForSingleObject(p->hReader, 1000); CloseHandle(p->hReader); }
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

bool local_alive_impl(void *impl) {
    LocalPty *p = impl;
    if (!p || !p->hProc) return false;
    DWORD ws = WaitForSingleObject(p->hProc, 0);
    return ws != WAIT_OBJECT_0;
}

int local_read_impl(void *impl, uint8_t *buf, size_t cap) {
    LocalPty *p = impl;
    if (!p) return -1;
    size_t n = 0;
    EnterCriticalSection(&p->lock);
    while (n < cap && p->ring_tail != p->ring_head) {
        buf[n++] = p->ring[p->ring_tail];
        p->ring_tail = (p->ring_tail + 1) & (RING_CAP - 1);
    }
    LeaveCriticalSection(&p->lock);
    if (n > 0) return (int)n;
    if (InterlockedCompareExchange(&p->exited, 0, 0) && !local_alive_impl(p)) return -1;
    return 0;
}

void local_write_impl(void *impl, const uint8_t *buf, size_t n) {
    LocalPty *p = impl;
    if (!p || !p->hInW) return;
    DWORD off = 0;
    while (off < (DWORD)n) {
        DWORD written = 0;
        if (!WriteFile(p->hInW, buf + off, (DWORD)n - off, &written, NULL)) return;
        if (written == 0) return;
        off += written;
    }
}

void local_resize_impl(void *impl, int cols, int rows) {
    LocalPty *p = impl;
    if (!p || !p->hpc) return;
    COORD size = { (SHORT)cols, (SHORT)rows };
    ResizePseudoConsole(p->hpc, size);
}

bool local_cwd_impl(void *impl, char *out, size_t cap) {
    (void)impl; (void)out; (void)cap;
    return false;
}
