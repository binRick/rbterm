#pragma once
/* Shared PTY internals.
 *
 * `struct Pty` is defined here and dispatches each operation to the right
 * backend based on `kind`:
 *   - PTY_LOCAL: local shell via forkpty / ConPTY (local_*_impl)
 *   - PTY_SSH:   remote session via libssh             (ssh_*_impl)
 *
 * Each backend's impl functions take a `void *impl` which they cast to
 * their own per-kind state struct (LocalPty / SshPty). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { PTY_LOCAL, PTY_SSH } PtyKind;

struct Pty {
    PtyKind kind;
    void *impl;
};

/* Local backend (src/pty_unix.c on macOS/Linux, src/pty_win.c on Windows). */
void *local_open_impl(int cols, int rows);
void  local_close_impl(void *impl);
bool  local_alive_impl(void *impl);
int   local_read_impl(void *impl, uint8_t *buf, size_t cap);
void  local_write_impl(void *impl, const uint8_t *buf, size_t n);
void  local_resize_impl(void *impl, int cols, int rows);
bool  local_cwd_impl(void *impl, char *out, size_t cap);

/* SSH backend (src/pty_ssh.c, cross-platform via libssh). See pty.h. */
void *ssh_open_impl(const char *user, const char *host, int port,
                    const char *password, const char *keyfile,
                    int cols, int rows,
                    char *err, size_t errsz);
void  ssh_close_impl(void *impl);
bool  ssh_alive_impl(void *impl);
int   ssh_read_impl(void *impl, uint8_t *buf, size_t cap);
void  ssh_write_impl(void *impl, const uint8_t *buf, size_t n);
void  ssh_resize_impl(void *impl, int cols, int rows);
