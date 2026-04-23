#include "raylib.h"
#include "screen.h"
#include "render.h"
#include "input.h"
#include "theme.h"
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
  #include <unistd.h>    /* fork/exec/setsid/readlink */
#endif
#ifdef __APPLE__
  #include <mach-o/dyld.h>  /* _NSGetExecutablePath */
  void mac_disable_close_menu_item(void);   /* defined in emoji_mac.m */
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

/* ---------- Tabs + panes ---------- */

/* A Pane is one PTY/Screen/Selection — i.e. one running shell. Tabs can
   host either one Pane (no split) or two (vertical or horizontal split). */
typedef struct {
    Pty *pty;
    Screen *scr;
    Selection sel;
    char title[256];
    bool title_dirty;
    int click_count;
    double last_click_time;
    int last_click_col, last_click_row;
    char cwd[PATH_MAX];
    double cwd_poll_at;
    FILE *log_fp;                /* session log file, NULL when disabled */
    char  log_path[PATH_MAX];
} Pane;

typedef enum {
    SPLIT_NONE = 0,
    SPLIT_VERTICAL   = 1,   /* pane[0] left, pane[1] right   — splitter is a vertical line */
    SPLIT_HORIZONTAL = 2    /* pane[0] top,  pane[1] bottom  — splitter is a horizontal line */
} SplitMode;

typedef struct {
    Pane panes[2];
    int  num_panes;              /* 1 or 2 */
    int  active_pane;            /* 0 or 1 */
    SplitMode split;
    float split_ratio;           /* fraction of available extent given to panes[0] (0.15..0.85) */
    bool splitter_drag;
    bool dead;
    bool  is_ssh;
    char  ssh_target[256];       /* user@host[:port] for SSH tabs */
    /* Stashed SSH connect params so a split can re-dial the same host. */
    char  ssh_host[256];
    char  ssh_user[96];
    char  ssh_pass[256];
    char  ssh_key[PATH_MAX];
    int   ssh_port;
    /* Appearance tied to the SSH host (applied to every pane of this
       tab — splits inherit). Empty ssh_theme means "don't touch". */
    char  ssh_theme[64];
    int   ssh_cursor_style;
    char  ssh_font[PATH_MAX];  /* empty = no per-host override */
    int   ssh_font_size;       /* 0 = no override */
    char  ssh_log_dir[PATH_MAX];
    int   ssh_log_mode;        /* 0 = inherit, 1 = on, 2 = off */
} Tab;

#define MAX_TABS 16
#define TAB_BAR_H 30
#define TAB_MIN_W 100
#define TAB_MAX_W 240
#define TAB_CLOSE_W 22
#define TAB_PLUS_W  30
#define TAB_GEAR_W  30
#define TAB_HELP_W  28
#define TAB_SPLIT_W 28          /* one split button (two of them — vertical + horizontal) */
#define TAB_SSH_W   48

static Tab *g_tabs[MAX_TABS];
static int g_num_tabs = 0;
static int g_active = 0;

/* Modal SSH connection form. When non-NORMAL, the terminal is locked:
   keystrokes edit form fields and mouse clicks focus them instead of
   going to the active tab. Layout is computed on the fly in
   ssh_form_layout() so draw and hit-test share one source of truth. */
typedef enum { UI_NORMAL = 0, UI_SSH_FORM, UI_SETTINGS, UI_HELP } UiMode;
typedef enum {
    F_NAME = 0, F_HOST, F_PORT, F_USER, F_PASS, F_KEY,
    F_NEW, F_CONNECT, F_DELETE, F_SAVE, F_CANCEL,
    F_COUNT
} SshField;
#define F_TEXT_FIELDS 6    /* name, host, port, user, pass, key */
typedef struct {
    char name[128];
    char host[256];
    char port[16];
    char user[96];
    char pass[256];
    char theme[64];
    int  cursor_style;
    char font[PATH_MAX];     /* font path; empty = use default */
    int  font_size;          /* 0 = use default */
    char log_dir[PATH_MAX];
    int  log_mode;           /* 0 = inherit, 1 = on, 2 = off */
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
    /* rbterm-specific fields. Stored in ~/.ssh/config as comments
       (`# rbterm-theme:` / `# rbterm-cursor:` / `# rbterm-font:` /
       `# rbterm-font-size:`) so plain ssh still sees a clean stanza but
       rbterm picks them up. */
    char theme[64];
    int  cursor_style;     /* CursorStyle enum, 0 = default */
    char font[PATH_MAX];
    int  font_size;        /* 0 = default */
    char log_dir[PATH_MAX];
    /* 0 = inherit app setting, 1 = force on, 2 = force off. */
    int  log_mode;
} SshProfile;

#define SSH_PROFILES_MAX 128
static SshProfile g_ssh_profiles[SSH_PROFILES_MAX];
static int        g_ssh_profile_count = 0;
static int        g_ssh_list_scroll = 0;   /* in rows */
static int        g_ssh_list_selected = -1; /* highlighted row, -1 = none */

typedef struct {
    Rect modal;
    Rect list;                  /* saved-hosts sidebar */
    Rect field[F_TEXT_FIELDS];  /* name, host, port, user, pass, key */
    Rect theme_list;            /* scrollable per-host theme picker */
    Rect font_list;             /* scrollable per-host font picker */
    Rect fs_val;                /* font-size value + -/+ buttons */
    Rect fs_dec;
    Rect fs_inc;
    Rect cur_block;             /* cursor style picker: block / underline / bar / blink */
    Rect cur_under;
    Rect cur_bar;
    Rect cur_blink;
    Rect log_inherit;           /* per-host logging: inherit / on / off + dir */
    Rect log_on;
    Rect log_off;
    Rect log_dir;
    Rect newbtn;
    Rect connect;
    Rect delbtn;                /* zero-sized when not deletable */
    Rect save;
    Rect cancel;
} SshFormLayout;

/* Per-list scroll state for the SSH form (independent of the Settings
   modal's scrolls so each picker remembers its own position). */
static int g_form_theme_scroll = 0;
static int g_form_font_scroll  = 0;

/* True for one frame after the help modal opens, so the same click
   that triggered the open doesn't immediately dismiss it via the
   "click outside" check. */
static bool g_help_just_opened = false;

/* Which scrollable list owns Up/Down keyboard nav while the modal is
   open. Set when the user clicks a list row, cleared when clicking
   elsewhere or closing the modal. */
typedef enum { SETTINGS_FOCUS_NONE, SETTINGS_FOCUS_FONT, SETTINGS_FOCUS_THEME } SettingsListFocus;
static SettingsListFocus g_settings_focused_list = SETTINGS_FOCUS_NONE;
typedef enum { FORM_FOCUS_NONE, FORM_FOCUS_THEME, FORM_FOCUS_FONT } SshFormListFocus;
static SshFormListFocus g_form_focused_list = FORM_FOCUS_NONE;
/* Selected row in each picker (for SSH form). The Settings font list
   already has g_font_list_selected; the theme list has g_theme_list_selected.
   These two mirror them for the SSH form's lists, where selection
   reflects what's currently in g_form.font / g_form.theme. */
static int g_form_theme_idx = -1;       /* -1 = "(inherit default)" row */
static int g_form_font_idx  = -1;

/* True when the per-host log-dir text input is focused (so character
   keys edit the path instead of cycling fields). */
static bool g_form_logdir_focus = false;

/* Renderer is owned by main() but the SSH form and per-host connect
   path need to apply font / font-size globally when a host is selected. */
static Renderer *g_renderer = NULL;

/* Forward decl — definition lives down by the pane-layout code. */
static void tabs_resize_all(const Renderer *r, int win_w, int win_h);
/* Forward decl — definition near the Settings modal. */
static void list_scroll_to(int *scroll, int selected, int total, int row_h, int list_h);

/* Pull in the embedded fonts table. Mac/Linux gets the bytes via
   .incbin in fonts_embedded.S; Windows gets them via the .rc resource
   compiler + embedded_fonts_init() at startup. Wasm has no embed
   path — assets/fonts is preloaded into MEMFS instead. */
#if defined(PLATFORM_WEB) || defined(RBTERM_NO_EMBEDDED_FONTS)
typedef struct {
    const char *name;
    const char *ext;
    const unsigned char *data;
    unsigned int data_size;
} EmbeddedFont;
static const EmbeddedFont k_embedded_fonts[1] = { {"", "", 0, 0} };
static const int k_embedded_font_count = 0;
static inline void embedded_fonts_init(void) {}
#else
#include "fonts_embedded.h"
#endif
static const EmbeddedFont *embedded_font_lookup(const char *path_or_name);
/* font_preview_load is defined later (after FontEntry's full def) but
   the SSH form picker needs to call it; forward-declare it here. */
typedef struct FontEntry FontEntry;
static Font font_preview_load(const FontEntry *fe, int size);

/* Enumerated monospace fonts. Populated by scan_bundled_fonts() before
   any UI opens; read by both the SSH form's per-host font picker and
   the Settings modal's font picker. `data` is non-null for fonts
   embedded into the binary via tools/gen_fonts.sh — load them with
   LoadFontFromMemory; otherwise fall back to LoadFontEx(path). */
typedef struct FontEntry {
    char name[128];
    char path[512];        /* For embedded fonts: "embedded:<name>" sentinel. */
    void *preview;         /* Font *, loaded lazily when the row is first visible. */
    bool  load_failed;
    const unsigned char *data;
    unsigned int          data_size;
    char ext[8];           /* "ttf" / "otf" / "ttc" — needed for LoadFontFromMemory. */
} FontEntry;

#define MAX_FONTS 256
static FontEntry g_fonts[MAX_FONTS];
static int       g_font_count = 0;
static int       g_font_list_scroll = 0;
static int       g_font_list_selected = -1;

static char g_form_status[192];   /* positive status line (e.g. "saved") */

static Tab *active_tab(void) {
    if (g_num_tabs == 0) return NULL;
    if (g_active < 0) g_active = 0;
    if (g_active >= g_num_tabs) g_active = g_num_tabs - 1;
    return g_tabs[g_active];
}

/* Open a fresh per-pane log file under the current log directory. Silent
   on failure so the user's session isn't derailed by a bad path. SSH
   tabs may override the directory + on/off via ssh_log_dir +
   ssh_log_mode (1=force on, 2=force off). */
static void pane_log_open(Tab *t, Pane *p, int pane_idx) {
    if (!t || !p || p->log_fp) return;
    bool enabled = g_app_settings.log_enabled;
    if (t->ssh_log_mode == 1) enabled = true;
    else if (t->ssh_log_mode == 2) enabled = false;
    if (!enabled) return;
    const char *dir_pref = (t->ssh_log_dir[0]) ? t->ssh_log_dir
                                               : g_app_settings.log_dir;
    if (!dir_pref || !*dir_pref) return;
    char dir[PATH_MAX];
    expand_home_path(dir_pref, dir, sizeof(dir));
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
    if (t->num_panes > 1 || pane_idx > 0) {
        snprintf(p->log_path, sizeof(p->log_path),
                 "%s/rbterm-%s-tab%d-p%d.log", dir, stamp, slot, pane_idx);
    } else {
        snprintf(p->log_path, sizeof(p->log_path),
                 "%s/rbterm-%s-tab%d.log", dir, stamp, slot);
    }
    p->log_fp = fopen(p->log_path, "ab");
    if (!p->log_fp) {
        fprintf(stderr, "rbterm: can't open log %s: %s\n",
                p->log_path, strerror(errno));
        p->log_path[0] = 0;
    }
}

static void pane_log_close(Pane *p) {
    if (!p) return;
    if (p->log_fp) { fclose(p->log_fp); p->log_fp = NULL; }
}

static void pane_log_write(Pane *p, const uint8_t *buf, size_t n) {
    if (!p || !p->log_fp || n == 0) return;
    fwrite(buf, 1, n, p->log_fp);
    fflush(p->log_fp);
}

static void tab_log_open_all(Tab *t) {
    if (!t) return;
    for (int i = 0; i < t->num_panes; i++) pane_log_open(t, &t->panes[i], i);
}
static void tab_log_close_all(Tab *t) {
    if (!t) return;
    for (int i = 0; i < t->num_panes; i++) pane_log_close(&t->panes[i]);
}

/* (Re-)open logs on every pane in every tab based on the current
   setting. Called when the user toggles "log to file" in Settings. */
static void refresh_tab_logs(void) {
    for (int i = 0; i < g_num_tabs; i++) {
        if (g_app_settings.log_enabled) tab_log_open_all(g_tabs[i]);
        else                            tab_log_close_all(g_tabs[i]);
    }
}

/* ---------- IO glue: screen callbacks route to owning pane's PTY ---------- */

static void io_write_cb(void *u, const uint8_t *buf, size_t n) {
    Pane *p = (Pane *)u;
    pty_write(p->pty, buf, n);
}
static void io_set_title_cb(void *u, const char *title) {
    Pane *p = (Pane *)u;
    strncpy(p->title, title, sizeof(p->title) - 1);
    p->title[sizeof(p->title) - 1] = 0;
    p->title_dirty = true;
}
static void io_bell_cb(void *u) { (void)u; }
static void io_set_clipboard_cb(void *u, const char *utf8) {
    (void)u;
    if (utf8 && *utf8) SetClipboardText(utf8);
}
static void io_set_cwd_cb(void *u, const char *path) {
    Pane *p = (Pane *)u;
    if (!p || !path || !*path) return;
    strncpy(p->cwd, path, sizeof(p->cwd) - 1);
    p->cwd[sizeof(p->cwd) - 1] = 0;
    p->title_dirty = true;
}

static void pane_init_click_state(Pane *p) {
    p->last_click_time = -1.0;
    p->last_click_col = p->last_click_row = -1;
}

/* Build the ScreenIO for a pane (caller owns the Pane pointer for its lifetime). */
static ScreenIO pane_io(Pane *p) {
    ScreenIO io = { .user = p, .write = io_write_cb,
                    .set_title = io_set_title_cb, .bell = io_bell_cb,
                    .set_clipboard = io_set_clipboard_cb,
                    .set_cwd = io_set_cwd_cb };
    return io;
}

static bool pane_open_local(Pane *p, int cols, int rows) {
    pane_init_click_state(p);
    strncpy(p->title, "shell", sizeof(p->title) - 1);
    p->pty = pty_open(cols, rows);
    if (!p->pty) return false;
    p->scr = screen_new(cols, rows, 5000, pane_io(p));
    return true;
}

static bool pane_open_ssh(Pane *p, const char *user, const char *host, int port,
                          const char *password, const char *keyfile,
                          int cols, int rows, char *err, size_t errsz) {
    pane_init_click_state(p);
    p->pty = pty_open_ssh(user, host, port, password, keyfile,
                          cols, rows, err, errsz);
    if (!p->pty) return false;
    p->scr = screen_new(cols, rows, 5000, pane_io(p));
    return true;
}

static void pane_free(Pane *p) {
    if (!p) return;
    pane_log_close(p);
    if (p->pty) { pty_close(p->pty); p->pty = NULL; }
    if (p->scr) { screen_free(p->scr); p->scr = NULL; }
}

static Tab *tab_open(int cols, int rows) {
    if (g_num_tabs >= MAX_TABS) return NULL;
    Tab *t = calloc(1, sizeof(Tab));
    t->num_panes = 1;
    t->active_pane = 0;
    t->split = SPLIT_NONE;
    t->split_ratio = 0.5f;
    if (!pane_open_local(&t->panes[0], cols, rows)) { free(t); return NULL; }
    g_tabs[g_num_tabs] = t;
    g_active = g_num_tabs;
    g_num_tabs++;
    tab_log_open_all(t);
    return t;
}

static void pane_apply_tab_appearance(const Tab *t, Pane *p);

static Tab *tab_open_ssh(const char *user, const char *host, int port,
                         const char *password, const char *keyfile,
                         const char *theme, int cursor_style,
                         const char *font, int font_size,
                         const char *log_dir, int log_mode,
                         int cols, int rows,
                         char *err, size_t errsz) {
    if (g_num_tabs >= MAX_TABS) return NULL;
    Tab *t = calloc(1, sizeof(Tab));
    t->num_panes = 1;
    t->active_pane = 0;
    t->split = SPLIT_NONE;
    t->split_ratio = 0.5f;
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
    /* Stash connect params so a split can re-dial the same host. */
    if (host) { strncpy(t->ssh_host, host, sizeof(t->ssh_host) - 1); t->ssh_host[sizeof(t->ssh_host) - 1] = 0; }
    if (user) { strncpy(t->ssh_user, user, sizeof(t->ssh_user) - 1); t->ssh_user[sizeof(t->ssh_user) - 1] = 0; }
    if (password) { strncpy(t->ssh_pass, password, sizeof(t->ssh_pass) - 1); t->ssh_pass[sizeof(t->ssh_pass) - 1] = 0; }
    if (keyfile)  { strncpy(t->ssh_key,  keyfile,  sizeof(t->ssh_key) - 1);  t->ssh_key[sizeof(t->ssh_key) - 1] = 0; }
    if (theme)    { strncpy(t->ssh_theme, theme, sizeof(t->ssh_theme) - 1);  t->ssh_theme[sizeof(t->ssh_theme) - 1] = 0; }
    t->ssh_cursor_style = cursor_style;
    if (font)     { strncpy(t->ssh_font, font, sizeof(t->ssh_font) - 1); t->ssh_font[sizeof(t->ssh_font) - 1] = 0; }
    t->ssh_font_size = font_size;
    if (log_dir)  { strncpy(t->ssh_log_dir, log_dir, sizeof(t->ssh_log_dir) - 1); t->ssh_log_dir[sizeof(t->ssh_log_dir) - 1] = 0; }
    t->ssh_log_mode = log_mode;
    t->ssh_port = port;
    snprintf(t->panes[0].title, sizeof(t->panes[0].title), "%s", t->ssh_target);
    if (!pane_open_ssh(&t->panes[0], user, host, port, password, keyfile,
                       cols, rows, err, errsz)) {
        free(t); return NULL;
    }
    pane_apply_tab_appearance(t, &t->panes[0]);
    /* Per-host font / font-size applied globally when we connect. Windows
       runs a single renderer so this affects every pane for the rest of
       the session — the user can override via Settings. */
    if (g_renderer) {
        bool resized = false;
        if (t->ssh_font[0]) {
            const EmbeddedFont *ef = embedded_font_lookup(t->ssh_font);
            bool ok = false;
            if (ef) {
                ok = renderer_set_font_data(g_renderer, ef->data,
                                            (int)ef->data_size,
                                            ef->ext, t->ssh_font);
            } else {
                ok = renderer_set_font_path(g_renderer, t->ssh_font);
            }
            if (ok) resized = true;
        }
        if (t->ssh_font_size > 0) {
            if (renderer_set_font_size(g_renderer, t->ssh_font_size)) resized = true;
        }
        if (resized) {
            tabs_resize_all(g_renderer, GetScreenWidth(), GetScreenHeight());
            SetWindowMinSize(g_renderer->cell_w * 20 + 2 * g_renderer->pad_x,
                             g_renderer->cell_h * 5 + TAB_BAR_H + 2 * g_renderer->pad_y);
        }
    }
    g_tabs[g_num_tabs] = t;
    g_active = g_num_tabs;
    g_num_tabs++;
    tab_log_open_all(t);
    return t;
}

static void tab_close(int idx) {
    if (idx < 0 || idx >= g_num_tabs) return;
    Tab *t = g_tabs[idx];
    for (int i = 0; i < t->num_panes; i++) pane_free(&t->panes[i]);
    /* Scrub any stashed SSH credentials before freeing. */
    memset(t->ssh_pass, 0, sizeof(t->ssh_pass));
    free(t);
    for (int i = idx; i < g_num_tabs - 1; i++) g_tabs[i] = g_tabs[i + 1];
    g_tabs[g_num_tabs - 1] = NULL;
    g_num_tabs--;
    if (g_active >= g_num_tabs) g_active = g_num_tabs - 1;
    if (g_active < 0) g_active = 0;
}

static inline Pane *active_pane_of(Tab *t) {
    if (!t) return NULL;
    if (t->active_pane < 0 || t->active_pane >= t->num_panes) t->active_pane = 0;
    return &t->panes[t->active_pane];
}

/* Apply the tab's stashed appearance (theme + cursor style) to a fresh
   pane. Used by tab_open_ssh after the first pane opens, and by
   tab_split for the second pane so the split inherits the look. */
static void pane_apply_tab_appearance(const Tab *t, Pane *p) {
    if (!t || !p || !p->scr) return;
    if (t->ssh_theme[0]) {
        const Theme *th = theme_find_by_name(t->ssh_theme);
        if (th) screen_apply_theme(p->scr, th);
    }
    if (t->ssh_cursor_style != CURSOR_STYLE_DEFAULT)
        screen_set_cursor_style(p->scr, (CursorStyle)t->ssh_cursor_style);
}

/* Split the active tab. If the tab was opened via SSH, re-dial the same
   host into the new pane; otherwise open a local shell. No-op when the
   tab already has two panes. cols/rows are the current full-window cell
   dims; the pane's real size is resized to fit afterwards by the main
   loop's tabs_resize_all call. Returns true on success. */
static bool tab_split(Tab *t, SplitMode mode, int cols, int rows,
                      char *err, size_t errsz) {
    if (!t) return false;
    if (t->num_panes >= 2) return false;
    if (mode != SPLIT_VERTICAL && mode != SPLIT_HORIZONTAL) return false;
    Pane *np = &t->panes[1];
    memset(np, 0, sizeof(*np));
    bool ok;
    if (t->is_ssh) {
        ok = pane_open_ssh(np,
                           t->ssh_user[0] ? t->ssh_user : NULL,
                           t->ssh_host,
                           t->ssh_port > 0 ? t->ssh_port : 22,
                           t->ssh_pass[0] ? t->ssh_pass : NULL,
                           t->ssh_key[0]  ? t->ssh_key  : NULL,
                           cols, rows, err, errsz);
        if (ok) snprintf(np->title, sizeof(np->title), "%s", t->ssh_target);
    } else {
        ok = pane_open_local(np, cols, rows);
    }
    if (!ok) {
        memset(np, 0, sizeof(*np));
        return false;
    }
    pane_apply_tab_appearance(t, np);
    t->split = mode;
    t->split_ratio = 0.5f;
    t->num_panes = 2;
    t->active_pane = 1;
    pane_log_open(t, np, 1);
    return true;
}

/* Close one specific pane of a tab. If it's the only pane, mark the
   tab itself dead (caller collects via the tab_close sweep). */
static void tab_close_pane(Tab *t, int pane_idx) {
    if (!t || pane_idx < 0 || pane_idx >= t->num_panes) return;
    if (t->num_panes < 2) { t->dead = true; return; }
    pane_free(&t->panes[pane_idx]);
    /* Collapse: if we closed pane 0, shift pane 1 down into slot 0. */
    if (pane_idx == 0) {
        t->panes[0] = t->panes[1];
        /* Re-point the screen's io.user to the new Pane address. */
        if (t->panes[0].scr) screen_set_io_user(t->panes[0].scr, &t->panes[0]);
        memset(&t->panes[1], 0, sizeof(t->panes[1]));
    } else {
        memset(&t->panes[1], 0, sizeof(t->panes[1]));
    }
    t->num_panes = 1;
    t->active_pane = 0;
    t->split = SPLIT_NONE;
    t->splitter_drag = false;
    /* Force a retitle so the tab label and window title pick up the
       surviving pane. */
    t->panes[0].title_dirty = true;
}

/* Close the active pane. If the tab only has one pane, defer to
   tab_close for the whole tab. */
static void pane_close_active(int tab_idx) {
    if (tab_idx < 0 || tab_idx >= g_num_tabs) return;
    Tab *t = g_tabs[tab_idx];
    if (t->num_panes < 2) { tab_close(tab_idx); return; }
    tab_close_pane(t, t->active_pane);
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
    /* Derive from the *active* pane: that's whatever the user is
       currently looking at in that tab, split or not. */
    const Pane *p = &t->panes[t->active_pane];
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
        if (!p->cwd[0]) {
            return t->ssh_target;
        }
        /* Basename of the remote path, with "~" or "/" shortcuts. */
        const char *dir_label = p->cwd;
        const char *b = strrchr(p->cwd, '/');
        if (strcmp(p->cwd, "/") == 0) dir_label = "/";
        else if (b) dir_label = (*(b + 1) ? b + 1 : b);   /* trailing slash → "/" */
        snprintf(out, sizeof(buf[0]), "%s %s", t->ssh_target, dir_label);
        return out;
    }
    if (p->cwd[0]) {
        const char *home = getenv("HOME");
#ifdef _WIN32
        if (!home || !*home) home = getenv("USERPROFILE");
#endif
        if (home && *home) {
            size_t hn = strlen(home);
            if (strncmp(p->cwd, home, hn) == 0 &&
                (p->cwd[hn] == 0 || p->cwd[hn] == '/' || p->cwd[hn] == '\\')) {
                if (p->cwd[hn] == 0) return "~";
            }
        }
        if (strcmp(p->cwd, "/") == 0) return "/";
        const char *b1 = strrchr(p->cwd, '/');
        const char *b2 = strrchr(p->cwd, '\\');
        const char *base = (b1 && b2) ? (b1 > b2 ? b1 : b2) : (b1 ? b1 : b2);
        if (base) return base + 1;
        return p->cwd;
    }
    return p->title[0] ? p->title : "shell";
}

/* ---------- Pane layout ---------- */

/* One pane's rectangle within the terminal area below the tab bar, in
   window-pixel coords. For an unsplit tab, pane 0 fills the whole area.
   For a split, the splitter itself is SPLITTER_PX thick and sits
   between the two panes. */
/* Visual thickness of the splitter line. Kept thin so split panes
   feel adjacent; the drag hit-test below pads it with SPLITTER_GRAB
   so it's still easy to grab. */
#define SPLITTER_PX   2
#define SPLITTER_GRAB 6

typedef struct {
    int x, y, w, h;
} PaneRect;

static void pane_rect(const Tab *t, int pane_idx, int win_w, int win_h,
                      PaneRect *out) {
    int top = TAB_BAR_H;
    int area_x = 0;
    int area_y = top;
    int area_w = win_w;
    int area_h = win_h - top;
    if (area_h < 0) area_h = 0;
    if (t->split == SPLIT_NONE || t->num_panes < 2) {
        out->x = area_x; out->y = area_y;
        out->w = area_w; out->h = area_h;
        return;
    }
    float ratio = t->split_ratio;
    if (ratio < 0.15f) ratio = 0.15f;
    if (ratio > 0.85f) ratio = 0.85f;
    if (t->split == SPLIT_VERTICAL) {
        int half = SPLITTER_PX / 2;
        int left_w = (int)((area_w - SPLITTER_PX) * ratio);
        if (pane_idx == 0) {
            out->x = area_x; out->y = area_y;
            out->w = left_w; out->h = area_h;
        } else {
            out->x = area_x + left_w + SPLITTER_PX;
            out->y = area_y;
            out->w = area_w - left_w - SPLITTER_PX;
            out->h = area_h;
        }
        (void)half;
    } else { /* SPLIT_HORIZONTAL */
        int top_h = (int)((area_h - SPLITTER_PX) * ratio);
        if (pane_idx == 0) {
            out->x = area_x; out->y = area_y;
            out->w = area_w; out->h = top_h;
        } else {
            out->x = area_x;
            out->y = area_y + top_h + SPLITTER_PX;
            out->w = area_w;
            out->h = area_h - top_h - SPLITTER_PX;
        }
    }
}

/* The splitter's own rectangle (the draggable strip between panes). */
static bool splitter_rect(const Tab *t, int win_w, int win_h, PaneRect *out) {
    if (t->split == SPLIT_NONE || t->num_panes < 2) return false;
    PaneRect a, b;
    pane_rect(t, 0, win_w, win_h, &a);
    pane_rect(t, 1, win_w, win_h, &b);
    if (t->split == SPLIT_VERTICAL) {
        out->x = a.x + a.w;
        out->y = a.y;
        out->w = SPLITTER_PX;
        out->h = a.h;
    } else {
        out->x = a.x;
        out->y = a.y + a.h;
        out->w = a.w;
        out->h = SPLITTER_PX;
    }
    return true;
}

/* Columns / rows that fit inside a pane rect, given the renderer's
   cell dimensions + padding. Guarantees >= 1. */
static void pane_dims(const Renderer *r, const PaneRect *pr,
                      int *cols_out, int *rows_out) {
    int c = (pr->w - 2 * r->pad_x) / r->cell_w;
    int rs = (pr->h - 2 * r->pad_y) / r->cell_h;
    if (c  < 1) c  = 1;
    if (rs < 1) rs = 1;
    *cols_out = c;
    *rows_out = rs;
}

/* Find which pane of the active tab a window-pixel coordinate falls
   into (or -1 if neither). Used for click-to-focus. */
static int pane_at(const Tab *t, int win_w, int win_h, int mx, int my) {
    if (!t) return -1;
    for (int i = 0; i < t->num_panes; i++) {
        PaneRect pr;
        pane_rect(t, i, win_w, win_h, &pr);
        if (mx >= pr.x && mx < pr.x + pr.w &&
            my >= pr.y && my < pr.y + pr.h)
            return i;
    }
    return -1;
}

/* Draw every pane of one tab (plus the splitter bar between them). */
static void draw_tab_contents(Renderer *r, Tab *t, int win_w, int win_h,
                              double time_sec, bool focused) {
    if (!t) return;
    for (int pi = 0; pi < t->num_panes; pi++) {
        Pane *p = &t->panes[pi];
        if (!p->scr) continue;
        PaneRect pr;
        pane_rect(t, pi, win_w, win_h, &pr);
        bool pane_focused = focused && (pi == t->active_pane);
        renderer_draw(r, p->scr, time_sec, pane_focused, &p->sel, pr.x, pr.y);
    }
    PaneRect sp;
    if (splitter_rect(t, win_w, win_h, &sp)) {
        DrawRectangle(sp.x, sp.y, sp.w, sp.h, (Color){60, 60, 75, 255});
    }
    (void)win_w; (void)win_h;
}

/* Resize every pane of every tab to fit its current pane rectangle.
   Called on window resize, split toggles, and font-size changes. */
static void tabs_resize_all(const Renderer *r, int win_w, int win_h) {
    for (int i = 0; i < g_num_tabs; i++) {
        Tab *t = g_tabs[i];
        for (int pi = 0; pi < t->num_panes; pi++) {
            Pane *p = &t->panes[pi];
            PaneRect pr;
            pane_rect(t, pi, win_w, win_h, &pr);
            int cols, rows;
            pane_dims(r, &pr, &cols, &rows);
            if (!p->scr) continue;
            if (screen_cols(p->scr) != cols || screen_rows(p->scr) != rows) {
                screen_resize(p->scr, cols, rows);
                pty_resize(p->pty, cols, rows);
            }
        }
    }
}

/* ---------- Tab bar UI ---------- */

typedef struct {
    int tab_idx;
    bool on_close;
    bool on_plus;
    bool on_ssh;
    bool on_gear;
    bool on_help;
    bool on_split_v;       /* side-by-side split button */
    bool on_split_h;       /* top/bottom split button */
} TabBarHit;

/* Split buttons are hidden when the active tab is already split — there's
   nothing useful clicking them would do, and the tabs reclaim the space. */
static bool split_buttons_visible(void) {
    if (g_active < 0 || g_active >= g_num_tabs) return true;
    Tab *t = g_tabs[g_active];
    return t && t->split == SPLIT_NONE;
}

static int tab_width_for(int win_w) {
    int split_w = split_buttons_visible() ? 2 * TAB_SPLIT_W : 0;
    int avail = win_w - TAB_PLUS_W - TAB_GEAR_W - TAB_HELP_W - split_w - TAB_SSH_W;
    if (g_num_tabs <= 0) return TAB_MIN_W;
    int w = avail / g_num_tabs;
    if (w > TAB_MAX_W) w = TAB_MAX_W;
    if (w < TAB_MIN_W) w = TAB_MIN_W;
    return w;
}

/* Layout: [ssh] | tab1 | tab2 | ... | [gear] [split-v] [split-h] [?] | [+]
   The split pair disappears entirely when the active tab is split. */
static TabBarHit tab_bar_hit_test(int win_w, int mx, int my) {
    TabBarHit h = { -1, false, false, false, false, false, false, false };
    if (my < 0 || my >= TAB_BAR_H) return h;
    bool show_splits = split_buttons_visible();
    int plus_x    = win_w - TAB_PLUS_W;
    int help_x    = plus_x - TAB_HELP_W;
    int split_h_x = show_splits ? help_x - TAB_SPLIT_W     : help_x;
    int split_v_x = show_splits ? split_h_x - TAB_SPLIT_W  : help_x;
    int gear_x    = split_v_x - TAB_GEAR_W;
    int tab_start = TAB_SSH_W;
    if (mx < TAB_SSH_W)     { h.on_ssh     = true; return h; }
    if (mx >= plus_x)       { h.on_plus    = true; return h; }
    if (mx >= help_x)       { h.on_help    = true; return h; }
    if (show_splits && mx >= split_h_x) { h.on_split_h = true; return h; }
    if (show_splits && mx >= split_v_x) { h.on_split_v = true; return h; }
    if (mx >= gear_x)       { h.on_gear    = true; return h; }
    int tw = tab_width_for(win_w);
    int idx = (mx - tab_start) / tw;
    if (idx < 0 || idx >= g_num_tabs) return h;
    h.tab_idx = idx;
    int tx = tab_start + idx * tw;
    if (mx >= tx + tw - TAB_CLOSE_W) h.on_close = true;
    return h;
}

/* Two small filled rectangles with a gap between them, in the layout
   that the resulting split would produce — horizontal pair for a
   vertical (side-by-side) split, stacked pair for a horizontal split. */
static void draw_split_icon(float cx, float cy, float size, bool vertical, Color c) {
    int w  = (int)(size * 0.82f);
    int h  = (int)(size * 0.66f);
    int gap = 2;
    int x0 = (int)(cx - w / 2.0f);
    int y0 = (int)(cy - h / 2.0f);
    if (vertical) {
        int half = (w - gap) / 2;
        DrawRectangle(x0, y0, half, h, c);
        DrawRectangle(x0 + half + gap, y0, w - half - gap, h, c);
    } else {
        int half = (h - gap) / 2;
        DrawRectangle(x0, y0, w, half, c);
        DrawRectangle(x0, y0 + half + gap, w, h - half - gap, c);
    }
}

static void draw_gear_icon(float cx, float cy, float size, Color c, Color hole_bg) {
    float r_body = size * 0.32f;
    float r_hole = size * 0.14f;
    float tw     = size * 0.20f;
    float th     = size * 0.22f;
    int teeth    = 8;
    for (int i = 0; i < teeth; i++) {
        float a = (float)i * (360.0f / (float)teeth);
        DrawRectanglePro(
            (Rectangle){ cx, cy, tw, th },
            (Vector2){ tw / 2.0f, r_body + th },
            a,
            c);
    }
    DrawCircle((int)cx, (int)cy, r_body, c);
    DrawCircle((int)cx, (int)cy, r_hole, hole_bg);
}

static void draw_tab_bar(Renderer *r, int win_w) {
    Color bar_bg = (Color){24, 24, 32, 255};
    DrawRectangle(0, 0, win_w, TAB_BAR_H, bar_bg);
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

    /* Help button (?) sits immediately left of the "+" button. */
    int help_x = plus_x - TAB_HELP_W;
    {
        Color help_bg = (Color){38, 48, 66, 255};
        DrawRectangle(help_x, 0, TAB_HELP_W, TAB_BAR_H, help_bg);
        DrawRectangleLines(help_x, 2, TAB_HELP_W - 1, TAB_BAR_H - 4,
                           (Color){125, 207, 255, 200});
        Vector2 hsz = MeasureTextEx(*f, "?", 18, 0);
        DrawTextEx(*f, "?",
                   (Vector2){ help_x + (TAB_HELP_W - hsz.x) / 2.0f,
                              (TAB_BAR_H - hsz.y) / 2.0f },
                   18, 0, (Color){220, 235, 255, 255});
    }

    /* Gear (settings) button — leftmost of the right-cluster (left of
       the split pair when shown, otherwise left of the help button). */
    int gear_x = split_buttons_visible() ? (help_x - 2 * TAB_SPLIT_W - TAB_GEAR_W)
                                         : (help_x - TAB_GEAR_W);
    Color gear_bg = (Color){38, 48, 66, 255};
    DrawRectangle(gear_x, 0, TAB_GEAR_W, TAB_BAR_H, gear_bg);
    DrawRectangleLines(gear_x, 2, TAB_GEAR_W - 1, TAB_BAR_H - 4,
                       (Color){125, 207, 255, 200});
    draw_gear_icon(gear_x + TAB_GEAR_W / 2.0f, TAB_BAR_H / 2.0f,
                   TAB_BAR_H * 0.55f, (Color){220, 235, 255, 255}, gear_bg);

    /* Split buttons — vertical (side-by-side) then horizontal (top/bottom),
       between gear and help. Hidden once the active tab is already
       split (clicking them would no-op). */
    if (split_buttons_visible()) {
        int split_v_x = gear_x + TAB_GEAR_W;
        int split_h_x = split_v_x + TAB_SPLIT_W;
        Color split_bg = (Color){38, 48, 66, 255};
        Color split_outline = (Color){125, 207, 255, 200};
        Color split_icon = (Color){220, 235, 255, 255};

        DrawRectangle(split_v_x, 0, TAB_SPLIT_W, TAB_BAR_H, split_bg);
        DrawRectangleLines(split_v_x, 2, TAB_SPLIT_W - 1, TAB_BAR_H - 4, split_outline);
        draw_split_icon(split_v_x + TAB_SPLIT_W / 2.0f, TAB_BAR_H / 2.0f,
                        TAB_BAR_H * 0.60f, true, split_icon);

        DrawRectangle(split_h_x, 0, TAB_SPLIT_W, TAB_BAR_H, split_bg);
        DrawRectangleLines(split_h_x, 2, TAB_SPLIT_W - 1, TAB_BAR_H - 4, split_outline);
        draw_split_icon(split_h_x + TAB_SPLIT_W / 2.0f, TAB_BAR_H / 2.0f,
                        TAB_BAR_H * 0.60f, false, split_icon);
    }

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
        if (*p == '\n' || *p == 0 || *p == '\r') continue;
        /* rbterm-specific comment directives — only meaningful inside
           a Host stanza (cur != NULL). Plain ssh ignores them. */
        if (*p == '#' && cur) {
            char *q = p + 1;
            while (*q == ' ' || *q == '\t') q++;
            const char *theme_pfx  = "rbterm-theme:";
            const char *cursor_pfx = "rbterm-cursor:";
            const char *font_pfx   = "rbterm-font:";
            const char *fsize_pfx  = "rbterm-font-size:";
            if (strncmp(q, theme_pfx, strlen(theme_pfx)) == 0) {
                q += strlen(theme_pfx);
                while (*q == ' ' || *q == '\t') q++;
                trim_end(q);
                strncpy(cur->theme, q, sizeof(cur->theme) - 1);
                cur->theme[sizeof(cur->theme) - 1] = 0;
            } else if (strncmp(q, cursor_pfx, strlen(cursor_pfx)) == 0) {
                q += strlen(cursor_pfx);
                while (*q == ' ' || *q == '\t') q++;
                trim_end(q);
                if      (!strcmp(q, "block"))      cur->cursor_style = CURSOR_STYLE_BLOCK;
                else if (!strcmp(q, "underline"))  cur->cursor_style = CURSOR_STYLE_UNDERLINE;
                else if (!strcmp(q, "bar") || !strcmp(q, "vertical"))
                                                   cur->cursor_style = CURSOR_STYLE_BAR;
                else if (!strcmp(q, "blink") || !strcmp(q, "block-blink"))
                                                   cur->cursor_style = CURSOR_STYLE_BLOCK_BLINK;
            } else if (strncmp(q, fsize_pfx, strlen(fsize_pfx)) == 0) {
                q += strlen(fsize_pfx);
                while (*q == ' ' || *q == '\t') q++;
                cur->font_size = atoi(q);
            } else if (strncmp(q, font_pfx, strlen(font_pfx)) == 0) {
                q += strlen(font_pfx);
                while (*q == ' ' || *q == '\t') q++;
                trim_end(q);
                strncpy(cur->font, q, sizeof(cur->font) - 1);
                cur->font[sizeof(cur->font) - 1] = 0;
            } else {
                const char *ldir_pfx = "rbterm-log-dir:";
                const char *log_pfx  = "rbterm-log:";
                if (strncmp(q, ldir_pfx, strlen(ldir_pfx)) == 0) {
                    q += strlen(ldir_pfx);
                    while (*q == ' ' || *q == '\t') q++;
                    trim_end(q);
                    strncpy(cur->log_dir, q, sizeof(cur->log_dir) - 1);
                    cur->log_dir[sizeof(cur->log_dir) - 1] = 0;
                } else if (strncmp(q, log_pfx, strlen(log_pfx)) == 0) {
                    q += strlen(log_pfx);
                    while (*q == ' ' || *q == '\t') q++;
                    trim_end(q);
                    if      (!strcmp(q, "on"))  cur->log_mode = 1;
                    else if (!strcmp(q, "off")) cur->log_mode = 2;
                }
            }
            continue;
        }
        if (*p == '#') continue;
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
    /* Name is the ssh_config alias ("Host <name>"); Host is the real
       destination (HostName). When a stanza has no HostName line, ssh
       treats the alias itself as the hostname, so mirror that here. */
    strncpy(g_form.name, prof->name, sizeof(g_form.name) - 1);
    g_form.name[sizeof(g_form.name) - 1] = 0;
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
    strncpy(g_form.theme, prof->theme, sizeof(g_form.theme) - 1);
    g_form.theme[sizeof(g_form.theme) - 1] = 0;
    g_form.cursor_style = prof->cursor_style;
    strncpy(g_form.font, prof->font, sizeof(g_form.font) - 1);
    g_form.font[sizeof(g_form.font) - 1] = 0;
    g_form.font_size = prof->font_size;
    strncpy(g_form.log_dir, prof->log_dir, sizeof(g_form.log_dir) - 1);
    g_form.log_dir[sizeof(g_form.log_dir) - 1] = 0;
    g_form.log_mode = prof->log_mode;
    g_form.sel_all = false;
    g_form.error[0] = 0;
    /* Sync the selection indices used by Up/Down nav so the cursor
       starts on the loaded host's choices rather than the top row. */
    g_form_theme_idx = -1;
    if (g_form.theme[0]) {
        for (int i = 0; i < themes_count(); i++) {
            if (strcmp(themes_all()[i].name, g_form.theme) == 0) {
                g_form_theme_idx = i; break;
            }
        }
    }
    g_form_font_idx = -1;
    if (g_form.font[0]) {
        for (int i = 0; i < g_font_count; i++) {
            if (strcmp(g_fonts[i].path, g_form.font) == 0) {
                g_form_font_idx = i; break;
            }
        }
    }
}

/* Blank every field and reset selection — used by the "New" button and
   the initial form open. Port and username keep their sensible defaults;
   theme/font/cursor/log fields reset to "inherit". */
static void ssh_form_clear(void) {
    memset(&g_form, 0, sizeof(g_form));
    strncpy(g_form.port, "22", sizeof(g_form.port) - 1);
    g_form_theme_scroll = 0;
    g_form_font_scroll  = 0;
    const char *u = getenv("USER");
#ifdef _WIN32
    if (!u || !*u) u = getenv("USERNAME");
#endif
    if (u && *u) {
        strncpy(g_form.user, u, sizeof(g_form.user) - 1);
        g_form.user[sizeof(g_form.user) - 1] = 0;
    }
    g_form.focus = F_NAME;
    g_ssh_list_selected = -1;
    g_form_status[0] = 0;
    g_form_logdir_focus = false;
    g_form_focused_list = FORM_FOCUS_NONE;
    g_form_theme_idx = -1;
    g_form_font_idx = -1;
}

static void fonts_load(const char *current_path);

static void ssh_form_open(void) {
    g_ui_mode = UI_SSH_FORM;
    ssh_form_clear();
    /* Refresh the saved-hosts list every time the modal opens so edits
       to ~/.ssh/config show up without restarting rbterm. */
    ssh_profiles_load();
    /* Ensure g_fonts is populated so the per-host font picker has
       entries even if Settings hasn't been opened yet. */
    if (g_font_count == 0)
        fonts_load(g_renderer ? g_renderer->font_path : NULL);
}

static char *form_buf(int field, size_t *cap) {
    switch (field) {
    case F_NAME:  *cap = sizeof(g_form.name);  return g_form.name;
    case F_HOST:  *cap = sizeof(g_form.host);  return g_form.host;
    case F_PORT:  *cap = sizeof(g_form.port);  return g_form.port;
    case F_USER:  *cap = sizeof(g_form.user);  return g_form.user;
    case F_PASS:  *cap = sizeof(g_form.pass);  return g_form.pass;
    case F_KEY:   *cap = sizeof(g_form.key);   return g_form.key;
    default: *cap = 0; return NULL;
    }
}

static SshFormLayout ssh_form_layout(int win_w, int win_h) {
    SshFormLayout L = {0};
    int w = 860, h = 780;
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

    /* Theme + font pickers sit side-by-side below the text fields. */
    int picker_h = 130;
    int picker_gap = 12;
    int picker_w = (field_w - picker_gap) / 2;
    int picker_y = y + 4;
    L.theme_list = (Rect){ field_x,                    picker_y, picker_w, picker_h };
    L.font_list  = (Rect){ field_x + picker_w + picker_gap, picker_y, picker_w, picker_h };

    /* Font-size row: number display + -/+ buttons below the pickers. */
    int fs_row_y = picker_y + picker_h + 10;
    int fs_btn = 28;
    L.fs_val = (Rect){ field_x,                fs_row_y, 66, fs_btn };
    L.fs_dec = (Rect){ field_x + 74,           fs_row_y, fs_btn, fs_btn };
    L.fs_inc = (Rect){ field_x + 74 + fs_btn + 6, fs_row_y, fs_btn, fs_btn };

    /* Cursor-style picker: four equal buttons below the font size. */
    int cur_row_y = fs_row_y + fs_btn + 10;
    {
        int btn_h_sty = 30;
        int gap_sty = 6;
        int bw = (field_w - 3 * gap_sty) / 4;
        L.cur_block = (Rect){ field_x,                           cur_row_y, bw, btn_h_sty };
        L.cur_under = (Rect){ field_x + (bw + gap_sty),           cur_row_y, bw, btn_h_sty };
        L.cur_bar   = (Rect){ field_x + 2 * (bw + gap_sty),       cur_row_y, bw, btn_h_sty };
        L.cur_blink = (Rect){ field_x + 3 * (bw + gap_sty),       cur_row_y, bw, btn_h_sty };
    }

    /* Logging row: 3-state pill (Inherit / On / Off) followed by a
       second row with the per-host log directory text input. */
    int log_row_y = cur_row_y + 30 + 10;
    {
        int log_btn_h = 28;
        int gap_log = 6;
        int bw = (field_w - 2 * gap_log) / 3;
        L.log_inherit = (Rect){ field_x,                       log_row_y, bw, log_btn_h };
        L.log_on      = (Rect){ field_x + (bw + gap_log),       log_row_y, bw, log_btn_h };
        L.log_off     = (Rect){ field_x + 2 * (bw + gap_log),   log_row_y, bw, log_btn_h };
    }
    int logdir_row_y = log_row_y + 28 + 8;
    L.log_dir = (Rect){ field_x, logdir_row_y, field_w, 28 };

    /* Buttons: [New] ... [Connect] [Delete] [Save] [Cancel].
       Delete has zero size when no saved host is selected so it can't
       receive clicks; everything else keeps its position. */
    int btn_h = 32;
    int btn_y = L.modal.y + L.modal.h - btn_h - pad - (g_form.error[0] ? 24 : 0);
    int new_w = 70, connect_w = 110, del_w = 80, save_w = 90, cancel_w = 96;
    int gap = 8;
    bool has_del = (g_ssh_list_selected >= 0 && g_form.name[0]);
    int right_edge = L.modal.x + L.modal.w - pad;
    L.cancel.w  = cancel_w;  L.cancel.h  = btn_h;
    L.save.w    = save_w;    L.save.h    = btn_h;
    L.connect.w = connect_w; L.connect.h = btn_h;
    L.newbtn.w  = new_w;     L.newbtn.h  = btn_h;
    L.delbtn.w  = has_del ? del_w : 0;  L.delbtn.h = btn_h;
    L.cancel.x  = right_edge - cancel_w;
    L.save.x    = L.cancel.x - gap - save_w;
    L.delbtn.x  = has_del ? (L.save.x - gap - del_w) : 0;
    L.connect.x = (has_del ? L.delbtn.x : L.save.x) - gap - connect_w;
    L.newbtn.x  = L.modal.x + pad + list_w + (list_w > 0 ? 14 : 0);
    /* "New" floats one row above the main button bar so it reads as a
       form-level reset, not a commit-style action. */
    L.newbtn.y  = btn_y - btn_h - 10;
    L.cancel.y = L.save.y = L.connect.y = L.delbtn.y = btn_y;
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
        g_form.theme[0] ? g_form.theme : NULL,
        g_form.cursor_style,
        g_form.font[0]    ? g_form.font    : NULL,
        g_form.font_size,
        g_form.log_dir[0] ? g_form.log_dir : NULL,
        g_form.log_mode,
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
    /* Skip F_DELETE while the button is hidden (no host selected). */
    bool can_del = (g_ssh_list_selected >= 0 && g_form.name[0]);
    for (int i = 0; i < F_COUNT; i++) {
        g_form.focus = (g_form.focus + delta + F_COUNT) % F_COUNT;
        if (g_form.focus == F_DELETE && !can_del) continue;
        break;
    }
    g_form.sel_all = false;
}

/* Write every directive the form owns (HostName / User / Port /
   IdentityFile + # rbterm-* comments) for the current g_form values.
   Shared by the append-new path and the rewrite-existing path. Lines
   are emitted with four-space indent so they read consistently with
   whatever the user or openssh wrote. */
static void emit_form_managed_lines(FILE *fp) {
    fprintf(fp, "    HostName %s\n", g_form.host);
    if (g_form.user[0])
        fprintf(fp, "    User %s\n", g_form.user);
    int port = atoi(g_form.port);
    if (port > 0 && port != 22)
        fprintf(fp, "    Port %d\n", port);
    if (g_form.key[0])
        fprintf(fp, "    IdentityFile %s\n", g_form.key);
    if (g_form.theme[0])
        fprintf(fp, "    # rbterm-theme: %s\n", g_form.theme);
    if (g_form.cursor_style != CURSOR_STYLE_DEFAULT) {
        const char *css = NULL;
        switch (g_form.cursor_style) {
        case CURSOR_STYLE_BLOCK:       css = "block"; break;
        case CURSOR_STYLE_UNDERLINE:   css = "underline"; break;
        case CURSOR_STYLE_BAR:         css = "bar"; break;
        case CURSOR_STYLE_BLOCK_BLINK: css = "blink"; break;
        default: break;
        }
        if (css) fprintf(fp, "    # rbterm-cursor: %s\n", css);
    }
    if (g_form.font[0])
        fprintf(fp, "    # rbterm-font: %s\n", g_form.font);
    if (g_form.font_size > 0)
        fprintf(fp, "    # rbterm-font-size: %d\n", g_form.font_size);
    if (g_form.log_dir[0])
        fprintf(fp, "    # rbterm-log-dir: %s\n", g_form.log_dir);
    if (g_form.log_mode == 1)
        fprintf(fp, "    # rbterm-log: on\n");
    else if (g_form.log_mode == 2)
        fprintf(fp, "    # rbterm-log: off\n");
}

/* Returns true if `line` (leading whitespace already skipped) is a
   directive we manage. Comparison is case-insensitive for directive
   keywords (per ssh_config(5)); our `# rbterm-*` comments are
   exact-match since we emit them ourselves. */
static bool line_is_managed_directive(const char *p) {
    if (ci_prefix(p, "hostname"))     return true;
    if (ci_prefix(p, "user"))         return true;
    if (ci_prefix(p, "port"))         return true;
    if (ci_prefix(p, "identityfile")) return true;
    if (*p == '#') {
        const char *q = p + 1;
        while (*q == ' ' || *q == '\t') q++;
        if (strncmp(q, "rbterm-",        7) == 0) return true;
    }
    return false;
}

/* Rewrite ~/.ssh/config so the `Host <name>` stanza reflects the
   form's current values. Unmanaged directives (anything outside our
   known set) are preserved verbatim in their original order. */
static bool ssh_form_update_in_config(const char *name) {
    char path[PATH_MAX];
    expand_home_path("~/.ssh/config", path, sizeof(path));
    FILE *in = fopen(path, "r");
    if (!in) {
        snprintf(g_form.error, sizeof(g_form.error),
                 "Can't open %s: %s", path, strerror(errno));
        return false;
    }
    char tmp_path[PATH_MAX];
    int np = snprintf(tmp_path, sizeof(tmp_path), "%s.rbterm.tmp", path);
    if (np <= 0 || np >= (int)sizeof(tmp_path)) {
        fclose(in);
        strncpy(g_form.error, "path too long", sizeof(g_form.error) - 1);
        return false;
    }
    FILE *out = fopen(tmp_path, "w");
    if (!out) {
        snprintf(g_form.error, sizeof(g_form.error),
                 "Can't open %s: %s", tmp_path, strerror(errno));
        fclose(in);
        return false;
    }

    bool in_target = false;
    bool emitted = false;
    bool found = false;
    char line[1024];
    while (fgets(line, sizeof(line), in)) {
        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        int k = ci_prefix(p, "host");
        if (k) {
            /* Entering a new stanza. If we were mid-target, flush our
               managed lines now so they come before the next Host
               directive. */
            if (in_target && !emitted) {
                emit_form_managed_lines(out);
                emitted = true;
            }
            const char *rest = p + k;
            while (*rest == ' ' || *rest == '\t' || *rest == '=') rest++;
            char alias[128]; size_t ni = 0;
            while (*rest && *rest != ' ' && *rest != '\t' &&
                   *rest != '\r' && *rest != '\n' && ni + 1 < sizeof(alias))
                alias[ni++] = *rest++;
            alias[ni] = 0;
            in_target = (strcmp(alias, name) == 0);
            if (in_target) { found = true; emitted = false; }
            fputs(line, out);
            continue;
        }
        if (!in_target) { fputs(line, out); continue; }
        /* Inside the target stanza: drop our managed lines, keep all
           others verbatim so custom directives the user hand-rolled
           survive. */
        if (line_is_managed_directive(p)) continue;
        fputs(line, out);
    }
    if (in_target && !emitted) emit_form_managed_lines(out);
    fclose(in);
    fclose(out);

    if (!found) {
        remove(tmp_path);
        snprintf(g_form.error, sizeof(g_form.error),
                 "No stanza named '%s' in %s", name, path);
        return false;
    }
    if (rename(tmp_path, path) != 0) {
        snprintf(g_form.error, sizeof(g_form.error),
                 "Can't replace %s: %s", path, strerror(errno));
        remove(tmp_path);
        return false;
    }
#ifndef _WIN32
    chmod(path, 0600);
#endif
    snprintf(g_form_status, sizeof(g_form_status),
             "Updated '%s' in %s", name, path);
    g_form.error[0] = 0;
    ssh_profiles_load();
    return true;
}

/* Append the form's current values as a new `Host` stanza, or, if the
   alias already exists, delegate to ssh_form_update_in_config which
   rewrites that stanza in place. */
static bool ssh_form_save_to_config(void) {
    if (!g_form.name[0]) {
        strncpy(g_form.error, "Name is required before saving",
                sizeof(g_form.error) - 1);
        g_form.focus = F_NAME;
        return false;
    }
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

    /* If the alias already exists, rewrite that stanza in place:
       preserve unmanaged lines verbatim, replace only the fields we
       own (HostName / User / Port / IdentityFile + our # rbterm-*
       comments). Blank form values cause the matching line to
       disappear, so the user can clear a field and re-save. */
    for (int i = 0; i < g_ssh_profile_count; i++) {
        if (strcmp(g_ssh_profiles[i].name, g_form.name) == 0) {
            return ssh_form_update_in_config(g_form.name);
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
    fprintf(fp, "Host %s\n", g_form.name);
    emit_form_managed_lines(fp);
    fclose(fp);
#ifndef _WIN32
    chmod(path, 0600);
#endif

    snprintf(g_form_status, sizeof(g_form_status),
             "Saved '%s' to %s", g_form.name, path);
    g_form.error[0] = 0;
    /* Refresh the sidebar so the new entry shows immediately. */
    ssh_profiles_load();
    return true;
}

/* Remove a Host stanza from ~/.ssh/config. Rewrites the file atomically
   via a temp sibling + rename so a crash mid-write can't corrupt the
   original. The block to drop runs from `Host <alias>` up to (but not
   including) the next top-level `Host` line, or EOF. Other stanzas +
   comments + global directives are preserved verbatim. */
static bool ssh_form_delete_from_config(const char *name) {
    if (!name || !*name) return false;
    char path[PATH_MAX];
    expand_home_path("~/.ssh/config", path, sizeof(path));
    FILE *in = fopen(path, "r");
    if (!in) {
        snprintf(g_form.error, sizeof(g_form.error),
                 "Can't open %s: %s", path, strerror(errno));
        return false;
    }
    char tmp_path[PATH_MAX];
    int np = snprintf(tmp_path, sizeof(tmp_path), "%s.rbterm.tmp", path);
    if (np <= 0 || np >= (int)sizeof(tmp_path)) {
        fclose(in);
        strncpy(g_form.error, "path too long", sizeof(g_form.error) - 1);
        return false;
    }
    FILE *out = fopen(tmp_path, "w");
    if (!out) {
        snprintf(g_form.error, sizeof(g_form.error),
                 "Can't open %s: %s", tmp_path, strerror(errno));
        fclose(in);
        return false;
    }
    char line[1024];
    bool skipping = false;
    bool found = false;
    while (fgets(line, sizeof(line), in)) {
        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        int k = ci_prefix(p, "host");
        if (k) {
            /* First token after Host is the alias. */
            const char *rest = skip_ws(p + k);
            char alias[128]; size_t ni = 0;
            while (*rest && *rest != ' ' && *rest != '\t' &&
                   *rest != '\r' && *rest != '\n' && ni + 1 < sizeof(alias))
                alias[ni++] = *rest++;
            alias[ni] = 0;
            if (strcmp(alias, name) == 0) {
                skipping = true;
                found = true;
                continue;
            }
            skipping = false;
        }
        if (!skipping) fputs(line, out);
    }
    fclose(in);
    fclose(out);
    if (!found) {
        remove(tmp_path);
        snprintf(g_form.error, sizeof(g_form.error),
                 "No stanza named '%s' in %s", name, path);
        return false;
    }
    if (rename(tmp_path, path) != 0) {
        snprintf(g_form.error, sizeof(g_form.error),
                 "Can't replace %s: %s", path, strerror(errno));
        remove(tmp_path);
        return false;
    }
#ifndef _WIN32
    chmod(path, 0600);
#endif
    snprintf(g_form_status, sizeof(g_form_status),
             "Deleted '%s' from %s", name, path);
    g_form.error[0] = 0;
    ssh_profiles_load();
    ssh_form_clear();
    return true;
}

static void ssh_form_handle_mouse(SshFormLayout L, int cols, int rows) {
    /* Wheel-scroll whichever list the pointer is over. */
    Vector2 mph = GetMousePosition();
    int mhx = (int)mph.x, mhy = (int)mph.y;
    if (L.list.w > 0 && rect_hit(L.list, mhx, mhy)) {
        float wheel = GetMouseWheelMove();
        if (wheel != 0.0f) {
            g_ssh_list_scroll -= (int)(wheel * 3.0f);
            if (g_ssh_list_scroll < 0) g_ssh_list_scroll = 0;
        }
    }
    if (rect_hit(L.theme_list, mhx, mhy)) {
        float wheel = GetMouseWheelMove();
        if (wheel != 0.0f) {
            g_form_theme_scroll -= (int)(wheel * 3.0f);
            if (g_form_theme_scroll < 0) g_form_theme_scroll = 0;
        }
    }
    if (rect_hit(L.font_list, mhx, mhy)) {
        float wheel = GetMouseWheelMove();
        if (wheel != 0.0f) {
            g_form_font_scroll -= (int)(wheel * 3.0f);
            if (g_form_font_scroll < 0) g_form_font_scroll = 0;
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
    /* Cursor-style buttons act as radio: clicking one sets the form's
       cursor_style. The choice takes effect on Connect (or Save). */
    if (rect_hit(L.cur_block, mx, my)) { g_form.cursor_style = CURSOR_STYLE_BLOCK;       return; }
    if (rect_hit(L.cur_under, mx, my)) { g_form.cursor_style = CURSOR_STYLE_UNDERLINE;   return; }
    if (rect_hit(L.cur_bar,   mx, my)) { g_form.cursor_style = CURSOR_STYLE_BAR;         return; }
    if (rect_hit(L.cur_blink, mx, my)) { g_form.cursor_style = CURSOR_STYLE_BLOCK_BLINK; return; }

    /* Per-host logging: tri-state (0 = inherit app default, 1 = force
       on, 2 = force off) and a directory text field. */
    if (rect_hit(L.log_inherit, mx, my)) { g_form.log_mode = 0; return; }
    if (rect_hit(L.log_on,      mx, my)) { g_form.log_mode = 1; return; }
    if (rect_hit(L.log_off,     mx, my)) { g_form.log_mode = 2; return; }
    if (rect_hit(L.log_dir,     mx, my)) {
        g_form_logdir_focus = true;
        return;
    }
    if (g_form_logdir_focus) g_form_logdir_focus = false;

    /* Per-host theme/font pickers. Empty-top-row blanks the selection.
       Click also focuses the list for keyboard nav (Up/Down). */
    if (rect_hit(L.theme_list, mx, my)) {
        int row_h = 22;
        int idx = (my - L.theme_list.y) / row_h + g_form_theme_scroll;
        if (idx == 0) {
            g_form.theme[0] = 0;
            g_form_theme_idx = -1;
        } else if (idx > 0 && idx - 1 < themes_count()) {
            const Theme *th = &themes_all()[idx - 1];
            strncpy(g_form.theme, th->name, sizeof(g_form.theme) - 1);
            g_form.theme[sizeof(g_form.theme) - 1] = 0;
            g_form_theme_idx = idx - 1;
        }
        g_form_focused_list = FORM_FOCUS_THEME;
        return;
    }
    if (rect_hit(L.font_list, mx, my)) {
        int row_h = 22;
        int idx = (my - L.font_list.y) / row_h + g_form_font_scroll;
        if (idx == 0) {
            g_form.font[0] = 0;
            g_form_font_idx = -1;
        } else if (idx > 0 && idx - 1 < g_font_count) {
            strncpy(g_form.font, g_fonts[idx - 1].path, sizeof(g_form.font) - 1);
            g_form.font[sizeof(g_form.font) - 1] = 0;
            g_form_font_idx = idx - 1;
        }
        g_form_focused_list = FORM_FOCUS_FONT;
        return;
    }
    /* Click anywhere else inside the form drops keyboard nav focus
       (so other shortcuts like Tab cycle through fields normally). */
    g_form_focused_list = FORM_FOCUS_NONE;
    if (rect_hit(L.fs_dec, mx, my)) {
        if (g_form.font_size == 0) g_form.font_size = 20; /* start from a sane default */
        g_form.font_size -= 1;
        if (g_form.font_size < 6) g_form.font_size = 6;
        return;
    }
    if (rect_hit(L.fs_inc, mx, my)) {
        if (g_form.font_size == 0) g_form.font_size = 20;
        g_form.font_size += 1;
        if (g_form.font_size > 96) g_form.font_size = 96;
        return;
    }
    if (rect_hit(L.fs_val, mx, my)) {
        /* Click on the number zeros it back to "inherit". */
        g_form.font_size = 0;
        return;
    }
    if (rect_hit(L.newbtn, mx, my)) {
        g_form.focus = F_NEW;
        ssh_form_clear();
        return;
    }
    if (rect_hit(L.connect, mx, my)) {
        g_form.focus = F_CONNECT;
        ssh_form_submit(cols, rows);
        return;
    }
    if (L.delbtn.w > 0 && rect_hit(L.delbtn, mx, my)) {
        g_form.focus = F_DELETE;
        ssh_form_delete_from_config(g_form.name);
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
    bool is_text_field = (g_form.focus >= F_NAME && g_form.focus <= F_KEY);

    if (IsKeyPressed(KEY_ESCAPE)) { g_ui_mode = UI_NORMAL; return; }

    /* Up/Down arrow navigation when a picker list has keyboard focus.
       Index of -1 represents the synthetic "(inherit default)" row at
       the top of the rendered list, so the visible row index is
       `idx + 1`. Auto-scroll keeps the selection inside the panel. */
    bool form_up   = IsKeyPressed(KEY_UP)   || IsKeyPressedRepeat(KEY_UP);
    bool form_down = IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN);
    if (g_form_focused_list == FORM_FOCUS_THEME && (form_up || form_down)) {
        int total = themes_count();
        int next = g_form_theme_idx + (form_up ? -1 : +1);
        if (next < -1) next = -1;
        if (next >= total) next = total - 1;
        g_form_theme_idx = next;
        if (next == -1) {
            g_form.theme[0] = 0;
        } else {
            strncpy(g_form.theme, themes_all()[next].name, sizeof(g_form.theme) - 1);
            g_form.theme[sizeof(g_form.theme) - 1] = 0;
        }
        list_scroll_to(&g_form_theme_scroll, g_form_theme_idx + 1,
                       total + 1, 22, L.theme_list.h);
        return;
    }
    if (g_form_focused_list == FORM_FOCUS_FONT && (form_up || form_down)) {
        int total = g_font_count;
        int next = g_form_font_idx + (form_up ? -1 : +1);
        if (next < -1) next = -1;
        if (next >= total) next = total - 1;
        g_form_font_idx = next;
        if (next == -1) {
            g_form.font[0] = 0;
        } else {
            strncpy(g_form.font, g_fonts[next].path, sizeof(g_form.font) - 1);
            g_form.font[sizeof(g_form.font) - 1] = 0;
        }
        list_scroll_to(&g_form_font_scroll, g_form_font_idx + 1,
                       total + 1, 22, L.font_list.h);
        return;
    }

    /* Per-host log-dir text input. Click-focused via the form's mouse
       handler; we own the keyboard while it has focus. */
    if (g_form_logdir_focus) {
        size_t len = strlen(g_form.log_dir);
        if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) {
            if (len > 0) g_form.log_dir[len - 1] = 0;
        }
        if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) ||
            IsKeyPressed(KEY_TAB)) {
            g_form_logdir_focus = false;
            return;
        }
        int cp;
        while ((cp = GetCharPressed()) != 0) {
            if (mod) continue;
            if (cp < 32 || cp >= 127) continue;
            if (len + 1 >= sizeof(g_form.log_dir)) continue;
            g_form.log_dir[len++] = (char)cp;
            g_form.log_dir[len] = 0;
        }
        return;
    }

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
        if (g_form.focus == F_NEW)    { ssh_form_clear(); return; }
        if (g_form.focus == F_DELETE) { ssh_form_delete_from_config(g_form.name); return; }
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

    /* Space on a button behaves as a click. */
    if (g_form.focus == F_NEW && IsKeyPressed(KEY_SPACE)) {
        ssh_form_clear(); return;
    }
    if (g_form.focus == F_CONNECT && IsKeyPressed(KEY_SPACE)) {
        ssh_form_submit(cols, rows); return;
    }
    if (g_form.focus == F_DELETE && IsKeyPressed(KEY_SPACE)) {
        ssh_form_delete_from_config(g_form.name); return;
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
        "Name", "Host", "Port", "Username", "Password", "Key file"
    };
    const char *hints[F_TEXT_FIELDS]  = {
        "(ssh_config alias, e.g. mia)", "example.com", "22", getenv("USER"),
        "(leave blank to use key)",
        "(default: ssh-agent + ~/.ssh/id_*)"
    };
    const char *values[F_TEXT_FIELDS] = {
        g_form.name, g_form.host, g_form.port, g_form.user, g_form.pass, g_form.key
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

    /* Theme picker. First row is a synthetic "(none)" to clear the
       override; real entries follow, each with an 8-colour swatch. */
    DrawTextEx(*f, "Theme",
               (Vector2){L.theme_list.x - 104, L.theme_list.y + 6},
               13, 0, (Color){180, 185, 200, 255});
    DrawRectangle(L.theme_list.x, L.theme_list.y, L.theme_list.w, L.theme_list.h,
                  (Color){22, 25, 34, 255});
    DrawRectangleLines(L.theme_list.x, L.theme_list.y, L.theme_list.w, L.theme_list.h,
                       (Color){70, 74, 90, 255});
    {
        int row_h = 22;
        int total = 1 + themes_count();   /* +1 for "(none)" */
        int visible = L.theme_list.h / row_h;
        int max_scroll = total - visible;
        if (max_scroll < 0) max_scroll = 0;
        if (g_form_theme_scroll > max_scroll) g_form_theme_scroll = max_scroll;
        if (g_form_theme_scroll < 0) g_form_theme_scroll = 0;
        BeginScissorMode(L.theme_list.x + 2, L.theme_list.y + 2,
                         L.theme_list.w - 4, L.theme_list.h - 4);
        const Theme *ts = themes_all();
        for (int i = 0; i < total; i++) {
            int ry = L.theme_list.y + (i - g_form_theme_scroll) * row_h;
            if (ry + row_h < L.theme_list.y ||
                ry > L.theme_list.y + L.theme_list.h) continue;
            bool sel = (i == 0 && !g_form.theme[0]) ||
                       (i > 0 && strcmp(ts[i-1].name, g_form.theme) == 0);
            if (sel) {
                DrawRectangle(L.theme_list.x + 2, ry,
                              L.theme_list.w - 4, row_h,
                              (Color){46, 62, 90, 220});
            }
            if (i == 0) {
                DrawTextEx(*f, "(inherit default)",
                           (Vector2){L.theme_list.x + 10, ry + 4},
                           13, 0, (Color){150, 155, 170, 255});
            } else {
                int swatch_w = 8, swatch_h = 11, swatch_gap = 1;
                int swatches_w = 8 * (swatch_w + swatch_gap);
                int sx = L.theme_list.x + L.theme_list.w - 6 - swatches_w;
                int sy = ry + (row_h - swatch_h) / 2;
                for (int k = 0; k < 8; k++) {
                    uint32_t c = ts[i-1].palette[k];
                    Color col = { (unsigned char)((c >> 16) & 0xff),
                                  (unsigned char)((c >> 8)  & 0xff),
                                  (unsigned char)( c        & 0xff), 255 };
                    DrawRectangle(sx + k * (swatch_w + swatch_gap), sy,
                                  swatch_w, swatch_h, col);
                }
                DrawTextEx(*f, ts[i-1].name,
                           (Vector2){L.theme_list.x + 10, ry + 4},
                           13, 0,
                           sel ? (Color){230, 232, 240, 255}
                               : (Color){200, 205, 220, 255});
            }
        }
        EndScissorMode();
        if (total > visible) {
            int track_x = L.theme_list.x + L.theme_list.w - 5;
            int bar_h = L.theme_list.h * visible / total;
            if (bar_h < 24) bar_h = 24;
            int bar_y = L.theme_list.y +
                        (L.theme_list.h - bar_h) * g_form_theme_scroll /
                        (max_scroll > 0 ? max_scroll : 1);
            DrawRectangle(track_x, L.theme_list.y, 3, L.theme_list.h,
                          (Color){40, 45, 58, 255});
            DrawRectangle(track_x, bar_y, 3, bar_h,
                          (Color){110, 130, 170, 255});
        }
    }

    /* Font picker — same layout pattern as the theme list. */
    DrawTextEx(*f, "Font",
               (Vector2){L.font_list.x, L.font_list.y - 18},
               11, 0, (Color){140, 145, 160, 255});
    DrawRectangle(L.font_list.x, L.font_list.y, L.font_list.w, L.font_list.h,
                  (Color){22, 25, 34, 255});
    DrawRectangleLines(L.font_list.x, L.font_list.y, L.font_list.w, L.font_list.h,
                       (Color){70, 74, 90, 255});
    {
        int row_h = 22;
        int total = 1 + g_font_count;
        int visible = L.font_list.h / row_h;
        int max_scroll = total - visible;
        if (max_scroll < 0) max_scroll = 0;
        if (g_form_font_scroll > max_scroll) g_form_font_scroll = max_scroll;
        if (g_form_font_scroll < 0) g_form_font_scroll = 0;
        BeginScissorMode(L.font_list.x + 2, L.font_list.y + 2,
                         L.font_list.w - 4, L.font_list.h - 4);
        for (int i = 0; i < total; i++) {
            int ry = L.font_list.y + (i - g_form_font_scroll) * row_h;
            if (ry + row_h < L.font_list.y ||
                ry > L.font_list.y + L.font_list.h) continue;
            bool sel = (i == 0 && !g_form.font[0]) ||
                       (i > 0 && strcmp(g_fonts[i-1].path, g_form.font) == 0);
            if (sel) {
                DrawRectangle(L.font_list.x + 2, ry,
                              L.font_list.w - 4, row_h,
                              (Color){46, 62, 90, 220});
            }
            if (i == 0) {
                DrawTextEx(*f, "(inherit default)",
                           (Vector2){L.font_list.x + 10, ry + 4},
                           13, 0, (Color){150, 155, 170, 255});
            } else {
                /* Lazy-load a small preview the first time the row
                   scrolls into view so the name renders in its own
                   typeface. Failures are sticky so we don't retry
                   each frame. */
                FontEntry *fe = &g_fonts[i - 1];
                if (!fe->preview && !fe->load_failed) {
                    Font fprev = font_preview_load(fe, 14);
                    if (fprev.texture.id == 0) {
                        fe->load_failed = true;
                    } else {
                        SetTextureFilter(fprev.texture, TEXTURE_FILTER_BILINEAR);
                        fe->preview = malloc(sizeof(Font));
                        *(Font *)fe->preview = fprev;
                    }
                }
                Font *row_font = fe->preview ? (Font *)fe->preview : f;
                DrawTextEx(*row_font, fe->name,
                           (Vector2){L.font_list.x + 10, ry + 4},
                           14, 0,
                           sel ? (Color){230, 232, 240, 255}
                               : (Color){200, 205, 220, 255});
            }
        }
        EndScissorMode();
        if (total > visible) {
            int track_x = L.font_list.x + L.font_list.w - 5;
            int bar_h = L.font_list.h * visible / total;
            if (bar_h < 24) bar_h = 24;
            int bar_y = L.font_list.y +
                        (L.font_list.h - bar_h) * g_form_font_scroll /
                        (max_scroll > 0 ? max_scroll : 1);
            DrawRectangle(track_x, L.font_list.y, 3, L.font_list.h,
                          (Color){40, 45, 58, 255});
            DrawRectangle(track_x, bar_y, 3, bar_h,
                          (Color){110, 130, 170, 255});
        }
    }

    /* Font-size row. Zero means "inherit"; -/+ clamps to [6, 96]. */
    DrawTextEx(*f, "Font size",
               (Vector2){L.fs_val.x - 104, L.fs_val.y + 6},
               13, 0, (Color){180, 185, 200, 255});
    {
        char fsb[16];
        if (g_form.font_size > 0) snprintf(fsb, sizeof(fsb), "%d", g_form.font_size);
        else                      snprintf(fsb, sizeof(fsb), "(—)");
        DrawRectangle(L.fs_val.x, L.fs_val.y, L.fs_val.w, L.fs_val.h,
                      (Color){22, 25, 34, 255});
        DrawRectangleLines(L.fs_val.x, L.fs_val.y, L.fs_val.w, L.fs_val.h,
                           (Color){70, 74, 90, 255});
        Vector2 vsz2 = MeasureTextEx(*f, fsb, 14, 0);
        DrawTextEx(*f, fsb,
                   (Vector2){L.fs_val.x + (L.fs_val.w - vsz2.x) / 2,
                             L.fs_val.y + (L.fs_val.h - vsz2.y) / 2},
                   14, 0, (Color){230, 232, 240, 255});
        DrawRectangle(L.fs_dec.x, L.fs_dec.y, L.fs_dec.w, L.fs_dec.h, (Color){46, 52, 70, 255});
        DrawRectangleLines(L.fs_dec.x, L.fs_dec.y, L.fs_dec.w, L.fs_dec.h, (Color){125, 207, 255, 180});
        Vector2 mss = MeasureTextEx(*f, "-", 18, 0);
        DrawTextEx(*f, "-",
                   (Vector2){L.fs_dec.x + (L.fs_dec.w - mss.x) / 2,
                             L.fs_dec.y + (L.fs_dec.h - mss.y) / 2},
                   18, 0, (Color){230, 232, 240, 255});
        DrawRectangle(L.fs_inc.x, L.fs_inc.y, L.fs_inc.w, L.fs_inc.h, (Color){46, 52, 70, 255});
        DrawRectangleLines(L.fs_inc.x, L.fs_inc.y, L.fs_inc.w, L.fs_inc.h, (Color){125, 207, 255, 180});
        Vector2 pss = MeasureTextEx(*f, "+", 18, 0);
        DrawTextEx(*f, "+",
                   (Vector2){L.fs_inc.x + (L.fs_inc.w - pss.x) / 2,
                             L.fs_inc.y + (L.fs_inc.h - pss.y) / 2},
                   18, 0, (Color){230, 232, 240, 255});
    }

    /* Cursor-style picker row. Each of the four buttons is a tiny
       button with a text label; the currently-selected one fills in
       blue. */
    {
        struct { Rect r; const char *label; int style; } opts[] = {
            { L.cur_block, "Block",     CURSOR_STYLE_BLOCK },
            { L.cur_under, "Underline", CURSOR_STYLE_UNDERLINE },
            { L.cur_bar,   "Vertical",  CURSOR_STYLE_BAR },
            { L.cur_blink, "Blink",     CURSOR_STYLE_BLOCK_BLINK },
        };
        DrawTextEx(*f, "Cursor",
                   (Vector2){L.cur_block.x - 104, L.cur_block.y + 7},
                   13, 0, (Color){180, 185, 200, 255});
        for (int i = 0; i < 4; i++) {
            Rect rr = opts[i].r;
            bool on = (g_form.cursor_style == opts[i].style);
            DrawRectangle(rr.x, rr.y, rr.w, rr.h,
                          on ? (Color){46, 92, 150, 255} : (Color){34, 38, 52, 255});
            DrawRectangleLines(rr.x, rr.y, rr.w, rr.h,
                               (Color){125, 207, 255, on ? 255 : 150});
            Vector2 tsz = MeasureTextEx(*f, opts[i].label, 13, 0);
            DrawTextEx(*f, opts[i].label,
                       (Vector2){rr.x + (rr.w - tsz.x) / 2,
                                 rr.y + (rr.h - tsz.y) / 2},
                       13, 0, (Color){230, 232, 240, 255});
        }
    }

    /* Logging tri-state pill — Inherit (use Settings → Log session),
       On (force on for this host), Off (force off). */
    {
        struct { Rect r; const char *label; int mode; } opts[] = {
            { L.log_inherit, "Inherit", 0 },
            { L.log_on,      "On",      1 },
            { L.log_off,     "Off",     2 },
        };
        DrawTextEx(*f, "Logging",
                   (Vector2){L.log_inherit.x - 104, L.log_inherit.y + 7},
                   13, 0, (Color){180, 185, 200, 255});
        for (int i = 0; i < 3; i++) {
            Rect rr = opts[i].r;
            bool on = (g_form.log_mode == opts[i].mode);
            DrawRectangle(rr.x, rr.y, rr.w, rr.h,
                          on ? (Color){46, 92, 150, 255} : (Color){34, 38, 52, 255});
            DrawRectangleLines(rr.x, rr.y, rr.w, rr.h,
                               (Color){125, 207, 255, on ? 255 : 150});
            Vector2 tsz = MeasureTextEx(*f, opts[i].label, 13, 0);
            DrawTextEx(*f, opts[i].label,
                       (Vector2){rr.x + (rr.w - tsz.x) / 2,
                                 rr.y + (rr.h - tsz.y) / 2},
                       13, 0, (Color){230, 232, 240, 255});
        }
    }

    /* Per-host log-directory text input. Empty = use Settings'
       global log_dir. Click to focus, type to edit. */
    {
        DrawTextEx(*f, "Log dir",
                   (Vector2){L.log_dir.x - 104, L.log_dir.y + 7},
                   13, 0, (Color){180, 185, 200, 255});
        DrawRectangle(L.log_dir.x, L.log_dir.y, L.log_dir.w, L.log_dir.h,
                      (Color){22, 25, 34, 255});
        DrawRectangleLines(L.log_dir.x, L.log_dir.y, L.log_dir.w, L.log_dir.h,
                           g_form_logdir_focus ? (Color){125, 207, 255, 255}
                                               : (Color){70, 74, 90, 255});
        const char *shown = g_form.log_dir[0]
                            ? g_form.log_dir
                            : "(blank = use Settings → Log session dir)";
        Color tc = g_form.log_dir[0]
                   ? (Color){230, 232, 240, 255}
                   : (Color){110, 115, 130, 255};
        BeginScissorMode(L.log_dir.x + 6, L.log_dir.y,
                         L.log_dir.w - 12, L.log_dir.h);
        DrawTextEx(*f, shown,
                   (Vector2){L.log_dir.x + 8, L.log_dir.y + 7},
                   14, 0, tc);
        if (g_form_logdir_focus && g_form.log_dir[0] &&
            ((long long)(GetTime() * 2.0) & 1) == 0) {
            Vector2 vsz = MeasureTextEx(*f, g_form.log_dir, 14, 0);
            DrawRectangle(L.log_dir.x + 8 + (int)vsz.x + 1,
                          L.log_dir.y + 6, 8, 16,
                          (Color){125, 207, 255, 255});
        }
        EndScissorMode();
    }

    /* Buttons. */
    bool nf = g_form.focus == F_NEW;
    DrawRectangle(L.newbtn.x, L.newbtn.y, L.newbtn.w, L.newbtn.h,
                  nf ? (Color){72, 76, 96, 255} : (Color){48, 52, 66, 255});
    DrawRectangleLines(L.newbtn.x, L.newbtn.y, L.newbtn.w, L.newbtn.h,
                       (Color){150, 155, 170, nf ? 255 : 150});
    Vector2 nsz = MeasureTextEx(*f, "New", 14, 0);
    DrawTextEx(*f, "New",
               (Vector2){L.newbtn.x + (L.newbtn.w - nsz.x) / 2,
                         L.newbtn.y + (L.newbtn.h - nsz.y) / 2},
               14, 0, (Color){210, 215, 230, 255});

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

    if (L.delbtn.w > 0) {
        bool df = g_form.focus == F_DELETE;
        DrawRectangle(L.delbtn.x, L.delbtn.y, L.delbtn.w, L.delbtn.h,
                      df ? (Color){170, 60, 60, 255} : (Color){110, 40, 40, 255});
        DrawRectangleLines(L.delbtn.x, L.delbtn.y, L.delbtn.w, L.delbtn.h,
                           (Color){240, 140, 140, df ? 255 : 180});
        Vector2 dsz = MeasureTextEx(*f, "Delete", 14, 0);
        DrawTextEx(*f, "Delete",
                   (Vector2){L.delbtn.x + (L.delbtn.w - dsz.x) / 2,
                             L.delbtn.y + (L.delbtn.h - dsz.y) / 2},
                   14, 0, (Color){250, 225, 225, 255});
    }

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
        tabs_resize_all(r, GetScreenWidth(), GetScreenHeight());
        SetWindowMinSize(r->cell_w * 20 + 2 * r->pad_x, r->cell_h * 5 + TAB_BAR_H + 2 * r->pad_y);
    }
}

typedef struct {
    Rect modal;
    Rect font_val;   /* current font size display */
    Rect dec, inc;   /* font -/+ buttons */
    Rect font_list;  /* scrollable list of monospace fonts */
    Rect theme_list; /* scrollable list of palette themes */
    Rect cur_block;  /* cursor-style picker: block / underline / bar / blink */
    Rect cur_under;
    Rect cur_bar;
    Rect cur_blink;
    Rect pad_val;
    Rect pad_dec, pad_inc;
    Rect spc_val;
    Rect spc_dec, spc_inc;
    Rect log_toggle; /* on/off button */
    Rect log_dir;    /* editable text box with log directory */
    Rect save_default; /* write current state to ~/.config/rbterm/config.ini */
    Rect close;
} SettingsLayout;

/* Theme picker state — modal-local since the picker is transient UI. */
static int g_theme_list_scroll = 0;
static int g_theme_list_selected = -1;

/* Available-fonts enumeration for the settings modal. Scans system + user
   font directories for monospace .ttf / .otf / .ttc files; only those whose
   filename hints at a monospace face (contains "Mono", "Code", "Fira", etc.)
   are included — scrolling through every font on disk would be noisy.
   FontEntry + g_fonts + scroll state are declared near the top of the
   file so the SSH form can read the same list for its per-host picker. */

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
        /* Skip duplicates by display name — the same font often shows
           up in both ~/Library/Fonts and the bundled assets/fonts. */
        char trimmed_w[256];
        strncpy(trimmed_w, name, sizeof(trimmed_w) - 1);
        trimmed_w[sizeof(trimmed_w) - 1] = 0;
        int tlw = (int)strlen(trimmed_w);
        if (tlw > 4 && (ends_with_ci(trimmed_w, ".ttf") ||
                        ends_with_ci(trimmed_w, ".otf") ||
                        ends_with_ci(trimmed_w, ".ttc"))) {
            trimmed_w[tlw - 4] = 0;
            tlw -= 4;
        }
        /* Strip the -Regular suffix so disk + embedded entries dedup
           on the same display name. */
        if (tlw > 8 && strcmp(trimmed_w + tlw - 8, "-Regular") == 0)
            trimmed_w[tlw - 8] = 0;
        bool dup_w = false;
        for (int j = 0; j < g_font_count; j++)
            if (strcmp(g_fonts[j].name, trimmed_w) == 0) { dup_w = true; break; }
        if (dup_w) continue;
        FontEntry *f = &g_fonts[g_font_count++];
        snprintf(f->path, sizeof(f->path), "%s/%s", dir, name);
        strncpy(f->name, trimmed_w, sizeof(f->name) - 1);
        f->name[sizeof(f->name) - 1] = 0;
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
        /* Skip duplicates by display name. */
        char trimmed[256];
        strncpy(trimmed, name, sizeof(trimmed) - 1);
        trimmed[sizeof(trimmed) - 1] = 0;
        int tl = (int)strlen(trimmed);
        if (tl > 4) { trimmed[tl - 4] = 0; tl -= 4; }
        if (tl > 8 && strcmp(trimmed + tl - 8, "-Regular") == 0)
            trimmed[tl - 8] = 0;
        bool dup = false;
        for (int j = 0; j < g_font_count; j++)
            if (strcmp(g_fonts[j].name, trimmed) == 0) { dup = true; break; }
        if (dup) continue;
        FontEntry *f = &g_fonts[g_font_count++];
        snprintf(f->path, sizeof(f->path), "%s/%s", dir, name);
        strncpy(f->name, trimmed, sizeof(f->name) - 1);
        f->name[sizeof(f->name) - 1] = 0;
    }
    closedir(dp);
#endif
}

/* Load a small preview Font for one entry — embedded blob if present,
   otherwise the disk path. Caller stores the returned Font in
   fe->preview (heap-allocated copy). Returns 0-textured Font on
   failure. */
static Font font_preview_load(const FontEntry *fe, int size) {
    if (fe->data && fe->data_size > 0) {
        char ft[8] = ".";
        const char *ext = (fe->ext[0]) ? fe->ext : "ttf";
        strncat(ft, ext, sizeof(ft) - 2);
        return LoadFontFromMemory(ft, fe->data, (int)fe->data_size,
                                  size, NULL, 0);
    }
    return LoadFontEx(fe->path, size, NULL, 0);
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

/* Append every font baked into the binary by gen_fonts.sh as a
   FontEntry. These take precedence over identically-named system
   fonts (because they get added first and the disk scan dedupes by
   display name). path is set to "embedded:<NAME>" so save/load
   roundtrip cleanly. */
static void scan_embedded_fonts(void) {
    for (int i = 0; i < k_embedded_font_count && g_font_count < MAX_FONTS; i++) {
        FontEntry *f = &g_fonts[g_font_count++];
        memset(f, 0, sizeof(*f));
        strncpy(f->name, k_embedded_fonts[i].name, sizeof(f->name) - 1);
        f->name[sizeof(f->name) - 1] = 0;
        snprintf(f->path, sizeof(f->path), "embedded:%s", f->name);
        f->data      = k_embedded_fonts[i].data;
        f->data_size = k_embedded_fonts[i].data_size;
        strncpy(f->ext, k_embedded_fonts[i].ext, sizeof(f->ext) - 1);
        f->ext[sizeof(f->ext) - 1] = 0;
    }
}

/* Find an embedded font by either its name or its `embedded:NAME`
   path. Returns NULL if not found. */
static const EmbeddedFont *embedded_font_lookup(const char *path_or_name) {
    if (!path_or_name || !*path_or_name) return NULL;
    const char *name = path_or_name;
    if (strncmp(name, "embedded:", 9) == 0) name += 9;
    for (int i = 0; i < k_embedded_font_count; i++) {
        if (strcmp(k_embedded_fonts[i].name, name) == 0)
            return &k_embedded_fonts[i];
    }
    return NULL;
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

    /* Embedded fonts first so the disk-scan dedupe makes them
       authoritative — a system-installed Hack.ttf shouldn't shadow
       the version baked into the binary. */
    scan_embedded_fonts();
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
    bool ok;
    if (fe->data && fe->data_size > 0) {
        ok = renderer_set_font_data(r, fe->data, (int)fe->data_size,
                                    fe->ext, fe->path);
    } else {
        ok = renderer_set_font_path(r, fe->path);
    }
    if (!ok) return;
    tabs_resize_all(r, GetScreenWidth(), GetScreenHeight());
    SetWindowMinSize(r->cell_w * 20 + 2 * r->pad_x,
                     r->cell_h * 5 + TAB_BAR_H + 2 * r->pad_y);
}

static void settings_apply_padding(Renderer *r, int new_pad) {
    if (new_pad < 0) new_pad = 0;
    if (new_pad > 64) new_pad = 64;
    r->pad_x = new_pad;
    r->pad_y = new_pad;
    tabs_resize_all(r, GetScreenWidth(), GetScreenHeight());
    SetWindowMinSize(r->cell_w * 20 + 2 * r->pad_x,
                     r->cell_h * 5 + TAB_BAR_H + 2 * r->pad_y);
}

static void settings_apply_spacing(Renderer *r, int new_extra) {
    renderer_set_cell_spacing(r, new_extra);
    tabs_resize_all(r, GetScreenWidth(), GetScreenHeight());
    SetWindowMinSize(r->cell_w * 20 + 2 * r->pad_x,
                     r->cell_h * 5 + TAB_BAR_H + 2 * r->pad_y);
}

static bool g_settings_dir_focus = false;
static bool g_settings_dir_sel_all = false;

static SettingsLayout settings_layout(int win_w, int win_h) {
    SettingsLayout L = {0};
    int w = 600, h = 790;
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

    int theme_list_y = font_list_y + L.font_list.h + 16;
    L.theme_list = (Rect){ L.modal.x + 140, theme_list_y, w - 140 - 22, 140 };

    /* Cursor-style row. Four equal buttons aligned with the list width. */
    int cur_row_y = theme_list_y + L.theme_list.h + 12;
    {
        int bwidth = L.theme_list.w;
        int gap_sty = 6;
        int bw = (bwidth - 3 * gap_sty) / 4;
        int bx = L.modal.x + 140;
        int bh = 30;
        L.cur_block = (Rect){ bx,                           cur_row_y, bw, bh };
        L.cur_under = (Rect){ bx + (bw + gap_sty),           cur_row_y, bw, bh };
        L.cur_bar   = (Rect){ bx + 2 * (bw + gap_sty),       cur_row_y, bw, bh };
        L.cur_blink = (Rect){ bx + 3 * (bw + gap_sty),       cur_row_y, bw, bh };
    }

    int pad_row_y = cur_row_y + 30 + 16;
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
    if (rect_hit(L.theme_list, mx, my)) {
        float wheel = GetMouseWheelMove();
        if (wheel != 0.0f) {
            g_theme_list_scroll -= (int)(wheel * 3.0f);
            if (g_theme_list_scroll < 0) g_theme_list_scroll = 0;
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
        g_settings_focused_list = SETTINGS_FOCUS_FONT;
        return;
    }
    if (rect_hit(L.theme_list, mx, my)) {
        int row_h = 22;
        int idx = (my - L.theme_list.y) / row_h + g_theme_list_scroll;
        if (idx >= 0 && idx < themes_count()) {
            g_theme_list_selected = idx;
            /* Apply the clicked theme to the active pane only. */
            Tab *t = active_tab();
            Pane *p = t ? active_pane_of(t) : NULL;
            if (p && p->scr) screen_apply_theme(p->scr, &themes_all()[idx]);
        }
        g_settings_focused_list = SETTINGS_FOCUS_THEME;
        return;
    }
    /* Cursor-style buttons: apply to the active pane. */
    {
        Tab *t = active_tab();
        Pane *p = t ? active_pane_of(t) : NULL;
        CursorStyle st = CURSOR_STYLE_DEFAULT;
        bool hit = false;
        if      (rect_hit(L.cur_block, mx, my)) { st = CURSOR_STYLE_BLOCK;       hit = true; }
        else if (rect_hit(L.cur_under, mx, my)) { st = CURSOR_STYLE_UNDERLINE;   hit = true; }
        else if (rect_hit(L.cur_bar,   mx, my)) { st = CURSOR_STYLE_BAR;         hit = true; }
        else if (rect_hit(L.cur_blink, mx, my)) { st = CURSOR_STYLE_BLOCK_BLINK; hit = true; }
        if (hit) {
            if (p && p->scr) screen_set_cursor_style(p->scr, st);
            return;
        }
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
    if (rect_hit(L.close, mx, my)) { g_ui_mode = UI_NORMAL; g_settings_dir_focus = false; g_settings_focused_list = SETTINGS_FOCUS_NONE; return; }
    /* Click elsewhere drops focus from the text input + list. */
    g_settings_dir_focus = false;
    g_settings_focused_list = SETTINGS_FOCUS_NONE;
}

/* Keep the selected list row visible by adjusting the scroll offset
   when the selection moves outside [scroll, scroll + visible). */
static void list_scroll_to(int *scroll, int selected, int total, int row_h, int list_h) {
    int visible = list_h / row_h;
    if (visible < 1) visible = 1;
    if (selected < *scroll) *scroll = selected;
    else if (selected >= *scroll + visible) *scroll = selected - visible + 1;
    int max = total - visible;
    if (max < 0) max = 0;
    if (*scroll > max) *scroll = max;
    if (*scroll < 0) *scroll = 0;
}

static void settings_handle_keys(Renderer *r, SettingsLayout L) {
    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
#if defined(__APPLE__)
    bool cmd  = IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER);
#else
    bool cmd  = false;
#endif
    bool mod  = ctrl || cmd;

    if (IsKeyPressed(KEY_ESCAPE)) {
        if (g_settings_dir_focus) { g_settings_dir_focus = false; return; }
        if (g_settings_focused_list != SETTINGS_FOCUS_NONE) {
            g_settings_focused_list = SETTINGS_FOCUS_NONE;
            return;
        }
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
                for (int i = 0; i < g_num_tabs; i++) tab_log_close_all(g_tabs[i]);
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

    /* If a picker list has keyboard focus (last clicked), Up/Down
       moves selection within that list — applying immediately just
       like a click. Falls back to font-size +/- when no list is
       focused so the existing chord still works. */
    bool up   = IsKeyPressed(KEY_UP)   || IsKeyPressedRepeat(KEY_UP);
    bool down = IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN);
    if (g_settings_focused_list == SETTINGS_FOCUS_FONT && (up || down)) {
        int next = g_font_list_selected + (up ? -1 : +1);
        if (next < 0) next = 0;
        if (next >= g_font_count) next = g_font_count - 1;
        if (next != g_font_list_selected && next >= 0) {
            g_font_list_selected = next;
            settings_apply_font(r, &g_fonts[next]);
        }
        list_scroll_to(&g_font_list_scroll, g_font_list_selected,
                       g_font_count, 22, L.font_list.h);
        return;
    }
    if (g_settings_focused_list == SETTINGS_FOCUS_THEME && (up || down)) {
        int total = themes_count();
        int next = g_theme_list_selected + (up ? -1 : +1);
        if (next < 0) next = 0;
        if (next >= total) next = total - 1;
        if (next != g_theme_list_selected && next >= 0) {
            g_theme_list_selected = next;
            Tab *t = active_tab();
            Pane *p = t ? active_pane_of(t) : NULL;
            if (p && p->scr) screen_apply_theme(p->scr, &themes_all()[next]);
        }
        list_scroll_to(&g_theme_list_scroll, g_theme_list_selected,
                       total, 22, L.theme_list.h);
        return;
    }
    /* No list focused — Up/Down (and = / -) adjust font size. */
    if (up || IsKeyPressed(KEY_EQUAL) || IsKeyPressed(KEY_KP_ADD))
        settings_apply_font_size(r, r->font_size + 1);
    if (down || IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_KP_SUBTRACT))
        settings_apply_font_size(r, r->font_size - 1);
    /* Space toggles logging when not editing the path. */
    if (IsKeyPressed(KEY_SPACE)) {
        g_app_settings.log_enabled = !g_app_settings.log_enabled;
        refresh_tab_logs();
    }
}

/* Help modal — a static cheatsheet of every chord wired up in main.c.
   When you add or change a binding, update this list too: it's the
   user-facing source of truth. */
static void draw_help_modal(Renderer *r, int win_w, int win_h) {
#if defined(__APPLE__)
    const char *MOD = "Cmd";
#else
    const char *MOD = "Ctrl";
#endif
    /* Each row is { chord, description } — two entries per visible row,
       so size to 2 × max-rows (with headroom for future bindings).
       After building, every "Cmd" in a chord cell is rewritten to MOD
       so Linux + Windows show "Ctrl+T" instead of "Cmd+T". */
    char buf[128][128];
    int n = 0;
    #define ROW(C, D) do { \
        snprintf(buf[n*2],     sizeof(buf[0]), "%s", C); \
        snprintf(buf[n*2 + 1], sizeof(buf[0]), "%s", D); \
        n++; \
    } while (0)
    #define ROW_END() do { \
        if (strcmp(MOD, "Cmd") != 0) { \
            for (int _i = 0; _i < n; _i++) { \
                char *_p = buf[_i*2]; \
                char *_m = strstr(_p, "Cmd"); \
                if (_m) { \
                    char _tmp[128]; \
                    snprintf(_tmp, sizeof(_tmp), "%.*s%s%s", \
                             (int)(_m - _p), _p, MOD, _m + 3); \
                    snprintf(_p, sizeof(buf[0]), "%s", _tmp); \
                } \
            } \
        } \
    } while (0)
    ROW("",                          "Tabs");
    ROW("Cmd+T",                     "New tab (local shell)");
    ROW("Cmd+Shift+T",               "New tab via SSH (open the connect form)");
    ROW("Cmd+W",                     "Close active tab (or pane if split)");
    ROW("Cmd+1..9",                  "Jump to tab N");
    ROW("Cmd+Left / Cmd+Right",      "Cycle to previous / next tab");
    ROW("Cmd+[ / Cmd+]",             "Cycle to previous / next tab (alt)");
    ROW("Cmd+Shift+Left/Right",      "Move active tab left / right");
    ROW("Cmd+N",                     "New rbterm window (separate process — Mission Control groups them)");
    ROW("",                          "Splits");
    ROW("Cmd+D",                     "Split active tab vertically (side-by-side)");
    ROW("Cmd+Shift+D",               "Split horizontally (top / bottom)");
    ROW("Cmd+Shift+W",               "Close active pane (collapses to single)");
    ROW("Click a pane",              "Focus that pane");
    ROW("Drag the splitter",         "Resize panes");
    ROW("",                          "Selection + clipboard");
    ROW("Cmd+A",                     "Select all visible text in active pane");
    ROW("Cmd+C",                     "Copy selection");
    ROW("Cmd+V",                     "Paste");
    ROW("Click + drag",              "Select text");
    ROW("Double-click / triple-click", "Select word / line");
    ROW("",                          "Scroll + view");
    ROW("Mouse wheel",               "Scroll into history (when not in app cursor mode)");
    ROW("Ctrl+Shift+Up/Down",        "Scroll one row");
    ROW("Shift+PageUp/PageDown",     "Scroll one screen");
    ROW("Cmd+= / Cmd+-",             "Font size up / down");
    ROW("Cmd+0",                     "Reset font size to 20pt");
    ROW("",                          "Modals");
    ROW("Cmd+,",                     "Settings");
    ROW("Cmd+Shift+T",               "SSH connect form");
    ROW("Esc / click outside",       "Dismiss the active modal");
    ROW("",                          "");
    ROW("Note",                      "Cmd shows on macOS; Ctrl is the modifier on Linux + Windows.");
    ROW_END();
    #undef ROW
    #undef ROW_END

    int row_h = 22;
    int header_h = 50;
    int side_pad = 28;
    int top_pad  = 18;
    int footer_h = 32;        /* room for the "Esc or click outside…" hint */
    int needed_h = header_h + n * row_h + top_pad + footer_h;
    int w = 720, h = needed_h;
    if (w > win_w - 40) w = win_w - 40;
    if (h > win_h - 40) h = win_h - 40;
    int mx = (win_w - w) / 2;
    int my = (win_h - h) / 2;

    DrawRectangle(0, 0, win_w, win_h, (Color){0, 0, 0, 150});
    DrawRectangle(mx, my, w, h, (Color){30, 34, 46, 255});
    DrawRectangleLines(mx, my, w, h, (Color){125, 207, 255, 220});
    DrawRectangle(mx + 1, my + 1, w - 2, 38, (Color){38, 42, 58, 255});

    Font *f = (Font *)r->font_data;
    char title[64];
    snprintf(title, sizeof(title), "Keyboard shortcuts (modifier = %s)", MOD);
    DrawTextEx(*f, title,
               (Vector2){ mx + 20, my + 11 },
               16, 0, (Color){230, 232, 240, 255});

    int chord_w = 220;
    BeginScissorMode(mx + 8, my + header_h, w - 16,
                     h - header_h - footer_h);
    int y = my + header_h;
    for (int i = 0; i < n; i++) {
        const char *chord = buf[i*2];
        const char *desc  = buf[i*2 + 1];
        if (!chord[0]) {
            /* Section header. */
            DrawTextEx(*f, desc,
                       (Vector2){ mx + side_pad, y + 3 },
                       14, 0, (Color){140, 200, 255, 255});
        } else {
            DrawTextEx(*f, chord,
                       (Vector2){ mx + side_pad, y + 4 },
                       13, 0, (Color){200, 230, 255, 255});
            DrawTextEx(*f, desc,
                       (Vector2){ mx + side_pad + chord_w, y + 4 },
                       13, 0, (Color){200, 205, 220, 255});
        }
        y += row_h;
    }
    EndScissorMode();

    DrawTextEx(*f, "Esc or click outside the panel to close",
               (Vector2){ mx + 20, my + h - 22 },
               11, 0, (Color){110, 115, 130, 255});
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
                Font fprev = font_preview_load(&g_fonts[i], 14);
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

    /* Theme list. Clicking a row applies that palette + default fg/bg/
       cursor to the *active* pane only; other panes keep their state. */
    DrawTextEx(*f, "Theme",
               (Vector2){L.modal.x + 22, L.theme_list.y + 6},
               14, 0, (Color){200, 205, 220, 255});
    DrawRectangle(L.theme_list.x, L.theme_list.y,
                  L.theme_list.w, L.theme_list.h,
                  (Color){22, 25, 34, 255});
    DrawRectangleLines(L.theme_list.x, L.theme_list.y,
                       L.theme_list.w, L.theme_list.h,
                       (Color){70, 74, 90, 255});
    int tcount = themes_count();
    if (tcount == 0) {
        DrawTextEx(*f, "(no themes baked into this build)",
                   (Vector2){L.theme_list.x + 10, L.theme_list.y + 10},
                   12, 0, (Color){140, 145, 160, 255});
    } else {
        int trow_h = 22;
        int tvisible = L.theme_list.h / trow_h;
        int tmax_scroll = tcount - tvisible;
        if (tmax_scroll < 0) tmax_scroll = 0;
        if (g_theme_list_scroll > tmax_scroll) g_theme_list_scroll = tmax_scroll;
        if (g_theme_list_scroll < 0) g_theme_list_scroll = 0;
        BeginScissorMode(L.theme_list.x + 2, L.theme_list.y + 2,
                         L.theme_list.w - 4, L.theme_list.h - 4);
        const Theme *ts = themes_all();
        for (int i = 0; i < tcount; i++) {
            int ry = L.theme_list.y + (i - g_theme_list_scroll) * trow_h;
            if (ry + trow_h < L.theme_list.y ||
                ry > L.theme_list.y + L.theme_list.h) continue;
            bool sel = (i == g_theme_list_selected);
            if (sel) {
                DrawRectangle(L.theme_list.x + 2, ry,
                              L.theme_list.w - 4, trow_h,
                              (Color){46, 62, 90, 220});
            }
            /* Little colour swatches on the right so the name isn't
               the only clue to what the theme actually looks like. */
            int swatch_w = 10, swatch_h = 12, swatch_gap = 1;
            int swatches_w = 8 * (swatch_w + swatch_gap);
            int sx = L.theme_list.x + L.theme_list.w - 8 - swatches_w;
            int sy = ry + (trow_h - swatch_h) / 2;
            for (int k = 0; k < 8; k++) {
                uint32_t c = ts[i].palette[k];
                Color col = { (unsigned char)((c >> 16) & 0xff),
                              (unsigned char)((c >> 8)  & 0xff),
                              (unsigned char)( c        & 0xff), 255 };
                DrawRectangle(sx + k * (swatch_w + swatch_gap), sy,
                              swatch_w, swatch_h, col);
            }
            DrawTextEx(*f, ts[i].name,
                       (Vector2){L.theme_list.x + 10, ry + 4},
                       14, 0,
                       sel ? (Color){230, 232, 240, 255}
                           : (Color){200, 205, 220, 255});
        }
        EndScissorMode();
        if (tcount > tvisible) {
            int track_x = L.theme_list.x + L.theme_list.w - 5;
            int bar_h = L.theme_list.h * tvisible / tcount;
            if (bar_h < 24) bar_h = 24;
            int bar_y = L.theme_list.y +
                        (L.theme_list.h - bar_h) * g_theme_list_scroll /
                        (tmax_scroll > 0 ? tmax_scroll : 1);
            DrawRectangle(track_x, L.theme_list.y, 3, L.theme_list.h,
                          (Color){40, 45, 58, 255});
            DrawRectangle(track_x, bar_y, 3, bar_h,
                          (Color){110, 130, 170, 255});
        }
    }

    /* Cursor-style row. Shows the currently-selected style of the
       *active* pane so the picker reflects state; clicking a button
       changes just that pane. */
    {
        DrawTextEx(*f, "Cursor",
                   (Vector2){L.modal.x + 22, L.cur_block.y + 8},
                   14, 0, (Color){200, 205, 220, 255});
        Tab *t = active_tab();
        Pane *p = t ? active_pane_of(t) : NULL;
        CursorStyle cur = (p && p->scr) ? screen_cursor_style(p->scr)
                                        : CURSOR_STYLE_DEFAULT;
        struct { Rect r; const char *label; int style; } opts[] = {
            { L.cur_block, "Block",     CURSOR_STYLE_BLOCK },
            { L.cur_under, "Underline", CURSOR_STYLE_UNDERLINE },
            { L.cur_bar,   "Vertical",  CURSOR_STYLE_BAR },
            { L.cur_blink, "Blink",     CURSOR_STYLE_BLOCK_BLINK },
        };
        for (int i = 0; i < 4; i++) {
            Rect rr = opts[i].r;
            bool on = ((int)cur == opts[i].style ||
                       ((int)cur == CURSOR_STYLE_DEFAULT &&
                        opts[i].style == CURSOR_STYLE_BLOCK_BLINK));
            DrawRectangle(rr.x, rr.y, rr.w, rr.h,
                          on ? (Color){46, 92, 150, 255} : (Color){34, 38, 52, 255});
            DrawRectangleLines(rr.x, rr.y, rr.w, rr.h,
                               (Color){125, 207, 255, on ? 255 : 150});
            Vector2 tsz = MeasureTextEx(*f, opts[i].label, 13, 0);
            DrawTextEx(*f, opts[i].label,
                       (Vector2){rr.x + (rr.w - tsz.x) / 2,
                                 rr.y + (rr.h - tsz.y) / 2},
                       13, 0, (Color){230, 232, 240, 255});
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
    themes_load_builtins();
    /* Windows fills the embedded font table from compiled-in .rc
       resources at this point; Mac/Linux is a no-op (the table is
       already populated at link time). */
    embedded_fonts_init();
    /* Hand the renderer a broad-coverage backup font so glyphs the
       primary font lacks (box-drawing, arrows, less common Unicode)
       fall through to it instead of rendering as "?". DejaVu Sans
       Mono is the right tool — bundled, generous Unicode coverage. */
    {
        const EmbeddedFont *backup = embedded_font_lookup("DejaVuSansMono");
        if (!backup) backup = embedded_font_lookup("DejaVuSansMono.ttf");
        if (backup) {
            renderer_set_backup_font_data(backup->data, (int)backup->data_size,
                                          backup->ext);
        }
    }
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
    g_renderer = &r;
#ifdef __APPLE__
    /* Strip Cmd+W from AppKit's File > Close Window menu item so our
       in-app handler can use the chord to close just the active tab. */
    mac_disable_close_menu_item();
#endif
    /* Resolve the font:
         1. "embedded:NAME" → look up in the embedded table.
         2. an existing disk path → load directly.
         3. anything else (NULL or stale path from a deleted file) →
            try the system defaults, then fall through to the first
            embedded font. The fallback keeps a stripped-down install
            (no fonts/ folder, no system fonts) bootable. */
    bool inited = false;
    if (font_path && strncmp(font_path, "embedded:", 9) == 0) {
        const EmbeddedFont *ef = embedded_font_lookup(font_path);
        if (ef) {
            inited = renderer_init_with_data(&r, ef->data, (int)ef->data_size,
                                             ef->ext, font_path, font_size);
        }
    }
    if (!inited && font_path && font_path[0]) {
        FILE *probe = fopen(font_path, "rb");
        if (probe) {
            fclose(probe);
            inited = renderer_init(&r, font_path, font_size);
        }
    }
    if (!inited) inited = renderer_init(&r, NULL, font_size);
    if (!inited && k_embedded_font_count > 0) {
        const EmbeddedFont *ef = &k_embedded_fonts[0];
        char dpath[128];
        snprintf(dpath, sizeof(dpath), "embedded:%s", ef->name);
        inited = renderer_init_with_data(&r, ef->data, (int)ef->data_size,
                                         ef->ext, dpath, font_size);
    }
    if (!inited) {
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

    /* Windows + VMs can report wildly wrong monitor dimensions through
       GetMonitorWidth() (DPI scaling, virtual desktops, Parallels's
       extra chrome), so trying to centre the window ourselves
       reliably places it partly off-screen. Let the OS pick its own
       default position on Windows; on macOS / Linux the monitor
       metrics are trustworthy enough to use. */
#ifndef _WIN32
    if (mw > 0 && mh > 0) {
        Vector2 mp = GetMonitorPosition(mi);
        int x = (int)mp.x + (mw - win_w) / 2;
        int y = (int)mp.y + (mh - win_h) / 2;
        if (y < (int)mp.y + 40) y = (int)mp.y + 40; /* stay below menu bar */
        if (x < (int)mp.x) x = (int)mp.x;
        if (x + win_w > (int)mp.x + mw) x = (int)mp.x + mw - win_w;
        if (y + win_h > (int)mp.y + mh) y = (int)mp.y + mh - win_h;
        if (x < (int)mp.x) x = (int)mp.x;
        if (y < (int)mp.y) y = (int)mp.y;
        SetWindowPosition(x, y);
    }
#endif

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

        /* Whole-window content cell dims — still used by tab_open for
           its initial PTY size; individual pane resizing goes through
           tabs_resize_all below. */
        int content_rows = (win_h_now - TAB_BAR_H - 2 * r.pad_y) / r.cell_h;
        int content_cols = (win_w_now - 2 * r.pad_x) / r.cell_w;
        if (content_cols < 1) content_cols = 1;
        if (content_rows < 1) content_rows = 1;
        tabs_resize_all(&r, win_w_now, win_h_now);

        /* Drain each pane's PTY until EAGAIN or a safety cap. The cap is
           just a watchdog for a runaway writer pinning the UI — under
           normal output bursts (`find /usr`, etc.) we want to pull
           everything the kernel has buffered before rendering,
           otherwise the shell stalls on full PTY-buffer writes and the
           whole command takes seconds longer than in a native
           terminal. Panes whose shell has exited are closed right
           after the drain — if they're the last pane the tab dies
           with them. */
        for (int i = 0; i < g_num_tabs; i++) {
            Tab *t = g_tabs[i];
            bool pane_dead[2] = { false, false };
            for (int pi = 0; pi < t->num_panes; pi++) {
                Pane *p = &t->panes[pi];
                size_t drained = 0;
                const size_t drain_cap = 16 * 1024 * 1024;
                for (;;) {
                    int n = pty_read(p->pty, readbuf, sizeof(readbuf));
                    if (n > 0) {
                        screen_feed(p->scr, readbuf, (size_t)n);
                        pane_log_write(p, readbuf, (size_t)n);
                        drained += (size_t)n;
                        if (drained >= drain_cap) break;
                        continue;
                    }
                    if (n < 0) pane_dead[pi] = true;
                    break;
                }
                if (!pty_alive(p->pty)) pane_dead[pi] = true;
            }
            /* Close dead panes in reverse so indices stay valid during
               the sweep. A single-pane tab promotes to t->dead inside
               tab_close_pane. */
            for (int pi = t->num_panes - 1; pi >= 0; pi--) {
                if (pane_dead[pi]) tab_close_pane(t, pi);
            }
        }
        for (int i = g_num_tabs - 1; i >= 0; i--) {
            if (g_tabs[i]->dead) tab_close(i);
        }
        if (g_num_tabs == 0) break;

        /* CWD label refresh (no-op on Windows — pty_cwd returns false).
           Track per pane; only the active pane's change bumps the tab
           title. */
        double now = GetTime();
        for (int i = 0; i < g_num_tabs; i++) {
            Tab *t = g_tabs[i];
            for (int pi = 0; pi < t->num_panes; pi++) {
                Pane *p = &t->panes[pi];
                if (now - p->cwd_poll_at < 0.3) continue;
                p->cwd_poll_at = now;
                char buf[PATH_MAX];
                if (pty_cwd(p->pty, buf, sizeof(buf)) &&
                    strcmp(buf, p->cwd) != 0) {
                    strncpy(p->cwd, buf, sizeof(p->cwd) - 1);
                    p->cwd[sizeof(p->cwd) - 1] = 0;
                    if (i == g_active && pi == t->active_pane)
                        p->title_dirty = true;
                }
            }
        }

        Tab *cur = active_tab();
        Vector2 mp = GetMousePosition();
        bool in_tab_bar = (mp.y < TAB_BAR_H);
        /* Modal UI owns all clicks. Without this gate, a click on a
           form button in the SSH or Settings modal also reaches the
           terminal-area handler below, starting a phantom selection
           on the pane drawn behind the modal. */
        bool modal_open = (g_ui_mode != UI_NORMAL);

        if (modal_open) {
            /* Skip both the tab-bar handler (modal hides the bar's
               affordances) and the terminal mouse handler. The modal's
               own ssh_form_handle_mouse / settings_handle_mouse runs
               below and consumes the click cleanly. */
        } else if (in_tab_bar) {
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                TabBarHit h = tab_bar_hit_test(win_w_now, (int)mp.x, (int)mp.y);
                if (h.on_plus) tab_open(content_cols, content_rows);
                else if (h.on_ssh) ssh_form_open();
                else if (h.on_gear) settings_open(&r);
                else if (h.on_help) {
                    g_ui_mode = UI_HELP;
                    g_help_just_opened = true;
                }
                else if (h.on_split_v || h.on_split_h) {
                    if (cur) {
                        SplitMode m = h.on_split_v ? SPLIT_VERTICAL : SPLIT_HORIZONTAL;
                        char err[256] = {0};
                        if (!tab_split(cur, m, content_cols, content_rows, err, sizeof(err))) {
                            if (err[0]) fprintf(stderr, "rbterm: split failed: %s\n", err);
                        }
                    }
                }
                else if (h.tab_idx >= 0) {
                    if (h.on_close) tab_close(h.tab_idx);
                    else g_active = h.tab_idx;
                }
                cur = active_tab();
            }
        } else if (cur) {
            /* Splitter drag takes priority over pane input when active.
               The grab zone is padded by SPLITTER_GRAB on each side of
               the visual line so the cursor can find it without
               pixel-perfect aim. */
            PaneRect sp;
            bool has_split = splitter_rect(cur, win_w_now, win_h_now, &sp);
            bool on_splitter = false;
            if (has_split) {
                int gx = sp.x, gy = sp.y, gw = sp.w, gh = sp.h;
                if (cur->split == SPLIT_VERTICAL) {
                    gx -= SPLITTER_GRAB;
                    gw += 2 * SPLITTER_GRAB;
                } else {
                    gy -= SPLITTER_GRAB;
                    gh += 2 * SPLITTER_GRAB;
                }
                on_splitter = mp.x >= gx && mp.x < gx + gw &&
                              mp.y >= gy && mp.y < gy + gh;
            }
            if (on_splitter && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                cur->splitter_drag = true;
            }
            if (cur->splitter_drag) {
                if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                    int top = TAB_BAR_H;
                    int area_w = win_w_now;
                    int area_h = win_h_now - top;
                    float nr;
                    if (cur->split == SPLIT_VERTICAL) {
                        nr = (area_w > SPLITTER_PX)
                             ? (float)((int)mp.x) / (float)(area_w - SPLITTER_PX)
                             : 0.5f;
                    } else {
                        nr = (area_h > SPLITTER_PX)
                             ? (float)((int)mp.y - top) / (float)(area_h - SPLITTER_PX)
                             : 0.5f;
                    }
                    if (nr < 0.15f) nr = 0.15f;
                    if (nr > 0.85f) nr = 0.85f;
                    cur->split_ratio = nr;
                } else {
                    cur->splitter_drag = false;
                }
            } else {
                /* Click-to-focus: left-press inside a pane makes it the active one. */
                if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    int pi = pane_at(cur, win_w_now, win_h_now, (int)mp.x, (int)mp.y);
                    if (pi >= 0) cur->active_pane = pi;
                }
                Pane *p = active_pane_of(cur);
                if (p && p->scr) {
                    /* Translate window-pixel coords → active pane's cell coords. */
                    PaneRect pr;
                    pane_rect(cur, cur->active_pane, win_w_now, win_h_now, &pr);
                    int mcol = (int)((mp.x - pr.x - r.pad_x) / r.cell_w);
                    int mrow = (int)((mp.y - pr.y - r.pad_y) / r.cell_h);
                    int cmax = screen_cols(p->scr) - 1;
                    int rmax = screen_rows(p->scr) - 1;
                    if (mcol < 0) mcol = 0; if (mcol > cmax) mcol = cmax;
                    if (mrow < 0) mrow = 0; if (mrow > rmax) mrow = rmax;

                    /* Mouse reporting: when the app has asked for it (DECSET 1000,
                       1002, 1003) and the user isn't holding Shift (the universal
                       override that lets you still select text), we translate
                       mouse events to byte reports on the PTY instead of using
                       them for selection. */
                    bool shift_held = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
                    int  mmode = screen_mouse_mode(p->scr);
                    bool to_pty = (mmode != 0) && !shift_held;

                    if (to_pty) {
                        bool sgr = screen_mouse_sgr(p->scr);
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
                            pty_write(p->pty, (const uint8_t *)_buf, _n);                 \
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
                            double tnow = GetTime();
                            bool fast = (tnow - p->last_click_time < 0.45);
                            bool same = (mcol == p->last_click_col && mrow == p->last_click_row);
                            p->click_count = (fast && same) ? (p->click_count + 1) : 1;
                            p->last_click_time = tnow;
                            p->last_click_col = mcol;
                            p->last_click_row = mrow;
                            if (p->click_count == 2)      select_word(p->scr, &p->sel, mcol, mrow);
                            else if (p->click_count >= 3) { select_line(p->scr, &p->sel, mrow); p->click_count = 3; }
                            else {
                                p->sel.active = true;
                                p->sel.dragging = true;
                                p->sel.a_col = p->sel.b_col = mcol;
                                p->sel.a_row = p->sel.b_row = mrow;
                            }
                        }
                        if (p->sel.dragging && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                            p->sel.b_col = mcol; p->sel.b_row = mrow;
                        }
                        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                            if (p->sel.dragging && p->click_count < 2) {
                                p->sel.dragging = false;
                                if (p->sel.a_col == p->sel.b_col &&
                                    p->sel.a_row == p->sel.b_row) {
                                    p->sel.active = false;
                                }
                            }
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
                draw_tab_contents(&r, cur, win_w_now, win_h_now, GetTime(), IsWindowFocused());
                EndDrawing();
                continue;
            }
            ssh_form_handle_keys(content_cols, content_rows, L);
            cur = active_tab();
            if (!cur) break;
            {
                Pane *ap = active_pane_of(cur);
                if (ap && ap->title_dirty) {
                    SetWindowTitle(ap->title[0] ? ap->title : tab_label(cur));
                    ap->title_dirty = false;
                }
            }
            BeginDrawing();
            ClearBackground((Color){0, 0, 0, 255});
            draw_tab_bar(&r, win_w_now);
            draw_tab_contents(&r, cur, win_w_now, win_h_now, GetTime(), IsWindowFocused());
            draw_ssh_form(&r, win_w_now, win_h_now, L);
            EndDrawing();
            continue;
        }

        /* Settings modal. */
        if (g_ui_mode == UI_SETTINGS) {
            SettingsLayout L = settings_layout(win_w_now, win_h_now);
            settings_handle_mouse(&r, L);
            settings_handle_keys(&r, L);
            cur = active_tab();
            if (!cur) break;
            BeginDrawing();
            ClearBackground((Color){0, 0, 0, 255});
            draw_tab_bar(&r, win_w_now);
            /* Layout may have changed if font was resized. */
            L = settings_layout(win_w_now, win_h_now);
            draw_tab_contents(&r, cur, win_w_now, win_h_now, GetTime(), IsWindowFocused());
            draw_settings(&r, win_w_now, win_h_now, L);
            EndDrawing();
            continue;
        }

        /* Help modal — read-only keyboard cheatsheet, dismissed on Esc
           or by clicking outside the modal panel. (A click anywhere
           would dismiss the modal on the same frame the ? button
           opened it, since the tab-bar handler runs first.) */
        if (g_ui_mode == UI_HELP) {
            cur = active_tab();
            if (!cur) break;
            int hw = 720, hh = 800;
            if (hw > win_w_now - 40) hw = win_w_now - 40;
            if (hh > win_h_now - 40) hh = win_h_now - 40;
            int hmx = (win_w_now - hw) / 2;
            int hmy = (win_h_now - hh) / 2;
            Vector2 mp_help = GetMousePosition();
            bool inside = (mp_help.x >= hmx && mp_help.x < hmx + hw &&
                           mp_help.y >= hmy && mp_help.y < hmy + hh);
            if (g_help_just_opened) {
                /* Eat the press from the same frame that opened the
                   modal — the help button sits outside the modal
                   panel, so the click-outside check would otherwise
                   close it instantly. */
                g_help_just_opened = false;
            } else if (IsKeyPressed(KEY_ESCAPE) ||
                       (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !inside)) {
                g_ui_mode = UI_NORMAL;
            }
            BeginDrawing();
            ClearBackground((Color){0, 0, 0, 255});
            draw_tab_bar(&r, win_w_now);
            draw_tab_contents(&r, cur, win_w_now, win_h_now, GetTime(), IsWindowFocused());
            draw_help_modal(&r, win_w_now, win_h_now);
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
            /* Cmd+W closes the active tab. macOS's AppKit ordinarily
               eats this for "Close Window" — disable_close_menu_item()
               (called at startup) clears the menu binding so the chord
               reaches us. Falls through to closing the window when the
               last tab is gone. */
            else if (IsKeyPressed(KEY_W)) {
                tab_close(g_active);
                if (g_num_tabs == 0) break;
                cur = active_tab();
            }
            /* Cmd+N opens an entirely new rbterm window: fork+exec the
               same binary that's running. macOS resolves it via
               _NSGetExecutablePath (gives the .app's MacOS/rbterm
               path); Linux via /proc/self/exe. The child detaches with
               setsid() so closing either window won't affect the other. */
            else if (IsKeyPressed(KEY_N)) {
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
                char self_path[PATH_MAX] = {0};
#if defined(__APPLE__)
                uint32_t _plen = (uint32_t)sizeof(self_path);
                _NSGetExecutablePath(self_path, &_plen);
#else
                ssize_t _n = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
                if (_n > 0) self_path[_n] = 0;
#endif
                if (self_path[0] && fork() == 0) {
                    setsid();
                    execl(self_path, self_path, (char *)NULL);
                    _exit(127);
                }
#endif
            }
            /* Cmd+Shift+W closes the active pane (or the tab if only one
               pane). We intentionally don't bind plain Cmd+W: macOS
               intercepts it at the AppKit layer for "Close Window",
               which quits rbterm regardless of what we do here. */
            else if (shift_held && IsKeyPressed(KEY_W)) {
                pane_close_active(g_active);
                if (g_num_tabs == 0) break;
                cur = active_tab();
            }
            /* Cmd+D / Cmd+Shift+D: split active tab (side-by-side / top-bottom). */
            else if (IsKeyPressed(KEY_D)) {
                if (cur) {
                    SplitMode m = shift_held ? SPLIT_HORIZONTAL : SPLIT_VERTICAL;
                    char err[256] = {0};
                    if (!tab_split(cur, m, content_cols, content_rows, err, sizeof(err))) {
                        /* Silent fail for now; err goes to stderr so the user
                           can see why an SSH re-dial didn't take. */
                        if (err[0]) fprintf(stderr, "rbterm: split failed: %s\n", err);
                    }
                }
            }
            else if (IsKeyPressed(KEY_LEFT_BRACKET))  { g_active = (g_active - 1 + g_num_tabs) % g_num_tabs; cur = active_tab(); }
            else if (IsKeyPressed(KEY_RIGHT_BRACKET)) { g_active = (g_active + 1) % g_num_tabs; cur = active_tab(); }
            /* Cmd+Shift+Left/Right reorders the active tab, sliding it
               past its neighbour (with wrap-around). Cmd+Left/Right
               (no Shift) just cycles which tab is active — same as
               Cmd+[ / Cmd+]. The shift_held check has to come first
               so the no-shift branch doesn't swallow the Shift case. */
            else if (shift_held && IsKeyPressed(KEY_LEFT) && g_num_tabs > 1) {
                int dst = (g_active - 1 + g_num_tabs) % g_num_tabs;
                Tab *swap = g_tabs[g_active];
                g_tabs[g_active] = g_tabs[dst];
                g_tabs[dst] = swap;
                g_active = dst;
                cur = active_tab();
            }
            else if (shift_held && IsKeyPressed(KEY_RIGHT) && g_num_tabs > 1) {
                int dst = (g_active + 1) % g_num_tabs;
                Tab *swap = g_tabs[g_active];
                g_tabs[g_active] = g_tabs[dst];
                g_tabs[dst] = swap;
                g_active = dst;
                cur = active_tab();
            }
            else if (IsKeyPressed(KEY_LEFT))  { g_active = (g_active - 1 + g_num_tabs) % g_num_tabs; cur = active_tab(); }
            else if (IsKeyPressed(KEY_RIGHT)) { g_active = (g_active + 1) % g_num_tabs; cur = active_tab(); }
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

        Pane *ap = active_pane_of(cur);
        InputActions acts;
        size_t in_n = ap ? input_poll(ap->scr, inputbuf, sizeof(inputbuf), &acts)
                         : 0;
        if (ap && acts.scroll_rows != 0) screen_scroll_view(ap->scr, acts.scroll_rows);
        if (ap && acts.select_all) {
            int cols = screen_cols(ap->scr);
            int rows = screen_rows(ap->scr);
            ap->sel.active = true;
            ap->sel.dragging = false;
            ap->sel.a_col = 0;
            ap->sel.a_row = 0;
            ap->sel.b_col = cols - 1;
            ap->sel.b_row = rows - 1;
        }
        if (ap && acts.copy) copy_selection(ap->scr, &ap->sel);
        if (ap && acts.paste) {
            const char *t = GetClipboardText();
            if (t && *t) {
                if (screen_bracketed_paste(ap->scr)) {
                    pty_write(ap->pty, (const uint8_t *)"\x1b[200~", 6);
                    pty_write(ap->pty, (const uint8_t *)t, strlen(t));
                    pty_write(ap->pty, (const uint8_t *)"\x1b[201~", 6);
                } else {
                    pty_write(ap->pty, (const uint8_t *)t, strlen(t));
                }
            }
        }

        /* Focus events (DECSET 1004). Emit CSI I on gain, CSI O on loss.
           Only the active pane learns about the window-focus change. */
        {
            bool is_focused = IsWindowFocused();
            if (is_focused != was_focused) {
                was_focused = is_focused;
                if (ap && screen_focus_report(ap->scr)) {
                    pty_write(ap->pty,
                              is_focused ? (const uint8_t *)"\x1b[I"
                                         : (const uint8_t *)"\x1b[O",
                              3);
                }
            }
        }
        if (ap && acts.font_delta != 100) {
            int old = r.font_size;
            int ns = (acts.font_delta == 0) ? 20
                    : old + (acts.font_delta > 0 ? 1 : -1);
            if (renderer_set_font_size(&r, ns)) {
                tabs_resize_all(&r, GetScreenWidth(), GetScreenHeight());
                SetWindowMinSize(r.cell_w * 20 + 2 * r.pad_x, r.cell_h * 5 + TAB_BAR_H + 2 * r.pad_y);
            }
        }
        if (ap && in_n > 0) {
            screen_scroll_reset(ap->scr);
            pty_write(ap->pty, inputbuf, in_n);
            if (ap->sel.active && !ap->sel.dragging) ap->sel.active = false;
        }

        if (ap && ap->title_dirty) {
            SetWindowTitle(ap->title[0] ? ap->title : tab_label(cur));
            ap->title_dirty = false;
        }

        BeginDrawing();
        ClearBackground((Color){0, 0, 0, 255});
        draw_tab_bar(&r, win_w_now);
        draw_tab_contents(&r, cur, win_w_now, win_h_now, GetTime(), IsWindowFocused());
        EndDrawing();
    }

    for (int i = g_num_tabs - 1; i >= 0; i--) tab_close(i);
    renderer_shutdown(&r);
    CloseWindow();
    return 0;
}
