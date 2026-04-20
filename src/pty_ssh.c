/* SSH backend for rbterm via libssh. Cross-platform.
 *
 * Connect + auth + channel setup run in blocking mode on the caller's
 * thread (SSH handshake stalls ~1-2 s), then the session flips to
 * non-blocking so pty_read can drain ssh_channel_read_nonblocking the
 * same way local PTYs drain read() + EAGAIN.
 *
 * Auth is key-based: explicit private key if the form supplies one,
 * otherwise ssh_userauth_publickey_auto (ssh-agent + ~/.ssh/id_*).
 * Host keys are trust-on-first-use: unknown keys go into
 * ~/.ssh/known_hosts; a changed key aborts with an error. */

#include "pty_internal.h"

#include <libssh/libssh.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    ssh_session session;
    ssh_channel channel;
    bool alive;
} SshPty;

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

void *ssh_open_impl(const char *user, const char *host, int port,
                    const char *password, const char *keyfile,
                    int cols, int rows,
                    char *err, size_t errsz) {
    if (!host || !*host) {
        set_err(err, errsz, "Host is required");
        return NULL;
    }
    if (!user || !*user) {
        user = getenv("USER");
#ifdef _WIN32
        if (!user || !*user) user = getenv("USERNAME");
#endif
        if (!user || !*user) user = "user";
    }
    if (port <= 0) port = 22;

    SshPty *p = calloc(1, sizeof(*p));
    if (!p) { set_err(err, errsz, "out of memory"); return NULL; }
    p->session = ssh_new();
    if (!p->session) {
        set_err(err, errsz, "ssh_new() failed");
        free(p);
        return NULL;
    }

    ssh_options_set(p->session, SSH_OPTIONS_HOST, host);
    ssh_options_set(p->session, SSH_OPTIONS_USER, user);
    ssh_options_set(p->session, SSH_OPTIONS_PORT, &port);
    long timeout_s = 10;
    ssh_options_set(p->session, SSH_OPTIONS_TIMEOUT, &timeout_s);
    /* Surface libssh's own warnings on stderr — run from a terminal
       (./run.sh or open --stdout=/dev/stdout) to see them. */
    int verbosity = SSH_LOG_WARNING;
    ssh_options_set(p->session, SSH_OPTIONS_LOG_VERBOSITY, &verbosity);

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

    /* 1. Password (if provided). */
    if (password && *password) {
        TRIED("password");
        auth = ssh_userauth_password(p->session, NULL, password);
        fprintf(stderr, "rbterm: ssh password auth => %s\n",
                auth == SSH_AUTH_SUCCESS ? "ok" : ssh_get_error(p->session));
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

    ssh_set_blocking(p->session, 0);
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

void ssh_close_impl(void *impl) {
    SshPty *p = impl;
    if (!p) return;
    if (p->channel) {
        ssh_channel_close(p->channel);
        ssh_channel_free(p->channel);
    }
    if (p->session) {
        ssh_disconnect(p->session);
        ssh_free(p->session);
    }
    free(p);
}

bool ssh_alive_impl(void *impl) {
    SshPty *p = impl;
    if (!p || !p->alive || !p->channel) return false;
    if (ssh_channel_is_closed(p->channel)) return false;
    if (ssh_channel_is_eof(p->channel)) return false;
    return true;
}

int ssh_read_impl(void *impl, uint8_t *buf, size_t cap) {
    SshPty *p = impl;
    if (!p || !p->channel) return -1;
    if (cap > (size_t)0xFFFFFFFF) cap = 0xFFFFFFFF;
    int n = ssh_channel_read_nonblocking(p->channel, buf, (uint32_t)cap, 0);
    if (n > 0) return n;
    if (n == SSH_EOF) { p->alive = false; return -1; }
    if (n == SSH_ERROR) { p->alive = false; return -1; }
    if (ssh_channel_is_closed(p->channel)) { p->alive = false; return -1; }
    return 0;
}

void ssh_write_impl(void *impl, const uint8_t *buf, size_t n) {
    SshPty *p = impl;
    if (!p || !p->channel) return;
    size_t off = 0;
    while (off < n) {
        int w = ssh_channel_write(p->channel, buf + off, (uint32_t)(n - off));
        if (w == SSH_ERROR) { p->alive = false; return; }
        if (w <= 0) return;
        off += (size_t)w;
    }
}

void ssh_resize_impl(void *impl, int cols, int rows) {
    SshPty *p = impl;
    if (!p || !p->channel) return;
    ssh_channel_change_pty_size(p->channel, cols, rows);
}
