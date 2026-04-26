/* Null SSH backend for builds configured with -DRBTERM_SSH=OFF.
 * Keeps the dispatch layer linkable without pulling in libssh. The
 * SSH modal still renders, but pty_open_ssh fails immediately with an
 * explanatory message. */

#include "pty_internal.h"
#include <stdint.h>
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

/* SFTP upload — no-op stubs. The upload UI shouldn't be reachable
   on the no-SSH build (SSH tabs never open), but we still need to
   satisfy the dispatch layer's link symbols. */
struct PtyUpload *ssh_upload_start_impl(void *impl, const char *l, const char *r,
                                        char *err, size_t errsz) {
    (void)impl; (void)l; (void)r;
    if (err && errsz) snprintf(err, errsz, "SSH disabled");
    return NULL;
}
int  ssh_upload_status_impl(struct PtyUpload *u, uint64_t *bd, uint64_t *bt,
                            char *err, size_t errsz) {
    (void)u; (void)bd; (void)bt;
    if (err && errsz) snprintf(err, errsz, "SSH disabled");
    return -1;
}
const char *ssh_upload_name_impl(struct PtyUpload *u) { (void)u; return ""; }
void ssh_upload_release_impl(struct PtyUpload *u)     { (void)u; }

/* SFTP listdir + download — same story. */
struct PtyDirEntry *ssh_listdir_impl(void *impl, const char *d, int *count,
                                     char *err, size_t errsz) {
    (void)impl; (void)d;
    if (count) *count = 0;
    if (err && errsz) snprintf(err, errsz, "SSH disabled");
    return NULL;
}
struct PtyDownload *ssh_download_start_impl(void *impl, const char *r, const char *l,
                                            char *err, size_t errsz) {
    (void)impl; (void)r; (void)l;
    if (err && errsz) snprintf(err, errsz, "SSH disabled");
    return NULL;
}
int  ssh_download_status_impl(struct PtyDownload *d, uint64_t *bd, uint64_t *bt,
                              char *err, size_t errsz) {
    (void)d; (void)bd; (void)bt;
    if (err && errsz) snprintf(err, errsz, "SSH disabled");
    return -1;
}
const char *ssh_download_name_impl(struct PtyDownload *d) { (void)d; return ""; }
void ssh_download_release_impl(struct PtyDownload *d)     { (void)d; }
