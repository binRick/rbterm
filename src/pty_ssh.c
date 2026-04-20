/* SSH backend for rbterm via libssh. Cross-platform.
 *
 * Connect + auth + channel setup happen in blocking mode on the calling
 * thread (opening a tab may stall for a second or two); the session is
 * then flipped into non-blocking mode so ssh_channel_read_nonblocking()
 * can drive the main-loop drain like our local Pty does.
 *
 * Auth is key-based only for now: ssh_userauth_publickey_auto tries
 * the ssh-agent first, then ~/.ssh/id_ed25519 / id_rsa / id_ecdsa. No
 * interactive password prompt yet. Host-key check is trust-on-first-use. */

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

/* Parse "[user@]host[:port]" into pieces. Trims whitespace. */
static bool parse_target(const char *in,
                         char *user, size_t user_sz,
                         char *host, size_t host_sz,
                         int  *port) {
    /* Skip leading whitespace. */
    while (*in == ' ' || *in == '\t') in++;

    /* Trim trailing whitespace by working on a local copy. */
    char tmp[512];
    strncpy(tmp, in, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = 0;
    size_t L = strlen(tmp);
    while (L > 0 && (tmp[L - 1] == ' ' || tmp[L - 1] == '\t' ||
                     tmp[L - 1] == '\n' || tmp[L - 1] == '\r'))
        tmp[--L] = 0;
    if (L == 0) return false;

    *port = 22;
    char *p = tmp;
    char *at = strchr(p, '@');
    if (at) {
        size_t ul = (size_t)(at - p);
        if (ul == 0 || ul >= user_sz) return false;
        memcpy(user, p, ul);
        user[ul] = 0;
        p = at + 1;
    } else {
        const char *env_user = getenv("USER");
#ifdef _WIN32
        if (!env_user) env_user = getenv("USERNAME");
#endif
        if (!env_user || !*env_user) env_user = "user";
        strncpy(user, env_user, user_sz - 1);
        user[user_sz - 1] = 0;
    }

    char *colon = strrchr(p, ':');
    if (colon) {
        size_t hl = (size_t)(colon - p);
        if (hl == 0 || hl >= host_sz) return false;
        memcpy(host, p, hl);
        host[hl] = 0;
        int n = atoi(colon + 1);
        if (n > 0 && n < 65536) *port = n;
    } else {
        strncpy(host, p, host_sz - 1);
        host[host_sz - 1] = 0;
    }
    return host[0] != 0;
}

void *ssh_open_impl(const char *target, int cols, int rows,
                    char *err, size_t errsz) {
    char user[96], host[256];
    int port = 22;
    if (!parse_target(target, user, sizeof(user), host, sizeof(host), &port)) {
        set_err(err, errsz, "Can't parse '%s' (expected user@host[:port])", target);
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

    /* Host-key check — trust-on-first-use. Unknown or missing entries are
       added to ~/.ssh/known_hosts; a *changed* key aborts with a warning. */
    enum ssh_known_hosts_e khs = ssh_session_is_known_server(p->session);
    if (khs == SSH_KNOWN_HOSTS_CHANGED) {
        set_err(err, errsz,
                "host key for %s changed — refusing to connect", host);
        goto fail;
    }
    if (khs == SSH_KNOWN_HOSTS_NOT_FOUND || khs == SSH_KNOWN_HOSTS_UNKNOWN) {
        ssh_session_update_known_hosts(p->session);
    }

    /* Key-based auth: agent first, then ~/.ssh/id_*. */
    int auth = ssh_userauth_publickey_auto(p->session, NULL, NULL);
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

    /* Flip the whole session into non-blocking mode for the read loop. */
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
    /* SSH_AGAIN and 0 both mean "no data yet". */
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
