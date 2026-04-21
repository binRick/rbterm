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
#ifdef _WIN32
  /* windows.h brings in wingdi.h / winuser.h which define Rectangle,
     CloseWindow, ShowCursor, DrawText as macros and collide with
     raylib's types of the same name. NOGDI / NOUSER strip those
     declarations; we only need kernel32 bits (FindFirstFile etc.)
     from main.c so nothing's lost. */
  #define NOGDI
  #define NOUSER
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #define strcasecmp  _stricmp
  #define strncasecmp _strnicmp
#else
  #include <strings.h>   /* strcasecmp */
  #include <dirent.h>
#endif

#ifndef _WIN32
#include <signal.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

/* ---------- Persistent config ----------
 * Plain key=value file at ~/.config/rbterm/config.ini. Loaded once at
 * startup and re-written by the "Save as Default" button in Settings. */

static void expand_home_path(const char *in, char *out, size_t cap);
static void mkdir_p(const char *path);

static void config_path(char *out, size_t cap) {
    expand_home_path("~/.config/rbterm/config.ini", out, cap);
}

static void config_dir(char *out, size_t cap) {
    expand_home_path("~/.config/rbterm", out, cap);
}

/* Lightweight key=value parser. Trims whitespace, ignores comments
   starting with '#'. */
static bool ini_split(char *line, char **k_out, char **v_out) {
    /* trim leading ws */
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '#' || *line == 0 || *line == '\n' || *line == '\r') return false;
    char *eq = strchr(line, '=');
    if (!eq) return false;
    *eq = 0;
    char *k = line;
    char *v = eq + 1;
    /* trim trailing ws on key */
    char *kp = k + strlen(k);
    while (kp > k && (kp[-1] == ' ' || kp[-1] == '\t')) *--kp = 0;
    /* trim leading ws on value, trailing newline/ws */
    while (*v == ' ' || *v == '\t') v++;
    char *vp = v + strlen(v);
    while (vp > v && (vp[-1] == '\n' || vp[-1] == '\r' ||
                      vp[-1] == ' '  || vp[-1] == '\t')) *--vp = 0;
    *k_out = k;
    *v_out = v;
    return true;
}

/* ---------- App-wide settings ---------- */

typedef struct {
    bool log_enabled;
    char log_dir[PATH_MAX];
} AppSettings;
static AppSettings g_app_settings;
static char        g_settings_status[160]; /* status line in the settings modal */

/* Defaults read from the config file at startup. Consumed (and cleared)
   by main() after the renderer and tabs exist. */
typedef struct {
    bool has_font_path;   char font_path[1024];
    bool has_font_size;   int  font_size;
    bool has_padding;     int  padding;
    bool has_spacing;     int  spacing;
    bool has_log;
} PersistedDefaults;
static PersistedDefaults g_persisted;

static void config_load_into_defaults(void) {
    char path[PATH_MAX];
    config_path(path, sizeof(path));
    FILE *fp = fopen(path, "r");
    if (!fp) return;
    char line[2048];
    while (fgets(line, sizeof(line), fp)) {
        char *k, *v;
        if (!ini_split(line, &k, &v)) continue;
        if (strcmp(k, "font_path") == 0) {
            char resolved[1024];
            expand_home_path(v, resolved, sizeof(resolved));
            strncpy(g_persisted.font_path, resolved, sizeof(g_persisted.font_path) - 1);
            g_persisted.font_path[sizeof(g_persisted.font_path) - 1] = 0;
            g_persisted.has_font_path = true;
        } else if (strcmp(k, "font_size") == 0) {
            g_persisted.font_size = atoi(v);
            g_persisted.has_font_size = true;
        } else if (strcmp(k, "padding") == 0) {
            g_persisted.padding = atoi(v);
            g_persisted.has_padding = true;
        } else if (strcmp(k, "cell_spacing") == 0) {
            g_persisted.spacing = atoi(v);
            g_persisted.has_spacing = true;
        } else if (strcmp(k, "log_enabled") == 0) {
            g_app_settings.log_enabled = (strcmp(v, "true") == 0 || strcmp(v, "1") == 0);
            g_persisted.has_log = true;
        } else if (strcmp(k, "log_dir") == 0) {
            strncpy(g_app_settings.log_dir, v, sizeof(g_app_settings.log_dir) - 1);
            g_app_settings.log_dir[sizeof(g_app_settings.log_dir) - 1] = 0;
        }
    }
    fclose(fp);
}

/* Write the current renderer / app settings back to ~/.config/rbterm/config.ini.
   Used by the "Save as Default" button. Returns true on success. */
static bool config_save(Renderer *r) {
    char dir[PATH_MAX], path[PATH_MAX];
    config_dir(dir, sizeof(dir));
    config_path(path, sizeof(path));
    mkdir_p(dir);
    FILE *fp = fopen(path, "w");
    if (!fp) return false;
    fprintf(fp, "# rbterm defaults — rewritten by Settings → Save as Default\n");
    if (r->font_path[0]) fprintf(fp, "font_path=%s\n", r->font_path);
    fprintf(fp, "font_size=%d\n",    r->font_size);
    fprintf(fp, "padding=%d\n",      r->pad_x);
    fprintf(fp, "cell_spacing=%d\n", r->cell_extra_w);
    fprintf(fp, "log_enabled=%s\n",  g_app_settings.log_enabled ? "true" : "false");
    if (g_app_settings.log_dir[0]) fprintf(fp, "log_dir=%s\n", g_app_settings.log_dir);
    fclose(fp);
#ifndef _WIN32
    chmod(path, 0600);
#endif
    return true;
}

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
    bool  is_ssh;
    char  ssh_target[256];       /* user@host[:port] for SSH tabs */
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
    F_CONNECT, F_SAVE, F_CANCEL,
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
static int        g_ssh_list_selected = -1; /* highlighted row, -1 = none */

typedef struct {
    Rect modal;
    Rect list;                  /* saved-hosts sidebar */
    Rect field[F_TEXT_FIELDS];  /* host, port, user, pass, key */
    Rect connect;
    Rect save;
    Rect cancel;
} SshFormLayout;

static char g_form_status[192];   /* positive status line (e.g. "saved") */

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
static void io_set_clipboard_cb(void *u, const char *utf8) {
    (void)u;
    if (utf8 && *utf8) SetClipboardText(utf8);
}
static void io_set_cwd_cb(void *u, const char *path) {
    Tab *t = (Tab *)u;
    if (!t || !path || !*path) return;
    strncpy(t->cwd, path, sizeof(t->cwd) - 1);
    t->cwd[sizeof(t->cwd) - 1] = 0;
    t->title_dirty = true;
}

static Tab *tab_open(int cols, int rows) {
    if (g_num_tabs >= MAX_TABS) return NULL;
    Tab *t = calloc(1, sizeof(Tab));
    strncpy(t->title, "shell", sizeof(t->title) - 1);
    t->last_click_time = -1.0;
    t->last_click_col = t->last_click_row = -1;
    t->pty = pty_open(cols, rows);
    if (!t->pty) { free(t); return NULL; }
    ScreenIO io = { .user = t, .write = io_write_cb,
                    .set_title = io_set_title_cb, .bell = io_bell_cb, .set_clipboard = io_set_clipboard_cb, .set_cwd = io_set_cwd_cb };
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
            snprintf(t->ssh_target, sizeof(t->ssh_target), "%s@%s", user, host);
        else
            snprintf(t->ssh_target, sizeof(t->ssh_target), "%s@%s:%d", user, host, port);
    } else {
        if (port == 22) snprintf(t->ssh_target, sizeof(t->ssh_target), "%s", host);
        else snprintf(t->ssh_target, sizeof(t->ssh_target), "%s:%d", host, port);
    }
    t->is_ssh = true;
    snprintf(t->title, sizeof(t->title), "%s", t->ssh_target);
    t->cwd[0] = 0;   /* OSC 7 from the remote shell will populate this */
    t->last_click_time = -1.0;
    t->last_click_col = t->last_click_row = -1;
    t->pty = pty_open_ssh(user, host, port, password, keyfile,
                          cols, rows, err, errsz);
    if (!t->pty) { free(t); return NULL; }
    ScreenIO io = { .user = t, .write = io_write_cb,
                    .set_title = io_set_title_cb, .bell = io_bell_cb, .set_clipboard = io_set_clipboard_cb, .set_cwd = io_set_cwd_cb };
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
    /* SSH tabs: "user@host[:port] [basename]". Buffer is a rotating
       set of per-tab statics so multiple tabs can be drawn in one
       frame without overwriting each other. */
    if (t->is_ssh) {
        static char buf[MAX_TABS][320];
        static int  slot = 0;
        int idx = -1;
        for (int i = 0; i < g_num_tabs; i++) if (g_tabs[i] == t) { idx = i; break; }
        if (idx < 0) idx = slot++ % MAX_TABS;
        char *out = buf[idx];
        if (!t->cwd[0]) {
            return t->ssh_target;
        }
        /* Basename of the remote path, with "~" or "/" shortcuts. */
        const char *dir_label = t->cwd;
        const char *b = strrchr(t->cwd, '/');
        if (strcmp(t->cwd, "/") == 0) dir_label = "/";
        else if (b) dir_label = (*(b + 1) ? b + 1 : b);   /* trailing slash → "/" */
        snprintf(out, sizeof(buf[0]), "%s %s", t->ssh_target, dir_label);
        return out;
    }
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

/* Layout: [ssh] | tab1 | tab2 | ... | [+] */
static TabBarHit tab_bar_hit_test(int win_w, int mx, int my) {
    TabBarHit h = { -1, false, false, false };
    if (my < 0 || my >= TAB_BAR_H) return h;
    int plus_x   = win_w - TAB_PLUS_W;
    int tab_start = TAB_SSH_W;
    if (mx < TAB_SSH_W)  { h.on_ssh  = true; return h; }
    if (mx >= plus_x)    { h.on_plus = true; return h; }
    int tw = tab_width_for(win_w);
    int idx = (mx - tab_start) / tw;
    if (idx < 0 || idx >= g_num_tabs) return h;
    h.tab_idx = idx;
    int tx = tab_start + idx * tw;
    if (mx >= tx + tw - TAB_CLOSE_W) h.on_close = true;
    return h;
}

static void draw_tab_bar(Renderer *r, int win_w) {
    DrawRectangle(0, 0, win_w, TAB_BAR_H, (Color){24, 24, 32, 255});
    DrawRectangle(0, TAB_BAR_H - 1, win_w, 1, (Color){60, 60, 75, 255});

    Font *f = (Font *)r->font_data;
    float fs = 13.0f;

    /* "ssh" button anchored top-left. */
    int ssh_x  = 0;
    DrawRectangle(ssh_x, 0, TAB_SSH_W, TAB_BAR_H, (Color){38, 48, 66, 255});
    DrawRectangleLines(ssh_x, 2, TAB_SSH_W - 1, TAB_BAR_H - 4,
                       (Color){125, 207, 255, 200});
    Vector2 ssz = MeasureTextEx(*f, "ssh", 13, 0);
    DrawTextEx(*f, "ssh",
               (Vector2){ ssh_x + (TAB_SSH_W - ssz.x) / 2.0f,
                          (TAB_BAR_H - ssz.y) / 2.0f },
               13, 0, (Color){180, 230, 255, 255});

    /* "+" button anchored top-right. */
    int plus_x = win_w - TAB_PLUS_W;
    DrawRectangle(plus_x, 0, TAB_PLUS_W, TAB_BAR_H, (Color){38, 48, 66, 255});
    DrawRectangleLines(plus_x, 2, TAB_PLUS_W - 1, TAB_BAR_H - 4,
                       (Color){125, 207, 255, 200});
    Vector2 psz = MeasureTextEx(*f, "+", 18, 0);
    DrawTextEx(*f, "+",
               (Vector2){ plus_x + (TAB_PLUS_W - psz.x) / 2.0f,
                          (TAB_BAR_H - psz.y) / 2.0f },
               18, 0, (Color){230, 240, 255, 255});

    /* Tabs fill the space between the two buttons. */
    int tab_start = TAB_SSH_W;
    int tw = tab_width_for(win_w);

    for (int i = 0; i < g_num_tabs; i++) {
        int x = tab_start + i * tw;
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
    g_ssh_list_selected = -1;
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
    g_form_status[0] = 0;
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
    int connect_w = 110, save_w = 90, cancel_w = 96;
    int gap = 8;
    int right_edge = L.modal.x + L.modal.w - pad;
    L.cancel.w  = cancel_w;  L.cancel.h  = btn_h;
    L.save.w    = save_w;    L.save.h    = btn_h;
    L.connect.w = connect_w; L.connect.h = btn_h;
    L.cancel.x  = right_edge - cancel_w;
    L.save.x    = L.cancel.x - gap - save_w;
    L.connect.x = L.save.x - gap - connect_w;
    L.cancel.y = L.save.y = L.connect.y = btn_y;
    return L;
}

static bool rect_hit(Rect r, int x, int y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static void ssh_form_submit(int cols, int rows) {
#ifdef __EMSCRIPTEN__
    /* Web demo: SSH is stubbed out and the browser can't open sockets
       to arbitrary hosts anyway. Make Connect a visible no-op with an
       explanatory status line — don't try to spawn anything. */
    (void)cols; (void)rows;
    strncpy(g_form.error,
            "SSH is disabled in the web demo. Try the native build for this.",
            sizeof(g_form.error) - 1);
    g_form.error[sizeof(g_form.error) - 1] = 0;
    return;
#else
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
#endif
}

static void ssh_form_advance_focus(int delta) {
    g_form.focus = (g_form.focus + delta + F_COUNT) % F_COUNT;
    g_form.sel_all = false;
}

/* Append the form's current values as a `Host` stanza to ~/.ssh/config.
   Refuses to overwrite an existing alias. The alias we save under is the
   Host field — convenient because after saving, clicking the new entry
   in the sidebar re-populates identical values. */
static bool ssh_form_save_to_config(void) {
    if (!g_form.host[0]) {
        strncpy(g_form.error, "Host is required before saving",
                sizeof(g_form.error) - 1);
        g_form.focus = F_HOST;
        return false;
    }
    char path[PATH_MAX];
    expand_home_path("~/.ssh/config", path, sizeof(path));
    /* Make sure the parent ~/.ssh exists. */
    char ssh_dir[PATH_MAX];
    expand_home_path("~/.ssh", ssh_dir, sizeof(ssh_dir));
    mkdir_p(ssh_dir);
#ifndef _WIN32
    chmod(ssh_dir, 0700);
#endif

    /* Refuse to clobber an existing stanza with the same alias. */
    for (int i = 0; i < g_ssh_profile_count; i++) {
        if (strcmp(g_ssh_profiles[i].name, g_form.host) == 0) {
            snprintf(g_form.error, sizeof(g_form.error),
                     "Host '%s' already exists in ~/.ssh/config — edit manually to overwrite",
                     g_form.host);
            return false;
        }
    }

    FILE *fp = fopen(path, "a");
    if (!fp) {
        snprintf(g_form.error, sizeof(g_form.error),
                 "Can't open %s for append: %s", path, strerror(errno));
        return false;
    }
    /* Leading blank line in case the previous content doesn't end with one. */
    fputc('\n', fp);
    fprintf(fp, "Host %s\n", g_form.host);
    fprintf(fp, "    HostName %s\n", g_form.host);
    if (g_form.user[0])
        fprintf(fp, "    User %s\n", g_form.user);
    int port = atoi(g_form.port);
    if (port > 0 && port != 22)
        fprintf(fp, "    Port %d\n", port);
    if (g_form.key[0])
        fprintf(fp, "    IdentityFile %s\n", g_form.key);
    fclose(fp);
#ifndef _WIN32
    chmod(path, 0600);
#endif

    snprintf(g_form_status, sizeof(g_form_status),
             "Saved '%s' to %s", g_form.host, path);
    g_form.error[0] = 0;
    /* Refresh the sidebar so the new entry shows immediately. */
    ssh_profiles_load();
    return true;
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

    /* Click on a saved host → populate fields from ~/.ssh/config. The
       user hits Connect (or double-clicks) to actually open the session
       — keeps single-click from making a tab before you've checked the
       fields match what you wanted. */
    if (L.list.w > 0 && rect_hit(L.list, mx, my)) {
        int row_h = 22;
        int idx = (my - L.list.y) / row_h + g_ssh_list_scroll;
        if (idx >= 0 && idx < g_ssh_profile_count) {
            static double last_pick_t = -1.0;
            static int    last_pick_i = -1;
            g_ssh_list_selected = idx;
            ssh_form_apply_profile(&g_ssh_profiles[idx]);
            g_form.focus = F_CONNECT;
            double now = GetTime();
            if (idx == last_pick_i && now - last_pick_t < 0.45) {
                ssh_form_submit(cols, rows);  /* double-click */
                last_pick_i = -1;
            } else {
                last_pick_i = idx;
                last_pick_t = now;
            }
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
    if (rect_hit(L.save, mx, my)) {
        g_form.focus = F_SAVE;
        ssh_form_save_to_config();
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
        if (g_form.focus == F_SAVE) {
            ssh_form_save_to_config();
            return;
        }
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
    if (g_form.focus == F_SAVE && IsKeyPressed(KEY_SPACE)) {
        ssh_form_save_to_config(); return;
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
            bool on_this = (i == g_ssh_list_selected);
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

    bool sf = g_form.focus == F_SAVE;
    DrawRectangle(L.save.x, L.save.y, L.save.w, L.save.h,
                  sf ? (Color){80, 120, 80, 255} : (Color){48, 78, 58, 255});
    DrawRectangleLines(L.save.x, L.save.y, L.save.w, L.save.h,
                       (Color){150, 220, 170, sf ? 255 : 170});
    Vector2 ssz2 = MeasureTextEx(*f, "Save", 14, 0);
    DrawTextEx(*f, "Save",
               (Vector2){L.save.x + (L.save.w - ssz2.x) / 2,
                         L.save.y + (L.save.h - ssz2.y) / 2},
               14, 0, (Color){220, 240, 225, 255});

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

    /* Status (success) / error line under the buttons. */
    if (g_form.error[0]) {
        DrawTextEx(*f, g_form.error,
                   (Vector2){L.modal.x + 22, L.connect.y + L.connect.h + 10},
                   12, 0, (Color){240, 100, 100, 255});
    } else if (g_form_status[0]) {
        DrawTextEx(*f, g_form_status,
                   (Vector2){L.modal.x + 22, L.connect.y + L.connect.h + 10},
                   12, 0, (Color){120, 220, 140, 255});
    }

    /* Footer hint. */
    DrawTextEx(*f, "Tab navigates · Enter connects · Save appends to ~/.ssh/config · Esc cancels",
               (Vector2){L.modal.x + 22, L.modal.y + L.modal.h - 22},
               11, 0, (Color){110, 115, 130, 255});
}

/* ---------- Settings modal ----------
 * Minimal today (just font size), but the intent is this is *the*
 * preferences surface — new settings get an entry here rather than a
 * one-off shortcut. Keep it boring and extensible. */

static void fonts_load(const char *current_path);
static void settings_open(Renderer *r) {
    g_ui_mode = UI_SETTINGS;
    g_settings_status[0] = 0;
    fonts_load(r ? r->font_path : NULL);
}

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
        SetWindowMinSize(r->cell_w * 20 + 2 * r->pad_x, r->cell_h * 5 + TAB_BAR_H + 2 * r->pad_y);
    }
}

typedef struct {
    Rect modal;
    Rect font_val;   /* current font size display */
    Rect dec, inc;   /* font -/+ buttons */
    Rect font_list;  /* scrollable list of monospace fonts */
    Rect pad_val;
    Rect pad_dec, pad_inc;
    Rect spc_val;
    Rect spc_dec, spc_inc;
    Rect log_toggle; /* on/off button */
    Rect log_dir;    /* editable text box with log directory */
    Rect save_default; /* write current state to ~/.config/rbterm/config.ini */
    Rect close;
} SettingsLayout;

/* Available-fonts enumeration for the settings modal. Scans system + user
   font directories for monospace .ttf / .otf / .ttc files; only those whose
   filename hints at a monospace face (contains "Mono", "Code", "Fira", etc.)
   are included — scrolling through every font on disk would be noisy. */
typedef struct {
    char name[128];
    char path[512];
    void *preview;       /* Font *, loaded lazily the first time the row
                            is visible in the settings font list. */
    bool  load_failed;   /* Set once LoadFontEx fails so we don't retry. */
} FontEntry;

#define MAX_FONTS 256
static FontEntry g_fonts[MAX_FONTS];
static int       g_font_count = 0;
static int       g_font_list_scroll = 0;
static int       g_font_list_selected = -1;

static bool looks_monospace(const char *name) {
    /* Anything whose filename contains one of these fragments is shown
       in the picker. Keep the list generous — false positives in the
       picker are cheap, false negatives leave fonts unreachable. */
    static const char *pat[] = {
        "Mono", "Menlo", "Monaco", "Consolas", "Courier", "Code",
        "Fira", "JetBrains", "Hack", "Inconsolata", "Terminus",
        "Fixed", "Source Code", "Anonymous", "Noto Sans Mono",
        "SF Mono", "SFMono", "Iosevka", "Cascadia",
        "IBMPlex", "Plex", "RobotoMono", "Roboto Mono", "SpaceMono",
        "Space Mono", "Victor", "Ubuntu Mono", "Liberation",
        "DejaVu", "Nimbus", "Office Code Pro", "OfficeCodePro",
        "GoMono", "Go Mono", "Iosevka", "Envy", "Pragmata",
        "Terminess", "Mononoki", "MPlus", "M+", "TerminalVector",
        NULL
    };
    for (int i = 0; pat[i]; i++) if (strstr(name, pat[i])) return true;
    return false;
}

static int cmp_font_name(const void *a, const void *b) {
    return strcasecmp(((const FontEntry *)a)->name,
                      ((const FontEntry *)b)->name);
}

static bool ends_with_ci(const char *s, const char *suffix) {
    size_t ls = strlen(s), lsfx = strlen(suffix);
    if (ls < lsfx) return false;
    return strcasecmp(s + ls - lsfx, suffix) == 0;
}

static void scan_font_dir(const char *dir) {
#ifdef _WIN32
    char pattern[1024];
    snprintf(pattern, sizeof(pattern), "%s/*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (g_font_count >= MAX_FONTS) break;
        const char *name = fd.cFileName;
        if (!(ends_with_ci(name, ".ttf") || ends_with_ci(name, ".otf") ||
              ends_with_ci(name, ".ttc"))) continue;
        if (!looks_monospace(name)) continue;
        FontEntry *f = &g_fonts[g_font_count++];
        snprintf(f->path, sizeof(f->path), "%s/%s", dir, name);
        strncpy(f->name, name, sizeof(f->name) - 1);
        f->name[sizeof(f->name) - 1] = 0;
        int nl = (int)strlen(f->name);
        if (nl > 4 && (ends_with_ci(f->name, ".ttf") ||
                       ends_with_ci(f->name, ".otf") ||
                       ends_with_ci(f->name, ".ttc")))
            f->name[nl - 4] = 0;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *dp = opendir(dir);
    if (!dp) return;
    struct dirent *e;
    while ((e = readdir(dp)) != NULL && g_font_count < MAX_FONTS) {
        const char *name = e->d_name;
        if (!(ends_with_ci(name, ".ttf") || ends_with_ci(name, ".otf") ||
              ends_with_ci(name, ".ttc"))) continue;
        if (!looks_monospace(name)) continue;
        FontEntry *f = &g_fonts[g_font_count++];
        snprintf(f->path, sizeof(f->path), "%s/%s", dir, name);
        strncpy(f->name, name, sizeof(f->name) - 1);
        f->name[sizeof(f->name) - 1] = 0;
        int nl = (int)strlen(f->name);
        if (nl > 4) f->name[nl - 4] = 0;
    }
    closedir(dp);
#endif
}

/* Release every loaded preview-font texture. Called whenever we re-scan
   the font directories (each scan rebuilds the list from scratch). */
static void fonts_free_previews(void) {
    for (int i = 0; i < g_font_count; i++) {
        if (g_fonts[i].preview) {
            UnloadFont(*(Font *)g_fonts[i].preview);
            free(g_fonts[i].preview);
            g_fonts[i].preview = NULL;
        }
        g_fonts[i].load_failed = false;
    }
}

/* Scan rbterm's own bundled fonts directory in every plausible location
   — next to the binary, inside a macOS .app Resources folder, and
   relative to the current working directory for dev runs from the repo. */
static void scan_bundled_fonts(void) {
    const char *exe_dir = GetApplicationDirectory();
    if (exe_dir && *exe_dir) {
        char p[PATH_MAX];
        snprintf(p, sizeof(p), "%sfonts", exe_dir);         /* exe-side */
        scan_font_dir(p);
        snprintf(p, sizeof(p), "%sassets/fonts", exe_dir);  /* dev build dir */
        scan_font_dir(p);
        /* macOS .app: exe at Contents/MacOS/, fonts at Contents/Resources/fonts */
        snprintf(p, sizeof(p), "%s../Resources/fonts", exe_dir);
        scan_font_dir(p);
    }
    /* Relative to the current working directory — useful when running
       `./rbterm` straight out of the repo root. */
    scan_font_dir("assets/fonts");
    scan_font_dir("fonts");
#ifdef __EMSCRIPTEN__
    /* Emscripten --preload-file mounts everything at an absolute root. */
    scan_font_dir("/fonts");
#endif
}

static void fonts_load(const char *current_path) {
    fonts_free_previews();
    g_font_count = 0;
    g_font_list_scroll = 0;
    g_font_list_selected = -1;

    scan_bundled_fonts();
#ifdef _WIN32
    scan_font_dir("C:/Windows/Fonts");
    char user_fonts[1024];
    const char *up = getenv("LOCALAPPDATA");
    if (up && *up) {
        snprintf(user_fonts, sizeof(user_fonts), "%s/Microsoft/Windows/Fonts", up);
        scan_font_dir(user_fonts);
    }
#elif defined(__APPLE__)
    scan_font_dir("/System/Library/Fonts");
    scan_font_dir("/System/Library/Fonts/Supplemental");
    scan_font_dir("/Library/Fonts");
    char user_fonts[1024];
    expand_home_path("~/Library/Fonts", user_fonts, sizeof(user_fonts));
    scan_font_dir(user_fonts);
#else
    scan_font_dir("/usr/share/fonts");
    scan_font_dir("/usr/share/fonts/truetype/dejavu");
    scan_font_dir("/usr/share/fonts/truetype/liberation");
    scan_font_dir("/usr/share/fonts/TTF");
    scan_font_dir("/usr/local/share/fonts");
    char user_fonts[1024];
    expand_home_path("~/.local/share/fonts", user_fonts, sizeof(user_fonts));
    scan_font_dir(user_fonts);
    expand_home_path("~/.fonts", user_fonts, sizeof(user_fonts));
    scan_font_dir(user_fonts);
#endif
    qsort(g_fonts, g_font_count, sizeof(FontEntry), cmp_font_name);
    /* Mark the currently-loaded font if it's in the list. */
    if (current_path) {
        for (int i = 0; i < g_font_count; i++) {
            if (strcmp(g_fonts[i].path, current_path) == 0) {
                g_font_list_selected = i;
                break;
            }
        }
    }
}

static void settings_apply_font(Renderer *r, const FontEntry *fe) {
    if (!fe) return;
    if (!renderer_set_font_path(r, fe->path)) return;
    int nc = (GetScreenWidth()  - 2 * r->pad_x) / r->cell_w;
    int nr = (GetScreenHeight() - TAB_BAR_H - 2 * r->pad_y) / r->cell_h;
    if (nc < 1) nc = 1;
    if (nr < 1) nr = 1;
    for (int i = 0; i < g_num_tabs; i++) {
        screen_resize(g_tabs[i]->scr, nc, nr);
        pty_resize(g_tabs[i]->pty, nc, nr);
    }
    SetWindowMinSize(r->cell_w * 20 + 2 * r->pad_x,
                     r->cell_h * 5 + TAB_BAR_H + 2 * r->pad_y);
}

static void settings_apply_padding(Renderer *r, int new_pad) {
    if (new_pad < 0) new_pad = 0;
    if (new_pad > 64) new_pad = 64;
    r->pad_x = new_pad;
    r->pad_y = new_pad;
    int nc = (GetScreenWidth()  - 2 * r->pad_x) / r->cell_w;
    int nr = (GetScreenHeight() - TAB_BAR_H - 2 * r->pad_y) / r->cell_h;
    if (nc < 1) nc = 1;
    if (nr < 1) nr = 1;
    for (int i = 0; i < g_num_tabs; i++) {
        screen_resize(g_tabs[i]->scr, nc, nr);
        pty_resize(g_tabs[i]->pty, nc, nr);
    }
    SetWindowMinSize(r->cell_w * 20 + 2 * r->pad_x,
                     r->cell_h * 5 + TAB_BAR_H + 2 * r->pad_y);
}

static void settings_apply_spacing(Renderer *r, int new_extra) {
    renderer_set_cell_spacing(r, new_extra);
    int nc = (GetScreenWidth()  - 2 * r->pad_x) / r->cell_w;
    int nr = (GetScreenHeight() - TAB_BAR_H - 2 * r->pad_y) / r->cell_h;
    if (nc < 1) nc = 1;
    if (nr < 1) nr = 1;
    for (int i = 0; i < g_num_tabs; i++) {
        screen_resize(g_tabs[i]->scr, nc, nr);
        pty_resize(g_tabs[i]->pty, nc, nr);
    }
    SetWindowMinSize(r->cell_w * 20 + 2 * r->pad_x,
                     r->cell_h * 5 + TAB_BAR_H + 2 * r->pad_y);
}

static bool g_settings_dir_focus = false;
static bool g_settings_dir_sel_all = false;

static SettingsLayout settings_layout(int win_w, int win_h) {
    SettingsLayout L = {0};
    int w = 600, h = 580;
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

    int font_list_y = font_row_y + btn + 16;
    L.font_list = (Rect){ L.modal.x + 140, font_list_y, w - 140 - 22, 140 };

    int pad_row_y = font_list_y + L.font_list.h + 16;
    L.pad_val  = (Rect){ L.modal.x + w - 214, pad_row_y, 66, btn };
    L.pad_dec  = (Rect){ L.modal.x + w - 138, pad_row_y, btn, btn };
    L.pad_inc  = (Rect){ L.modal.x + w - 60,  pad_row_y, btn, btn };

    int spc_row_y = pad_row_y + btn + 10;
    L.spc_val  = (Rect){ L.modal.x + w - 214, spc_row_y, 66, btn };
    L.spc_dec  = (Rect){ L.modal.x + w - 138, spc_row_y, btn, btn };
    L.spc_inc  = (Rect){ L.modal.x + w - 60,  spc_row_y, btn, btn };

    int log_row1_y = spc_row_y + btn + 14;
    L.log_toggle = (Rect){ L.modal.x + w - 140, log_row1_y, 110, btn };

    int log_row2_y = log_row1_y + btn + 10;
    L.log_dir = (Rect){ L.modal.x + 140, log_row2_y, w - 140 - 22, btn };

    int close_w = 90, close_h = 32, save_def_w = 150;
    int row_y = L.modal.y + h - 22 - close_h;
    L.close = (Rect){ L.modal.x + w - 22 - close_w, row_y, close_w, close_h };
    L.save_default = (Rect){ L.close.x - 8 - save_def_w, row_y, save_def_w, close_h };
    return L;
}

static void settings_handle_mouse(Renderer *r, SettingsLayout L) {
    Vector2 mp = GetMousePosition();
    int mx = (int)mp.x, my = (int)mp.y;
    /* Wheel-scroll the font list whenever the pointer is over it. */
    if (rect_hit(L.font_list, mx, my)) {
        float wheel = GetMouseWheelMove();
        if (wheel != 0.0f) {
            g_font_list_scroll -= (int)(wheel * 3.0f);
            if (g_font_list_scroll < 0) g_font_list_scroll = 0;
        }
    }
    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) return;
    if (rect_hit(L.dec, mx, my))   { settings_apply_font_size(r, r->font_size - 1); return; }
    if (rect_hit(L.inc, mx, my))   { settings_apply_font_size(r, r->font_size + 1); return; }
    if (rect_hit(L.pad_dec, mx, my)) { settings_apply_padding(r, r->pad_x - 2); return; }
    if (rect_hit(L.pad_inc, mx, my)) { settings_apply_padding(r, r->pad_x + 2); return; }
    if (rect_hit(L.spc_dec, mx, my)) { settings_apply_spacing(r, r->cell_extra_w - 1); return; }
    if (rect_hit(L.spc_inc, mx, my)) { settings_apply_spacing(r, r->cell_extra_w + 1); return; }
    if (rect_hit(L.font_list, mx, my)) {
        int row_h = 22;
        int idx = (my - L.font_list.y) / row_h + g_font_list_scroll;
        if (idx >= 0 && idx < g_font_count) {
            g_font_list_selected = idx;
            settings_apply_font(r, &g_fonts[idx]);
        }
        return;
    }
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
    if (rect_hit(L.save_default, mx, my)) {
        if (config_save(r)) {
            snprintf(g_settings_status, sizeof(g_settings_status),
                     "Defaults saved. Next launch will use these settings.");
        } else {
            snprintf(g_settings_status, sizeof(g_settings_status),
                     "Failed to write ~/.config/rbterm/config.ini: %s",
                     strerror(errno));
        }
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

    /* Font family list. */
    DrawTextEx(*f, "Font",
               (Vector2){L.modal.x + 22, L.font_list.y + 6},
               14, 0, (Color){200, 205, 220, 255});
    DrawRectangle(L.font_list.x, L.font_list.y, L.font_list.w, L.font_list.h,
                  (Color){22, 25, 34, 255});
    DrawRectangleLines(L.font_list.x, L.font_list.y, L.font_list.w, L.font_list.h,
                       (Color){70, 74, 90, 255});
    if (g_font_count == 0) {
        DrawTextEx(*f, "(no monospace fonts found)",
                   (Vector2){L.font_list.x + 10, L.font_list.y + 10},
                   12, 0, (Color){140, 145, 160, 255});
    } else {
        int row_h = 22;
        int visible = L.font_list.h / row_h;
        int max_scroll = g_font_count - visible;
        if (max_scroll < 0) max_scroll = 0;
        if (g_font_list_scroll > max_scroll) g_font_list_scroll = max_scroll;
        if (g_font_list_scroll < 0) g_font_list_scroll = 0;
        BeginScissorMode(L.font_list.x + 2, L.font_list.y + 2,
                         L.font_list.w - 4, L.font_list.h - 4);
        for (int i = 0; i < g_font_count; i++) {
            int ry = L.font_list.y + (i - g_font_list_scroll) * row_h;
            if (ry + row_h < L.font_list.y || ry > L.font_list.y + L.font_list.h)
                continue;
            bool sel = (i == g_font_list_selected);
            if (sel) {
                DrawRectangle(L.font_list.x + 2, ry,
                              L.font_list.w - 4, row_h,
                              (Color){46, 62, 90, 220});
            }
            /* Lazy-load a small preview of the font the first time its
               row scrolls into view so each name renders in its own
               typeface. Failures are sticky so we don't retry each frame. */
            if (!g_fonts[i].preview && !g_fonts[i].load_failed) {
                Font fprev = LoadFontEx(g_fonts[i].path, 14, NULL, 0);
                if (fprev.texture.id == 0) {
                    g_fonts[i].load_failed = true;
                } else {
                    SetTextureFilter(fprev.texture, TEXTURE_FILTER_BILINEAR);
                    g_fonts[i].preview = malloc(sizeof(Font));
                    *(Font *)g_fonts[i].preview = fprev;
                }
            }
            Font *row_font = g_fonts[i].preview
                                ? (Font *)g_fonts[i].preview
                                : f;
            DrawTextEx(*row_font, g_fonts[i].name,
                       (Vector2){L.font_list.x + 10, ry + 4},
                       14, 0,
                       sel ? (Color){230, 232, 240, 255}
                           : (Color){200, 205, 220, 255});
        }
        EndScissorMode();
        if (g_font_count > visible) {
            int track_x = L.font_list.x + L.font_list.w - 5;
            int bar_h = L.font_list.h * visible / g_font_count;
            if (bar_h < 24) bar_h = 24;
            int bar_y = L.font_list.y +
                        (L.font_list.h - bar_h) * g_font_list_scroll /
                        (max_scroll > 0 ? max_scroll : 1);
            DrawRectangle(track_x, L.font_list.y, 3, L.font_list.h,
                          (Color){40, 45, 58, 255});
            DrawRectangle(track_x, bar_y, 3, bar_h,
                          (Color){110, 130, 170, 255});
        }
    }

    /* Padding row. */
    DrawTextEx(*f, "Padding",
               (Vector2){L.modal.x + 22, L.pad_val.y + 8},
               14, 0, (Color){200, 205, 220, 255});
    char pbuf[32];
    snprintf(pbuf, sizeof(pbuf), "%d", r->pad_x);
    DrawRectangle(L.pad_val.x, L.pad_val.y, L.pad_val.w, L.pad_val.h,
                  (Color){22, 25, 34, 255});
    DrawRectangleLines(L.pad_val.x, L.pad_val.y, L.pad_val.w, L.pad_val.h,
                       (Color){70, 74, 90, 255});
    Vector2 pvsz = MeasureTextEx(*f, pbuf, 16, 0);
    DrawTextEx(*f, pbuf,
               (Vector2){L.pad_val.x + (L.pad_val.w - pvsz.x) / 2,
                         L.pad_val.y + (L.pad_val.h - pvsz.y) / 2},
               16, 0, (Color){230, 232, 240, 255});
    DrawRectangle(L.pad_dec.x, L.pad_dec.y, L.pad_dec.w, L.pad_dec.h, (Color){46, 52, 70, 255});
    DrawRectangleLines(L.pad_dec.x, L.pad_dec.y, L.pad_dec.w, L.pad_dec.h, (Color){125, 207, 255, 180});
    DrawTextEx(*f, "-",
               (Vector2){L.pad_dec.x + (L.pad_dec.w - ms.x) / 2,
                         L.pad_dec.y + (L.pad_dec.h - ms.y) / 2},
               18, 0, (Color){230, 232, 240, 255});
    DrawRectangle(L.pad_inc.x, L.pad_inc.y, L.pad_inc.w, L.pad_inc.h, (Color){46, 52, 70, 255});
    DrawRectangleLines(L.pad_inc.x, L.pad_inc.y, L.pad_inc.w, L.pad_inc.h, (Color){125, 207, 255, 180});
    DrawTextEx(*f, "+",
               (Vector2){L.pad_inc.x + (L.pad_inc.w - ps.x) / 2,
                         L.pad_inc.y + (L.pad_inc.h - ps.y) / 2},
               18, 0, (Color){230, 232, 240, 255});

    /* Letter-spacing row. */
    DrawTextEx(*f, "Spacing",
               (Vector2){L.modal.x + 22, L.spc_val.y + 8},
               14, 0, (Color){200, 205, 220, 255});
    char sbuf[32];
    snprintf(sbuf, sizeof(sbuf), "%d", r->cell_extra_w);
    DrawRectangle(L.spc_val.x, L.spc_val.y, L.spc_val.w, L.spc_val.h,
                  (Color){22, 25, 34, 255});
    DrawRectangleLines(L.spc_val.x, L.spc_val.y, L.spc_val.w, L.spc_val.h,
                       (Color){70, 74, 90, 255});
    Vector2 svsz = MeasureTextEx(*f, sbuf, 16, 0);
    DrawTextEx(*f, sbuf,
               (Vector2){L.spc_val.x + (L.spc_val.w - svsz.x) / 2,
                         L.spc_val.y + (L.spc_val.h - svsz.y) / 2},
               16, 0, (Color){230, 232, 240, 255});
    DrawRectangle(L.spc_dec.x, L.spc_dec.y, L.spc_dec.w, L.spc_dec.h, (Color){46, 52, 70, 255});
    DrawRectangleLines(L.spc_dec.x, L.spc_dec.y, L.spc_dec.w, L.spc_dec.h, (Color){125, 207, 255, 180});
    DrawTextEx(*f, "-",
               (Vector2){L.spc_dec.x + (L.spc_dec.w - ms.x) / 2,
                         L.spc_dec.y + (L.spc_dec.h - ms.y) / 2},
               18, 0, (Color){230, 232, 240, 255});
    DrawRectangle(L.spc_inc.x, L.spc_inc.y, L.spc_inc.w, L.spc_inc.h, (Color){46, 52, 70, 255});
    DrawRectangleLines(L.spc_inc.x, L.spc_inc.y, L.spc_inc.w, L.spc_inc.h, (Color){125, 207, 255, 180});
    DrawTextEx(*f, "+",
               (Vector2){L.spc_inc.x + (L.spc_inc.w - ps.x) / 2,
                         L.spc_inc.y + (L.spc_inc.h - ps.y) / 2},
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

    /* Save-as-Default button. */
    DrawRectangle(L.save_default.x, L.save_default.y,
                  L.save_default.w, L.save_default.h,
                  (Color){48, 78, 58, 255});
    DrawRectangleLines(L.save_default.x, L.save_default.y,
                       L.save_default.w, L.save_default.h,
                       (Color){150, 220, 170, 200});
    Vector2 sdsz = MeasureTextEx(*f, "Save as Default", 13, 0);
    DrawTextEx(*f, "Save as Default",
               (Vector2){L.save_default.x + (L.save_default.w - sdsz.x) / 2,
                         L.save_default.y + (L.save_default.h - sdsz.y) / 2},
               13, 0, (Color){220, 240, 225, 255});

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

    /* Status line above the footer. */
    if (g_settings_status[0]) {
        DrawTextEx(*f, g_settings_status,
                   (Vector2){L.modal.x + 22, L.save_default.y - 22},
                   11, 0, (Color){140, 220, 160, 255});
    }

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
           "  --padding N     inset around the grid in pixels (default 6)\n"
           "  --opacity F     window opacity 0.2..1.0 (default 1.0)\n"
           "  --undecorated   hide the native title bar\n"
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
    int init_padding = 6;
    float init_opacity = 1.0f;
    bool init_undecorated = false;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(); return 0; }
        else if (!strcmp(argv[i], "--font") && i + 1 < argc) font_path = argv[++i];
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) font_size = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cols") && i + 1 < argc) init_cols = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--rows") && i + 1 < argc) init_rows = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--padding") && i + 1 < argc) init_padding = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--opacity") && i + 1 < argc) init_opacity = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--undecorated")) init_undecorated = true;
        else { fprintf(stderr, "unknown arg: %s\n", argv[i]); usage(); return 2; }
    }
    if (init_padding < 0) init_padding = 0;
    if (init_padding > 64) init_padding = 64;
    if (init_opacity < 0.2f) init_opacity = 0.2f;
    if (init_opacity > 1.0f) init_opacity = 1.0f;
    if (init_cols < 20) init_cols = 20;
    if (init_rows < 5)  init_rows = 5;

    app_settings_init();
    /* Apply ~/.config/rbterm/config.ini before we commit to font path,
       size, padding, etc. CLI flags the user passed on the command line
       still win — they were parsed before this. */
    config_load_into_defaults();
    if (g_persisted.has_font_path && !font_path) font_path = g_persisted.font_path;
    if (g_persisted.has_font_size) font_size = g_persisted.font_size;
    if (g_persisted.has_padding)   init_padding = g_persisted.padding;

#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

#ifdef __EMSCRIPTEN__
    /* Web demo: keep the window at a fixed size that matches the
       canvas CSS in web/shell.html. Both FLAG_WINDOW_RESIZABLE and
       FLAG_WINDOW_HIGHDPI cause raylib's EmscriptenResizeCallback to
       resize CORE.Window.screen to window.innerWidth/Height, while
       GLFW keeps mouse coords in canvas-CSS space (0..960). Layout
       rects computed from GetScreenWidth() then sit in one coord
       system, mouse clicks in another, and every button misses. */
    unsigned int cfg_flags = FLAG_VSYNC_HINT;
#else
    unsigned int cfg_flags = FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT
                           | FLAG_WINDOW_HIGHDPI;
#endif
    if (init_opacity < 0.999f) cfg_flags |= FLAG_WINDOW_TRANSPARENT;
    if (init_undecorated)      cfg_flags |= FLAG_WINDOW_UNDECORATED;
    SetConfigFlags(cfg_flags);
    SetTraceLogLevel(LOG_WARNING);
#ifdef __EMSCRIPTEN__
    /* Match the CSS size of the canvas in web/shell.html so that mouse
       event coordinates (which come through in canvas-element space)
       line up 1:1 with layout rects drawn with GetScreenWidth/Height.
       If these diverge, buttons like Cancel/+ visually land in one
       place but hit-test at another. */
    InitWindow(960, 560, "rbterm");
#else
    InitWindow(800, 500, "rbterm");
#endif
    SetExitKey(KEY_NULL);

    Renderer r;
    if (!renderer_init(&r, font_path, font_size)) {
        CloseWindow();
        return 1;
    }
    r.pad_x = init_padding;
    r.pad_y = init_padding;
    r.bg_alpha = init_opacity;
    if (g_persisted.has_spacing) renderer_set_cell_spacing(&r, g_persisted.spacing);

    int win_w = init_cols * r.cell_w + 2 * r.pad_x;
    int win_h = init_rows * r.cell_h + TAB_BAR_H + 2 * r.pad_y;

    /* Clamp the requested size to what fits on the current monitor
       (minus rough room for the menu bar + Dock), then centre — otherwise
       at 100x30 / 20 pt the default window is tall enough to land below
       the visible area on smaller laptops and you can't reach the bottom. */
    int mi = GetCurrentMonitor();
    int mw = GetMonitorWidth(mi);
    int mh = GetMonitorHeight(mi);
    if (mw > 0 && mh > 0) {
        int safe_w = mw - 40;
        int safe_h = mh - 120;              /* menu bar + Dock headroom */
        if (win_w > safe_w) {
            int nc = safe_w / r.cell_w;
            if (nc < 20) nc = 20;
            init_cols = nc;
            win_w = nc * r.cell_w;
        }
        if (win_h > safe_h) {
            int nr = (safe_h - TAB_BAR_H) / r.cell_h;
            if (nr < 10) nr = 10;
            init_rows = nr;
            win_h = nr * r.cell_h + TAB_BAR_H;
        }
    }

    SetWindowSize(win_w, win_h);
    SetWindowMinSize(r.cell_w * 20 + 2 * r.pad_x, r.cell_h * 5 + TAB_BAR_H + 2 * r.pad_y);

    if (mw > 0 && mh > 0) {
        Vector2 mp = GetMonitorPosition(mi);
        int x = (int)mp.x + (mw - win_w) / 2;
        int y = (int)mp.y + (mh - win_h) / 2;
        if (y < (int)mp.y + 40) y = (int)mp.y + 40; /* stay below menu bar */
        SetWindowPosition(x, y);
    }

    if (!tab_open(init_cols, init_rows)) {
        renderer_shutdown(&r);
        CloseWindow();
        return 1;
    }

    SetTargetFPS(60);

    uint8_t readbuf[65536];
    uint8_t inputbuf[4096];
    bool was_focused = true;
    int  prev_mouse_btn = -1;
    int  prev_mouse_col = -1, prev_mouse_row = -1;

    while (!WindowShouldClose() && g_num_tabs > 0) {
        int win_w_now = GetScreenWidth();
        int win_h_now = GetScreenHeight();

        int content_rows = (win_h_now - TAB_BAR_H - 2 * r.pad_y) / r.cell_h;
        int content_cols = (win_w_now - 2 * r.pad_x) / r.cell_w;
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
            int mcol = (int)((mp.x - r.pad_x) / r.cell_w);
            int mrow = (int)((mp.y - TAB_BAR_H - r.pad_y) / r.cell_h);
            int cmax = screen_cols(cur->scr) - 1;
            int rmax = screen_rows(cur->scr) - 1;
            if (mcol < 0) mcol = 0; if (mcol > cmax) mcol = cmax;
            if (mrow < 0) mrow = 0; if (mrow > rmax) mrow = rmax;

            /* Mouse reporting: when the app has asked for it (DECSET 1000,
               1002, 1003) and the user isn't holding Shift (the universal
               override that lets you still select text), we translate
               mouse events to byte reports on the PTY instead of using
               them for selection. */
            bool shift_held = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
            int  mmode = screen_mouse_mode(cur->scr);
            bool to_pty = (mmode != 0) && !shift_held;

            if (to_pty) {
                bool sgr = screen_mouse_sgr(cur->scr);
                int shf = shift_held ? 4 : 0;
                int alt = (IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT)) ? 8 : 0;
                int ctl = (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) ? 16 : 0;
                int mods = shf | alt | ctl;

                /* Helper to write one mouse report. */
                #define MOUSE_EMIT(btn, release, motion) do {                     \
                    int _b = (btn) + mods + ((motion) ? 32 : 0);                  \
                    char _buf[32];                                                \
                    int _n;                                                       \
                    if (sgr) {                                                    \
                        _n = snprintf(_buf, sizeof(_buf), "\x1b[<%d;%d;%d%c",     \
                                      _b, mcol + 1, mrow + 1,                    \
                                      (release) ? 'm' : 'M');                    \
                    } else {                                                      \
                        int _eb = (release) ? (3 + mods + ((motion) ? 32 : 0))    \
                                            : _b;                                 \
                        if (mcol + 33 > 255 || mrow + 33 > 255) break;            \
                        _n = 6;                                                   \
                        _buf[0] = 0x1b; _buf[1] = '['; _buf[2] = 'M';             \
                        _buf[3] = (char)(_eb + 32);                               \
                        _buf[4] = (char)(mcol + 1 + 32);                          \
                        _buf[5] = (char)(mrow + 1 + 32);                          \
                    }                                                             \
                    pty_write(cur->pty, (const uint8_t *)_buf, _n);               \
                } while (0)

                if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))   MOUSE_EMIT(0, false, false);
                if (IsMouseButtonPressed(MOUSE_BUTTON_MIDDLE)) MOUSE_EMIT(1, false, false);
                if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT))  MOUSE_EMIT(2, false, false);
                if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT))   MOUSE_EMIT(0, true, false);
                if (IsMouseButtonReleased(MOUSE_BUTTON_MIDDLE)) MOUSE_EMIT(1, true, false);
                if (IsMouseButtonReleased(MOUSE_BUTTON_RIGHT))  MOUSE_EMIT(2, true, false);
                if (mcol != prev_mouse_col || mrow != prev_mouse_row) {
                    int held = IsMouseButtonDown(MOUSE_BUTTON_LEFT)   ? 0
                             : IsMouseButtonDown(MOUSE_BUTTON_MIDDLE) ? 1
                             : IsMouseButtonDown(MOUSE_BUTTON_RIGHT)  ? 2 : -1;
                    if (mmode == 1003 || (mmode == 1002 && held >= 0)) {
                        int b = (held >= 0) ? held : 3;
                        MOUSE_EMIT(b, false, true);
                    }
                    prev_mouse_col = mcol; prev_mouse_row = mrow;
                }
                float wheel = GetMouseWheelMove();
                if (wheel != 0.0f) {
                    int b = (wheel > 0) ? 64 : 65;
                    MOUSE_EMIT(b, false, false);
                }
                /* Update button cache for 1002 motion tracking. */
                prev_mouse_btn = IsMouseButtonDown(MOUSE_BUTTON_LEFT) ? 0 :
                                 IsMouseButtonDown(MOUSE_BUTTON_MIDDLE) ? 1 :
                                 IsMouseButtonDown(MOUSE_BUTTON_RIGHT) ? 2 : -1;
                (void)prev_mouse_btn;
                #undef MOUSE_EMIT
            } else {
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
                settings_open(&r);
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
            if (t && *t) {
                if (screen_bracketed_paste(cur->scr)) {
                    pty_write(cur->pty, (const uint8_t *)"\x1b[200~", 6);
                    pty_write(cur->pty, (const uint8_t *)t, strlen(t));
                    pty_write(cur->pty, (const uint8_t *)"\x1b[201~", 6);
                } else {
                    pty_write(cur->pty, (const uint8_t *)t, strlen(t));
                }
            }
        }

        /* Focus events (DECSET 1004). Emit CSI I on gain, CSI O on loss. */
        {
            bool is_focused = IsWindowFocused();
            if (is_focused != was_focused) {
                was_focused = is_focused;
                if (cur && screen_focus_report(cur->scr)) {
                    pty_write(cur->pty,
                              is_focused ? (const uint8_t *)"\x1b[I"
                                         : (const uint8_t *)"\x1b[O",
                              3);
                }
            }
        }
        if (acts.font_delta != 100) {
            int old = r.font_size;
            int ns = (acts.font_delta == 0) ? 20
                    : old + (acts.font_delta > 0 ? 1 : -1);
            if (renderer_set_font_size(&r, ns)) {
                int nc = (GetScreenWidth() - 2 * r.pad_x) / r.cell_w;
                int nr = (GetScreenHeight() - TAB_BAR_H - 2 * r.pad_y) / r.cell_h;
                if (nc < 1) nc = 1;
                if (nr < 1) nr = 1;
                for (int i = 0; i < g_num_tabs; i++) {
                    screen_resize(g_tabs[i]->scr, nc, nr);
                    pty_resize(g_tabs[i]->pty, nc, nr);
                }
                SetWindowMinSize(r.cell_w * 20 + 2 * r.pad_x, r.cell_h * 5 + TAB_BAR_H + 2 * r.pad_y);
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
