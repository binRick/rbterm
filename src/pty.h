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
   Returns NULL on failure. */
Pty *pty_open(int cols, int rows);

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
