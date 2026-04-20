/* Dispatch layer for the Pty abstraction.
 * `struct Pty` (see pty_internal.h) owns a PtyKind tag and a void* impl;
 * every public pty_* call routes to the appropriate backend. */

#include "pty.h"
#include "pty_internal.h"

#include <stdlib.h>
#include <string.h>

Pty *pty_open(int cols, int rows) {
    void *impl = local_open_impl(cols, rows);
    if (!impl) return NULL;
    Pty *p = calloc(1, sizeof(*p));
    if (!p) { local_close_impl(impl); return NULL; }
    p->kind = PTY_LOCAL;
    p->impl = impl;
    return p;
}

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

void pty_close(Pty *p) {
    if (!p) return;
    if (p->kind == PTY_SSH) ssh_close_impl(p->impl);
    else                    local_close_impl(p->impl);
    free(p);
}

bool pty_alive(Pty *p) {
    if (!p) return false;
    if (p->kind == PTY_SSH) return ssh_alive_impl(p->impl);
    return local_alive_impl(p->impl);
}

int pty_read(Pty *p, uint8_t *buf, size_t cap) {
    if (!p) return -1;
    if (p->kind == PTY_SSH) return ssh_read_impl(p->impl, buf, cap);
    return local_read_impl(p->impl, buf, cap);
}

void pty_write(Pty *p, const uint8_t *buf, size_t n) {
    if (!p) return;
    if (p->kind == PTY_SSH) ssh_write_impl(p->impl, buf, n);
    else                    local_write_impl(p->impl, buf, n);
}

void pty_resize(Pty *p, int cols, int rows) {
    if (!p) return;
    if (p->kind == PTY_SSH) ssh_resize_impl(p->impl, cols, rows);
    else                    local_resize_impl(p->impl, cols, rows);
}

bool pty_cwd(Pty *p, char *out, size_t cap) {
    if (!p) return false;
    if (p->kind == PTY_SSH) return false;
    return local_cwd_impl(p->impl, out, cap);
}
