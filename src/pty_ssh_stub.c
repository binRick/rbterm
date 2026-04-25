/* Null SSH backend for builds configured with -DRBTERM_SSH=OFF.
 * Keeps the dispatch layer linkable without pulling in libssh. The
 * SSH modal still renders, but pty_open_ssh fails immediately with an
 * explanatory message. */

#include "pty_internal.h"
#include <stdio.h>
#include <string.h>

void *ssh_open_impl(const char *user, const char *host, int port,
                    const char *password, const char *keyfile,
                    int cols, int rows,
                    char *err, size_t errsz) {
    (void)user; (void)host; (void)port; (void)password; (void)keyfile;
    (void)cols; (void)rows;
    if (err && errsz > 0) {
        snprintf(err, errsz,
                 "SSH support is not compiled into this rbterm build. "
                 "Rebuild with -DRBTERM_SSH=ON (requires libssh + mbedTLS).");
    }
    return NULL;
}

void ssh_close_impl(void *impl)                       { (void)impl; }
bool ssh_alive_impl(void *impl)                       { (void)impl; return false; }
int  ssh_read_impl(void *impl, uint8_t *buf, size_t cap) { (void)impl; (void)buf; (void)cap; return -1; }
void ssh_write_impl(void *impl, const uint8_t *buf, size_t n) { (void)impl; (void)buf; (void)n; }
void ssh_resize_impl(void *impl, int cols, int rows)  { (void)impl; (void)cols; (void)rows; }
bool ssh_hud_snapshot_impl(void *impl, struct PtyHudSnapshot *out) { (void)impl; (void)out; return false; }
