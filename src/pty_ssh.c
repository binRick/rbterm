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
#include "pty.h"        /* PtyHudSnapshot */

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

    /* HUD probe thread — runs a small remote command every 2 sec on
       a dedicated exec channel, parses the output, and stores it in
       `hud_snap` under hud_lock. main.c reads via
       ssh_hud_snapshot_impl. trylock against session_lock so heavy
       shell I/O doesn't get starved. */
    pthread_t       hud_probe;
    bool            hud_probe_started;
    volatile int    hud_probe_stop;
    pthread_mutex_t hud_lock;
    PtyHudSnapshot  hud_snap;
    bool            hud_snap_valid;
} SshPty;

/* Reader-thread entry. Polls the SSH channel non-blocking under a
   briefly-held session lock; sleeps a few ms between empty polls
   so write latency from the main thread (i.e. typed input) stays
   low — without the sleep the reader can lock-starve the writer
   and key echo lags noticeably. The session is set non-blocking
   in ssh_open_impl so each call returns immediately with whatever
   libssh has already pumped from the socket. */

/* Forward decl — defined further down (alongside the HUD probe
   helpers) but ssh_open_impl needs it as a pthread entry point. */
static void *ssh_hud_probe(void *arg);

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

    /* Forward enough of the local environment that the remote shell
       behaves the same way it would if the user had typed `ssh host`
       from rbterm's local PTY (where pty_unix sets these directly).
       Best-effort: many sshd configs only accept names listed in
       AcceptEnv (default: LANG and LC_*) and silently drop the rest.
       That used to leave `pal random` over libssh broken because
       tmux on the remote ran in 256-colour mode without COLORTERM
       — but pal itself is now tmux-aware (DCS-tmux passthrough),
       so the remaining missing env on stock sshd is cosmetic only.
       No visible-stdin fallback: writing `export …; clear` as the
       first input flashed the export line before the prompt cleared
       it, which the user noticed. */
    {
        struct { const char *name; const char *value; } envs[] = {
            { "COLORTERM",            "truecolor" },
            { "TERM_PROGRAM",         "rbterm"    },
            { "TERM_PROGRAM_VERSION", "0.1.0"     },
            { "LANG",                 "en_US.UTF-8" },
            { "LC_CTYPE",             "en_US.UTF-8" },
        };
        for (size_t i = 0; i < sizeof(envs) / sizeof(envs[0]); i++) {
            (void)ssh_channel_request_env(p->channel,
                                          envs[i].name, envs[i].value);
        }
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
    pthread_mutex_init(&p->hud_lock, NULL);
    if (pthread_create(&p->reader, NULL, ssh_reader, p) != 0) {
        set_err(err, errsz, "pthread_create");
        pthread_mutex_destroy(&p->session_lock);
        pthread_mutex_destroy(&p->ring_lock);
        pthread_mutex_destroy(&p->hud_lock);
        free(p->ring);
        goto fail;
    }
    p->reader_started = true;
    /* HUD probe runs in its own thread so the 100-500ms exec
       round-trip can't stall the shell reader. Failure is silent —
       the snapshot just stays !valid and main.c will treat the
       pane as having no remote stats yet. */
    if (pthread_create(&p->hud_probe, NULL, ssh_hud_probe, p) == 0) {
        p->hud_probe_started = true;
    }
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

/* HUD probe shell snippet — Linux + BSD/macOS portable, sub-100ms
   typical, silent-failure tolerant. Each line is `KEY=VALUE` ending
   with `END=1` so the parser knows when to stop reading. CPU line
   is cumulative tick counts; main.c computes % busy as a delta.

   Wrapped in `sh -c` so we don't depend on the user's login shell
   being sh-compatible (some remotes default to fish or csh, where
   our $(...) and arithmetic expansion would break). */
static const char *HUD_PROBE_SCRIPT =
    "exec /bin/sh -c '"
    /* Hostname: env var first (always set by login shells), then
       `hostname` command. Strip a trailing newline if any. */
    "H=${HOSTNAME:-$(hostname 2>/dev/null)};"
    /* IPv4: cascade through Linux + BSD locations. */
    "I=$(hostname -I 2>/dev/null | awk \"{print \\$1}\");"
    "[ -z \"$I\" ] && I=$(hostname -i 2>/dev/null | awk \"{print \\$1}\");"
    "[ -z \"$I\" ] && for IF in en0 en1 eth0 ens33 enp0s3 wlan0; do"
    "  V=$(ifconfig $IF 2>/dev/null | awk \"/inet /{print \\$2; exit}\");"
    "  [ -n \"$V\" ] && I=$V && break;"
    "done;"
    "[ -z \"$I\" ] && I=$(ip -4 -o addr 2>/dev/null | awk \"\\$2!=\\\"lo\\\" {sub(/\\/.*/, \\\"\\\", \\$4); print \\$4; exit}\");"
    /* Load average: /proc/loadavg first (cheapest, most reliable),
       then uptime. */
    "L=$(awk \"{print \\$1; exit}\" /proc/loadavg 2>/dev/null);"
    "[ -z \"$L\" ] && L=$(uptime 2>/dev/null | sed -E \"s/.*load averages?: ([0-9.]+).*/\\1/\" | awk \"{print \\$1}\");"
    /* Free memory in MB. Order: free -m ($7 = available on modern
       util-linux); /proc/meminfo MemAvailable; vm_stat + page size
       on macOS. */
    "M=$(free -m 2>/dev/null | awk \"/^Mem:/{print \\$7}\");"
    "[ -z \"$M\" ] && M=$(awk \"/^MemAvailable:/{print int(\\$2/1024); exit}\" /proc/meminfo 2>/dev/null);"
    "if [ -z \"$M\" ]; then"
    "  PS=$(sysctl -n hw.pagesize 2>/dev/null);"
    "  if [ -n \"$PS\" ]; then"
    "    M=$(vm_stat 2>/dev/null | awk -v ps=$PS \"/free/{f=\\$3} /inactive/{i=\\$3} END{gsub(/\\\\./,\\\"\\\",f); gsub(/\\\\./,\\\"\\\",i); print int((f+i)*ps/1024/1024)}\");"
    "  fi;"
    "fi;"
    "D=$(df -P / 2>/dev/null | awk \"NR==2{gsub(/%/,\\\"\\\",\\$5); print 100-\\$5}\");"
    /* Cumulative CPU ticks. */
    "if [ -r /proc/stat ]; then"
    "  set -- $(awk \"/^cpu / {print \\$2,\\$3,\\$4,\\$5,\\$6,\\$7,\\$8,\\$9}\" /proc/stat);"
    "  USER=$1; NICE=$2; SYSTEM=$3; IDLE=$4; IOWAIT=${5:-0}; IRQ=${6:-0}; SOFTIRQ=${7:-0}; STEAL=${8:-0};"
    "  BUSY=$((USER+NICE+SYSTEM+IRQ+SOFTIRQ+STEAL));"
    "  TOTAL=$((BUSY+IDLE+IOWAIT));"
    "else"
    "  CPT=$(sysctl -n kern.cp_time 2>/dev/null);"
    "  set -- $CPT;"
    "  USER=${1:-0}; NICE=${2:-0}; SYSTEM=${3:-0}; INTR=${4:-0}; IDLE=${5:-0};"
    "  BUSY=$((USER+NICE+SYSTEM+INTR));"
    "  TOTAL=$((BUSY+IDLE));"
    "fi;"
    "echo HOST=$H;"
    "echo IP=$I;"
    "echo LOAD=$L;"
    "echo MEM_MB=$M;"
    "echo DISK_PCT=$D;"
    "echo CPU_BUSY=$BUSY;"
    "echo CPU_TOTAL=$TOTAL;"
    "echo END=1;"
    "' 2>/dev/null";

/* Read the next line from `chan` into `out` (NUL-terminated, no
   trailing newline). Returns the byte length or -1 on close/error.
   Reads 1 byte at a time so we can't overshoot past END=1. */
static int read_line_locked(ssh_channel chan, char *out, size_t cap,
                            int timeout_ms) {
    size_t n = 0;
    if (!chan || cap < 2) return -1;
    while (n + 1 < cap) {
        char c;
        int r = ssh_channel_read_timeout(chan, &c, 1, 0, timeout_ms);
        if (r <= 0) return (n > 0) ? (int)n : -1;
        if (c == '\n') break;
        if (c == '\r') continue;
        out[n++] = c;
    }
    out[n] = 0;
    return (int)n;
}

/* Returns true iff RBTERM_HUD_DEBUG is set in the env. Cached
   after the first call. */
static int hud_debug_enabled(void) {
    static int v = -1;
    if (v < 0) {
        const char *dv = getenv("RBTERM_HUD_DEBUG");
        v = (dv && *dv && *dv != '0') ? 1 : 0;
    }
    return v;
}

/* Parse one HUD probe response and write it to `*out` under hud_lock.
   On any parse failure we leave the previous snapshot intact so the
   render keeps the last-known values rather than flicking to "?". */
static bool ssh_run_one_probe(SshPty *p) {
    int dbg = hud_debug_enabled();
    /* Try-lock: skip this probe cycle if shell I/O is mid-burst.
       Better to show stale stats than to add 50ms of latency to a
       cat / find / git log running in the same session. */
    if (pthread_mutex_trylock(&p->session_lock) != 0) {
        if (dbg) fprintf(stderr, "rbterm hud probe: trylock contended, skipping\n");
        return false;
    }
    /* The session is set non-blocking after auth so the shell-reader
       thread can use ssh_channel_read_nonblocking. But channel-open
       and request-exec need to block on the round-trip — in
       non-blocking mode they return SSH_AGAIN (-2) and the channel
       gets confused. Briefly flip to blocking; we restore before
       releasing the lock so the reader thread's next call still
       sees non-blocking. */
    ssh_set_blocking(p->session, 1);
    ssh_channel chan = ssh_channel_new(p->session);
    if (!chan) {
        if (dbg) fprintf(stderr, "rbterm hud probe: ssh_channel_new failed\n");
        ssh_set_blocking(p->session, 0);
        pthread_mutex_unlock(&p->session_lock);
        return false;
    }
    int rc = ssh_channel_open_session(chan);
    if (rc != SSH_OK) {
        if (dbg) fprintf(stderr, "rbterm hud probe: ssh_channel_open_session=%d (%s)\n",
                         rc, ssh_get_error(p->session));
        ssh_channel_free(chan);
        ssh_set_blocking(p->session, 0);
        pthread_mutex_unlock(&p->session_lock);
        return false;
    }
    rc = ssh_channel_request_exec(chan, HUD_PROBE_SCRIPT);
    if (rc != SSH_OK) {
        if (dbg) fprintf(stderr, "rbterm hud probe: ssh_channel_request_exec=%d (%s)\n",
                         rc, ssh_get_error(p->session));
        ssh_channel_close(chan);
        ssh_channel_free(chan);
        ssh_set_blocking(p->session, 0);
        pthread_mutex_unlock(&p->session_lock);
        return false;
    }
    if (dbg) fprintf(stderr, "rbterm hud probe: channel open + exec ok, reading...\n");

    /* Drain the probe's output line by line. We hold the session
       lock the whole time which means shell I/O can't interleave
       — the reader thread will queue up briefly. The probe is
       sized to be done in <100ms on a typical link. */
    PtyHudSnapshot snap = {0};
    snap.load1         = -1;
    snap.mem_free_mb   = -1;
    snap.disk_free_pct = -1;
    bool got_end = false;
    char line[256];
    int line_count = 0;
    for (int i = 0; i < 16; i++) {   /* hard cap on lines */
        int n = read_line_locked(chan, line, sizeof(line), 500);
        if (n <= 0) {
            if (dbg) fprintf(stderr, "rbterm hud probe: read returned %d after %d lines\n",
                             n, line_count);
            break;
        }
        line_count++;
        if (dbg) fprintf(stderr, "rbterm hud probe: %s\n", line);
        if (strncmp(line, "HOST=", 5) == 0) {
            strncpy(snap.hostname, line + 5, sizeof(snap.hostname) - 1);
        } else if (strncmp(line, "IP=", 3) == 0) {
            strncpy(snap.ip, line + 3, sizeof(snap.ip) - 1);
        } else if (strncmp(line, "LOAD=", 5) == 0) {
            snap.load1 = strtod(line + 5, NULL);
        } else if (strncmp(line, "MEM_MB=", 7) == 0) {
            snap.mem_free_mb = strtol(line + 7, NULL, 10);
            if (snap.mem_free_mb == 0 && line[7] != '0') snap.mem_free_mb = -1;
        } else if (strncmp(line, "DISK_PCT=", 9) == 0) {
            char *end = NULL;
            long v = strtol(line + 9, &end, 10);
            if (end != line + 9) snap.disk_free_pct = (int)v;
        } else if (strncmp(line, "CPU_BUSY=", 9) == 0) {
            snap.cpu_busy = strtoull(line + 9, NULL, 10);
        } else if (strncmp(line, "CPU_TOTAL=", 10) == 0) {
            snap.cpu_total = strtoull(line + 10, NULL, 10);
            snap.cpu_valid = (snap.cpu_total > 0);
        } else if (strncmp(line, "END=", 4) == 0) {
            got_end = true;
            break;
        }
    }

    ssh_channel_send_eof(chan);
    ssh_channel_close(chan);
    ssh_channel_free(chan);
    /* Restore non-blocking for the next reader-thread iteration. */
    ssh_set_blocking(p->session, 0);
    pthread_mutex_unlock(&p->session_lock);

    if (!got_end) return false;

    pthread_mutex_lock(&p->hud_lock);
    p->hud_snap = snap;
    p->hud_snap_valid = true;
    pthread_mutex_unlock(&p->hud_lock);
    return true;
}

static void *ssh_hud_probe(void *arg) {
    SshPty *p = (SshPty *)arg;
    if (hud_debug_enabled())
        fprintf(stderr, "rbterm hud probe: thread started\n");
    /* Initial 1 sec delay so the channel/auth dust settles before
       we open a second channel. */
    for (int slept = 0; slept < 10; slept++) {
        if (__atomic_load_n(&p->hud_probe_stop, __ATOMIC_RELAXED)) return NULL;
        struct timespec ts = {0, 100000000};   /* 100ms */
        nanosleep(&ts, NULL);
    }
    int probe_n = 0;
    while (!__atomic_load_n(&p->hud_probe_stop, __ATOMIC_RELAXED) &&
           !__atomic_load_n(&p->stop, __ATOMIC_RELAXED)) {
        probe_n++;
        if (hud_debug_enabled())
            fprintf(stderr, "rbterm hud probe: --- attempt %d ---\n", probe_n);
        ssh_run_one_probe(p);
        for (int slept = 0; slept < 20; slept++) {
            if (__atomic_load_n(&p->hud_probe_stop, __ATOMIC_RELAXED)) return NULL;
            struct timespec ts = {0, 100000000};
            nanosleep(&ts, NULL);
        }
    }
    return NULL;
}

bool ssh_hud_snapshot_impl(void *impl, struct PtyHudSnapshot *out) {
    SshPty *p = impl;
    if (!p || !out) return false;
    pthread_mutex_lock(&p->hud_lock);
    bool ok = p->hud_snap_valid;
    if (ok) *out = p->hud_snap;
    pthread_mutex_unlock(&p->hud_lock);
    return ok;
}

/* Close the channel, signal + join the reader thread, free libssh
   handles. The reader's 50ms timeout caps shutdown latency. */
void ssh_close_impl(void *impl) {
    SshPty *p = impl;
    if (!p) return;
    __atomic_store_n(&p->stop, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&p->hud_probe_stop, 1, __ATOMIC_RELAXED);
    if (p->hud_probe_started) {
        pthread_join(p->hud_probe, NULL);
        p->hud_probe_started = false;
    }
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
        pthread_mutex_destroy(&p->hud_lock);
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

/* ---------------- SFTP upload ----------------
 *
 * The PtyUpload owns its own worker thread. The worker takes
 * `session_lock` to call into libssh (so it cooperates with the
 * reader / writer / HUD probe), opens an SFTP session over the
 * existing SSH session, and streams the local file in chunks.
 * Status is published via atomics so the main thread can poll
 * without locking. The handle is freed by ssh_upload_release_impl,
 * which joins the worker first. */

#include <libssh/sftp.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <stdint.h>
#include <time.h>

#define SFTP_CHUNK_SZ (32u * 1024u)
#define PATH_MAX_FALLBACK 4096

/* Status: 0 = in flight, 1 = done OK, -1 = failed. Matches the
   public pty_upload_status return value. Updated by the worker
   under no lock — single-byte ints are atomic on every platform
   we target. */
struct PtyUpload {
    SshPty   *ssh;
    pthread_t thread;
    bool      thread_started;

    char      local_path[PATH_MAX_FALLBACK];
    char      remote_path[PATH_MAX_FALLBACK];
    char      display_name[256];

    volatile int      status;          /* see above */
    volatile uint64_t bytes_done;
    volatile uint64_t bytes_total;
    char              err[256];        /* written by worker, read by main */

    volatile int      cancel;          /* main asks worker to stop */
};

static void upload_set_err(struct PtyUpload *u, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(u->err, sizeof(u->err), fmt, ap);
    va_end(ap);
}

/* Append a line to ~/rbterm-upload.log so we can post-mortem upload
   failures without a debugger. Always-on (logs are tiny). The file
   is created on first write and grows from there; nothing rotates
   it — clear it manually if it gets large. */
static void upload_log(const char *fmt, ...) {
    char path[PATH_MAX_FALLBACK];
    const char *home = getenv("HOME");
    if (!home || !*home) home = "/tmp";
    snprintf(path, sizeof(path), "%s/rbterm-upload.log", home);
    FILE *fp = fopen(path, "a");
    if (!fp) return;
    /* Timestamp prefix so consecutive runs are distinguishable. */
    time_t now = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
    fprintf(fp, "[%s] ", ts);
    va_list ap; va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fputc('\n', fp);
    fclose(fp);
}

static const char *path_basename(const char *p) {
    if (!p) return "";
    const char *b = p;
    for (const char *q = p; *q; q++) {
        if (*q == '/' || *q == '\\') b = q + 1;
    }
    return b;
}

static void *sftp_worker(void *arg) {
    struct PtyUpload *u = arg;
    SshPty *p = u->ssh;

    upload_log("---- start: local=%s remote=%s", u->local_path, u->remote_path);

    /* Open the local file before grabbing the session lock so we
       don't block the reader on file I/O. */
    FILE *fp = fopen(u->local_path, "rb");
    if (!fp) {
        upload_set_err(u, "open local: %s", strerror(errno));
        upload_log("FAIL: %s", u->err);
        u->status = -1;
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) == 0) {
        long sz = ftell(fp);
        if (sz > 0) u->bytes_total = (uint64_t)sz;
        fseek(fp, 0, SEEK_SET);
    }
    upload_log("local opened: %llu bytes",
               (unsigned long long)u->bytes_total);

    /* Decide the final remote path. If the user gave a directory
       (trailing slash or libssh stat says it's one), append the
       basename.

       SFTP itself has no concept of `~` — that's a shell expansion.
       The SFTP session opens with cwd = the auth'd user's home, so
       we can rewrite a leading `~/` (or bare `~`) into a relative
       path that lands in the same place. `~user/...` would need
       sftp_canonicalize_path which is more involved; skip for now. */
    char final_remote[PATH_MAX_FALLBACK];
    {
        const char *src = u->remote_path;
        if (src[0] == '~' && (src[1] == '/' || src[1] == 0)) {
            /* Skip "~" + optional "/". An empty result means "the
               cwd itself" (home), which we'll pair with the local
               basename below to form e.g. "foo.png". */
            src += 1;
            while (*src == '/') src++;
        }
        snprintf(final_remote, sizeof(final_remote), "%s", src);
        upload_log("remote path after ~ expansion: '%s'", final_remote);
    }

    /* libssh calls — all under session_lock. */
    pthread_mutex_lock(&p->session_lock);
    upload_log("got session_lock");

    bool was_blocking = ssh_is_blocking(p->session);
    ssh_set_blocking(p->session, 1);

    sftp_session sftp = sftp_new(p->session);
    if (!sftp) {
        upload_set_err(u, "sftp_new: %s", ssh_get_error(p->session));
        upload_log("FAIL sftp_new: %s", u->err);
        if (!was_blocking) ssh_set_blocking(p->session, 0);
        pthread_mutex_unlock(&p->session_lock);
        fclose(fp);
        u->status = -1;
        return NULL;
    }
    upload_log("sftp_new ok");
    int init_rc = sftp_init(sftp);
    if (init_rc != SSH_OK) {
        upload_set_err(u, "sftp_init: %s", ssh_get_error(p->session));
        upload_log("FAIL sftp_init rc=%d sftp_err=%d ssh_err=%s",
                   init_rc, sftp_get_error(sftp), ssh_get_error(p->session));
        sftp_free(sftp);
        if (!was_blocking) ssh_set_blocking(p->session, 0);
        pthread_mutex_unlock(&p->session_lock);
        fclose(fp);
        u->status = -1;
        return NULL;
    }
    upload_log("sftp_init ok");

    /* Empty path after ~ stripping = "upload to home dir", same as
       a trailing slash. Force the basename-append branch below. */
    if (!final_remote[0]) {
        snprintf(final_remote, sizeof(final_remote), "./");
    }
    /* If remote path looks like a directory (trailing /), or the
       stat says it is, append the basename of the local file. */
    {
        size_t rl = strlen(final_remote);
        bool ends_slash = rl > 0 && final_remote[rl - 1] == '/';
        if (!ends_slash) {
            sftp_attributes a = sftp_stat(sftp, final_remote);
            if (a) {
                if (a->type == SSH_FILEXFER_TYPE_DIRECTORY) ends_slash = true;
                sftp_attributes_free(a);
                upload_log("stat(%s): is_dir=%d",
                           final_remote, ends_slash ? 1 : 0);
            } else {
                upload_log("stat(%s): not found (%s) — treating as file path",
                           final_remote, ssh_get_error(p->session));
            }
        }
        if (ends_slash) {
            const char *bn = path_basename(u->local_path);
            if (final_remote[rl - 1] != '/') {
                if (rl + 1 < sizeof(final_remote)) {
                    final_remote[rl] = '/';
                    final_remote[rl + 1] = 0;
                }
            }
            strncat(final_remote, bn,
                    sizeof(final_remote) - strlen(final_remote) - 1);
        }
    }
    upload_log("final remote path: %s", final_remote);

    sftp_file rf = sftp_open(sftp, final_remote,
                             O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (!rf) {
        upload_set_err(u, "sftp_open %s: %s",
                       final_remote, ssh_get_error(p->session));
        upload_log("FAIL sftp_open: sftp_err=%d ssh_err=%s",
                   sftp_get_error(sftp), ssh_get_error(p->session));
        sftp_free(sftp);
        if (!was_blocking) ssh_set_blocking(p->session, 0);
        pthread_mutex_unlock(&p->session_lock);
        fclose(fp);
        u->status = -1;
        return NULL;
    }
    upload_log("sftp_open ok, streaming");

    uint8_t buf[SFTP_CHUNK_SZ];
    bool ok = true;
    int chunk_count = 0;
    for (;;) {
        if (u->cancel) { ok = false;
                         upload_set_err(u, "cancelled");
                         upload_log("cancelled mid-transfer");
                         break; }
        size_t n = fread(buf, 1, sizeof(buf), fp);
        if (n == 0) {
            if (ferror(fp)) { ok = false;
                              upload_set_err(u, "read local: %s",
                                             strerror(errno));
                              upload_log("FAIL read local: %s",
                                         strerror(errno)); }
            break;
        }
        ssize_t w = sftp_write(rf, buf, n);
        if (w < 0 || (size_t)w != n) {
            ok = false;
            upload_set_err(u, "sftp_write: %s",
                           ssh_get_error(p->session));
            upload_log("FAIL sftp_write n=%zu w=%zd sftp_err=%d ssh_err=%s",
                       n, w, sftp_get_error(sftp), ssh_get_error(p->session));
            break;
        }
        u->bytes_done += n;
        if ((++chunk_count % 32) == 0) {
            upload_log("progress: %llu / %llu bytes",
                       (unsigned long long)u->bytes_done,
                       (unsigned long long)u->bytes_total);
        }
    }

    sftp_close(rf);
    sftp_free(sftp);
    if (!was_blocking) ssh_set_blocking(p->session, 0);
    pthread_mutex_unlock(&p->session_lock);
    fclose(fp);

    if (ok && !u->bytes_total) u->bytes_total = u->bytes_done;
    u->status = ok ? 1 : -1;
    upload_log("---- done: status=%d done=%llu total=%llu err=%s",
               u->status,
               (unsigned long long)u->bytes_done,
               (unsigned long long)u->bytes_total,
               u->err[0] ? u->err : "(none)");
    return NULL;
}

struct PtyUpload *ssh_upload_start_impl(void *impl,
                                        const char *local_path,
                                        const char *remote_path,
                                        char *err, size_t errsz) {
    if (err && errsz) err[0] = 0;
    SshPty *p = impl;
    if (!p || !p->session || !p->alive) {
        if (err && errsz) snprintf(err, errsz, "session not alive");
        return NULL;
    }
    if (!local_path || !*local_path) {
        if (err && errsz) snprintf(err, errsz, "no local file selected");
        return NULL;
    }
    if (!remote_path || !*remote_path) remote_path = "~/";

    struct PtyUpload *u = calloc(1, sizeof(*u));
    if (!u) {
        if (err && errsz) snprintf(err, errsz, "out of memory");
        return NULL;
    }
    u->ssh = p;
    snprintf(u->local_path,  sizeof(u->local_path),  "%s", local_path);
    snprintf(u->remote_path, sizeof(u->remote_path), "%s", remote_path);
    snprintf(u->display_name, sizeof(u->display_name), "%s",
             path_basename(local_path));

    upload_log("==== upload requested: local=%s remote=%s",
               u->local_path, u->remote_path);
    if (pthread_create(&u->thread, NULL, sftp_worker, u) != 0) {
        upload_log("FAIL pthread_create");
        free(u);
        if (err && errsz) snprintf(err, errsz, "pthread_create failed");
        return NULL;
    }
    u->thread_started = true;
    return u;
}

int ssh_upload_status_impl(struct PtyUpload *u,
                           uint64_t *bytes_done, uint64_t *bytes_total,
                           char *err, size_t errsz) {
    if (!u) {
        if (err && errsz) snprintf(err, errsz, "no upload");
        return -1;
    }
    if (bytes_done)  *bytes_done  = u->bytes_done;
    if (bytes_total) *bytes_total = u->bytes_total;
    if (u->status < 0 && err && errsz) snprintf(err, errsz, "%s", u->err);
    return u->status;
}

const char *ssh_upload_name_impl(struct PtyUpload *u) {
    return u ? u->display_name : "";
}

void ssh_upload_release_impl(struct PtyUpload *u) {
    if (!u) return;
    u->cancel = 1;
    if (u->thread_started) pthread_join(u->thread, NULL);
    free(u);
}
