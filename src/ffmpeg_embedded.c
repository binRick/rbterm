#include "ffmpeg_embedded.h"
#include <stdio.h>

#ifdef _WIN32
/* Don't strip NOUSER here — MAKEINTRESOURCEA + the resource API
   live in winuser.h. Same convention as fonts_embedded_win.c. */
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#include <windows.h>

bool ffmpeg_embedded_extract(char *out, size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = 0;
    HMODULE mod = GetModuleHandleW(NULL);
    HRSRC h = FindResourceA(mod, MAKEINTRESOURCEA(2000), "RBTERMFFMPEG");
    if (!h) return false;
    DWORD sz = SizeofResource(mod, h);
    if (sz == 0) return false;
    HGLOBAL g = LoadResource(mod, h);
    if (!g) return false;
    void *p = LockResource(g);
    if (!p) return false;

    /* Stable path under %TEMP%. The size in the filename gives a
       cheap "is the cached extract still in sync with this rbterm
       build" check — re-extract whenever the bundled ffmpeg's size
       changes (i.e. CI fetches a new BtbN release). */
    char tmpdir[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, tmpdir);
    if (n == 0 || n >= MAX_PATH) return false;
    snprintf(out, cap, "%srbterm-ffmpeg-%lu.exe",
             tmpdir, (unsigned long)sz);

    /* Skip rewrite if the cached extract is already present and
       the right size — extracting 200 MB on every recording-save
       would add a couple of seconds per click. */
    HANDLE existing = CreateFileA(out, GENERIC_READ, FILE_SHARE_READ, NULL,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (existing != INVALID_HANDLE_VALUE) {
        DWORD existing_size = GetFileSize(existing, NULL);
        CloseHandle(existing);
        if (existing_size == sz) return true;
    }

    /* Atomic write: stream into a .part next to the destination,
       MoveFileEx to swap. Avoids a half-written exe being picked up
       by a concurrent rbterm process or by an interrupted run. */
    char tmp_write[MAX_PATH];
    snprintf(tmp_write, sizeof(tmp_write), "%s.part", out);
    HANDLE wfh = CreateFileA(tmp_write, GENERIC_WRITE, 0, NULL,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (wfh == INVALID_HANDLE_VALUE) return false;
    /* WriteFile only takes a 32-bit count, but ffmpeg.exe is up to
       ~200 MB which still fits in DWORD. If we ever bake in something
       bigger, chunk this. */
    DWORD written = 0;
    BOOL ok = WriteFile(wfh, p, sz, &written, NULL);
    CloseHandle(wfh);
    if (!ok || written != sz) {
        DeleteFileA(tmp_write);
        return false;
    }
    if (!MoveFileExA(tmp_write, out, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileA(tmp_write);
        return false;
    }
    return true;
}
#else
bool ffmpeg_embedded_extract(char *out, size_t cap) {
    if (out && cap > 0) out[0] = 0;
    return false;
}
#endif
