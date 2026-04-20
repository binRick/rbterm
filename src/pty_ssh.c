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
                    const char *keyfile, int cols, int rows,
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
    if (keyfile && *keyfile) {
        char expanded[1024];
        expand_tilde(keyfile, expanded, sizeof(expanded));
        ssh_key priv = NULL;
        int r = ssh_pki_import_privkey_file(expanded, NULL, NULL, NULL, &priv);
        if (r != SSH_OK || !priv) {
            set_err(err, errsz, "can't load key %s%s%s",
                    expanded,
                    r == SSH_EOF ? " (passphrase-protected?)" : "",
                    ssh_get_error(p->session));
            goto fail;
        }
        auth = ssh_userauth_publickey(p->session, NULL, priv);
        ssh_key_free(priv);
    } else {
        auth = ssh_userauth_publickey_auto(p->session, NULL, NULL);
    }
    if (auth != SSH_AUTH_SUCCESS) {
        const char *msg = ssh_get_error(p->session);
        set_err(err, errsz, "authentication failed%s%s",
                msg && *msg ? ": " : "", msg ? msg : "");
        goto fail;
    }

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
