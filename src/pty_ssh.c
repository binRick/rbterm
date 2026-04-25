/* SSH backend for rbterm via libssh. Cross-platform.
 *
 * Connect + auth + channel setup run in blocking mode on the
 * caller's thread (SSH handshake stalls ~1-2 s). Once the channel
 * is up, a dedicated reader thread keeps the channel drained into a
 * ring buffer, mirroring the local-PTY pattern in pty_unix.c —
 * without it, SSH throughput is capped at one read per UI frame
 * and `find /usr | head -10000` runs ~5x slower than iTerm2.
 *
 * Threading: libssh sessions aren't thread-safe by default. We
 * serialise every libssh call (read / write / resize / close)
 * behind `session_lock`. The reader holds the lock only during
 * the actual ssh_channel_read_timeout call (50ms max) so the main
 * thread's writes interleave fluidly between reads.
 *
 * Auth is key-based: explicit private key if the form supplies one,
 * otherwise ssh_userauth_publickey_auto (ssh-agent + ~/.ssh/id_*).
 * Host keys are trust-on-first-use: unknown keys go into
 * ~/.ssh/known_hosts; a changed key aborts with an error. */

#include "pty_internal.h"

#include <libssh/libssh.h>

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SSH_RING_CAP (1u << 20)   /* 1 MB per SSH session */

typedef struct {
    ssh_session session;
    ssh_channel channel;
    bool alive;

    /* Reader thread + ring (mirrors pty_unix.c). */
    pthread_t       reader;
    bool            reader_started;
    pthread_mutex_t session_lock;   /* serialises libssh calls */
    pthread_mutex_t ring_lock;      /* protects ring head/tail */
    uint8_t        *ring;
    size_t          head;
    size_t          tail;
    volatile int    stop;
    volatile int    eof;
} SshPty;

/* Reader-thread entry. Polls the SSH channel non-blocking under a
   briefly-held session lock; sleeps a few ms between empty polls
   so write latency from the main thread (i.e. typed input) stays
   low — without the sleep the reader can lock-starve the writer
   and key echo lags noticeably. The session is set non-blocking
   in ssh_open_impl so each call returns immediately with whatever
   libssh has already pumped from the socket. */
static void *ssh_reader(void *arg) {
    SshPty *p = (SshPty *)arg;
    uint8_t buf[16384];
    while (!__atomic_load_n(&p->stop, __ATOMIC_RELAXED)) {
        pthread_mutex_lock(&p->session_lock);
        int n = (p->channel)
            ? ssh_channel_read_nonblocking(p->channel, buf,
                                           (uint32_t)sizeof(buf), 0)
            : SSH_ERROR;
        pthread_mutex_unlock(&p->session_lock);
        if (n > 0) {
            pthread_mutex_lock(&p->ring_lock);
            for (int i = 0; i < n; i++) {
                p->ring[p->head] = buf[i];
                p->head = (p->head + 1) & (SSH_RING_CAP - 1);
                if (p->head == p->tail)
                    p->tail = (p->tail + 1) & (SSH_RING_CAP - 1);
            }
            pthread_mutex_unlock(&p->ring_lock);
            /* Got data — loop hot but yield so a pending writer
               can grab the session lock before we re-acquire. */
            sched_yield();
            continue;
        }
        if (n == SSH_EOF || n == SSH_ERROR) {
            __atomic_store_n(&p->eof, 1, __ATOMIC_RELAXED);
            break;
        }
        /* No data right now — sleep ~3 ms so the writer thread
           wins the lock immediately when the user types. Idle CPU
           stays well under 1%. */
        struct timespec ts = { 0, 3 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    return NULL;
}

/* printf-style writer for the caller-supplied error buffer.
   NULL/zero-cap-safe so call sites don't have to guard. */
static void set_err(char *err, size_t errsz, const char *fmt, ...) {
    if (!err || errsz == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errsz, fmt, ap);
    va_end(ap);
}

/* Replace leading "~/" with $HOME (or %USERPROFILE% on Windows). */
static void expand_tilde(const char *in, char *out, size_t cap) {
    if (cap == 0) return;
    out[0] = 0;
    if (!in) return;
    if (in[0] == '~' && (in[1] == '/' || in[1] == 0 || in[1] == '\\')) {
        const char *home = getenv("HOME");
#ifdef _WIN32
        if (!home || !*home) home = getenv("USERPROFILE");
#endif
        if (home && *home) {
            snprintf(out, cap, "%s%s", home, in + 1);
            return;
        }
    }
    strncpy(out, in, cap - 1);
    out[cap - 1] = 0;
}

/* Establish an SSH session, authenticate, and request an interactive
   shell on a remote PTY of the given size. Connect/auth runs blocking
   on the caller's thread (~1-2s of UI freeze); the session flips to
   non-blocking before returning so subsequent reads/writes don't
   stall the main loop. On failure writes a human-readable reason
   into `err` and returns NULL — see set_err / TRIED above. Auth
   order: explicit password, explicit key file, ssh-agent, default
   ~/.ssh/id_* keys. */
void *ssh_open_impl(const char *user, const char *host, int port,
                    const char *password, const char *keyfile,
                    int cols, int rows,
                    char *err, size_t errsz) {
    if (!host || !*host) {
        set_err(err, errsz, "Host is required");
        return NULL;
    }

    SshPty *p = calloc(1, sizeof(*p));
    if (!p) { set_err(err, errsz, "out of memory"); return NULL; }
    p->session = ssh_new();
    if (!p->session) {
        set_err(err, errsz, "ssh_new() failed");
        free(p);
        return NULL;
    }

    int verbosity = SSH_LOG_WARNING;
    ssh_options_set(p->session, SSH_OPTIONS_LOG_VERBOSITY, &verbosity);
    long timeout_s = 10;
    ssh_options_set(p->session, SSH_OPTIONS_TIMEOUT, &timeout_s);

    /* `host` may be an alias from ~/.ssh/config (e.g. "mia"). Set it,
       then let libssh parse ~/.ssh/config to apply HostName, User,
       Port, IdentityFile, ProxyJump, PubkeyAcceptedAlgorithms and
       everything else the user has configured. Form-supplied values
       override the config ones below. */
    ssh_options_set(p->session, SSH_OPTIONS_HOST, host);
    if (ssh_options_parse_config(p->session, NULL) != 0) {
        fprintf(stderr, "rbterm: ssh_options_parse_config: %s\n",
                ssh_get_error(p->session));
    }

    if (user && *user) {
        ssh_options_set(p->session, SSH_OPTIONS_USER, user);
    }
    if (port > 0) {
        ssh_options_set(p->session, SSH_OPTIONS_PORT, &port);
    }

    /* Log what libssh settled on so the user can see whether the
       config alias resolved the way they expect. */
    char *resolved_host = NULL, *resolved_user = NULL;
    unsigned int resolved_port = 0;
    ssh_options_get(p->session, SSH_OPTIONS_HOST, &resolved_host);
    ssh_options_get(p->session, SSH_OPTIONS_USER, &resolved_user);
    ssh_options_get_port(p->session, &resolved_port);
    fprintf(stderr, "rbterm: ssh connecting as %s@%s:%u (alias: %s)\n",
            resolved_user ? resolved_user : "?",
            resolved_host ? resolved_host : host,
            resolved_port,
            host);
    ssh_string_free_char(resolved_host);
    ssh_string_free_char(resolved_user);

    if (ssh_connect(p->session) != SSH_OK) {
        set_err(err, errsz, "connect %s@%s:%d: %s",
                user, host, port, ssh_get_error(p->session));
        ssh_free(p->session);
        free(p);
        return NULL;
    }

    enum ssh_known_hosts_e khs = ssh_session_is_known_server(p->session);
    if (khs == SSH_KNOWN_HOSTS_CHANGED) {
        set_err(err, errsz,
                "host key for %s changed — refusing to connect", host);
        goto fail;
    }
    if (khs == SSH_KNOWN_HOSTS_NOT_FOUND || khs == SSH_KNOWN_HOSTS_UNKNOWN) {
        ssh_session_update_known_hosts(p->session);
    }

    int auth = SSH_AUTH_ERROR;

    /* Track which methods we attempted so we can report something
       concrete back to the user if nothing worked. */
    char tried[1024];
    size_t tried_len = 0;
    #define TRIED(fmt, ...) do { \
        int _w = snprintf(tried + tried_len, sizeof(tried) - tried_len, \
                          (tried_len ? ", " fmt : fmt), ##__VA_ARGS__); \
        if (_w > 0 && (size_t)_w < sizeof(tried) - tried_len) tried_len += (size_t)_w; \
    } while (0)

    /* Advertise "none" first so the server tells us which methods it
       actually accepts — lets us skip auth types the server rejects. */
    (void)ssh_userauth_none(p->session, NULL);

    /* 1. Password (if provided). Try plain "password" auth first; if the
          server only accepts keyboard-interactive (common with PAM),
          fall through and answer every prompt with the same password.
          If the server demands a 2FA code this will fail; that's fine
          for v1 — user can still fall back to pubkey. */
    if (password && *password) {
        TRIED("password");
        auth = ssh_userauth_password(p->session, NULL, password);
        fprintf(stderr, "rbterm: ssh password auth => %s\n",
                auth == SSH_AUTH_SUCCESS ? "ok" : ssh_get_error(p->session));

        if (auth != SSH_AUTH_SUCCESS) {
            TRIED("keyboard-interactive");
            int rc = ssh_userauth_kbdint(p->session, NULL, NULL);
            /* Bail out early if the server doesn't even offer kbdint so
               we don't spin on SSH_AUTH_DENIED. */
            int spins = 0;
            while (rc == SSH_AUTH_INFO && spins++ < 8) {
                int n = ssh_userauth_kbdint_getnprompts(p->session);
                for (int i = 0; i < n; i++) {
                    char echo = 0;
                    (void)ssh_userauth_kbdint_getprompt(p->session, i, &echo);
                    ssh_userauth_kbdint_setanswer(p->session, i, password);
                }
                rc = ssh_userauth_kbdint(p->session, NULL, NULL);
            }
            auth = rc;
            fprintf(stderr, "rbterm: ssh kbd-interactive auth => %s\n",
                    auth == SSH_AUTH_SUCCESS ? "ok" : ssh_get_error(p->session));
        }
    }

    /* 2. Explicit private key path. If the user typed a `.pub` path by
          mistake, strip it — libssh wants the private key counterpart. */
    if (auth != SSH_AUTH_SUCCESS && keyfile && *keyfile) {
        char expanded[1024];
        expand_tilde(keyfile, expanded, sizeof(expanded));
        size_t el = strlen(expanded);
        if (el > 4 && strcmp(expanded + el - 4, ".pub") == 0) {
            expanded[el - 4] = 0;
            fprintf(stderr, "rbterm: stripped '.pub' from key path; "
                            "using private key %s\n", expanded);
        }
        ssh_key priv = NULL;
        int r = ssh_pki_import_privkey_file(expanded, NULL, NULL, NULL, &priv);
        if (r != SSH_OK || !priv) {
            set_err(err, errsz, "can't load key %s%s%s",
                    expanded,
                    r == SSH_EOF ? " (passphrase-protected?)" : "",
                    ssh_get_error(p->session));
            goto fail;
        }
        TRIED("key %s", expanded);
        auth = ssh_userauth_publickey(p->session, NULL, priv);
        ssh_key_free(priv);
        fprintf(stderr, "rbterm: ssh explicit-key auth => %s\n",
                auth == SSH_AUTH_SUCCESS ? "ok" : ssh_get_error(p->session));
    }

    /* 3. ssh-agent (explicit so we can log what happened). */
    if (auth != SSH_AUTH_SUCCESS && !(password && *password)) {
        const char *sock = getenv("SSH_AUTH_SOCK");
        if (sock && *sock) {
            TRIED("ssh-agent");
            auth = ssh_userauth_agent(p->session, NULL);
            fprintf(stderr, "rbterm: ssh-agent auth (%s) => %s\n",
                    sock,
                    auth == SSH_AUTH_SUCCESS ? "ok" : ssh_get_error(p->session));
        } else {
            fprintf(stderr, "rbterm: SSH_AUTH_SOCK not set — skipping agent "
                            "(GUI launches on macOS often lose this; try "
                            "`./run.sh` from a login shell)\n");
        }
    }

    /* 4. Each of the common default identity files, explicitly. Lets us
          report "tried id_rsa" in the error and also works around edge
          cases where ssh_userauth_publickey_auto silently skips a key. */
    if (auth != SSH_AUTH_SUCCESS && !(password && *password)) {
        static const char *defaults[] = {
            "~/.ssh/id_ed25519",
            "~/.ssh/id_ecdsa",
            "~/.ssh/id_rsa",
            "~/.ssh/id_dsa",
            NULL,
        };
        for (int i = 0; defaults[i] && auth != SSH_AUTH_SUCCESS; i++) {
            char path[1024];
            expand_tilde(defaults[i], path, sizeof(path));
            FILE *fp = fopen(path, "r");
            if (!fp) continue;
            fclose(fp);
            ssh_key priv = NULL;
            int r = ssh_pki_import_privkey_file(path, NULL, NULL, NULL, &priv);
            if (r != SSH_OK || !priv) {
                fprintf(stderr, "rbterm: %s: can't load (passphrase?)\n", path);
                TRIED("%s [load failed]", defaults[i]);
                continue;
            }
            TRIED("%s", defaults[i]);
            auth = ssh_userauth_publickey(p->session, NULL, priv);
            ssh_key_free(priv);
            fprintf(stderr, "rbterm: %s => %s\n", path,
                    auth == SSH_AUTH_SUCCESS ? "ok" : ssh_get_error(p->session));
        }
    }

    if (auth != SSH_AUTH_SUCCESS) {
        const char *msg = ssh_get_error(p->session);
        if (tried_len > 0) {
            set_err(err, errsz, "auth failed. Tried: %s%s%s",
                    tried,
                    (msg && *msg) ? ". Last error: " : "",
                    msg ? msg : "");
        } else {
            set_err(err, errsz,
                    "auth failed — no credentials available (no password, "
                    "no key file, no ssh-agent, no ~/.ssh/id_*)");
        }
        goto fail;
    }
    #undef TRIED

    p->channel = ssh_channel_new(p->session);
    if (!p->channel) {
        set_err(err, errsz, "ssh_channel_new: %s", ssh_get_error(p->session));
        goto fail;
    }
    if (ssh_channel_open_session(p->channel) != SSH_OK) {
        set_err(err, errsz, "open session: %s", ssh_get_error(p->session));
        goto fail;
    }
    if (ssh_channel_request_pty_size(p->channel, "xterm-256color", cols, rows) != SSH_OK) {
        set_err(err, errsz, "request pty: %s", ssh_get_error(p->session));
        goto fail;
    }
    if (ssh_channel_request_shell(p->channel) != SSH_OK) {
        set_err(err, errsz, "request shell: %s", ssh_get_error(p->session));
        goto fail;
    }

    /* Non-blocking session so the reader thread's
       ssh_channel_read_nonblocking returns immediately with
       whatever libssh has already pumped. The reader sleeps a few
       ms between empty polls instead of holding the lock. */
    ssh_set_blocking(p->session, 0);
    p->ring = malloc(SSH_RING_CAP);
    if (!p->ring) {
        set_err(err, errsz, "out of memory (ring)");
        goto fail;
    }
    pthread_mutex_init(&p->session_lock, NULL);
    pthread_mutex_init(&p->ring_lock, NULL);
    if (pthread_create(&p->reader, NULL, ssh_reader, p) != 0) {
        set_err(err, errsz, "pthread_create");
        pthread_mutex_destroy(&p->session_lock);
        pthread_mutex_destroy(&p->ring_lock);
        free(p->ring);
        goto fail;
    }
    p->reader_started = true;
    p->alive = true;
    return p;

fail:
    if (p->channel) {
        ssh_channel_close(p->channel);
        ssh_channel_free(p->channel);
    }
    if (p->session) {
        ssh_disconnect(p->session);
        ssh_free(p->session);
    }
    free(p);
    return NULL;
}

/* Close the channel, signal + join the reader thread, free libssh
   handles. The reader's 50ms timeout caps shutdown latency. */
void ssh_close_impl(void *impl) {
    SshPty *p = impl;
    if (!p) return;
    __atomic_store_n(&p->stop, 1, __ATOMIC_RELAXED);
    if (p->reader_started) {
        pthread_join(p->reader, NULL);
        p->reader_started = false;
    }
    if (p->channel) {
        pthread_mutex_lock(&p->session_lock);
        ssh_channel_close(p->channel);
        ssh_channel_free(p->channel);
        p->channel = NULL;
        pthread_mutex_unlock(&p->session_lock);
    }
    if (p->session) {
        ssh_disconnect(p->session);
        ssh_free(p->session);
    }
    if (p->ring) {
        pthread_mutex_destroy(&p->ring_lock);
        pthread_mutex_destroy(&p->session_lock);
        free(p->ring);
    }
    free(p);
}

/* True while the channel is open + the reader hasn't seen EOF. */
bool ssh_alive_impl(void *impl) {
    SshPty *p = impl;
    if (!p || !p->alive) return false;
    if (__atomic_load_n(&p->eof, __ATOMIC_RELAXED)) return false;
    return true;
}

/* Drain bytes the reader thread has already buffered. Returns the
   count, 0 when nothing's pending but the channel is still up,
   -1 once the reader has signalled EOF and the ring is empty. */
int ssh_read_impl(void *impl, uint8_t *buf, size_t cap) {
    SshPty *p = impl;
    if (!p || !p->ring) return -1;
    size_t n = 0;
    pthread_mutex_lock(&p->ring_lock);
    while (n < cap && p->tail != p->head) {
        buf[n++] = p->ring[p->tail];
        p->tail = (p->tail + 1) & (SSH_RING_CAP - 1);
    }
    pthread_mutex_unlock(&p->ring_lock);
    if (n > 0) return (int)n;
    if (__atomic_load_n(&p->eof, __ATOMIC_RELAXED)) {
        p->alive = false;
        return -1;
    }
    return 0;
}

/* Send bytes to the remote shell. Holds session_lock so it can't
   collide with the reader thread's libssh call. SSH_ERROR marks
   the channel dead so pty_alive() flips false on the next poll. */
void ssh_write_impl(void *impl, const uint8_t *buf, size_t n) {
    SshPty *p = impl;
    if (!p || !p->channel) return;
    pthread_mutex_lock(&p->session_lock);
    size_t off = 0;
    while (off < n && p->channel) {
        int w = ssh_channel_write(p->channel, buf + off, (uint32_t)(n - off));
        if (w == SSH_ERROR) { p->alive = false; break; }
        if (w <= 0) break;
        off += (size_t)w;
    }
    pthread_mutex_unlock(&p->session_lock);
}

/* Send a "window-change" SSH_MSG_CHANNEL_REQUEST under the session
   lock. Result ignored — if the remote disagrees there's nothing
   useful we can do. */
void ssh_resize_impl(void *impl, int cols, int rows) {
    SshPty *p = impl;
    if (!p || !p->channel) return;
    pthread_mutex_lock(&p->session_lock);
    if (p->channel) ssh_channel_change_pty_size(p->channel, cols, rows);
    pthread_mutex_unlock(&p->session_lock);
}
