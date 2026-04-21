#include "raylib.h"
#include "screen.h"
#include "render.h"
#include "input.h"
#include "pty.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>

#ifndef _WIN32
#include <signal.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

/* ---------- App-wide settings ---------- */

typedef struct {
    bool log_enabled;
    char log_dir[PATH_MAX];
} AppSettings;
static AppSettings g_app_settings;

static void expand_home_path(const char *in, char *out, size_t cap);

static void app_settings_init(void) {
    g_app_settings.log_enabled = false;
    const char *home = getenv("HOME");
#ifdef _WIN32
    if (!home || !*home) home = getenv("USERPROFILE");
#endif
    if (home && *home) {
        snprintf(g_app_settings.log_dir, sizeof(g_app_settings.log_dir),
                 "%s/.rbterm/logs", home);
    } else {
        strncpy(g_app_settings.log_dir, "./rbterm-logs",
                sizeof(g_app_settings.log_dir) - 1);
    }
}

/* Expand a leading "~/" using $HOME / %USERPROFILE%. */
static void expand_home_path(const char *in, char *out, size_t cap) {
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

/* mkdir -p equivalent (portable-ish). */
static void mkdir_p(const char *path) {
    if (!path || !*path) return;
    char tmp[PATH_MAX];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = 0;
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char c = *p; *p = 0;
#ifdef _WIN32
            (void)_mkdir(tmp);
#else
            (void)mkdir(tmp, 0700);
#endif
            *p = c;
        }
    }
#ifdef _WIN32
    (void)_mkdir(tmp);
#else
    (void)mkdir(tmp, 0700);
#endif
}

/* ---------- Tabs ---------- */

typedef struct {
    Pty *pty;
    Screen *scr;
    Selection sel;
    char title[256];
    bool title_dirty;
    bool dead;
    int click_count;
    double last_click_time;
    int last_click_col, last_click_row;
    char cwd[PATH_MAX];
    double cwd_poll_at;
    FILE *log_fp;                /* session log file, NULL when disabled */
    char  log_path[PATH_MAX];
} Tab;

#define MAX_TABS 16
#define TAB_BAR_H 30
#define TAB_MIN_W 100
#define TAB_MAX_W 240
#define TAB_CLOSE_W 22
#define TAB_PLUS_W  30
#define TAB_SSH_W   48

static Tab *g_tabs[MAX_TABS];
static int g_num_tabs = 0;
static int g_active = 0;

/* Modal SSH connection form. When non-NORMAL, the terminal is locked:
   keystrokes edit form fields and mouse clicks focus them instead of
   going to the active tab. Layout is computed on the fly in
   ssh_form_layout() so draw and hit-test share one source of truth. */
typedef enum { UI_NORMAL = 0, UI_SSH_FORM, UI_SETTINGS } UiMode;
typedef enum {
    F_HOST = 0, F_PORT, F_USER, F_PASS, F_KEY,
    F_CONNECT, F_CANCEL,
    F_COUNT
} SshField;
#define F_TEXT_FIELDS 5    /* host, port, user, pass, key */
typedef struct {
    char host[256];
    char port[16];
    char user[96];
    char pass[256];
    char key[512];
    int  focus;              /* SshField */
    bool sel_all;            /* focused text field's contents are fully selected */
    char error[256];
} SshForm;
static UiMode  g_ui_mode = UI_NORMAL;
static SshForm g_form;

typedef struct {
    int x, y, w, h;
} Rect;

typedef struct {
    char name[128];        /* alias from `Host X` */
    char hostname[256];    /* HostName */
    char user[96];         /* User */
    char identity[PATH_MAX];
    int  port;
} SshProfile;

#define SSH_PROFILES_MAX 128
static SshProfile g_ssh_profiles[SSH_PROFILES_MAX];
static int        g_ssh_profile_count = 0;
static int        g_ssh_list_scroll = 0;   /* in rows */

typedef struct {
    Rect modal;
    Rect list;                  /* saved-hosts sidebar */
    Rect field[F_TEXT_FIELDS];  /* host, port, user, pass, key */
    Rect connect;
    Rect cancel;
} SshFormLayout;

static Tab *active_tab(void) {
    if (g_num_tabs == 0) return NULL;
    if (g_active < 0) g_active = 0;
    if (g_active >= g_num_tabs) g_active = g_num_tabs - 1;
    return g_tabs[g_active];
}

/* Open a fresh per-tab log file under the current log directory. Silent
   on failure so the user's session isn't derailed by a bad path. */
static void tab_log_open(Tab *t) {
    if (!t || t->log_fp) return;
    if (!g_app_settings.log_enabled) return;
    if (!g_app_settings.log_dir[0]) return;
    char dir[PATH_MAX];
    expand_home_path(g_app_settings.log_dir, dir, sizeof(dir));
    mkdir_p(dir);
    time_t now = time(NULL);
    struct tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &now);
    struct tm *lt = &tm_buf;
#else
    struct tm *lt = localtime_r(&now, &tm_buf);
#endif
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", lt);
    /* Slot number stabilises the filename if multiple tabs open in the
       same second. */
    int slot = 0;
    for (int i = 0; i < g_num_tabs; i++) if (g_tabs[i] == t) { slot = i; break; }
    snprintf(t->log_path, sizeof(t->log_path), "%s/rbterm-%s-tab%d.log",
             dir, stamp, slot);
    t->log_fp = fopen(t->log_path, "ab");
    if (!t->log_fp) {
        t->log_path[0] = 0;
        fprintf(stderr, "rbterm: can't open log %s: %s\n",
                t->log_path, strerror(errno));
    }
}

static void tab_log_close(Tab *t) {
    if (!t) return;
    if (t->log_fp) { fclose(t->log_fp); t->log_fp = NULL; }
}

static void tab_log_write(Tab *t, const uint8_t *buf, size_t n) {
    if (!t || !t->log_fp || n == 0) return;
    fwrite(buf, 1, n, t->log_fp);
    fflush(t->log_fp);
}

/* (Re-)open logs on every tab based on the current setting. Called when
   the user toggles "log to file" in Settings. */
static void refresh_tab_logs(void) {
    for (int i = 0; i < g_num_tabs; i++) {
        if (g_app_settings.log_enabled) tab_log_open(g_tabs[i]);
        else                            tab_log_close(g_tabs[i]);
    }
}

/* ---------- IO glue: screen callbacks route to owning tab's PTY ---------- */

static void io_write_cb(void *u, const uint8_t *buf, size_t n) {
    Tab *t = (Tab *)u;
    pty_write(t->pty, buf, n);
}
static void io_set_title_cb(void *u, const char *title) {
    Tab *t = (Tab *)u;
    strncpy(t->title, title, sizeof(t->title) - 1);
    t->title[sizeof(t->title) - 1] = 0;
    t->title_dirty = true;
}
static void io_bell_cb(void *u) { (void)u; }

static Tab *tab_open(int cols, int rows) {
    if (g_num_tabs >= MAX_TABS) return NULL;
    Tab *t = calloc(1, sizeof(Tab));
    strncpy(t->title, "shell", sizeof(t->title) - 1);
    t->last_click_time = -1.0;
    t->last_click_col = t->last_click_row = -1;
    t->pty = pty_open(cols, rows);
    if (!t->pty) { free(t); return NULL; }
    ScreenIO io = { .user = t, .write = io_write_cb,
                    .set_title = io_set_title_cb, .bell = io_bell_cb };
    t->scr = screen_new(cols, rows, 5000, io);
    g_tabs[g_num_tabs] = t;
    g_active = g_num_tabs;
    g_num_tabs++;
    tab_log_open(t);
    return t;
}

static Tab *tab_open_ssh(const char *user, const char *host, int port,
                         const char *password, const char *keyfile,
                         int cols, int rows,
                         char *err, size_t errsz) {
    if (g_num_tabs >= MAX_TABS) return NULL;
    Tab *t = calloc(1, sizeof(Tab));
    if (port <= 0) port = 22;
    /* Build a pretty label for the tab. */
    if (user && *user) {
        if (port == 22)
            snprintf(t->cwd, sizeof(t->cwd), "%s@%s", user, host);
        else
            snprintf(t->cwd, sizeof(t->cwd), "%s@%s:%d", user, host, port);
    } else {
        if (port == 22) snprintf(t->cwd, sizeof(t->cwd), "%s", host);
        else snprintf(t->cwd, sizeof(t->cwd), "%s:%d", host, port);
    }
    snprintf(t->title, sizeof(t->title), "%s", t->cwd);
    t->last_click_time = -1.0;
    t->last_click_col = t->last_click_row = -1;
    t->pty = pty_open_ssh(user, host, port, password, keyfile,
                          cols, rows, err, errsz);
    if (!t->pty) { free(t); return NULL; }
    ScreenIO io = { .user = t, .write = io_write_cb,
                    .set_title = io_set_title_cb, .bell = io_bell_cb };
    t->scr = screen_new(cols, rows, 5000, io);
    g_tabs[g_num_tabs] = t;
    g_active = g_num_tabs;
    g_num_tabs++;
    tab_log_open(t);
    return t;
}

static void tab_close(int idx) {
    if (idx < 0 || idx >= g_num_tabs) return;
    Tab *t = g_tabs[idx];
    tab_log_close(t);
    pty_close(t->pty);
    screen_free(t->scr);
    free(t);
    for (int i = idx; i < g_num_tabs - 1; i++) g_tabs[i] = g_tabs[i + 1];
    g_tabs[g_num_tabs - 1] = NULL;
    g_num_tabs--;
    if (g_active >= g_num_tabs) g_active = g_num_tabs - 1;
    if (g_active < 0) g_active = 0;
}

/* ---------- Clipboard + selection helpers ---------- */

static size_t utf8_emit(char *buf, uint32_t cp) {
    if (cp < 0x80)    { buf[0] = (char)cp; return 1; }
    if (cp < 0x800)   { buf[0] = 0xC0 | (cp >> 6); buf[1] = 0x80 | (cp & 0x3F); return 2; }
    if (cp < 0x10000) {
        buf[0] = 0xE0 | (cp >> 12);
        buf[1] = 0x80 | ((cp >> 6) & 0x3F);
        buf[2] = 0x80 | (cp & 0x3F);
        return 3;
    }
    buf[0] = 0xF0 | (cp >> 18);
    buf[1] = 0x80 | ((cp >> 12) & 0x3F);
    buf[2] = 0x80 | ((cp >> 6) & 0x3F);
    buf[3] = 0x80 | (cp & 0x3F);
    return 4;
}

static void copy_selection(Screen *s, const Selection *sel) {
    if (!sel || !sel->active) return;
    int cols = screen_cols(s), rows = screen_rows(s);
    int r1 = sel->a_row, c1 = sel->a_col;
    int r2 = sel->b_row, c2 = sel->b_col;
    if (r1 > r2 || (r1 == r2 && c1 > c2)) {
        int tr = r1, tc = c1; r1 = r2; c1 = c2; r2 = tr; c2 = tc;
    }
    if (r1 < 0) r1 = 0; if (r2 >= rows) r2 = rows - 1;
    size_t cap = (size_t)(r2 - r1 + 1) * cols * 4 + (r2 - r1) + 1;
    char *buf = malloc(cap);
    size_t n = 0;
    for (int y = r1; y <= r2; y++) {
        int xs = (y == r1) ? c1 : 0;
        int xe = (y == r2) ? c2 : cols - 1;
        if (xs < 0) xs = 0;
        if (xe >= cols) xe = cols - 1;
        int last = xs - 1;
        for (int x = xs; x <= xe; x++) {
            Cell c = screen_view_cell(s, x, y);
            if (c.attrs & ATTR_WIDE_CONT) continue;
            if (c.cp != 0 && c.cp != ' ') last = x;
        }
        for (int x = xs; x <= last; x++) {
            Cell c = screen_view_cell(s, x, y);
            if (c.attrs & ATTR_WIDE_CONT) continue;
            uint32_t cp = c.cp ? c.cp : ' ';
            n += utf8_emit(buf + n, cp);
        }
        if (y < r2) buf[n++] = '\n';
    }
    buf[n] = 0;
    if (n > 0) SetClipboardText(buf);
    free(buf);
}

static bool cell_is_word(Screen *s, int col, int row) {
    if (col < 0 || row < 0 || col >= screen_cols(s) || row >= screen_rows(s)) return false;
    Cell c = screen_view_cell(s, col, row);
    if (c.cp == 0 || c.cp == ' ' || c.cp == '\t') return false;
    return true;
}

static void select_word(Screen *s, Selection *sel, int col, int row) {
    int cols = screen_cols(s);
    if (!cell_is_word(s, col, row)) { sel->active = false; return; }
    int c1 = col, c2 = col;
    while (c1 > 0 && cell_is_word(s, c1 - 1, row)) c1--;
    while (c2 < cols - 1 && cell_is_word(s, c2 + 1, row)) c2++;
    sel->active = true;
    sel->dragging = false;
    sel->a_col = c1; sel->a_row = row;
    sel->b_col = c2; sel->b_row = row;
}

static void select_line(Screen *s, Selection *sel, int row) {
    int cols = screen_cols(s);
    sel->active = true;
    sel->dragging = false;
    sel->a_col = 0; sel->a_row = row;
    sel->b_col = cols - 1; sel->b_row = row;
}

/* Label shown on a tab: prefer the shell's current working directory
   (shortened to a basename, with $HOME / %USERPROFILE% rewritten to "~")
   over any OSC title the shell set. Falls back to title, then "shell". */
static const char *tab_label(const Tab *t) {
    if (t->cwd[0]) {
        const char *home = getenv("HOME");
#ifdef _WIN32
        if (!home || !*home) home = getenv("USERPROFILE");
#endif
        if (home && *home) {
            size_t hn = strlen(home);
            if (strncmp(t->cwd, home, hn) == 0 &&
                (t->cwd[hn] == 0 || t->cwd[hn] == '/' || t->cwd[hn] == '\\')) {
                if (t->cwd[hn] == 0) return "~";
            }
        }
        if (strcmp(t->cwd, "/") == 0) return "/";
        const char *b1 = strrchr(t->cwd, '/');
        const char *b2 = strrchr(t->cwd, '\\');
        const char *base = (b1 && b2) ? (b1 > b2 ? b1 : b2) : (b1 ? b1 : b2);
        if (base) return base + 1;
        return t->cwd;
    }
    return t->title[0] ? t->title : "shell";
}

/* ---------- Tab bar UI ---------- */

typedef struct {
    int tab_idx;
    bool on_close;
    bool on_plus;
    bool on_ssh;
} TabBarHit;

static int tab_width_for(int win_w) {
    int avail = win_w - TAB_PLUS_W - TAB_SSH_W;
    if (g_num_tabs <= 0) return TAB_MIN_W;
    int w = avail / g_num_tabs;
    if (w > TAB_MAX_W) w = TAB_MAX_W;
    if (w < TAB_MIN_W) w = TAB_MIN_W;
    return w;
}

static TabBarHit tab_bar_hit_test(int win_w, int mx, int my) {
    TabBarHit h = { -1, false, false, false };
    if (my < 0 || my >= TAB_BAR_H) return h;
    int plus_x = win_w - TAB_PLUS_W;
    int ssh_x  = plus_x - TAB_SSH_W;
    if (mx >= plus_x) { h.on_plus = true; return h; }
    if (mx >= ssh_x)  { h.on_ssh  = true; return h; }
    int tw = tab_width_for(win_w);
    int idx = mx / tw;
    if (idx < 0 || idx >= g_num_tabs) return h;
    h.tab_idx = idx;
    if (mx >= idx * tw + tw - TAB_CLOSE_W) h.on_close = true;
    return h;
}

static void draw_tab_bar(Renderer *r, int win_w) {
    DrawRectangle(0, 0, win_w, TAB_BAR_H, (Color){24, 24, 32, 255});
    DrawRectangle(0, TAB_BAR_H - 1, win_w, 1, (Color){60, 60, 75, 255});

    int tw = tab_width_for(win_w);
    Font *f = (Font *)r->font_data;
    float fs = 13.0f;

    for (int i = 0; i < g_num_tabs; i++) {
        int x = i * tw;
        bool active = (i == g_active);
        Color bg = active ? (Color){46, 52, 70, 255} : (Color){28, 32, 44, 255};
        Color fg = active ? (Color){230, 230, 240, 255} : (Color){150, 150, 165, 255};
        DrawRectangle(x, 0, tw, TAB_BAR_H, bg);
        if (active) DrawRectangle(x, 0, tw, 2, (Color){125, 207, 255, 255});

        const char *title = tab_label(g_tabs[i]);
        BeginScissorMode(x + 8, 0, tw - TAB_CLOSE_W - 12, TAB_BAR_H);
        Vector2 tsz = MeasureTextEx(*f, title, fs, 0);
        Vector2 tp  = { x + 10, (TAB_BAR_H - tsz.y) / 2.0f };
        DrawTextEx(*f, title, tp, fs, 0, fg);
        EndScissorMode();
        const char *cross = "x";
        Vector2 csz = MeasureTextEx(*f, cross, fs, 0);
        DrawTextEx(*f, cross,
                   (Vector2){ x + tw - TAB_CLOSE_W / 2 - csz.x / 2,
                              (TAB_BAR_H - csz.y) / 2.0f },
                   fs, 0, fg);
        if (i > 0) DrawLine(x, 4, x, TAB_BAR_H - 4, (Color){60, 60, 75, 255});
    }

    int plus_x = win_w - TAB_PLUS_W;
    int ssh_x  = plus_x - TAB_SSH_W;

    /* "ssh" button. */
    DrawRectangle(ssh_x, 0, TAB_SSH_W, TAB_BAR_H, (Color){28, 32, 44, 255});
    DrawLine(ssh_x, 4, ssh_x, TAB_BAR_H - 4, (Color){60, 60, 75, 255});
    const char *ssh_label = "ssh";
    Vector2 ssz = MeasureTextEx(*f, ssh_label, 13, 0);
    DrawTextEx(*f, ssh_label,
               (Vector2){ ssh_x + (TAB_SSH_W - ssz.x) / 2.0f,
                          (TAB_BAR_H - ssz.y) / 2.0f },
               13, 0, (Color){125, 207, 255, 255});

    /* "+" button. */
    DrawRectangle(plus_x, 0, TAB_PLUS_W, TAB_BAR_H, (Color){28, 32, 44, 255});
    DrawLine(plus_x, 4, plus_x, TAB_BAR_H - 4, (Color){60, 60, 75, 255});
    const char *plus = "+";
    Vector2 psz = MeasureTextEx(*f, plus, 18, 0);
    DrawTextEx(*f, plus,
               (Vector2){ plus_x + (TAB_PLUS_W - psz.x) / 2.0f,
                          (TAB_BAR_H - psz.y) / 2.0f },
               18, 0, (Color){200, 200, 215, 255});
}

/* ---------- SSH connection form (PuTTY-style) ---------- */

/* Parse ~/.ssh/config into g_ssh_profiles. Only the fields we actually
   surface in the form are extracted (HostName / User / Port / IdentityFile);
   wildcard stanzas (`Host *`, `Host !foo`) are skipped. Keys are
   case-insensitive per the ssh_config(5) spec. */
static int ci_prefix(const char *s, const char *pfx) {
    /* Returns length of pfx if s starts with pfx case-insensitively
       AND the next char is whitespace or '='; else 0. */
    size_t n = strlen(pfx);
    for (size_t i = 0; i < n; i++) {
        char a = s[i], b = pfx[i];
        if (a >= 'A' && a <= 'Z') a = a - 'A' + 'a';
        if (b >= 'A' && b <= 'Z') b = b - 'A' + 'a';
        if (a != b) return 0;
    }
    char c = s[n];
    return (c == ' ' || c == '\t' || c == '=') ? (int)n : 0;
}

static const char *skip_ws(const char *s) {
    while (*s == ' ' || *s == '\t' || *s == '=') s++;
    return s;
}

static void trim_end(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t' ||
                     s[n-1] == '\r' || s[n-1] == '\n'))
        s[--n] = 0;
}

static void ssh_profiles_load(void) {
    g_ssh_profile_count = 0;
    g_ssh_list_scroll = 0;
    char path[PATH_MAX];
    expand_home_path("~/.ssh/config", path, sizeof(path));
    FILE *fp = fopen(path, "r");
    if (!fp) return;

    SshProfile *cur = NULL;
    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == 0 || *p == '\r') continue;
        trim_end(p);

        int k = ci_prefix(p, "host");
        if (k) {
            const char *rest = skip_ws(p + k);
            /* Take just the first token on multi-alias lines. */
            char name[128];
            size_t ni = 0;
            while (*rest && *rest != ' ' && *rest != '\t' && ni + 1 < sizeof(name))
                name[ni++] = *rest++;
            name[ni] = 0;
            /* Skip pattern / wildcard stanzas. */
            if (!name[0] || strchr(name, '*') || strchr(name, '?') || name[0] == '!') {
                cur = NULL;
                continue;
            }
            if (g_ssh_profile_count >= SSH_PROFILES_MAX) break;
            cur = &g_ssh_profiles[g_ssh_profile_count++];
            memset(cur, 0, sizeof(*cur));
            strncpy(cur->name, name, sizeof(cur->name) - 1);
            cur->port = 0;
            continue;
        }

        if (!cur) continue;  /* directive outside any Host stanza */

        k = ci_prefix(p, "hostname");
        if (k) {
            const char *rest = skip_ws(p + k);
            strncpy(cur->hostname, rest, sizeof(cur->hostname) - 1);
            continue;
        }
        k = ci_prefix(p, "user");
        if (k) {
            const char *rest = skip_ws(p + k);
            strncpy(cur->user, rest, sizeof(cur->user) - 1);
            continue;
        }
        k = ci_prefix(p, "port");
        if (k) {
            const char *rest = skip_ws(p + k);
            cur->port = atoi(rest);
            continue;
        }
        k = ci_prefix(p, "identityfile");
        if (k) {
            const char *rest = skip_ws(p + k);
            strncpy(cur->identity, rest, sizeof(cur->identity) - 1);
            continue;
        }
    }
    fclose(fp);
}

static void ssh_form_apply_profile(const SshProfile *prof) {
    if (!prof) return;
    /* HostName falls back to the alias itself (ssh(1) does the same). */
    const char *hostname = prof->hostname[0] ? prof->hostname : prof->name;
    strncpy(g_form.host, hostname, sizeof(g_form.host) - 1);
    g_form.host[sizeof(g_form.host) - 1] = 0;
    if (prof->port > 0) snprintf(g_form.port, sizeof(g_form.port), "%d", prof->port);
    else strncpy(g_form.port, "22", sizeof(g_form.port) - 1);
    if (prof->user[0]) {
        strncpy(g_form.user, prof->user, sizeof(g_form.user) - 1);
        g_form.user[sizeof(g_form.user) - 1] = 0;
    }
    if (prof->identity[0]) {
        strncpy(g_form.key, prof->identity, sizeof(g_form.key) - 1);
        g_form.key[sizeof(g_form.key) - 1] = 0;
    } else {
        g_form.key[0] = 0;
    }
    g_form.sel_all = false;
    g_form.error[0] = 0;
}

static void ssh_form_open(void) {
    g_ui_mode = UI_SSH_FORM;
    memset(&g_form, 0, sizeof(g_form));
    strncpy(g_form.port, "22", sizeof(g_form.port) - 1);
    const char *u = getenv("USER");
#ifdef _WIN32
    if (!u || !*u) u = getenv("USERNAME");
#endif
    if (u && *u) {
        strncpy(g_form.user, u, sizeof(g_form.user) - 1);
        g_form.user[sizeof(g_form.user) - 1] = 0;
    }
    g_form.focus = F_HOST;
    /* Refresh the saved-hosts list every time the modal opens so edits
       to ~/.ssh/config show up without restarting rbterm. */
    ssh_profiles_load();
}

static char *form_buf(int field, size_t *cap) {
    switch (field) {
    case F_HOST: *cap = sizeof(g_form.host); return g_form.host;
    case F_PORT: *cap = sizeof(g_form.port); return g_form.port;
    case F_USER: *cap = sizeof(g_form.user); return g_form.user;
    case F_PASS: *cap = sizeof(g_form.pass); return g_form.pass;
    case F_KEY:  *cap = sizeof(g_form.key);  return g_form.key;
    default: *cap = 0; return NULL;
    }
}

static SshFormLayout ssh_form_layout(int win_w, int win_h) {
    SshFormLayout L = {0};
    int w = 800, h = 380;
    if (w > win_w - 40) w = win_w - 40;
    if (h > win_h - 40) h = win_h - 40;
    L.modal.x = (win_w - w) / 2;
    L.modal.y = (win_h - h) / 2;
    L.modal.w = w;
    L.modal.h = h;

    int pad = 22;
    int title_h = 46;
    int list_w = (g_ssh_profile_count > 0) ? 210 : 0;
    if (list_w > 0) {
        L.list.x = L.modal.x + pad;
        L.list.y = L.modal.y + title_h + 10;
        L.list.w = list_w;
        L.list.h = h - title_h - 10 - pad - 40;   /* leaves room for buttons */
    }

    int form_x = L.modal.x + pad + list_w + (list_w > 0 ? 14 : 0);
    int label_w = 100;
    int field_x = form_x + label_w;
    int field_w = L.modal.x + w - pad - field_x;
    int field_h = 28;
    int y = L.modal.y + title_h + 10;
    for (int i = 0; i < F_TEXT_FIELDS; i++) {
        L.field[i].x = field_x;
        L.field[i].y = y;
        L.field[i].w = field_w;
        L.field[i].h = field_h;
        y += field_h + 8;
    }

    int btn_h = 32;
    int btn_y = L.modal.y + L.modal.h - btn_h - pad - (g_form.error[0] ? 24 : 0);
    int connect_w = 120, cancel_w = 96;
    L.connect.w = connect_w;  L.connect.h = btn_h;
    L.cancel.w  = cancel_w;   L.cancel.h  = btn_h;
    L.connect.x = L.modal.x + L.modal.w - pad - connect_w - 8 - cancel_w;
    L.connect.y = btn_y;
    L.cancel.x  = L.modal.x + L.modal.w - pad - cancel_w;
    L.cancel.y  = btn_y;
    return L;
}

static bool rect_hit(Rect r, int x, int y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static void ssh_form_submit(int cols, int rows) {
    if (!g_form.host[0]) {
        strncpy(g_form.error, "Host is required", sizeof(g_form.error) - 1);
        g_form.focus = F_HOST;
        return;
    }
    int port = atoi(g_form.port);
    if (port <= 0) port = 22;
    char err[256] = {0};
    Tab *t = tab_open_ssh(
        g_form.user[0] ? g_form.user : NULL,
        g_form.host,
        port,
        g_form.pass[0] ? g_form.pass : NULL,
        g_form.key[0]  ? g_form.key  : NULL,
        cols, rows, err, sizeof(err));
    if (t) {
        /* Clear the password from memory as soon as we no longer need it. */
        memset(g_form.pass, 0, sizeof(g_form.pass));
        g_ui_mode = UI_NORMAL;
    } else {
        strncpy(g_form.error, err[0] ? err : "connection failed",
                sizeof(g_form.error) - 1);
        g_form.error[sizeof(g_form.error) - 1] = 0;
    }
}

static void ssh_form_advance_focus(int delta) {
    g_form.focus = (g_form.focus + delta + F_COUNT) % F_COUNT;
    g_form.sel_all = false;
}

static void ssh_form_handle_mouse(SshFormLayout L, int cols, int rows) {
    /* Wheel-scroll the hosts list whenever the pointer is over it. */
    if (L.list.w > 0) {
        Vector2 mp = GetMousePosition();
        if (rect_hit(L.list, (int)mp.x, (int)mp.y)) {
            float wheel = GetMouseWheelMove();
            if (wheel != 0.0f) {
                g_ssh_list_scroll -= (int)(wheel * 3.0f);
                if (g_ssh_list_scroll < 0) g_ssh_list_scroll = 0;
            }
        }
    }

    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) return;
    Vector2 mp = GetMousePosition();
    int mx = (int)mp.x, my = (int)mp.y;

    /* Click on a saved host → populate fields from ~/.ssh/config and
       connect immediately. The form closes on success. */
    if (L.list.w > 0 && rect_hit(L.list, mx, my)) {
        int row_h = 22;
        int idx = (my - L.list.y) / row_h + g_ssh_list_scroll;
        if (idx >= 0 && idx < g_ssh_profile_count) {
            ssh_form_apply_profile(&g_ssh_profiles[idx]);
            ssh_form_submit(cols, rows);
        }
        return;
    }

    for (int i = 0; i < F_TEXT_FIELDS; i++) {
        if (rect_hit(L.field[i], mx, my)) { g_form.focus = i; g_form.sel_all = false; return; }
    }
    if (rect_hit(L.connect, mx, my)) {
        g_form.focus = F_CONNECT;
        ssh_form_submit(cols, rows);
        return;
    }
    if (rect_hit(L.cancel, mx, my)) {
        g_ui_mode = UI_NORMAL;
        return;
    }
}

static void ssh_form_handle_keys(int cols, int rows, SshFormLayout L) {
    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    bool ctrl  = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
#if defined(__APPLE__)
    bool cmd   = IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER);
#else
    bool cmd   = false;
#endif
    bool mod   = ctrl || cmd;
    bool is_text_field = (g_form.focus >= F_HOST && g_form.focus <= F_KEY);

    if (IsKeyPressed(KEY_ESCAPE)) { g_ui_mode = UI_NORMAL; return; }

    /* Select-all (Ctrl+A / Cmd+A). */
    if (mod && IsKeyPressed(KEY_A)) {
        if (is_text_field) g_form.sel_all = true;
        return;
    }

    if (IsKeyPressed(KEY_TAB) || IsKeyPressedRepeat(KEY_TAB)) {
        ssh_form_advance_focus(shift ? -1 : +1);
        return;
    }

    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
        if (g_form.focus == F_CANCEL) { g_ui_mode = UI_NORMAL; return; }
        if (g_form.focus == F_CONNECT || g_form.focus == F_KEY) {
            ssh_form_submit(cols, rows);
            return;
        }
        ssh_form_advance_focus(+1);
        return;
    }
    /* Left/right arrow in a text field just clears any select-all marker. */
    if (is_text_field && (IsKeyPressed(KEY_LEFT) || IsKeyPressed(KEY_RIGHT) ||
                          IsKeyPressed(KEY_HOME) || IsKeyPressed(KEY_END))) {
        g_form.sel_all = false;
    }

    /* Space on Connect / Cancel acts as a click. */
    if (g_form.focus == F_CONNECT && IsKeyPressed(KEY_SPACE)) {
        ssh_form_submit(cols, rows); return;
    }
    if (g_form.focus == F_CANCEL && IsKeyPressed(KEY_SPACE)) {
        g_ui_mode = UI_NORMAL; return;
    }

    /* Text edit the focused text field. */
    if (is_text_field) {
        size_t cap;
        char *buf = form_buf(g_form.focus, &cap);
        int len = (int)strlen(buf);

        if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) {
            if (g_form.sel_all) {
                memset(buf, 0, cap);
                g_form.sel_all = false;
            } else if (len > 0) {
                buf[len - 1] = 0;
            }
            g_form.error[0] = 0;
        }

        int cp;
        while ((cp = GetCharPressed()) != 0) {
            /* Swallow characters generated alongside a modifier chord
               (e.g. the 'a' from Cmd+A) so they don't land in the field. */
            if (mod) continue;
            if (cp < 32 || cp >= 127) continue;
            if (g_form.focus == F_PORT && !(cp >= '0' && cp <= '9')) continue;
            if (g_form.sel_all) {
                memset(buf, 0, cap);
                len = 0;
                g_form.sel_all = false;
            }
            if (len + 1 >= (int)cap) continue;
            buf[len++] = (char)cp;
            buf[len] = 0;
            g_form.error[0] = 0;
        }
    }
    (void)L;
}

static void draw_ssh_form(Renderer *r, int win_w, int win_h, SshFormLayout L) {
    DrawRectangle(0, 0, win_w, win_h, (Color){0, 0, 0, 150});
    DrawRectangle(L.modal.x, L.modal.y, L.modal.w, L.modal.h,
                  (Color){30, 34, 46, 255});
    DrawRectangleLines(L.modal.x, L.modal.y, L.modal.w, L.modal.h,
                       (Color){125, 207, 255, 220});

    Font *f = (Font *)r->font_data;
    /* Title bar. */
    DrawRectangle(L.modal.x + 1, L.modal.y + 1, L.modal.w - 2, 38,
                  (Color){38, 42, 58, 255});
    DrawTextEx(*f, "SSH — Connect", (Vector2){L.modal.x + 20, L.modal.y + 11},
               16, 0, (Color){230, 232, 240, 255});

    /* Fields. */
    const char *labels[F_TEXT_FIELDS] = {
        "Host", "Port", "Username", "Password", "Key file"
    };
    const char *hints[F_TEXT_FIELDS]  = {
        "example.com", "22", getenv("USER"),
        "(leave blank to use key)",
        "(default: ssh-agent + ~/.ssh/id_*)"
    };
    const char *values[F_TEXT_FIELDS] = {
        g_form.host, g_form.port, g_form.user, g_form.pass, g_form.key
    };

    /* Saved-hosts sidebar. */
    if (L.list.w > 0) {
        DrawRectangle(L.list.x, L.list.y, L.list.w, L.list.h,
                      (Color){22, 25, 34, 255});
        DrawRectangleLines(L.list.x, L.list.y, L.list.w, L.list.h,
                           (Color){70, 74, 90, 255});
        DrawTextEx(*f, "Saved hosts (~/.ssh/config)",
                   (Vector2){L.list.x + 8, L.list.y - 18},
                   11, 0, (Color){140, 145, 160, 255});
        int row_h = 22;
        int visible = L.list.h / row_h;
        if (g_ssh_list_scroll < 0) g_ssh_list_scroll = 0;
        int max_scroll = g_ssh_profile_count - visible;
        if (max_scroll < 0) max_scroll = 0;
        if (g_ssh_list_scroll > max_scroll) g_ssh_list_scroll = max_scroll;
        BeginScissorMode(L.list.x + 2, L.list.y + 2,
                         L.list.w - 4, L.list.h - 4);
        for (int i = 0; i < g_ssh_profile_count; i++) {
            int ry = L.list.y + (i - g_ssh_list_scroll) * row_h;
            if (ry + row_h < L.list.y || ry > L.list.y + L.list.h) continue;
            bool on_this = strcmp(g_form.host, g_ssh_profiles[i].hostname[0]
                                                ? g_ssh_profiles[i].hostname
                                                : g_ssh_profiles[i].name) == 0;
            if (on_this) {
                DrawRectangle(L.list.x + 2, ry, L.list.w - 4, row_h,
                              (Color){46, 62, 90, 220});
            }
            DrawTextEx(*f, g_ssh_profiles[i].name,
                       (Vector2){L.list.x + 10, ry + 4},
                       13, 0,
                       on_this ? (Color){230, 232, 240, 255}
                               : (Color){200, 205, 220, 255});
        }
        EndScissorMode();
        if (g_ssh_profile_count > visible) {
            /* Thin scrollbar indicator. */
            int track_x = L.list.x + L.list.w - 5;
            int bar_h = L.list.h * visible / g_ssh_profile_count;
            if (bar_h < 24) bar_h = 24;
            int bar_y = L.list.y + (L.list.h - bar_h) * g_ssh_list_scroll /
                        (max_scroll > 0 ? max_scroll : 1);
            DrawRectangle(track_x, L.list.y, 3, L.list.h,
                          (Color){40, 45, 58, 255});
            DrawRectangle(track_x, bar_y, 3, bar_h,
                          (Color){110, 130, 170, 255});
        }
    }

    char masked[256];
    for (int i = 0; i < F_TEXT_FIELDS; i++) {
        /* Labels sit just to the left of each field, not hugging the
           modal edge — otherwise they'd collide with the hosts list. */
        DrawTextEx(*f, labels[i],
                   (Vector2){L.field[i].x - 104, L.field[i].y + 7},
                   13, 0, (Color){180, 185, 200, 255});

        bool focused = g_form.focus == i;
        DrawRectangle(L.field[i].x, L.field[i].y, L.field[i].w, L.field[i].h,
                      (Color){22, 25, 34, 255});
        DrawRectangleLines(L.field[i].x, L.field[i].y, L.field[i].w, L.field[i].h,
                           focused ? (Color){125, 207, 255, 255}
                                   : (Color){70, 74, 90, 255});

        /* Mask password field as dots so onlookers don't get a freebie. */
        const char *shown;
        Color tc;
        if (values[i][0]) {
            if (i == F_PASS) {
                int n = (int)strlen(values[i]);
                if (n > (int)sizeof(masked) - 1) n = (int)sizeof(masked) - 1;
                for (int k = 0; k < n; k++) masked[k] = '*';
                masked[n] = 0;
                shown = masked;
            } else {
                shown = values[i];
            }
            tc = (Color){230, 232, 240, 255};
        } else {
            shown = hints[i] ? hints[i] : "";
            tc = (Color){110, 115, 130, 255};
        }

        BeginScissorMode(L.field[i].x + 6, L.field[i].y,
                         L.field[i].w - 12, L.field[i].h);
        /* Selection highlight when Ctrl/Cmd-A selected the whole field. */
        if (focused && g_form.sel_all && values[i][0]) {
            Vector2 ssz = MeasureTextEx(*f, shown, 14, 0);
            int sw = (int)ssz.x + 4;
            if (sw > L.field[i].w - 12) sw = L.field[i].w - 12;
            DrawRectangle(L.field[i].x + 6, L.field[i].y + 4,
                          sw, L.field[i].h - 8,
                          (Color){64, 100, 150, 200});
        }
        DrawTextEx(*f, shown,
                   (Vector2){L.field[i].x + 8, L.field[i].y + 7},
                   14, 0, tc);
        if (focused && values[i][0] && !g_form.sel_all &&
            ((long long)(GetTime() * 2.0) & 1) == 0) {
            Vector2 vsz = MeasureTextEx(*f, shown, 14, 0);
            DrawRectangle(L.field[i].x + 8 + (int)vsz.x + 1,
                          L.field[i].y + 6, 8, 16,
                          (Color){125, 207, 255, 255});
        }
        EndScissorMode();
    }

    /* Buttons. */
    bool cf = g_form.focus == F_CONNECT;
    DrawRectangle(L.connect.x, L.connect.y, L.connect.w, L.connect.h,
                  cf ? (Color){64, 132, 210, 255} : (Color){46, 92, 150, 255});
    DrawRectangleLines(L.connect.x, L.connect.y, L.connect.w, L.connect.h,
                       (Color){125, 207, 255, cf ? 255 : 160});
    Vector2 csz = MeasureTextEx(*f, "Connect", 14, 0);
    DrawTextEx(*f, "Connect",
               (Vector2){L.connect.x + (L.connect.w - csz.x) / 2,
                         L.connect.y + (L.connect.h - csz.y) / 2},
               14, 0, (Color){240, 245, 255, 255});

    bool xf = g_form.focus == F_CANCEL;
    DrawRectangle(L.cancel.x, L.cancel.y, L.cancel.w, L.cancel.h,
                  xf ? (Color){72, 76, 96, 255} : (Color){48, 52, 66, 255});
    DrawRectangleLines(L.cancel.x, L.cancel.y, L.cancel.w, L.cancel.h,
                       (Color){150, 155, 170, xf ? 255 : 150});
    Vector2 xsz = MeasureTextEx(*f, "Cancel", 14, 0);
    DrawTextEx(*f, "Cancel",
               (Vector2){L.cancel.x + (L.cancel.w - xsz.x) / 2,
                         L.cancel.y + (L.cancel.h - xsz.y) / 2},
               14, 0, (Color){210, 215, 230, 255});

    /* Error line. */
    if (g_form.error[0]) {
        int ey = L.connect.y + L.connect.h + 10;
        DrawTextEx(*f, g_form.error,
                   (Vector2){L.modal.x + 22, ey}, 12, 0,
                   (Color){240, 100, 100, 255});
    }

    /* Footer hint. */
    DrawTextEx(*f, "Tab / Shift+Tab navigate   Enter connects   Esc cancels",
               (Vector2){L.modal.x + 22, L.modal.y + L.modal.h - 22},
               11, 0, (Color){110, 115, 130, 255});
}

/* ---------- Settings modal ----------
 * Minimal today (just font size), but the intent is this is *the*
 * preferences surface — new settings get an entry here rather than a
 * one-off shortcut. Keep it boring and extensible. */

static void settings_open(void) { g_ui_mode = UI_SETTINGS; }

static void settings_apply_font_size(Renderer *r, int new_size) {
    if (renderer_set_font_size(r, new_size)) {
        int nc = GetScreenWidth()  / r->cell_w;
        int nr = (GetScreenHeight() - TAB_BAR_H) / r->cell_h;
        if (nc < 1) nc = 1;
        if (nr < 1) nr = 1;
        for (int i = 0; i < g_num_tabs; i++) {
            screen_resize(g_tabs[i]->scr, nc, nr);
            pty_resize(g_tabs[i]->pty, nc, nr);
        }
        SetWindowMinSize(r->cell_w * 20, r->cell_h * 5 + TAB_BAR_H);
    }
}

typedef struct {
    Rect modal;
    Rect font_val;   /* current font size display */
    Rect dec, inc;   /* font -/+ buttons */
    Rect log_toggle; /* on/off button */
    Rect log_dir;    /* editable text box with log directory */
    Rect close;
} SettingsLayout;

static bool g_settings_dir_focus = false;
static bool g_settings_dir_sel_all = false;

static SettingsLayout settings_layout(int win_w, int win_h) {
    SettingsLayout L = {0};
    int w = 560, h = 300;
    if (w > win_w - 40) w = win_w - 40;
    if (h > win_h - 40) h = win_h - 40;
    L.modal.x = (win_w - w) / 2;
    L.modal.y = (win_h - h) / 2;
    L.modal.w = w;
    L.modal.h = h;

    int btn = 32;
    int font_row_y = L.modal.y + 70;
    L.font_val = (Rect){ L.modal.x + w - 214, font_row_y, 66, btn };
    L.dec      = (Rect){ L.modal.x + w - 138, font_row_y, btn, btn };
    L.inc      = (Rect){ L.modal.x + w - 60,  font_row_y, btn, btn };

    int log_row1_y = font_row_y + btn + 22;
    L.log_toggle = (Rect){ L.modal.x + w - 140, log_row1_y, 110, btn };

    int log_row2_y = log_row1_y + btn + 10;
    L.log_dir = (Rect){ L.modal.x + 140, log_row2_y, w - 140 - 22, btn };

    int close_w = 90, close_h = 32;
    L.close = (Rect){ L.modal.x + w - 22 - close_w,
                      L.modal.y + h - 22 - close_h,
                      close_w, close_h };
    return L;
}

static void settings_handle_mouse(Renderer *r, SettingsLayout L) {
    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) return;
    Vector2 mp = GetMousePosition();
    int mx = (int)mp.x, my = (int)mp.y;
    if (rect_hit(L.dec, mx, my))   { settings_apply_font_size(r, r->font_size - 1); return; }
    if (rect_hit(L.inc, mx, my))   { settings_apply_font_size(r, r->font_size + 1); return; }
    if (rect_hit(L.log_toggle, mx, my)) {
        g_app_settings.log_enabled = !g_app_settings.log_enabled;
        refresh_tab_logs();
        return;
    }
    if (rect_hit(L.log_dir, mx, my)) {
        g_settings_dir_focus = true;
        g_settings_dir_sel_all = false;
        return;
    }
    if (rect_hit(L.close, mx, my)) { g_ui_mode = UI_NORMAL; g_settings_dir_focus = false; return; }
    /* Click elsewhere drops focus from the text input. */
    g_settings_dir_focus = false;
}

static void settings_handle_keys(Renderer *r) {
    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
#if defined(__APPLE__)
    bool cmd  = IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER);
#else
    bool cmd  = false;
#endif
    bool mod  = ctrl || cmd;

    if (IsKeyPressed(KEY_ESCAPE)) {
        if (g_settings_dir_focus) { g_settings_dir_focus = false; return; }
        g_ui_mode = UI_NORMAL; return;
    }

    if (g_settings_dir_focus) {
        /* Text input on the log directory. */
        if (mod && IsKeyPressed(KEY_A)) { g_settings_dir_sel_all = true; return; }
        size_t len = strlen(g_app_settings.log_dir);
        if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) {
            if (g_settings_dir_sel_all) {
                g_app_settings.log_dir[0] = 0;
                g_settings_dir_sel_all = false;
            } else if (len > 0) {
                g_app_settings.log_dir[len - 1] = 0;
            }
        }
        if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
            /* Re-open logs against the (possibly new) path. */
            if (g_app_settings.log_enabled) {
                for (int i = 0; i < g_num_tabs; i++) tab_log_close(g_tabs[i]);
                refresh_tab_logs();
            }
            g_settings_dir_focus = false;
            return;
        }
        int cp;
        while ((cp = GetCharPressed()) != 0) {
            if (mod) continue;
            if (cp < 32 || cp >= 127) continue;
            if (g_settings_dir_sel_all) {
                g_app_settings.log_dir[0] = 0;
                len = 0;
                g_settings_dir_sel_all = false;
            }
            if (len + 1 >= sizeof(g_app_settings.log_dir)) continue;
            g_app_settings.log_dir[len++] = (char)cp;
            g_app_settings.log_dir[len] = 0;
        }
        return;
    }

    /* Not editing the path — keyboard shortcuts adjust font size. */
    if (IsKeyPressed(KEY_UP)   || IsKeyPressedRepeat(KEY_UP) ||
        IsKeyPressed(KEY_EQUAL) || IsKeyPressed(KEY_KP_ADD))
        settings_apply_font_size(r, r->font_size + 1);
    if (IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN) ||
        IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_KP_SUBTRACT))
        settings_apply_font_size(r, r->font_size - 1);
    /* Space toggles logging when not editing the path. */
    if (IsKeyPressed(KEY_SPACE)) {
        g_app_settings.log_enabled = !g_app_settings.log_enabled;
        refresh_tab_logs();
    }
}

static void draw_settings(Renderer *r, int win_w, int win_h, SettingsLayout L) {
    DrawRectangle(0, 0, win_w, win_h, (Color){0, 0, 0, 150});
    DrawRectangle(L.modal.x, L.modal.y, L.modal.w, L.modal.h,
                  (Color){30, 34, 46, 255});
    DrawRectangleLines(L.modal.x, L.modal.y, L.modal.w, L.modal.h,
                       (Color){125, 207, 255, 220});
    DrawRectangle(L.modal.x + 1, L.modal.y + 1, L.modal.w - 2, 38,
                  (Color){38, 42, 58, 255});

    Font *f = (Font *)r->font_data;
    DrawTextEx(*f, "Settings",
               (Vector2){L.modal.x + 20, L.modal.y + 11},
               16, 0, (Color){230, 232, 240, 255});

    /* Font size row. */
    DrawTextEx(*f, "Font size",
               (Vector2){L.modal.x + 22, L.font_val.y + 8},
               14, 0, (Color){200, 205, 220, 255});
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", r->font_size);
    DrawRectangle(L.font_val.x, L.font_val.y, L.font_val.w, L.font_val.h,
                  (Color){22, 25, 34, 255});
    DrawRectangleLines(L.font_val.x, L.font_val.y, L.font_val.w, L.font_val.h,
                       (Color){70, 74, 90, 255});
    Vector2 vsz = MeasureTextEx(*f, buf, 16, 0);
    DrawTextEx(*f, buf,
               (Vector2){L.font_val.x + (L.font_val.w - vsz.x) / 2,
                         L.font_val.y + (L.font_val.h - vsz.y) / 2},
               16, 0, (Color){230, 232, 240, 255});

    DrawRectangle(L.dec.x, L.dec.y, L.dec.w, L.dec.h, (Color){46, 52, 70, 255});
    DrawRectangleLines(L.dec.x, L.dec.y, L.dec.w, L.dec.h, (Color){125, 207, 255, 180});
    Vector2 ms = MeasureTextEx(*f, "-", 18, 0);
    DrawTextEx(*f, "-",
               (Vector2){L.dec.x + (L.dec.w - ms.x) / 2,
                         L.dec.y + (L.dec.h - ms.y) / 2},
               18, 0, (Color){230, 232, 240, 255});
    DrawRectangle(L.inc.x, L.inc.y, L.inc.w, L.inc.h, (Color){46, 52, 70, 255});
    DrawRectangleLines(L.inc.x, L.inc.y, L.inc.w, L.inc.h, (Color){125, 207, 255, 180});
    Vector2 ps = MeasureTextEx(*f, "+", 18, 0);
    DrawTextEx(*f, "+",
               (Vector2){L.inc.x + (L.inc.w - ps.x) / 2,
                         L.inc.y + (L.inc.h - ps.y) / 2},
               18, 0, (Color){230, 232, 240, 255});

    /* Session logging rows. */
    DrawTextEx(*f, "Log session",
               (Vector2){L.modal.x + 22, L.log_toggle.y + 8},
               14, 0, (Color){200, 205, 220, 255});
    bool on = g_app_settings.log_enabled;
    Color tbg = on ? (Color){64, 120, 90, 255} : (Color){60, 60, 72, 255};
    DrawRectangle(L.log_toggle.x, L.log_toggle.y, L.log_toggle.w, L.log_toggle.h, tbg);
    DrawRectangleLines(L.log_toggle.x, L.log_toggle.y, L.log_toggle.w, L.log_toggle.h,
                       (Color){125, 207, 255, 180});
    const char *toggle_text = on ? "Logging: ON" : "Logging: OFF";
    Vector2 tsz = MeasureTextEx(*f, toggle_text, 13, 0);
    DrawTextEx(*f, toggle_text,
               (Vector2){L.log_toggle.x + (L.log_toggle.w - tsz.x) / 2,
                         L.log_toggle.y + (L.log_toggle.h - tsz.y) / 2},
               13, 0, (Color){230, 232, 240, 255});

    DrawTextEx(*f, "Log directory",
               (Vector2){L.modal.x + 22, L.log_dir.y + 8},
               14, 0, (Color){200, 205, 220, 255});
    DrawRectangle(L.log_dir.x, L.log_dir.y, L.log_dir.w, L.log_dir.h,
                  (Color){22, 25, 34, 255});
    DrawRectangleLines(L.log_dir.x, L.log_dir.y, L.log_dir.w, L.log_dir.h,
                       g_settings_dir_focus ? (Color){125, 207, 255, 255}
                                            : (Color){70, 74, 90, 255});
    BeginScissorMode(L.log_dir.x + 6, L.log_dir.y,
                     L.log_dir.w - 12, L.log_dir.h);
    if (g_settings_dir_focus && g_settings_dir_sel_all && g_app_settings.log_dir[0]) {
        Vector2 ssz = MeasureTextEx(*f, g_app_settings.log_dir, 13, 0);
        int sw = (int)ssz.x + 4;
        if (sw > L.log_dir.w - 12) sw = L.log_dir.w - 12;
        DrawRectangle(L.log_dir.x + 6, L.log_dir.y + 4, sw,
                      L.log_dir.h - 8, (Color){64, 100, 150, 200});
    }
    DrawTextEx(*f, g_app_settings.log_dir,
               (Vector2){L.log_dir.x + 8, L.log_dir.y + 8},
               13, 0, (Color){230, 232, 240, 255});
    if (g_settings_dir_focus && !g_settings_dir_sel_all &&
        ((long long)(GetTime() * 2.0) & 1) == 0) {
        Vector2 dsz = MeasureTextEx(*f, g_app_settings.log_dir, 13, 0);
        DrawRectangle(L.log_dir.x + 8 + (int)dsz.x + 1,
                      L.log_dir.y + 8, 8, 14,
                      (Color){125, 207, 255, 255});
    }
    EndScissorMode();

    /* Close button. */
    DrawRectangle(L.close.x, L.close.y, L.close.w, L.close.h,
                  (Color){48, 52, 66, 255});
    DrawRectangleLines(L.close.x, L.close.y, L.close.w, L.close.h,
                       (Color){150, 155, 170, 200});
    Vector2 cs = MeasureTextEx(*f, "Close", 14, 0);
    DrawTextEx(*f, "Close",
               (Vector2){L.close.x + (L.close.w - cs.x) / 2,
                         L.close.y + (L.close.h - cs.y) / 2},
               14, 0, (Color){210, 215, 230, 255});

    DrawTextEx(*f, "Up / Down adjust font   Space toggles logs   Esc closes",
               (Vector2){L.modal.x + 22, L.modal.y + L.modal.h - 22},
               11, 0, (Color){110, 115, 130, 255});
}

/* ---------- Modifier helpers ---------- */

static bool ui_key_down(void) {
#if defined(__APPLE__)
    return IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER);
#else
    return IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
#endif
}

/* ---------- Main ---------- */

static void usage(void) {
    printf("rbterm — raylib terminal emulator\n"
           "Usage: rbterm [--font PATH] [--size N] [--cols N] [--rows N]\n"
           "  --font PATH     path to a .ttf / .otf / .ttc monospace font\n"
           "  --size N        font size in points (default 20)\n"
           "  --cols N        initial cols (default 100)\n"
           "  --rows N        initial rows (default 30)\n"
           "\nKeys (Cmd on macOS, Ctrl on Linux/Windows):\n"
           "  Cmd + T           open a new tab (local shell)\n"
           "  Cmd + Shift + T   open an SSH tab (user@host[:port])\n"
           "  Cmd + W           close the current tab\n"
           "  Cmd + 1..9        switch to tab N\n"
           "  Cmd + [ / ]       prev / next tab\n"
           "  Cmd + + / - / 0   grow/shrink/reset font size\n"
           "  Shift + PgUp/PgDn scroll through history\n"
           "  Mouse wheel       scroll through history\n"
           "  Cmd + C / V       copy selection / paste\n");
}

int main(int argc, char **argv) {
    const char *font_path = NULL;
    int font_size = 20;
    int init_cols = 100, init_rows = 30;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(); return 0; }
        else if (!strcmp(argv[i], "--font") && i + 1 < argc) font_path = argv[++i];
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) font_size = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cols") && i + 1 < argc) init_cols = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--rows") && i + 1 < argc) init_rows = atoi(argv[++i]);
        else { fprintf(stderr, "unknown arg: %s\n", argv[i]); usage(); return 2; }
    }
    if (init_cols < 20) init_cols = 20;
    if (init_rows < 5)  init_rows = 5;

    app_settings_init();

#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT | FLAG_WINDOW_HIGHDPI);
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(800, 500, "rbterm");
    SetExitKey(KEY_NULL);

    Renderer r;
    if (!renderer_init(&r, font_path, font_size)) {
        CloseWindow();
        return 1;
    }

    int win_w = init_cols * r.cell_w;
    int win_h = init_rows * r.cell_h + TAB_BAR_H;
    SetWindowSize(win_w, win_h);
    SetWindowMinSize(r.cell_w * 20, r.cell_h * 5 + TAB_BAR_H);

    if (!tab_open(init_cols, init_rows)) {
        renderer_shutdown(&r);
        CloseWindow();
        return 1;
    }

    SetTargetFPS(60);

    uint8_t readbuf[65536];
    uint8_t inputbuf[4096];

    while (!WindowShouldClose() && g_num_tabs > 0) {
        int win_w_now = GetScreenWidth();
        int win_h_now = GetScreenHeight();

        int content_rows = (win_h_now - TAB_BAR_H) / r.cell_h;
        int content_cols = win_w_now / r.cell_w;
        if (content_cols < 1) content_cols = 1;
        if (content_rows < 1) content_rows = 1;
        for (int i = 0; i < g_num_tabs; i++) {
            if (screen_cols(g_tabs[i]->scr) != content_cols ||
                screen_rows(g_tabs[i]->scr) != content_rows) {
                screen_resize(g_tabs[i]->scr, content_cols, content_rows);
                pty_resize(g_tabs[i]->pty, content_cols, content_rows);
            }
        }

        /* Drain each PTY until EAGAIN or a safety cap. The cap is just a
           watchdog for a runaway writer pinning the UI — under normal
           output bursts (`find /usr`, etc.) we want to pull everything
           the kernel has buffered before rendering, otherwise the shell
           stalls on full PTY-buffer writes and the whole command takes
           seconds longer than in a native terminal. */
        for (int i = 0; i < g_num_tabs; i++) {
            Tab *t = g_tabs[i];
            size_t drained = 0;
            const size_t drain_cap = 16 * 1024 * 1024;
            for (;;) {
                int n = pty_read(t->pty, readbuf, sizeof(readbuf));
                if (n > 0) {
                    screen_feed(t->scr, readbuf, (size_t)n);
                    tab_log_write(t, readbuf, (size_t)n);
                    drained += (size_t)n;
                    if (drained >= drain_cap) break;
                    continue;
                }
                if (n < 0) { t->dead = true; }
                break;
            }
            if (!pty_alive(t->pty)) t->dead = true;
        }
        for (int i = g_num_tabs - 1; i >= 0; i--) {
            if (g_tabs[i]->dead) tab_close(i);
        }
        if (g_num_tabs == 0) break;

        /* CWD label refresh (no-op on Windows — pty_cwd returns false). */
        double now = GetTime();
        for (int i = 0; i < g_num_tabs; i++) {
            Tab *t = g_tabs[i];
            if (now - t->cwd_poll_at < 0.3) continue;
            t->cwd_poll_at = now;
            char buf[PATH_MAX];
            if (pty_cwd(t->pty, buf, sizeof(buf)) &&
                strcmp(buf, t->cwd) != 0) {
                strncpy(t->cwd, buf, sizeof(t->cwd) - 1);
                t->cwd[sizeof(t->cwd) - 1] = 0;
                if (i == g_active) t->title_dirty = true;
            }
        }

        Tab *cur = active_tab();
        Vector2 mp = GetMousePosition();
        bool in_tab_bar = (mp.y < TAB_BAR_H);

        if (in_tab_bar) {
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                TabBarHit h = tab_bar_hit_test(win_w_now, (int)mp.x, (int)mp.y);
                if (h.on_plus) tab_open(content_cols, content_rows);
                else if (h.on_ssh) ssh_form_open();
                else if (h.tab_idx >= 0) {
                    if (h.on_close) tab_close(h.tab_idx);
                    else g_active = h.tab_idx;
                }
                cur = active_tab();
            }
        } else if (cur) {
            int mcol = (int)(mp.x / r.cell_w);
            int mrow = (int)((mp.y - TAB_BAR_H) / r.cell_h);
            int cmax = screen_cols(cur->scr) - 1;
            int rmax = screen_rows(cur->scr) - 1;
            if (mcol < 0) mcol = 0; if (mcol > cmax) mcol = cmax;
            if (mrow < 0) mrow = 0; if (mrow > rmax) mrow = rmax;

            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                double t = GetTime();
                bool fast = (t - cur->last_click_time < 0.45);
                bool same = (mcol == cur->last_click_col && mrow == cur->last_click_row);
                cur->click_count = (fast && same) ? (cur->click_count + 1) : 1;
                cur->last_click_time = t;
                cur->last_click_col = mcol;
                cur->last_click_row = mrow;
                if (cur->click_count == 2)      select_word(cur->scr, &cur->sel, mcol, mrow);
                else if (cur->click_count >= 3) { select_line(cur->scr, &cur->sel, mrow); cur->click_count = 3; }
                else {
                    cur->sel.active = true;
                    cur->sel.dragging = true;
                    cur->sel.a_col = cur->sel.b_col = mcol;
                    cur->sel.a_row = cur->sel.b_row = mrow;
                }
            }
            if (cur->sel.dragging && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                cur->sel.b_col = mcol; cur->sel.b_row = mrow;
            }
            if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                if (cur->sel.dragging && cur->click_count < 2) {
                    cur->sel.dragging = false;
                    if (cur->sel.a_col == cur->sel.b_col &&
                        cur->sel.a_row == cur->sel.b_row) {
                        cur->sel.active = false;
                    }
                }
            }
        }

        /* Modal SSH form — swallow input until the user connects or cancels. */
        if (g_ui_mode == UI_SSH_FORM) {
            SshFormLayout L = ssh_form_layout(win_w_now, win_h_now);
            ssh_form_handle_mouse(L, content_cols, content_rows);
            if (g_ui_mode != UI_SSH_FORM) {
                cur = active_tab();
                if (!cur) break;
                BeginDrawing();
                ClearBackground((Color){0, 0, 0, 255});
                draw_tab_bar(&r, win_w_now);
                renderer_draw(&r, cur->scr, GetTime(), IsWindowFocused(),
                              &cur->sel, TAB_BAR_H);
                EndDrawing();
                continue;
            }
            ssh_form_handle_keys(content_cols, content_rows, L);
            cur = active_tab();
            if (!cur) break;
            if (cur->title_dirty) {
                SetWindowTitle(cur->title[0] ? cur->title : tab_label(cur));
                cur->title_dirty = false;
            }
            BeginDrawing();
            ClearBackground((Color){0, 0, 0, 255});
            draw_tab_bar(&r, win_w_now);
            renderer_draw(&r, cur->scr, GetTime(), IsWindowFocused(),
                          &cur->sel, TAB_BAR_H);
            draw_ssh_form(&r, win_w_now, win_h_now, L);
            EndDrawing();
            continue;
        }

        /* Settings modal. */
        if (g_ui_mode == UI_SETTINGS) {
            SettingsLayout L = settings_layout(win_w_now, win_h_now);
            settings_handle_mouse(&r, L);
            settings_handle_keys(&r);
            cur = active_tab();
            if (!cur) break;
            BeginDrawing();
            ClearBackground((Color){0, 0, 0, 255});
            draw_tab_bar(&r, win_w_now);
            /* Layout may have changed if font was resized. */
            L = settings_layout(win_w_now, win_h_now);
            renderer_draw(&r, cur->scr, GetTime(), IsWindowFocused(),
                          &cur->sel, TAB_BAR_H);
            draw_settings(&r, win_w_now, win_h_now, L);
            EndDrawing();
            continue;
        }

        /* Tab shortcuts (pre-input so Cmd/Ctrl+T/W/digit don't slip into the shell). */
        bool shift_held = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
        if (ui_key_down()) {
            if (IsKeyPressed(KEY_COMMA)) {
                settings_open();
            }
            else if (shift_held && IsKeyPressed(KEY_T)) {
                ssh_form_open();
            }
            else if (IsKeyPressed(KEY_T)) { tab_open(content_cols, content_rows); cur = active_tab(); }
            else if (IsKeyPressed(KEY_W)) {
                tab_close(g_active);
                if (g_num_tabs == 0) break;
                cur = active_tab();
            }
            else if (IsKeyPressed(KEY_LEFT_BRACKET))  { g_active = (g_active - 1 + g_num_tabs) % g_num_tabs; cur = active_tab(); }
            else if (IsKeyPressed(KEY_RIGHT_BRACKET)) { g_active = (g_active + 1) % g_num_tabs; cur = active_tab(); }
            else {
                for (int k = 0; k < 9; k++) {
                    if (IsKeyPressed(KEY_ONE + k)) {
                        if (k < g_num_tabs) { g_active = k; cur = active_tab(); }
                        break;
                    }
                }
            }
        }

        if (!cur) break;

        InputActions acts;
        size_t in_n = input_poll(cur->scr, inputbuf, sizeof(inputbuf), &acts);
        if (acts.scroll_rows != 0) screen_scroll_view(cur->scr, acts.scroll_rows);
        if (acts.copy) copy_selection(cur->scr, &cur->sel);
        if (acts.paste) {
            const char *t = GetClipboardText();
            if (t && *t) pty_write(cur->pty, (const uint8_t *)t, strlen(t));
        }
        if (acts.font_delta != 100) {
            int old = r.font_size;
            int ns = (acts.font_delta == 0) ? 20
                    : old + (acts.font_delta > 0 ? 1 : -1);
            if (renderer_set_font_size(&r, ns)) {
                int nc = GetScreenWidth()  / r.cell_w;
                int nr = (GetScreenHeight() - TAB_BAR_H) / r.cell_h;
                if (nc < 1) nc = 1;
                if (nr < 1) nr = 1;
                for (int i = 0; i < g_num_tabs; i++) {
                    screen_resize(g_tabs[i]->scr, nc, nr);
                    pty_resize(g_tabs[i]->pty, nc, nr);
                }
                SetWindowMinSize(r.cell_w * 20, r.cell_h * 5 + TAB_BAR_H);
            }
        }
        if (in_n > 0) {
            screen_scroll_reset(cur->scr);
            pty_write(cur->pty, inputbuf, in_n);
            if (cur->sel.active && !cur->sel.dragging) cur->sel.active = false;
        }

        if (cur->title_dirty) {
            SetWindowTitle(cur->title[0] ? cur->title : tab_label(cur));
            cur->title_dirty = false;
        }

        BeginDrawing();
        ClearBackground((Color){0, 0, 0, 255});
        draw_tab_bar(&r, win_w_now);
        renderer_draw(&r, cur->scr, GetTime(), IsWindowFocused(),
                      &cur->sel, TAB_BAR_H);
        EndDrawing();
    }

    for (int i = g_num_tabs - 1; i >= 0; i--) tab_close(i);
    renderer_shutdown(&r);
    CloseWindow();
    return 0;
}
