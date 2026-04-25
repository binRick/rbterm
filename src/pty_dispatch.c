/* Dispatch layer for the Pty abstraction.
 * `struct Pty` (see pty_internal.h) owns a PtyKind tag and a void* impl;
 * every public pty_* call routes to the appropriate backend. */

#include "pty.h"
#include "pty_internal.h"

#include <stdlib.h>
#include <string.h>

/* Allocate a local-shell Pty (forkpty on Unix, ConPTY on Windows).
   Returns NULL on backend failure. */
Pty *pty_open(int cols, int rows, const char *cwd) {
    void *impl = local_open_impl(cols, rows, cwd);
    if (!impl) return NULL;
    Pty *p = calloc(1, sizeof(*p));
    if (!p) { local_close_impl(impl); return NULL; }
    p->kind = PTY_LOCAL;
    p->impl = impl;
    return p;
}

/* Allocate an SSH-backed Pty. On failure writes a libssh error
   message into `err` (truncated to errsz bytes) and returns NULL. */
Pty *pty_open_ssh(const char *user, const char *host, int port,
                  const char *password, const char *keyfile,
                  int cols, int rows,
                  char *err, size_t errsz) {
    if (err && errsz) err[0] = 0;
    void *impl = ssh_open_impl(user, host, port, password, keyfile,
                               cols, rows, err, errsz);
    if (!impl) return NULL;
    Pty *p = calloc(1, sizeof(*p));
    if (!p) { ssh_close_impl(impl); return NULL; }
    p->kind = PTY_SSH;
    p->impl = impl;
    return p;
}

/* Tear down a Pty: routes to the right backend's close + frees the
   wrapper. NULL-safe. */
void pty_close(Pty *p) {
    if (!p) return;
    if (p->kind == PTY_SSH) ssh_close_impl(p->impl);
    else                    local_close_impl(p->impl);
    free(p);
}

/* True while the underlying shell / SSH session is still up. */
bool pty_alive(Pty *p) {
    if (!p) return false;
    if (p->kind == PTY_SSH) return ssh_alive_impl(p->impl);
    return local_alive_impl(p->impl);
}

/* Non-blocking read of up to `cap` bytes. Returns the byte count,
   0 on EAGAIN/empty, or -1 on EOF / fatal error. */
int pty_read(Pty *p, uint8_t *buf, size_t cap) {
    if (!p) return -1;
    if (p->kind == PTY_SSH) return ssh_read_impl(p->impl, buf, cap);
    return local_read_impl(p->impl, buf, cap);
}

/* Send `n` bytes to the shell stdin (typed input, paste, mouse
   reports, etc.). Best-effort: short writes are not retried here. */
void pty_write(Pty *p, const uint8_t *buf, size_t n) {
    if (!p) return;
    if (p->kind == PTY_SSH) ssh_write_impl(p->impl, buf, n);
    else                    local_write_impl(p->impl, buf, n);
}

/* Notify the shell of a new terminal size (TIOCSWINSZ on Unix,
   ResizePseudoConsole on Windows, "window-change" on SSH). */
void pty_resize(Pty *p, int cols, int rows) {
    if (!p) return;
    if (p->kind == PTY_SSH) ssh_resize_impl(p->impl, cols, rows);
    else                    local_resize_impl(p->impl, cols, rows);
}

/* Best-effort current-working-directory lookup of the shell process.
   Local-only: SSH sessions return false (the remote cwd isn't
   reachable through libssh); Windows local also returns false. */
bool pty_cwd(Pty *p, char *out, size_t cap) {
    if (!p) return false;
    if (p->kind == PTY_SSH) return false;
    return local_cwd_impl(p->impl, out, cap);
}

bool pty_is_local(Pty *p) {
    return p && p->kind == PTY_LOCAL;
}

/* Publish the screen's cursor position to the backend so it can
   fast-path CSI 6n (DSR) replies from the reader thread. Local-only
   for now; SSH path is no-op since the I/O thread already pushes
   bytes to the parser without frame-rate gating. */
void pty_snap_cursor(Pty *p, int cy, int cx) {
    if (!p) return;
    if (p->kind == PTY_SSH) return;
    local_snap_cursor_impl(p->impl, cy, cx);
}
