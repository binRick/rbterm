#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Cross-platform PTY abstraction.
 *
 * The Unix backend (pty_unix.c) uses forkpty + non-blocking read.
 * The Windows backend (pty_win.c) uses ConPTY + a reader thread that
 * feeds a ring buffer so the main loop can poll without blocking.
 */

typedef struct Pty Pty;

/* Spawn the user's default shell inside a PTY sized `cols`x`rows`.
   `cwd` selects the child's initial working directory; NULL or ""
   falls back to $HOME. Returns NULL on failure. */
Pty *pty_open(int cols, int rows, const char *cwd);

/* Connect a PTY to a remote shell over SSH.
 *   user:     NULL/"" → $USER / $USERNAME.
 *   host:     required, e.g. "myserver.example.com".
 *   port:     <= 0 → 22.
 *   password: NULL/"" → skip password auth; otherwise tried first.
 *   keyfile:  NULL/"" → ssh_userauth_publickey_auto (agent + ~/.ssh/id_*);
 *             otherwise an explicit private-key path.
 * Auth order: password (if set) → explicit key (if set) → publickey_auto.
 * On failure, returns NULL and writes a human-readable reason into `err`
 * (if non-NULL). Host keys are trust-on-first-use: unknown keys are
 * added to ~/.ssh/known_hosts; a *changed* key aborts. */
Pty *pty_open_ssh(const char *user, const char *host, int port,
                  const char *password, const char *keyfile,
                  int cols, int rows,
                  char *err, size_t errsz);

/* Kill (SIGHUP or TerminateProcess) + wait for child, release all
   resources. Safe to call on a dead Pty. */
void pty_close(Pty *p);

/* True if the child process is still running. */
bool pty_alive(Pty *p);

/* Non-blocking read. Returns the number of bytes placed in `buf`, 0 if no
   data is currently available, or -1 if the child has exited / the PTY
   is permanently closed. */
int  pty_read(Pty *p, uint8_t *buf, size_t cap);

/* Best-effort write of all bytes. */
void pty_write(Pty *p, const uint8_t *buf, size_t n);

/* Notify the child of a new terminal size. */
void pty_resize(Pty *p, int cols, int rows);

/* Best-effort current-working-directory query for the child shell.
   Returns true iff `out` was populated with a NUL-terminated path. Not
   implemented on Windows (returns false); on Windows users rely on the
   shell's OSC 0/2 title instead. */
bool pty_cwd(Pty *p, char *out, size_t cap);

/* True iff `p` is a local-shell PTY (forkpty / ConPTY); false for
   SSH sessions. Used by main.c to route the HUD probe to the right
   data source. */
bool pty_is_local(Pty *p);

/* Publish the screen's cursor position so the local backend's
   reader thread can fast-path CSI 6n (Device Status Report)
   replies — sending a response from the reader thread the moment
   the query arrives, rather than waiting for the next render
   frame to drain the ring buffer. Call after every `screen_feed`.
   No-op on backends that don't have the optimisation (currently
   SSH; the SSH I/O thread already hands data straight to the
   parser via libssh callbacks). */
void pty_snap_cursor(Pty *p, int cy, int cx);
