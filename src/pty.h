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

/* HUD snapshot — last batch of remote-host stats gathered by an
   SSH backend probe thread. Local PTYs don't populate this and
   pty_hud_snapshot returns false for them; main.c polls local
   stats directly via the syscalls in hud.c. */
typedef struct PtyHudSnapshot {
    char hostname[64];
    char ip[48];
    double load1;          /* -1 if unknown */
    long   mem_free_mb;    /* -1 if unknown */
    int    disk_free_pct;  /* -1 if unknown */

    /* Cumulative CPU tick counters (busy + total). main.c keeps the
       previous sample and computes % busy as a delta the same way it
       does for local panes — single-point readings are meaningless. */
    unsigned long long cpu_busy;
    unsigned long long cpu_total;
    bool cpu_valid;        /* false until the probe has parsed at least one CPU line */
} PtyHudSnapshot;

/* Fill `out` with the most recent HUD snapshot the SSH probe
   produced. Returns true if a usable snapshot is available
   (timestamp non-zero), false if the pane isn't SSH or the probe
   hasn't reported yet. Thread-safe — copies under the session
   mutex. */
bool pty_hud_snapshot(Pty *p, PtyHudSnapshot *out);

/* Publish the screen's cursor position so the local backend's
   reader thread can fast-path CSI 6n (Device Status Report)
   replies — sending a response from the reader thread the moment
   the query arrives, rather than waiting for the next render
   frame to drain the ring buffer. Call after every `screen_feed`.
   No-op on backends that don't have the optimisation (currently
   SSH; the SSH I/O thread already hands data straight to the
   parser via libssh callbacks). */
void pty_snap_cursor(Pty *p, int cy, int cx);

/* SFTP upload (SSH only). Spawns a worker thread that streams the
   local file to `remote_path` over the same libssh session, taking
   the session lock cooperatively with read / write / probe. Caller
   polls via pty_upload_status and frees the handle with
   pty_upload_release once it's done with status display. Returns
   NULL for non-SSH PTYs or if the upload couldn't be started. */
typedef struct PtyUpload PtyUpload;

PtyUpload *pty_upload_start(Pty *p, const char *local_path,
                            const char *remote_path,
                            char *err, size_t errsz);

/* Atomic snapshot. Returns:
     0  → still in flight; bytes_done / bytes_total reflect progress
     1  → completed successfully
    -1  → failed; err filled with libssh / errno reason. */
int  pty_upload_status(PtyUpload *u,
                       uint64_t *bytes_done, uint64_t *bytes_total,
                       char *err, size_t errsz);

/* Display name (basename of local_path). Stable for the upload's
   lifetime; valid until pty_upload_release. */
const char *pty_upload_name(PtyUpload *u);

/* Joins the worker thread (if any) and frees the handle. Safe to
   call before completion (cancels by best-effort: the worker will
   notice on its next chunk boundary and bail). */
void pty_upload_release(PtyUpload *u);

/* SFTP listdir — synchronous; returns a heap-allocated array of
   entries (caller frees via pty_listdir_free). Sorted directories-
   first, then files alphabetically. Hidden entries ("." / "..")
   are excluded except "..", which is included so the picker can
   navigate up. NULL on failure, with `err` populated. */
typedef struct {
    char     name[256];
    uint64_t size;
    long     mtime;       /* unix seconds; 0 if unknown */
    bool     is_dir;
    bool     is_symlink;
} PtyDirEntry;

PtyDirEntry *pty_listdir(Pty *p, const char *remote_dir, int *count_out,
                         char *err, size_t errsz);
void pty_listdir_free(PtyDirEntry *entries);

/* SFTP download — same pattern as pty_upload_*. Caller polls
   pty_download_status and frees with pty_download_release once
   it's done with status display. */
typedef struct PtyDownload PtyDownload;

PtyDownload *pty_download_start(Pty *p, const char *remote_path,
                                const char *local_path,
                                char *err, size_t errsz);
int  pty_download_status(PtyDownload *d,
                         uint64_t *bytes_done, uint64_t *bytes_total,
                         char *err, size_t errsz);
const char *pty_download_name(PtyDownload *d);
void pty_download_release(PtyDownload *d);
