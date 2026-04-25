#include "raylib.h"
#include "cast.h"
#include "gif_encoder.h"
#include "webp_encoder.h"
#include <stdarg.h>
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
  void mac_install_ctrl_tab_monitor(void);
  int  mac_consume_ctrl_tab(void);
  void mac_enter_native_fullscreen(void);
#endif

#ifndef _WIN32
#include <signal.h>
#include <fcntl.h>
#include <sys/wait.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

/* ---------- Persistent config ----------
 * Plain key=value file at ~/.config/rbterm/config.ini. Loaded once at
 * startup and re-written by the "Save as Default" button in Settings. */

static void expand_home_path(const char *in, char *out, size_t cap);
static void mkdir_p(const char *path);

/* Resolve "~/.config/rbterm/config.ini" with $HOME expansion. */
static void config_path(char *out, size_t cap) {
    expand_home_path("~/.config/rbterm/config.ini", out, cap);
}

/* Resolve "~/.config/rbterm/" with $HOME expansion. */
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

/* Custom raylib trace-log callback. Swallows the glyph-bigger-than-
   font-size warnings stb_truetype emits for box-drawing characters
   (U+2502 etc. are *deliberately* over-height so vertical lines
   connect across cells), while still forwarding everything else to
   stderr so real problems aren't hidden. */
static void rl_trace_log(int logType, const char *text, va_list args) {
    char line[1024];
    vsnprintf(line, sizeof(line), text, args);
    if (strstr(line, "size is bigger than expected font size")) return;
    /* stb_truetype rejects some .ttc collections + the occasional
       odd .ttf in the picker — the affected font just won't render,
       not a fatal problem. Drop the noise. */
    if (strstr(line, "Failed to process TTF font data")) return;
    const char *pfx = "INFO";
    switch (logType) {
    case LOG_DEBUG:   pfx = "DEBUG";   break;
    case LOG_INFO:    pfx = "INFO";    break;
    case LOG_WARNING: pfx = "WARNING"; break;
    case LOG_ERROR:   pfx = "ERROR";   break;
    case LOG_FATAL:   pfx = "FATAL";   break;
    default:          pfx = "TRACE";   break;
    }
    fprintf(stderr, "%s: %s\n", pfx, line);
}

/* ---------- App-wide settings ---------- */

/* Startup window layout. `MAXIMIZED` is the OS-native "own Space"
   fullscreen on macOS (green-button behavior: its own desktop,
   switchable via three-finger trackpad swipe) and a regular maximized
   window on Linux/Windows. `FULLSCREEN` is borderless/exclusive
   fullscreen on the current Space — covers the display, no menu bar,
   no own Space. */
typedef enum {
    STARTUP_WINDOW_DEFAULT    = 0,
    STARTUP_WINDOW_FULLSCREEN = 1,
    STARTUP_WINDOW_MAXIMIZED  = 2,
} StartupWindowMode;

typedef struct {
    bool log_enabled;
    char log_dir[PATH_MAX];
    int  key_repeat_initial_ms;  /* delay before first repeat fires (held key) */
    int  key_repeat_rate_ms;     /* period between subsequent repeats */
    int  cursor_style;           /* CursorStyle enum — default for new panes */
    int  startup_window;         /* StartupWindowMode */
    char rec_dir[PATH_MAX];      /* default folder for saved recordings */
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

/* Read ~/.config/rbterm/config.ini at startup and seed
   g_persisted (font path/size, padding, spacing) and a few
   AppSettings fields directly. main() consumes g_persisted after
   the renderer is up and tabs are spawned. Missing file is
   silently fine — defaults stay in place. */
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
        } else if (strcmp(k, "rec_dir") == 0) {
            strncpy(g_app_settings.rec_dir, v, sizeof(g_app_settings.rec_dir) - 1);
            g_app_settings.rec_dir[sizeof(g_app_settings.rec_dir) - 1] = 0;
        } else if (strcmp(k, "key_repeat_initial_ms") == 0) {
            int vi = atoi(v);
            if (vi >= 0 && vi <= 2000) g_app_settings.key_repeat_initial_ms = vi;
        } else if (strcmp(k, "key_repeat_rate_ms") == 0) {
            int vi = atoi(v);
            if (vi >= 0 && vi <= 500) g_app_settings.key_repeat_rate_ms = vi;
        } else if (strcmp(k, "cursor_style") == 0) {
            if      (!strcmp(v, "block"))     g_app_settings.cursor_style = CURSOR_STYLE_BLOCK;
            else if (!strcmp(v, "underline")) g_app_settings.cursor_style = CURSOR_STYLE_UNDERLINE;
            else if (!strcmp(v, "bar"))       g_app_settings.cursor_style = CURSOR_STYLE_BAR;
            else if (!strcmp(v, "blink"))     g_app_settings.cursor_style = CURSOR_STYLE_BLOCK_BLINK;
        } else if (strcmp(k, "startup_window") == 0) {
            /* "fullscreen" is accepted for back-compat but demoted to
               default — the option was removed because the backing
               ToggleFullscreen call was broken. */
            if      (!strcmp(v, "maximized"))  g_app_settings.startup_window = STARTUP_WINDOW_MAXIMIZED;
            else                               g_app_settings.startup_window = STARTUP_WINDOW_DEFAULT;
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
    if (g_app_settings.rec_dir[0]) fprintf(fp, "rec_dir=%s\n", g_app_settings.rec_dir);
    fprintf(fp, "key_repeat_initial_ms=%d\n", g_app_settings.key_repeat_initial_ms);
    fprintf(fp, "key_repeat_rate_ms=%d\n",    g_app_settings.key_repeat_rate_ms);
    {
        const char *cs = NULL;
        switch (g_app_settings.cursor_style) {
        case CURSOR_STYLE_BLOCK:       cs = "block";     break;
        case CURSOR_STYLE_UNDERLINE:   cs = "underline"; break;
        case CURSOR_STYLE_BAR:         cs = "bar";       break;
        case CURSOR_STYLE_BLOCK_BLINK: cs = "blink";     break;
        default: break;
        }
        if (cs) fprintf(fp, "cursor_style=%s\n", cs);
    }
    {
        const char *sw = (g_app_settings.startup_window == STARTUP_WINDOW_MAXIMIZED)
                             ? "maximized" : "default";
        fprintf(fp, "startup_window=%s\n", sw);
    }
    fclose(fp);
#ifndef _WIN32
    chmod(path, 0600);
#endif
    return true;
}

/* Seed g_app_settings with first-run defaults: logging off,
   default cursor style, sensible key-repeat rates, log dir
   under $HOME/.rbterm/logs. Called once before the config-file
   load potentially overwrites individual fields. */
static void app_settings_init(void) {
    g_app_settings.log_enabled = false;
    g_app_settings.key_repeat_initial_ms = 300;
    g_app_settings.key_repeat_rate_ms    = 25;
    g_app_settings.cursor_style          = CURSOR_STYLE_DEFAULT;
    g_app_settings.startup_window        = STARTUP_WINDOW_DEFAULT;
    const char *home = getenv("HOME");
#ifdef _WIN32
    if (!home || !*home) home = getenv("USERPROFILE");
#endif
    if (home && *home) {
        snprintf(g_app_settings.log_dir, sizeof(g_app_settings.log_dir),
                 "%s/.rbterm/logs", home);
        snprintf(g_app_settings.rec_dir, sizeof(g_app_settings.rec_dir),
                 "%s/Downloads", home);
    } else {
        strncpy(g_app_settings.log_dir, "./rbterm-logs",
                sizeof(g_app_settings.log_dir) - 1);
        strncpy(g_app_settings.rec_dir, "./",
                sizeof(g_app_settings.rec_dir) - 1);
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
/* In-pane search state. Cmd+F opens a one-line search bar docked
   over the top of the pane. Matches are single-row substrings
   (case-insensitive) across scrollback + live grid. Each match
   stores (abs_row, col_start, col_end) in absolute-row coordinates
   (see screen_cell_abs). The active "current" match gets brighter
   highlighting and auto-scrolls into view. */
typedef struct {
    bool active;
    char query[128];
    int  caret;       /* byte index into query where the cursor sits */
    int  sel_anchor;  /* -1 = no selection; else the "other" end of the
                         selection (selection spans [lo..hi) of
                         min(caret, sel_anchor)..max(caret, sel_anchor)). */
    bool mouse_down;  /* left button held inside the search bar */
    int *rows;        /* heap; length = count */
    int *cols;
    int *ends;
    int  count;
    int  cap;
    int  current;     /* -1 when no matches */
} Search;

typedef struct {
    Pty *pty;
    Screen *scr;
    Selection sel;
    Search  search;
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
    /* Background-tab activity: set when any pane of a non-active tab
       receives PTY output. Cleared when the tab becomes active. */
    bool  activity;
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
#define TAB_REC_W   26          /* one record button (start + stop, both always visible) */
#define TAB_SSH_W   48

static Tab *g_tabs[MAX_TABS];

/* Tab-reorder-by-drag state. Set on left-press over a tab's body (not
   the close 'x'); cleared on release. Once the cursor moves past a
   threshold, `g_tab_dragging` flips on and subsequent mouse movement
   swaps the tab into whichever slot the cursor is over. */
static int  g_tab_press_idx = -1;
static int  g_tab_press_mx  = 0;
static bool g_tab_dragging  = false;
static int g_num_tabs = 0;
static int g_active = 0;

/* Modal SSH connection form. When non-NORMAL, the terminal is locked:
   keystrokes edit form fields and mouse clicks focus them instead of
   going to the active tab. Layout is computed on the fly in
   ssh_form_layout() so draw and hit-test share one source of truth. */
typedef enum { UI_NORMAL = 0, UI_SSH_FORM, UI_SETTINGS, UI_HELP, UI_REC_SAVE } UiMode;
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
/* 0 = Navigation, 1 = Edit & Search, 2 = Shell integration. Persists
   between opens so the user lands on whichever tab they last
   looked at. */
static int  g_help_tab = 0;
/* Filled by draw_help_modal each frame so the main-loop click
   handler can hit-test the tab buttons AND know the actual panel
   rect (which depends on the active tab's row count, so a fixed
   fallback would over-claim "inside" for short tabs). */
#define HELP_TAB_COUNT 3
static Rect g_help_tab_rects[HELP_TAB_COUNT];
static Rect g_help_modal_rect;

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

/* Close a pane's open log file (if any). Idempotent. */
static void pane_log_close(Pane *p) {
    if (!p) return;
    if (p->log_fp) { fclose(p->log_fp); p->log_fp = NULL; }
}

/* Append raw PTY bytes to the pane's log file. fsync each call so a
   forced quit still leaves a complete log up to the last byte read. */
static void pane_log_write(Pane *p, const uint8_t *buf, size_t n) {
    if (!p || !p->log_fp || n == 0) return;
    fwrite(buf, 1, n, p->log_fp);
    fflush(p->log_fp);
}

/* Open log files on every pane in a tab. */
static void tab_log_open_all(Tab *t) {
    if (!t) return;
    for (int i = 0; i < t->num_panes; i++) pane_log_open(t, &t->panes[i], i);
}
/* Symmetric — close every pane's log in a tab. */
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

/* Send screen-originated bytes (CSI responses, mouse reports etc.)
   back to the owning pane's PTY. */
static void io_write_cb(void *u, const uint8_t *buf, size_t n) {
    Pane *p = (Pane *)u;
    pty_write(p->pty, buf, n);
}
/* OSC 0/2 — apply the shell's reported title to the pane and flag
   it dirty so the tab bar / window title repaints. */
static void io_set_title_cb(void *u, const char *title) {
    Pane *p = (Pane *)u;
    strncpy(p->title, title, sizeof(p->title) - 1);
    p->title[sizeof(p->title) - 1] = 0;
    p->title_dirty = true;
}
/* BEL handler — placeholder. Visual/audio bell is a future feature. */
static void io_bell_cb(void *u) { (void)u; }
/* OSC 52 — push a remote-supplied string into the system clipboard. */
static void io_set_clipboard_cb(void *u, const char *utf8) {
    (void)u;
    if (utf8 && *utf8) SetClipboardText(utf8);
}
/* OSC 7 — record the shell's current working directory on the pane
   for tab-label / new-tab-cwd purposes. */
static void io_set_cwd_cb(void *u, const char *path) {
    Pane *p = (Pane *)u;
    if (!p || !path || !*path) return;
    strncpy(p->cwd, path, sizeof(p->cwd) - 1);
    p->cwd[sizeof(p->cwd) - 1] = 0;
    p->title_dirty = true;
}

/* OSC 9 / OSC 777 — desktop notification. Shell out to the platform's
   native notifier so we don't depend on a daemon library. fork+execvp
   so the message arrives as argv[N], no shell escaping needed. */
static void io_notify_cb(void *u, const char *body) {
    (void)u;
    if (!body || !*body) return;
#ifdef _WIN32
    /* PowerShell toast via BurntToast-ish one-liner. Falls back to a
       MessageBox if the WinRT classes aren't available — at least
       something pops. Quoted with single-quotes; embedded apostrophes
       are escaped by doubling. Best-effort. */
    char esc[1024]; size_t bj = 0;
    for (const char *q = body; *q && bj + 2 < sizeof(esc); q++) {
        if (*q == '\'') { esc[bj++] = '\''; esc[bj++] = '\''; }
        else            esc[bj++] = *q;
    }
    esc[bj] = 0;
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
        "powershell -NoProfile -Command "
        "\"[Windows.UI.Notifications.ToastNotificationManager,Windows.UI.Notifications,ContentType=WindowsRuntime]>$null;"
        "$t=[Windows.UI.Notifications.ToastNotificationManager]::GetTemplateContent("
        "[Windows.UI.Notifications.ToastTemplateType]::ToastText02);"
        "$t.GetElementsByTagName('text')[0].AppendChild($t.CreateTextNode('rbterm'))>$null;"
        "$t.GetElementsByTagName('text')[1].AppendChild($t.CreateTextNode('%s'))>$null;"
        "[Windows.UI.Notifications.ToastNotificationManager]::CreateToastNotifier('rbterm').Show("
        "[Windows.UI.Notifications.ToastNotification]::new($t))\"",
        esc);
    system(cmd);
#else
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        #ifdef __APPLE__
            char script[1024];
            /* AppleScript single-quotes need to be doubled. */
            char esc[768]; size_t bj = 0;
            for (const char *q = body; *q && bj + 2 < sizeof(esc); q++) {
                if (*q == '"' || *q == '\\') esc[bj++] = '\\';
                esc[bj++] = *q;
            }
            esc[bj] = 0;
            snprintf(script, sizeof(script),
                "display notification \"%s\" with title \"rbterm\"", esc);
            execlp("osascript", "osascript", "-e", script, (char *)NULL);
        #else
            execlp("notify-send", "notify-send", "rbterm", body, (char *)NULL);
        #endif
        _exit(127);
    }
#endif
}

/* Reset double/triple-click tracking on a fresh pane so the first
   click in it is never seen as the second of a multi-click. */
static void pane_init_click_state(Pane *p) {
    p->last_click_time = -1.0;
    p->last_click_col = p->last_click_row = -1;
}

/* Build the ScreenIO for a pane (caller owns the Pane pointer for its lifetime). */
static ScreenIO pane_io(Pane *p) {
    ScreenIO io = { .user = p, .write = io_write_cb,
                    .set_title = io_set_title_cb, .bell = io_bell_cb,
                    .set_clipboard = io_set_clipboard_cb,
                    .set_cwd = io_set_cwd_cb,
                    .notify = io_notify_cb };
    return io;
}

/* Spawn a local shell in a fresh pane: pty_open + screen_new with
   5000 rows of scrollback + cursor-style seed from app settings.
   `cwd` is the directory the shell starts in (NULL = $HOME). */
static bool pane_open_local(Pane *p, int cols, int rows, const char *cwd) {
    pane_init_click_state(p);
    strncpy(p->title, "shell", sizeof(p->title) - 1);
    p->pty = pty_open(cols, rows, cwd);
    if (!p->pty) return false;
    p->scr = screen_new(cols, rows, 5000, pane_io(p));
    /* Seed the cursor style from the rbterm-wide default. DECSCUSR
       sequences from the shell (or programs that set their own style)
       overwrite this on the per-Screen level later. */
    if (g_app_settings.cursor_style != CURSOR_STYLE_DEFAULT) {
        screen_set_cursor_style(p->scr, (CursorStyle)g_app_settings.cursor_style);
    }
    if (cwd && *cwd) {
        strncpy(p->cwd, cwd, sizeof(p->cwd) - 1);
        p->cwd[sizeof(p->cwd) - 1] = 0;
    }
    return true;
}

/* Like pane_open_local but the PTY is an SSH session. On failure
   (handshake / auth / channel) writes the libssh error message
   into `err` and returns false. */
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

static void open_url(const char *url);

/* Asciinema recording — global singleton. We tap the PTY drain in
   the main loop and write timestamped events to a .cast file
   (asciinema v2 spec). Cmd-style buttons in the tab bar start /
   stop. Pinned to whichever pane was active when start fired so
   the user can tab around without losing the recording. */
typedef struct {
    bool   active;
    Pane  *pane;
    FILE  *fp;
    double start_time;
    char   path[PATH_MAX];
} Recording;
static Recording g_rec;
static void rec_stop(void);

/* Post-recording save modal state. When the user clicks Stop we
   close the .cast and pop a modal asking where + what format to
   save as. Phase 2a only handles `.cast` cleanly; other formats
   render dim with a "coming soon" tooltip until the converter
   shell-out lands. */
typedef enum {
    REC_FMT_CAST = 0,
    REC_FMT_TXT,
    REC_FMT_GIF,
    REC_FMT_MP4,
    REC_FMT_WEBM,
    REC_FMT_APNG,
    REC_FMT_WEBP,
    REC_FMT_COUNT
} RecFmt;
typedef struct {
    char   src_path[PATH_MAX];   /* the temp .cast file already on disk */
    double duration_s;
    char   dst_path[PATH_MAX];   /* user-editable destination */
    RecFmt fmt;
    bool   path_focus;
    bool   path_sel_all;
    char   status[256];
} RecSave;
static RecSave g_rec_save;

/* Free everything a pane owns: log file, PTY, screen, search-match
   arrays. Idempotent — safe to call on a partially-initialised
   pane (e.g. after a failed pane_open_*). */
static void pane_free(Pane *p) {
    if (!p) return;
    /* Stop a recording if its target pane is going away — the file
       pointer would dangle otherwise. */
    if (g_rec.active && g_rec.pane == p) rec_stop();
    pane_log_close(p);
    if (p->pty) { pty_close(p->pty); p->pty = NULL; }
    if (p->scr) { screen_free(p->scr); p->scr = NULL; }
    free(p->search.rows);  p->search.rows = NULL;
    free(p->search.cols);  p->search.cols = NULL;
    free(p->search.ends);  p->search.ends = NULL;
    p->search.count = p->search.cap = 0;
    p->search.active = false;
    p->search.caret = 0;
    p->search.sel_anchor = -1;
    p->search.mouse_down = false;
}

/* JSON-escape `n` bytes from `buf` into `fp`. Per asciinema v2 the
   data field is a UTF-8 string, so valid multi-byte UTF-8 passes
   through verbatim; only ASCII control chars + " and \\ get escaped. */
static void rec_json_escape(FILE *fp, const uint8_t *buf, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c = buf[i];
        switch (c) {
        case '"':  fputs("\\\"", fp); break;
        case '\\': fputs("\\\\", fp); break;
        case '\b': fputs("\\b", fp); break;
        case '\f': fputs("\\f", fp); break;
        case '\n': fputs("\\n", fp); break;
        case '\r': fputs("\\r", fp); break;
        case '\t': fputs("\\t", fp); break;
        default:
            if (c < 0x20 || c == 0x7f) fprintf(fp, "\\u%04x", c);
            else fputc(c, fp);
            break;
        }
    }
}

/* Walk every cell of the active screen and emit ANSI bytes that
   reconstruct what the user sees right now. Used as the first
   event of a recording so playback doesn't open on a blank screen
   when the user starts recording mid-session. Best-effort: drops
   attributes (bold/italic/colours) but preserves the cell text +
   cursor position, which is what people typically want to see. */
static void rec_emit_initial_snapshot(FILE *fp, Pane *p) {
    if (!p || !p->scr) return;
    Screen *s = p->scr;
    int cols = screen_cols(s);
    int rows = screen_rows(s);
    /* Build the byte stream into a buffer, then JSON-escape it
       through the existing rec_json_escape so the syntax matches
       any other "o" event line. */
    size_t cap = (size_t)cols * rows * 4 + 256;
    uint8_t *buf = malloc(cap);
    if (!buf) return;
    size_t n = 0;
    /* Reset attrs, clear screen, home cursor. */
    const char *prefix = "\x1b[0m\x1b[2J\x1b[H";
    size_t plen = strlen(prefix);
    if (n + plen < cap) { memcpy(buf + n, prefix, plen); n += plen; }
    for (int y = 0; y < rows; y++) {
        /* Position cursor at the start of this row. */
        int wrote = snprintf((char *)buf + n, cap - n, "\x1b[%d;1H", y + 1);
        if (wrote < 0 || (size_t)wrote >= cap - n) break;
        n += (size_t)wrote;
        /* Find last non-blank column so we don't emit huge tail-runs of spaces. */
        int last = -1;
        for (int x = cols - 1; x >= 0; x--) {
            Cell c = screen_view_cell(s, x, y);
            uint32_t cp = c.cp;
            if (cp != 0 && cp != ' ' && !(c.attrs & ATTR_WIDE_CONT)) { last = x; break; }
        }
        for (int x = 0; x <= last && n + 4 < cap; x++) {
            Cell c = screen_view_cell(s, x, y);
            if (c.attrs & ATTR_WIDE_CONT) continue;
            uint32_t cp = c.cp ? c.cp : ' ';
            if (cp < 0x80) {
                buf[n++] = (uint8_t)cp;
            } else if (cp < 0x800) {
                buf[n++] = (uint8_t)(0xC0 | (cp >> 6));
                buf[n++] = (uint8_t)(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                buf[n++] = (uint8_t)(0xE0 | (cp >> 12));
                buf[n++] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
                buf[n++] = (uint8_t)(0x80 | (cp & 0x3F));
            } else {
                buf[n++] = (uint8_t)(0xF0 | (cp >> 18));
                buf[n++] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
                buf[n++] = (uint8_t)(0x80 | ((cp >> 6)  & 0x3F));
                buf[n++] = (uint8_t)(0x80 | (cp & 0x3F));
            }
        }
    }
    /* Move cursor back to where the shell currently has it. */
    int curx = screen_cursor_x(s) + 1;
    int cury = screen_cursor_y(s) + 1;
    int wrote = snprintf((char *)buf + n, cap - n, "\x1b[%d;%dH", cury, curx);
    if (wrote > 0 && (size_t)wrote < cap - n) n += (size_t)wrote;

    /* Emit as a single t=0 event. */
    fprintf(fp, "[0.000000, \"o\", \"");
    rec_json_escape(fp, buf, n);
    fputs("\"]\n", fp);
    fflush(fp);
    free(buf);
}

/* Begin recording the bytes coming out of pane `p`. Writes the
   asciinema v2 header (cols, rows, unix timestamp), seeds the
   recording with a snapshot of the current screen state so
   playback opens with what the user sees, and pins the pane
   pointer so they can switch tabs without disturbing the
   recording. Returns false on file-open failure. */
static bool rec_start(Pane *p) {
    if (!p || !p->scr || g_rec.active) return false;
    char dir[PATH_MAX];
    /* Use the Settings → Recording directory; fall back to
       ~/Downloads if it's empty (first run with a stale config). */
    const char *want = g_app_settings.rec_dir[0]
                       ? g_app_settings.rec_dir : "~/Downloads";
    expand_home_path(want, dir, sizeof(dir));
    mkdir_p(dir);
    char stamp[64];
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv);
    snprintf(g_rec.path, sizeof(g_rec.path),
             "%s/rbterm-%s.cast", dir, stamp);
    FILE *fp = fopen(g_rec.path, "wb");
    if (!fp) {
        fprintf(stderr, "rbterm: rec_start: %s: %s\n",
                g_rec.path, strerror(errno));
        return false;
    }
    int cols = screen_cols(p->scr);
    int rows = screen_rows(p->scr);
    fprintf(fp,
            "{\"version\": 2, \"width\": %d, \"height\": %d, "
            "\"timestamp\": %lld}\n",
            cols, rows, (long long)now);
    /* Seed the recording with the current screen so playback opens
       on what the user already sees, rather than a blank terminal
       until the next byte happens to arrive. */
    rec_emit_initial_snapshot(fp, p);
    g_rec.active = true;
    g_rec.pane = p;
    g_rec.fp = fp;
    g_rec.start_time = GetTime();
    fprintf(stderr, "rbterm: recording → %s\n", g_rec.path);
    return true;
}

/* Default-format extension. Used when seeding the modal's path. */
static const char *rec_fmt_ext(RecFmt f) {
    switch (f) {
    case REC_FMT_TXT:  return "txt";
    case REC_FMT_GIF:  return "gif";
    case REC_FMT_MP4:  return "mp4";
    case REC_FMT_WEBM: return "webm";
    case REC_FMT_APNG: return "apng";
    case REC_FMT_WEBP: return "webp";
    case REC_FMT_CAST:
    default:           return "cast";
    }
}

/* Replace the extension on a path with `new_ext` (no leading dot).
   If the path has no extension, `new_ext` is appended. Edits in
   place; caller guarantees `cap` bytes. */
static void rec_replace_ext(char *path, size_t cap, const char *new_ext) {
    char *slash = strrchr(path, '/');
    char *dot   = strrchr(path, '.');
    if (dot && (!slash || dot > slash)) *dot = 0;
    size_t cur = strlen(path);
    snprintf(path + cur, cap - cur, ".%s", new_ext);
}

/* Close the recording file and pop the post-record save modal so
   the user can pick a destination path + format. Caller transitions
   into UI_REC_SAVE; the actual save / discard / cancel work happens
   in the modal handlers. */
static void rec_stop(void) {
    if (!g_rec.active) return;
    double dur = GetTime() - g_rec.start_time;
    if (g_rec.fp) { fclose(g_rec.fp); g_rec.fp = NULL; }
    fprintf(stderr, "rbterm: recording stopped (%.1fs) → %s\n",
            dur, g_rec.path);
    /* Stage save-modal state. The .cast file lives at g_rec.path
       until the user picks a final destination. */
    memset(&g_rec_save, 0, sizeof(g_rec_save));
    strncpy(g_rec_save.src_path, g_rec.path, sizeof(g_rec_save.src_path) - 1);
    g_rec_save.duration_s = dur;
    g_rec_save.fmt = REC_FMT_CAST;
    /* Default destination = the temp path itself (user can edit). */
    strncpy(g_rec_save.dst_path, g_rec.path, sizeof(g_rec_save.dst_path) - 1);
    g_ui_mode = UI_REC_SAVE;
    g_rec.active = false;
    g_rec.pane = NULL;
}

/* Append a chunk of PTY-output bytes as one asciinema event row.
   No-op when no recording is active or `p` isn't the pinned pane. */
static void rec_write(Pane *p, const uint8_t *buf, size_t n) {
    if (!g_rec.active || g_rec.pane != p || !g_rec.fp || n == 0) return;
    double t = GetTime() - g_rec.start_time;
    fprintf(g_rec.fp, "[%.6f, \"o\", \"", t);
    rec_json_escape(g_rec.fp, buf, n);
    fputs("\"]\n", g_rec.fp);
    fflush(g_rec.fp);
}

static Tab *tab_open(int cols, int rows) {
    if (g_num_tabs >= MAX_TABS) return NULL;
    /* Inherit cwd from the currently-active pane so Cmd+T drops a new
       shell in the same directory the user is working in. Falls back
       to whatever the shell defaults to (HOME on Unix) for the first
       tab of the session or when OSC 7 never fired. */
    const char *inherit_cwd = NULL;
    if (g_num_tabs > 0) {
        Tab *cur_t = g_tabs[g_active];
        if (cur_t && cur_t->active_pane >= 0
            && cur_t->active_pane < cur_t->num_panes) {
            const Pane *cp = &cur_t->panes[cur_t->active_pane];
            if (cp->cwd[0]) inherit_cwd = cp->cwd;
        }
    }
    Tab *t = calloc(1, sizeof(Tab));
    t->num_panes = 1;
    t->active_pane = 0;
    t->split = SPLIT_NONE;
    t->split_ratio = 0.5f;
    if (!pane_open_local(&t->panes[0], cols, rows, inherit_cwd)) { free(t); return NULL; }
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

/* Close tab at index `idx`: free all panes, scrub stashed SSH
   creds, free the Tab struct, then shift remaining tabs down to
   keep the array dense. Clamps g_active. */
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

/* Pointer to the currently-focused pane of a tab, or NULL when the
   tab itself is NULL. Self-corrects an out-of-range active_pane. */
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
        /* New split pane inherits cwd from the other pane in the same tab. */
        const char *split_cwd = t->panes[0].cwd[0] ? t->panes[0].cwd : NULL;
        ok = pane_open_local(np, cols, rows, split_cwd);
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

/* Codepoint -> 1..4 UTF-8 bytes appended to `buf`. Caller reserves
   >= 4 bytes; returns the byte count actually written. */
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

/* Copy the active selection's cells into the system clipboard as
   UTF-8. Trims trailing whitespace per row so a column-aligned
   selection from `top` doesn't paste with stray padding. Wide-char
   continuation cells are skipped (their head cell carries the cp). */
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

static bool ui_key_down(void);

/* True when the theme's background is bright enough to count as a
   "light" colour scheme. ITU-R BT.601 luma; threshold at 50% works
   well in practice — Solarized Light is ~98%, GitHub light is
   ~96%, atelier-dune (a "lighter dark") sits around 12%. */
static bool theme_is_light(const Theme *t) {
    if (!t) return false;
    uint32_t bg = t->bg;
    int r = (bg >> 16) & 0xff;
    int g = (bg >> 8)  & 0xff;
    int b = (bg)       & 0xff;
    int y = (299 * r + 587 * g + 114 * b) / 1000;
    return y >= 128;
}

/* Active filter for both theme pickers: 0 = Dark, 1 = Light.
   Persists across opens. */
static int g_theme_filter = 0;

/* Compute the Dark / Light filter-strip rects inside `list_rect`.
   Returns the strip's pixel height — callers carve that off the
   top of the list area before drawing rows. */
static int theme_filter_strip_layout(int list_x, int list_y, int list_w,
                                     Rect *out_dark, Rect *out_light) {
    int strip_h = 26;
    int gap = 4;
    int margin = 6;
    int bw = (list_w - 2 * margin - gap) / 2;
    int by = list_y + 4;
    int bh = strip_h - 8;
    *out_dark  = (Rect){ list_x + margin,                 by, bw, bh };
    *out_light = (Rect){ list_x + margin + bw + gap,      by, bw, bh };
    return strip_h;
}

/* Render the Dark / Light filter strip. Caller draws the list
   panel first; this paints the two pill buttons on top. */
static void theme_filter_strip_draw(Font *f, Rect dark_r, Rect light_r) {
    const char *labels[2] = { "Dark", "Light" };
    Rect rects[2] = { dark_r, light_r };
    for (int i = 0; i < 2; i++) {
        bool on = (g_theme_filter == i);
        Rect rr = rects[i];
        DrawRectangle(rr.x, rr.y, rr.w, rr.h,
                      on ? (Color){46, 92, 150, 255}
                         : (Color){34, 38, 52, 255});
        DrawRectangleLines(rr.x, rr.y, rr.w, rr.h,
                           (Color){125, 207, 255, on ? 255 : 120});
        Vector2 ts = MeasureTextEx(*f, labels[i], 13, 0);
        DrawTextEx(*f, labels[i],
                   (Vector2){rr.x + (rr.w - ts.x) / 2,
                             rr.y + (rr.h - ts.y) / 2},
                   13, 0, (Color){230, 232, 240, 255});
    }
}

/* Split a theme name like "base16-3024" into category ("base16")
   and bare name ("3024") for the picker's two-column display.
   Only known multi-entry prefixes are recognised; standalone names
   ("3024", "monokai", "tempus_summer") return cat="" and bare =
   the full name. The underlying theme name (used for matching in
   saved profiles, OSC writes, etc.) is unchanged — this is purely
   a display split. Caller supplies a small `cat_buf` (>= 16 bytes)
   that holds the category string after the call; returns a pointer
   into `name` for the bare-name portion. */
static const char *theme_display_split(const char *name,
                                       char *cat_buf, size_t cat_cap) {
    if (cat_buf && cat_cap > 0) cat_buf[0] = 0;
    if (!name) return "";
    static const char *known[] = { "base16-", "dkeg-", "sexy-", NULL };
    for (int i = 0; known[i]; i++) {
        size_t n = strlen(known[i]);
        if (strncmp(name, known[i], n) == 0) {
            if (cat_buf && cat_cap > 0) {
                size_t cn = n - 1;            /* drop trailing '-' */
                if (cn >= cat_cap) cn = cat_cap - 1;
                memcpy(cat_buf, known[i], cn);
                cat_buf[cn] = 0;
            }
            return name + n;
        }
    }
    return name;
}

/* Treat a cell as part of a "word" for double-click selection:
   any non-blank, non-tab, non-empty cell. Position checks first
   so out-of-range cells don't crash. */
static bool cell_is_word(Screen *s, int col, int row) {
    if (col < 0 || row < 0 || col >= screen_cols(s) || row >= screen_rows(s)) return false;
    Cell c = screen_view_cell(s, col, row);
    if (c.cp == 0 || c.cp == ' ' || c.cp == '\t') return false;
    return true;
}

/* ----- Plain-text URL detection for Cmd/Ctrl+click. -----
 *
 * We scan the hovered row for spans that start with a known URI
 * scheme (http://, https://, ftp(s)://, ssh://, file://, git://,
 * mailto:, www.) and walk forward across URL-body characters.
 * Trailing sentence punctuation and unmatched closing brackets get
 * trimmed the same way as double-click word selection. OSC 8
 * hyperlinks are handled separately — the click handler consults
 * screen_link_url first, and falls through to this only when the
 * cell carries no link_id. */
static bool is_url_body_cp(unsigned char c) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9')) return true;
    switch (c) {
    case '-': case '_': case '.': case '/': case '?': case '#':
    case '=': case '&': case '%': case '+': case ':': case ',':
    case ';': case '@': case '~': case '!': case '*': case '$':
    case '(': case ')': case '[': case ']': case '\'':
        return true;
    }
    return false;
}

static int url_scheme_at(const char *row, int col, int cols) {
    static const char *schemes[] = {
        "https://", "http://", "ftps://", "ftp://", "ssh://",
        "file://", "git://", "mailto:", "www.", NULL
    };
    for (int i = 0; schemes[i]; i++) {
        int n = (int)strlen(schemes[i]);
        if (col + n > cols) continue;
        if (memcmp(row + col, schemes[i], (size_t)n) == 0) return n;
    }
    return 0;
}

/* If (col, row) sits inside a URL span, write start/end (inclusive)
   column positions and return true. Scans non-ASCII cells as spaces
   — URLs in the wild are ASCII. */
static bool url_at_view_pos(Screen *s, int col, int row,
                            int *start_col, int *end_col) {
    int cols = screen_cols(s);
    int rows = screen_rows(s);
    if (col < 0 || col >= cols || row < 0 || row >= rows) return false;

    char rowbuf[512];
    int ncols = cols < (int)sizeof(rowbuf) ? cols : (int)sizeof(rowbuf);
    for (int c = 0; c < ncols; c++) {
        Cell cc = screen_view_cell(s, c, row);
        uint32_t cp = cc.cp;
        if (cc.attrs & ATTR_WIDE_CONT) cp = ' ';
        rowbuf[c] = (cp < 128) ? (char)cp : ' ';
    }

    /* Walk backward from col looking for a scheme start. */
    for (int c = col; c >= 0; c--) {
        int slen = url_scheme_at(rowbuf, c, ncols);
        if (slen == 0) continue;
        /* Scheme at c. Walk forward across body chars. */
        int end = c + slen;
        while (end < ncols && is_url_body_cp((unsigned char)rowbuf[end])) end++;
        /* Trim trailing sentence punctuation. */
        const char *trail = ".,;:!?";
        while (end > c + slen && strchr(trail, rowbuf[end - 1])) end--;
        /* Trim unmatched closing brackets. */
        while (end > c + slen) {
            char last = rowbuf[end - 1];
            char want = 0;
            if      (last == ')') want = '(';
            else if (last == ']') want = '[';
            else if (last == '}') want = '{';
            else break;
            bool matched = false;
            for (int k = c; k < end - 1; k++) {
                if (rowbuf[k] == want) { matched = true; break; }
            }
            if (matched) break;
            end--;
        }
        if (col >= c && col < end) {
            *start_col = c;
            *end_col   = end - 1;
            return true;
        }
        /* Scheme matched but col is outside the span — keep walking
           left in case of a second URL earlier on the line. */
    }
    return false;
}

/* Copy the URL span into a heap buffer (caller frees). Wide-char
   continuation cells are skipped. Prepends https:// for www.*
   URLs so the OS-level opener has a real scheme. */
static char *url_copy_span(Screen *s, int row, int c0, int c1) {
    char buf[1024];
    int n = 0;
    for (int c = c0; c <= c1 && n + 4 < (int)sizeof(buf); c++) {
        Cell cc = screen_view_cell(s, c, row);
        if (cc.attrs & ATTR_WIDE_CONT) continue;
        uint32_t cp = cc.cp ? cc.cp : ' ';
        if (cp < 128) {
            buf[n++] = (char)cp;
        } else if (cp < 0x800) {
            buf[n++] = (char)(0xC0 | (cp >> 6));
            buf[n++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            buf[n++] = (char)(0xE0 | (cp >> 12));
            buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            buf[n++] = (char)(0x80 | (cp & 0x3F));
        } else {
            buf[n++] = (char)(0xF0 | (cp >> 18));
            buf[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            buf[n++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    buf[n] = 0;
    const char *prefix = (n >= 4 && memcmp(buf, "www.", 4) == 0) ? "https://" : "";
    char *out = malloc(strlen(prefix) + n + 1);
    if (!out) return NULL;
    strcpy(out, prefix);
    strcat(out, buf);
    return out;
}

/* Intelligent double-click trim rules.
 *
 * Naive word-select grabs any run of non-whitespace, which pulls in
 * trailing punctuation (`--bold,` instead of `--bold`). We post-process
 * the span to shave off boundary chars that are almost never part of
 * what the user meant to copy:
 *
 *   - trailing   , . ; : ! ?   — sentence punctuation
 *   - trailing   ) ] } > "     — closing delimiters (iff unmatched on left)
 *   - leading    ( [ { < "     — opening delimiters (iff unmatched on right)
 *
 * The leading/trailing symmetric delimiters are only trimmed when
 * unmatched, so `(x)` double-clicks to `(x)` but `foo)` trims to `foo`.
 * Keep internal `-` `_` `/` `.` etc. — they're genuinely part of
 * flags / paths / identifiers. Extend this list as new cases come in. */
static bool is_trailing_trim(uint32_t cp) {
    return cp == ',' || cp == ';' || cp == ':'
        || cp == '.' || cp == '!' || cp == '?';
}
/* True if `cp` is a closing bracket-like char that should be
   trimmed off the right side of a double-clicked word when its
   matching opener isn't inside the span. */
static bool is_close_delim(uint32_t cp) {
    return cp == ')' || cp == ']' || cp == '}' || cp == '>' || cp == '"';
}
/* Symmetric — opener that gets trimmed off the LEFT when its
   matching closer is missing on the right. */
static bool is_open_delim(uint32_t cp) {
    return cp == '(' || cp == '[' || cp == '{' || cp == '<' || cp == '"';
}
/* Map a closing delimiter to its opener (for the bracket-matching
   logic in select_word). 0 for unknown / self-pairing chars. */
static uint32_t matching_open(uint32_t close_cp) {
    switch (close_cp) {
    case ')': return '(';
    case ']': return '[';
    case '}': return '{';
    case '>': return '<';
    case '"': return '"';
    default:  return 0;
    }
}

static void select_word(Screen *s, Selection *sel, int col, int row) {
    int cols = screen_cols(s);
    int rows = screen_rows(s);
    if (!cell_is_word(s, col, row)) { sel->active = false; return; }
    int r1 = row, c1 = col;
    int r2 = row, c2 = col;

    /* Scan left, crossing auto-wrap boundaries: if the click-row starts
       with a word char and the previous row ended by auto-wrapping into
       this one, the word continues from the last column of that row. */
    for (;;) {
        while (c1 > 0 && cell_is_word(s, c1 - 1, r1)) c1--;
        if (c1 == 0 && r1 > 0 && screen_view_row_wrapped(s, r1 - 1)
            && cell_is_word(s, cols - 1, r1 - 1)) {
            r1--;
            c1 = cols - 1;
            continue;
        }
        break;
    }
    /* Symmetric scan right across wraps. */
    for (;;) {
        while (c2 < cols - 1 && cell_is_word(s, c2 + 1, r2)) c2++;
        if (c2 == cols - 1 && r2 < rows - 1 && screen_view_row_wrapped(s, r2)
            && cell_is_word(s, 0, r2 + 1)) {
            r2++;
            c2 = 0;
            continue;
        }
        break;
    }

    /* Trim trailing sentence punctuation, but never through the click
       cell itself. May walk back over a wrap boundary. */
    for (;;) {
        if (r2 == r1 && c2 == c1) break;
        if (r2 == row && c2 == col) break;
        uint32_t cp = screen_view_cell(s, c2, r2).cp;
        if (!is_trailing_trim(cp)) break;
        if (c2 > 0) c2--;
        else if (r2 > 0 && screen_view_row_wrapped(s, r2 - 1)) { r2--; c2 = cols - 1; }
        else break;
    }

    /* Matched-delimiter trim only for single-row spans — the span-scan
       across wraps above is the dominant case we want to help, and
       these trims need balanced-paren scans that aren't worth
       generalising to multi-row. */
    if (r1 == r2 && r1 == row) {
        /* Drop unmatched closing delimiter on the right (`foo)` → `foo`). */
        if (c2 > c1 && c2 != col) {
            uint32_t cp = screen_view_cell(s, c2, r2).cp;
            if (is_close_delim(cp)) {
                uint32_t want = matching_open(cp);
                bool matched = false;
                for (int k = c1; k < c2; k++) {
                    if (screen_view_cell(s, k, r2).cp == want) { matched = true; break; }
                }
                if (!matched) c2--;
            }
        }
        /* Drop unmatched opening delimiter on the left (`(foo` → `foo`). */
        if (c1 < c2 && c1 != col) {
            uint32_t cp = screen_view_cell(s, c1, r1).cp;
            if (is_open_delim(cp)) {
                uint32_t want = (cp == '"') ? '"'
                              : (cp == '(') ? ')'
                              : (cp == '[') ? ']'
                              : (cp == '{') ? '}'
                              : (cp == '<') ? '>' : 0;
                bool matched = false;
                for (int k = c1 + 1; k <= c2; k++) {
                    if (screen_view_cell(s, k, r1).cp == want) { matched = true; break; }
                }
                if (!matched) c1++;
            }
        }
    }

    sel->active = true;
    sel->dragging = false;
    sel->a_col = c1; sel->a_row = r1;
    sel->b_col = c2; sel->b_row = r2;
}

static void select_line(Screen *s, Selection *sel, int row) {
    int cols = screen_cols(s);
    int rows = screen_rows(s);
    int r1 = row, r2 = row;
    /* Extend up across rows that auto-wrapped into this one. */
    while (r1 > 0 && screen_view_row_wrapped(s, r1 - 1)) r1--;
    /* Extend down while this row auto-wrapped into the next. */
    while (r2 < rows - 1 && screen_view_row_wrapped(s, r2)) r2++;
    sel->active = true;
    sel->dragging = false;
    sel->a_col = 0; sel->a_row = r1;
    sel->b_col = cols - 1; sel->b_row = r2;
}

/* ---------- Per-pane in-grid search ---------- */

/* Drop the current match list (without freeing the heap allocations
   — those get reused on the next search). */
static void search_clear_matches(Search *s) {
    s->count = 0;
    s->current = -1;
}

/* Lowercase a 7-bit ASCII byte; leave higher bytes untouched so we
   don't accidentally collide unrelated UTF-8 bytes. Good enough for
   v1 — proper Unicode folding is future work. */
static inline uint32_t cp_fold(uint32_t cp) {
    if (cp >= 'A' && cp <= 'Z') return cp + 32;
    return cp;
}

/* Walk every absolute row of the screen, record match positions.
   Matches are within a single row; cross-wrap matches are a v2
   concern. cp_fold case-folds ASCII only. */
static void search_recompute(Pane *p) {
    Search *S = &p->search;
    search_clear_matches(S);
    if (!S->active || !S->query[0] || !p->scr) return;

    int cols = screen_cols(p->scr);
    int total_rows = screen_total_rows(p->scr);

    /* Fold the query once into a codepoint array. */
    uint32_t q[128];
    int qlen = 0;
    for (const char *qp = S->query; *qp && qlen < (int)(sizeof(q)/sizeof(q[0])); qp++) {
        q[qlen++] = cp_fold((unsigned char)*qp);
    }
    if (qlen == 0) return;

    for (int r = 0; r < total_rows; r++) {
        /* Build a per-row codepoint array for substring matching. */
        uint32_t row_cps[512];
        int row_cols = cols < (int)(sizeof(row_cps)/sizeof(row_cps[0]))
                           ? cols : (int)(sizeof(row_cps)/sizeof(row_cps[0]));
        for (int c = 0; c < row_cols; c++) {
            Cell cc = screen_cell_abs(p->scr, c, r);
            /* Skip wide-char continuation cells so multi-col glyphs
               don't create phantom positions inside the query space. */
            if (cc.attrs & ATTR_WIDE_CONT) { row_cps[c] = 0; continue; }
            row_cps[c] = cc.cp ? cp_fold(cc.cp) : ' ';
        }
        /* Scan for needle. */
        for (int c = 0; c + qlen <= row_cols; c++) {
            bool hit = true;
            for (int k = 0; k < qlen; k++) {
                if (row_cps[c + k] != q[k]) { hit = false; break; }
            }
            if (!hit) continue;
            if (S->count == S->cap) {
                int ncap = S->cap ? S->cap * 2 : 32;
                S->rows = realloc(S->rows, sizeof(*S->rows) * ncap);
                S->cols = realloc(S->cols, sizeof(*S->cols) * ncap);
                S->ends = realloc(S->ends, sizeof(*S->ends) * ncap);
                S->cap = ncap;
            }
            S->rows[S->count] = r;
            S->cols[S->count] = c;
            S->ends[S->count] = c + qlen;
            S->count++;
        }
    }
    if (S->count > 0) S->current = 0;
}

/* Scroll the screen view so match `idx` is visible, roughly centred.
   abs_row indexes full history (scrollback + live). */
static void search_scroll_to_match(Pane *p, int idx) {
    Search *S = &p->search;
    if (!p->scr || idx < 0 || idx >= S->count) return;
    int abs_row = S->rows[idx];
    int rows = screen_rows(p->scr);
    int sb_len = screen_scrollback_len(p->scr);
    /* view_off = rows of scrollback pulled into top of view.
       abs_row = sb_len + vy - view_off  (derivation: view row vy
       shows abs_row sb_len+vy when view_off==0; each extra view_off
       shifts the whole window back by one row). Solve for view_off
       with vy = rows/2 to centre. */
    int want = sb_len + rows / 2 - abs_row;
    if (want < 0) want = 0;
    if (want > sb_len) want = sb_len;
    /* If the match is already visible with view_off=0, leave it alone. */
    int cur_off = screen_view_offset(p->scr);
    int cur_vy = abs_row - sb_len + cur_off;
    if (cur_vy >= 0 && cur_vy < rows) return;
    /* Otherwise centre. */
    screen_scroll_reset(p->scr);
    if (want > 0) screen_scroll_view(p->scr, want);
}

/* Advance the current-match index by `delta` (+1 = next, -1 =
   prev). Wraps around. Triggers a viewport scroll so the new
   current match is visible. */
static void search_next(Pane *p, int delta) {
    Search *S = &p->search;
    if (S->count == 0) return;
    int n = S->count;
    int i = S->current < 0 ? 0 : (S->current + delta) % n;
    if (i < 0) i += n;
    S->current = i;
    search_scroll_to_match(p, i);
}

/* Open the search bar on a pane. Resets the query, caret, and
   selection state so the bar starts fresh — caller is expected to
   have already saved any view state they want to preserve. */
static void search_open(Pane *p) {
    if (!p) return;
    p->search.active = true;
    p->search.query[0] = 0;
    p->search.caret = 0;
    p->search.sel_anchor = -1;
    p->search.mouse_down = false;
    search_clear_matches(&p->search);
}

/* Close the search bar and snap the viewport back to the live
   bottom (matches iTerm2's "Esc-out reverts scroll" behaviour). */
static void search_close(Pane *p) {
    if (!p) return;
    p->search.active = false;
    p->search.query[0] = 0;
    p->search.caret = 0;
    p->search.sel_anchor = -1;
    p->search.mouse_down = false;
    search_clear_matches(&p->search);
    if (p->scr) screen_scroll_reset(p->scr);
}

/* ----- Search-bar query-text helpers (caret / selection / editing) ----- */

/* True when the search bar has a non-empty range selection (caret
   and sel_anchor differ). */
static bool search_has_sel(const Search *S) {
    return S->sel_anchor >= 0 && S->sel_anchor != S->caret;
}
/* Lower bound (inclusive) of the search-bar selection range, or
   the caret position when nothing is selected. */
static int search_sel_lo(const Search *S) {
    if (!search_has_sel(S)) return S->caret;
    return S->sel_anchor < S->caret ? S->sel_anchor : S->caret;
}
/* Upper bound (exclusive) of the search-bar selection range. */
static int search_sel_hi(const Search *S) {
    if (!search_has_sel(S)) return S->caret;
    return S->sel_anchor > S->caret ? S->sel_anchor : S->caret;
}
/* Drop any active range selection in the search bar. */
static void search_clear_sel(Search *S) { S->sel_anchor = -1; }
/* Delete the selected substring from the query and snap the caret
   to the start of the deletion. No-op when nothing's selected. */
static void search_delete_sel(Search *S) {
    if (!search_has_sel(S)) return;
    int lo = search_sel_lo(S), hi = search_sel_hi(S);
    int len = (int)strlen(S->query);
    memmove(S->query + lo, S->query + hi, (size_t)(len - hi));
    S->query[len - (hi - lo)] = 0;
    S->caret = lo;
    search_clear_sel(S);
}
/* Insert one ASCII char at the caret (replacing any selection).
   Returns false if the query buffer is full. */
static bool search_insert_char(Search *S, char c) {
    if (search_has_sel(S)) search_delete_sel(S);
    int len = (int)strlen(S->query);
    if (len + 1 >= (int)sizeof(S->query)) return false;
    memmove(S->query + S->caret + 1, S->query + S->caret,
            (size_t)(len - S->caret + 1));
    S->query[S->caret] = c;
    S->caret++;
    return true;
}

/* Map a mouse-x (relative to the query's left edge) to a caret
   position. We measure every possible prefix and snap to the nearest.
   Query is short (<128), O(n²) work per click is fine. */
static int search_x_to_caret(Font f, const char *q, int rel_x) {
    if (rel_x <= 0) return 0;
    int len = (int)strlen(q);
    int best = 0;
    float best_d = 1e9f;
    char buf[128];
    for (int i = 0; i <= len && i < (int)sizeof(buf); i++) {
        memcpy(buf, q, (size_t)i);
        buf[i] = 0;
        Vector2 sz = MeasureTextEx(f, buf, 13, 0);
        float d = sz.x - (float)rel_x; if (d < 0) d = -d;
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

/* Convert a caret position (byte index into `q`) to its pixel
   x-offset, by measuring the prefix string. Used for selection
   highlight + caret blink draw. */
static int search_caret_to_x(Font f, const char *q, int caret) {
    char buf[128];
    int n = caret > 127 ? 127 : caret;
    if (n < 0) n = 0;
    memcpy(buf, q, (size_t)n);
    buf[n] = 0;
    Vector2 sz = MeasureTextEx(f, buf, 13, 0);
    return (int)sz.x;
}

/* Search-bar screen-space rect for a pane. Kept in sync with the
   drawing code in draw_tab_contents. */
#define SEARCH_BAR_XPAD 8
#define SEARCH_BAR_YPAD 6
#define SEARCH_BAR_H    30
#define SEARCH_TEXT_OFF 54   /* x offset from bar left to query text */

/* Compute the search-bar rect (top-of-pane dock) given the pane's
   position + width. Shared between draw and click hit-test so they
   stay in sync. */
static void search_bar_rect(int pane_x, int pane_y, int pane_w,
                            int *out_x, int *out_y, int *out_w, int *out_h) {
    *out_x = pane_x + SEARCH_BAR_XPAD;
    *out_y = pane_y + SEARCH_BAR_YPAD;
    *out_w = pane_w - 2 * SEARCH_BAR_XPAD;
    *out_h = SEARCH_BAR_H;
}

/* Route a frame's worth of key input to the search bar. Called in
   place of input_poll when the active pane's search bar is open so
   keystrokes don't leak into the shell. */
static void search_handle_input(Pane *p) {
    if (!p || !p->search.active) return;
    Search *S = &p->search;
    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    bool mod   = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)
#if defined(__APPLE__)
              || IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER)
#endif
              ;

    /* Mouse-wheel and Ctrl+Shift+Up/Down/PageUp/PageDown scroll the
       scrollback even while the search bar owns the keyboard — users
       often want to glance at nearby context without closing search. */
    if (p->scr) {
        float wheel = GetMouseWheelMove();
        if (wheel != 0.0f) screen_scroll_view(p->scr, (int)(wheel * 3.0f));
        bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
        if (ctrl && shift && IsKeyPressed(KEY_UP))    screen_scroll_view(p->scr, +1);
        if (ctrl && shift && IsKeyPressed(KEY_DOWN))  screen_scroll_view(p->scr, -1);
        if (shift && IsKeyPressed(KEY_PAGE_UP))       screen_scroll_view(p->scr, +screen_rows(p->scr) - 1);
        if (shift && IsKeyPressed(KEY_PAGE_DOWN))     screen_scroll_view(p->scr, -screen_rows(p->scr) + 1);
    }

    if (IsKeyPressed(KEY_ESCAPE)) { search_close(p); return; }
    bool step_next = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) ||
                     IsKeyPressed(KEY_DOWN)  || IsKeyPressed(KEY_F3);
    if (step_next) { search_next(p, shift ? -1 : +1); return; }
    if (IsKeyPressed(KEY_UP)) { search_next(p, -1); return; }

    /* Cmd/Ctrl+A: select the whole query (anchor at 0, caret at end). */
    if (mod && IsKeyPressed(KEY_A)) {
        int len = (int)strlen(S->query);
        if (len > 0) { S->sel_anchor = 0; S->caret = len; }
        return;
    }
    /* Cmd/Ctrl+C: a mouse-drag selection on the grid wins (the user
       just highlighted terminal text). Else copy the search-bar
       selection if there is one, else the whole query. */
    if (mod && IsKeyPressed(KEY_C)) {
        if (p->sel.active && !p->sel.dragging) {
            copy_selection(p->scr, &p->sel);
        } else if (search_has_sel(S)) {
            int lo = search_sel_lo(S), hi = search_sel_hi(S);
            char buf[128];
            int n = hi - lo;
            if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
            memcpy(buf, S->query + lo, (size_t)n);
            buf[n] = 0;
            if (n > 0) SetClipboardText(buf);
        } else if (S->query[0]) {
            SetClipboardText(S->query);
        }
        return;
    }
    /* Caret movement. Shift isn't plumbed through to keyboard
       selection in v1 — shift-click on the bar is the path for
       partial selection. */
    if (IsKeyPressed(KEY_LEFT) || IsKeyPressedRepeat(KEY_LEFT)) {
        if (search_has_sel(S)) { S->caret = search_sel_lo(S); search_clear_sel(S); }
        else if (S->caret > 0) S->caret--;
        return;
    }
    if (IsKeyPressed(KEY_RIGHT) || IsKeyPressedRepeat(KEY_RIGHT)) {
        int len = (int)strlen(S->query);
        if (search_has_sel(S)) { S->caret = search_sel_hi(S); search_clear_sel(S); }
        else if (S->caret < len) S->caret++;
        return;
    }
    if (IsKeyPressed(KEY_HOME)) { S->caret = 0; search_clear_sel(S); return; }
    if (IsKeyPressed(KEY_END))  { S->caret = (int)strlen(S->query); search_clear_sel(S); return; }

    bool query_changed = false;
    /* Cmd/Ctrl+V: paste clipboard at caret (replacing any selection),
       filtered to printable ASCII. */
    if (mod && IsKeyPressed(KEY_V)) {
        const char *clip = GetClipboardText();
        if (clip && *clip) {
            if (search_has_sel(S)) search_delete_sel(S);
            for (const char *q = clip; *q; q++) {
                unsigned char c = (unsigned char)*q;
                if (c == '\r' || c == '\n' || c == '\t') continue;
                if (c < 32 || c >= 127) continue;
                if (!search_insert_char(S, (char)c)) break;
            }
            query_changed = true;
        }
    }
    if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) {
        if (search_has_sel(S)) { search_delete_sel(S); query_changed = true; }
        else if (S->caret > 0) {
            int len = (int)strlen(S->query);
            memmove(S->query + S->caret - 1, S->query + S->caret,
                    (size_t)(len - S->caret + 1));
            S->caret--;
            query_changed = true;
        }
    }
    if (IsKeyPressed(KEY_DELETE) || IsKeyPressedRepeat(KEY_DELETE)) {
        if (search_has_sel(S)) { search_delete_sel(S); query_changed = true; }
        else {
            int len = (int)strlen(S->query);
            if (S->caret < len) {
                memmove(S->query + S->caret, S->query + S->caret + 1,
                        (size_t)(len - S->caret));
                query_changed = true;
            }
        }
    }
    int cp;
    while ((cp = GetCharPressed()) != 0) {
        if (mod) continue;                  /* ignore chars from chord presses */
        if (cp < 32 || cp >= 127) continue; /* ASCII only for v1 */
        if (search_insert_char(S, (char)cp)) query_changed = true;
    }
    if (query_changed) {
        search_recompute(p);
        if (S->count > 0) search_scroll_to_match(p, 0);
    }
}

/* Label shown on a tab. Preference order:
 *   1. p->title (OSC 0 / OSC 2 from the running program) if set
 *   2. p->cwd basename with HOME rewritten to "~"
 *   3. "shell"
 *
 * Most shell prompts write OSC 0/2 with the cwd on every prompt, so
 * title-first still shows the cwd at an idle prompt. Programs that
 * explicitly set a title (tmux, vim, ssh foo, `ansi --title=wow`)
 * surface until the shell's next prompt rewrites it. */
static const char *tab_label(const Tab *t) {
    const Pane *p = &t->panes[t->active_pane];
    /* SSH tabs: prefix with the target so two remote hosts don't look
       identical in the bar. Show "user@host title" when a title has
       been set, or "user@host cwd-basename" otherwise. */
    if (t->is_ssh) {
        static char buf[MAX_TABS][320];
        static int  slot = 0;
        int idx = -1;
        for (int i = 0; i < g_num_tabs; i++) if (g_tabs[i] == t) { idx = i; break; }
        if (idx < 0) idx = slot++ % MAX_TABS;
        char *out = buf[idx];
        if (p->title[0]) {
            snprintf(out, sizeof(buf[0]), "%s %s", t->ssh_target, p->title);
            return out;
        }
        if (!p->cwd[0]) {
            return t->ssh_target;
        }
        const char *dir_label = p->cwd;
        const char *b = strrchr(p->cwd, '/');
        if (strcmp(p->cwd, "/") == 0) dir_label = "/";
        else if (b) dir_label = (*(b + 1) ? b + 1 : b);
        snprintf(out, sizeof(buf[0]), "%s %s", t->ssh_target, dir_label);
        return out;
    }
    if (p->title[0]) return p->title;
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
    return "shell";
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
    Vector2 mpos = GetMousePosition();
    bool want_url_cursor = false;
    for (int pi = 0; pi < t->num_panes; pi++) {
        Pane *p = &t->panes[pi];
        if (!p->scr) continue;
        PaneRect pr;
        pane_rect(t, pi, win_w, win_h, &pr);
        bool pane_focused = focused && (pi == t->active_pane);
        /* Hover coords in viewport cells for this pane. -1 when the
           cursor is outside the pane so OSC 8 highlight doesn't leak. */
        int hcol = -1, hrow = -1;
        int rel_x = (int)mpos.x - pr.x - r->pad_x;
        int rel_y = (int)mpos.y - pr.y - r->pad_y;
        if (rel_x >= 0 && rel_y >= 0 && r->cell_w > 0 && r->cell_h > 0) {
            int c = rel_x / r->cell_w;
            int rr = rel_y / r->cell_h;
            if (c >= 0 && c < screen_cols(p->scr)
                && rr >= 0 && rr < screen_rows(p->scr)) {
                hcol = c; hrow = rr;
            }
        }
        /* Paint the pane's full rect with the screen's default
           background first. The renderer only covers cols*cw + pad
           and rows*ch + pad — when the window height isn't a clean
           multiple of cell_h, a thin sliver at the bottom would
           otherwise show the window's black ClearBackground. */
        {
            uint32_t bg = screen_default_bg(p->scr);
            Color bgc = { (unsigned char)((bg >> 16) & 0xff),
                          (unsigned char)((bg >> 8)  & 0xff),
                          (unsigned char)( bg        & 0xff), 255 };
            DrawRectangle(pr.x, pr.y, pr.w, pr.h, bgc);
        }
        renderer_draw(r, p->scr, time_sec, pane_focused, &p->sel,
                      pr.x, pr.y, hcol, hrow);

        /* Cmd/Ctrl-hover over a plain-text URL: tint the URL cells
           and paint a 2px underline, and switch the mouse cursor to
           a pointer so the user knows it's clickable. OSC 8
           hyperlinks are already brightened by renderer_draw via the
           link_id path. */
        if (ui_key_down() && hcol >= 0 && hrow >= 0) {
            int uc0, uc1;
            Cell hc = screen_view_cell(p->scr, hcol, hrow);
            bool has_osc8 = (hc.link_id != 0);
            if (!has_osc8 && url_at_view_pos(p->scr, hcol, hrow, &uc0, &uc1)) {
                int ux = pr.x + r->pad_x + uc0 * r->cell_w;
                int uw = (uc1 - uc0 + 1) * r->cell_w;
                int uy_top = pr.y + r->pad_y + hrow * r->cell_h;
                int uy_bot = uy_top + r->cell_h - 2;
                /* Translucent blue tint behind the text — unambiguous
                   "this is a link" cue even on dense displays. */
                DrawRectangle(ux, uy_top, uw, r->cell_h,
                              (Color){60, 120, 200, 70});
                /* 2px underline at cell bottom. */
                DrawRectangle(ux, uy_bot, uw, 2,
                              (Color){125, 207, 255, 255});
                want_url_cursor = true;
            } else if (has_osc8) {
                want_url_cursor = true;
            }
        }

        /* Search match highlights. Drawn after the grid so they sit
           over glyphs; alpha is low enough that text reads through.
           The current match gets a brighter fill to stand out. */
        if (p->search.count > 0) {
            int cols_p = screen_cols(p->scr);
            int rows_p = screen_rows(p->scr);
            int sb = screen_scrollback_len(p->scr);
            int off = screen_view_offset(p->scr);
            for (int i = 0; i < p->search.count; i++) {
                int abs_row = p->search.rows[i];
                int vy = abs_row - sb + off;
                if (vy < 0 || vy >= rows_p) continue;
                int c0 = p->search.cols[i];
                int c1 = p->search.ends[i];
                if (c0 < 0) c0 = 0;
                if (c1 > cols_p) c1 = cols_p;
                if (c1 <= c0) continue;
                bool is_cur = (i == p->search.current);
                Color col = is_cur ? (Color){255, 210, 0, 180}
                                   : (Color){255, 210, 0, 100};
                DrawRectangle(pr.x + r->pad_x + c0 * r->cell_w,
                              pr.y + r->pad_y + vy * r->cell_h,
                              (c1 - c0) * r->cell_w, r->cell_h, col);
            }
        }

        /* Match map: thin strip on the right edge of the pane showing
           where every match sits in the full buffer (scrollback +
           live). Ticks are positioned by abs_row / total_rows, and
           the current viewport range is shaded so the user can see
           where they are vs the hits. Only visible while search is
           open. */
        if (p->search.active && p->search.count > 0) {
            int total = screen_total_rows(p->scr);
            if (total > 0) {
                int rows_p = screen_rows(p->scr);
                int sb = screen_scrollback_len(p->scr);
                int off = screen_view_offset(p->scr);
                int strip_w = 8;
                int strip_x = pr.x + pr.w - r->pad_x - strip_w;
                int strip_y = pr.y + r->pad_y;
                int strip_h = rows_p * r->cell_h;
                DrawRectangle(strip_x, strip_y, strip_w, strip_h,
                              (Color){24, 27, 36, 180});
                /* Viewport range: translucent fill showing which
                   abs_rows are currently on screen. */
                int top_abs = sb - off;
                int bot_abs = top_abs + rows_p;
                if (top_abs < 0) top_abs = 0;
                if (bot_abs > total) bot_abs = total;
                int yt = strip_y + (int)((float)top_abs / (float)total * (float)strip_h);
                int yb = strip_y + (int)((float)bot_abs / (float)total * (float)strip_h);
                if (yb <= yt) yb = yt + 1;
                DrawRectangle(strip_x, yt, strip_w, yb - yt,
                              (Color){70, 100, 140, 110});
                /* Match ticks. */
                for (int i = 0; i < p->search.count; i++) {
                    int abs_row = p->search.rows[i];
                    int ty = strip_y + (int)((float)abs_row / (float)total
                                             * (float)strip_h);
                    bool is_cur = (i == p->search.current);
                    Color col = is_cur ? (Color){255, 140, 0, 255}
                                       : (Color){255, 210, 0, 200};
                    int tick_h = is_cur ? 3 : 2;
                    int tick_w = is_cur ? strip_w : strip_w - 2;
                    DrawRectangle(strip_x + (strip_w - tick_w),
                                  ty - tick_h / 2,
                                  tick_w, tick_h, col);
                }
                DrawRectangleLines(strip_x, strip_y, strip_w, strip_h,
                                   (Color){80, 90, 110, 200});
            }
        }

        /* Search bar (docked top of pane). */
        if (p->search.active) {
            int bx, by, bw, bh;
            search_bar_rect(pr.x, pr.y, pr.w, &bx, &by, &bw, &bh);
            DrawRectangle(bx, by, bw, bh, (Color){28, 32, 44, 240});
            DrawRectangleLines(bx, by, bw, bh,
                               pane_focused ? (Color){125, 207, 255, 220}
                                            : (Color){90, 100, 120, 180});
            Font *ff = (Font *)r->font_data;
            DrawTextEx(*ff, "Find:",
                       (Vector2){bx + 10, by + (bh - 13) / 2},
                       13, 0, (Color){180, 190, 210, 255});
            int tx = bx + SEARCH_TEXT_OFF;
            /* Range selection highlight behind the query. */
            if (search_has_sel(&p->search)) {
                int lo = search_sel_lo(&p->search);
                int hi = search_sel_hi(&p->search);
                int xlo = tx + search_caret_to_x(*ff, p->search.query, lo);
                int xhi = tx + search_caret_to_x(*ff, p->search.query, hi);
                if (xhi > xlo) {
                    DrawRectangle(xlo - 1, by + (bh - 16) / 2,
                                  xhi - xlo + 2, 16,
                                  (Color){64, 100, 150, 200});
                }
            }
            DrawTextEx(*ff, p->search.query,
                       (Vector2){tx, by + (bh - 13) / 2},
                       13, 0, (Color){230, 232, 240, 255});
            /* Right-aligned count / status. */
            char cbuf[40];
            if (p->search.count > 0) {
                snprintf(cbuf, sizeof(cbuf), "%d / %d",
                         p->search.current + 1, p->search.count);
            } else if (p->search.query[0]) {
                snprintf(cbuf, sizeof(cbuf), "no matches");
            } else {
                cbuf[0] = 0;
            }
            if (cbuf[0]) {
                Vector2 csz = MeasureTextEx(*ff, cbuf, 13, 0);
                DrawTextEx(*ff, cbuf,
                           (Vector2){bx + bw - 12 - csz.x, by + (bh - 13) / 2},
                           13, 0, (Color){180, 190, 210, 255});
            }
            /* Blinking caret at its current position. Hidden while a
               range selection is active; it'd overlap the highlight. */
            if (!search_has_sel(&p->search) &&
                ((long long)(GetTime() * 2.0) & 1) == 0) {
                int cx = tx + search_caret_to_x(*ff, p->search.query, p->search.caret);
                DrawRectangle(cx, by + (bh - 16) / 2, 2, 16,
                              (Color){125, 207, 255, 255});
            }
        }
    }
    PaneRect sp;
    if (splitter_rect(t, win_w, win_h, &sp)) {
        DrawRectangle(sp.x, sp.y, sp.w, sp.h, (Color){60, 60, 75, 255});
    }
    SetMouseCursor(want_url_cursor ? MOUSE_CURSOR_POINTING_HAND
                                   : MOUSE_CURSOR_DEFAULT);
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
    bool on_rec_start;     /* start asciinema recording */
    bool on_rec_stop;      /* stop asciinema recording */
} TabBarHit;

/* Split buttons are hidden when the active tab is already split — there's
   nothing useful clicking them would do, and the tabs reclaim the space. */
static bool split_buttons_visible(void) {
    if (g_active < 0 || g_active >= g_num_tabs) return true;
    Tab *t = g_tabs[g_active];
    return t && t->split == SPLIT_NONE;
}

/* Compute the per-tab pixel width in the tab bar from the
   available room (window width minus the SSH/help/+/-/split
   buttons). Splits the leftover width evenly across the open
   tabs, clamped to TAB_MIN_W..TAB_MAX_W. */
static int tab_width_for(int win_w) {
    int split_w = split_buttons_visible() ? 2 * TAB_SPLIT_W : 0;
    int avail = win_w - TAB_PLUS_W - TAB_GEAR_W - 2 * TAB_REC_W
                - TAB_HELP_W - split_w - TAB_SSH_W;
    if (g_num_tabs <= 0) return TAB_MIN_W;
    int w = avail / g_num_tabs;
    if (w > TAB_MAX_W) w = TAB_MAX_W;
    if (w < TAB_MIN_W) w = TAB_MIN_W;
    return w;
}

/* Layout: [ssh] | tab1 | tab2 | ... | [gear] [split-v] [split-h] [?] | [+]
   The split pair disappears entirely when the active tab is split. */
static TabBarHit tab_bar_hit_test(int win_w, int mx, int my) {
    TabBarHit h = { -1, false, false, false, false, false, false, false, false, false };
    if (my < 0 || my >= TAB_BAR_H) return h;
    bool show_splits = split_buttons_visible();
    int plus_x      = win_w - TAB_PLUS_W;
    int help_x      = plus_x - TAB_HELP_W;
    int split_h_x   = show_splits ? help_x - TAB_SPLIT_W     : help_x;
    int split_v_x   = show_splits ? split_h_x - TAB_SPLIT_W  : help_x;
    int rec_stop_x  = split_v_x - TAB_REC_W;
    int rec_start_x = rec_stop_x - TAB_REC_W;
    int gear_x      = rec_start_x - TAB_GEAR_W;
    int tab_start = TAB_SSH_W;
    if (mx < TAB_SSH_W)     { h.on_ssh     = true; return h; }
    if (mx >= plus_x)       { h.on_plus    = true; return h; }
    if (mx >= help_x)       { h.on_help    = true; return h; }
    if (show_splits && mx >= split_h_x) { h.on_split_h = true; return h; }
    if (show_splits && mx >= split_v_x) { h.on_split_v = true; return h; }
    if (mx >= rec_stop_x)   { h.on_rec_stop  = true; return h; }
    if (mx >= rec_start_x)  { h.on_rec_start = true; return h; }
    if (mx >= gear_x)       { h.on_gear      = true; return h; }
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

/* Render the settings-button gear icon: outer ring, inner hole, and
   the cog teeth. Pure shape primitives — no texture / glyph. */
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

/* Paint the strip across the very top of the window: SSH-connect
   button on the left, then per-tab buttons (with activity dot or
   command-running spinner where applicable), then the gear (settings),
   help (?), help/split icons on the right. */
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

    /* Right-cluster layout (mirrors tab_bar_hit_test):
         [REC mm:ss?] [gear] [rec ●] [rec ■] [split-v?] [split-h?] [?] [+]
       Compute every x from right to left so it stays in sync. The
       REC pill renders only while a recording is active. */
    bool show_splits = split_buttons_visible();
    int split_h_x   = show_splits ? help_x - TAB_SPLIT_W    : help_x;
    int split_v_x   = show_splits ? split_h_x - TAB_SPLIT_W : help_x;
    int rec_stop_x  = split_v_x - TAB_REC_W;
    int rec_start_x = rec_stop_x - TAB_REC_W;
    int gear_x      = rec_start_x - TAB_GEAR_W;

    /* REC pill: shows while a recording is active. Sits just left
       of the gear and counts up from the recording's start time.
       The first ~1.5 s pulses brighter so users can tell recording
       actually engaged. Layout doesn't reserve space when idle —
       on-recording it overdraws the whitespace between tabs and
       the gear, which is fine since we never run that close on a
       reasonable window. */
    if (g_rec.active) {
        double elapsed = GetTime() - g_rec.start_time;
        int  mins = (int)(elapsed / 60.0);
        int  secs = (int)(elapsed - mins * 60);
        char label[32];
        snprintf(label, sizeof(label), "REC %d:%02d", mins, secs);
        Vector2 lsz = MeasureTextEx(*f, label, 13, 0);
        int pill_pad = 8;
        int dot_w = 8;
        int pill_w = (int)lsz.x + dot_w + 12 + pill_pad * 2;
        int pill_x = gear_x - pill_w - 6;
        if (pill_x < TAB_SSH_W + 4) pill_x = TAB_SSH_W + 4;
        bool pulse = elapsed < 1.5;
        bool blink = ((long long)(GetTime() * 2.0) & 1) == 0;
        Color pill_bg = pulse ? (Color){70, 28, 28, 255} : (Color){52, 22, 22, 255};
        Color pill_outline = (Color){200, 80, 80, 220};
        Color dot_col = (pulse || blink) ? (Color){255, 80, 80, 255}
                                         : (Color){180, 60, 60, 255};
        Color text_col = (Color){240, 200, 200, 255};
        DrawRectangle(pill_x, 4, pill_w, TAB_BAR_H - 8, pill_bg);
        DrawRectangleLines(pill_x, 4, pill_w, TAB_BAR_H - 8, pill_outline);
        DrawCircle(pill_x + pill_pad + dot_w / 2,
                   TAB_BAR_H / 2, dot_w / 2.0f, dot_col);
        DrawTextEx(*f, label,
                   (Vector2){pill_x + pill_pad + dot_w + 6,
                             (TAB_BAR_H - lsz.y) / 2.0f},
                   13, 0, text_col);
    }

    /* Gear (settings). */
    Color gear_bg = (Color){38, 48, 66, 255};
    DrawRectangle(gear_x, 0, TAB_GEAR_W, TAB_BAR_H, gear_bg);
    DrawRectangleLines(gear_x, 2, TAB_GEAR_W - 1, TAB_BAR_H - 4,
                       (Color){125, 207, 255, 200});
    draw_gear_icon(gear_x + TAB_GEAR_W / 2.0f, TAB_BAR_H / 2.0f,
                   TAB_BAR_H * 0.55f, (Color){220, 235, 255, 255}, gear_bg);

    /* Recording start/stop buttons. Start is a red disc (dim when
       a recording is already running); Stop is a red square (dim
       when no recording is active). */
    {
        Color rec_bg = (Color){38, 48, 66, 255};
        Color rec_outline = (Color){125, 207, 255, 200};
        bool armed = !g_rec.active;
        Color start_col = armed ? (Color){230, 80, 80, 255}
                                : (Color){120, 60, 60, 200};
        DrawRectangle(rec_start_x, 0, TAB_REC_W, TAB_BAR_H, rec_bg);
        DrawRectangleLines(rec_start_x, 2, TAB_REC_W - 1, TAB_BAR_H - 4, rec_outline);
        DrawCircle(rec_start_x + TAB_REC_W / 2, TAB_BAR_H / 2, 6.0f, start_col);

        bool can_stop = g_rec.active;
        Color stop_col = can_stop ? (Color){230, 80, 80, 255}
                                  : (Color){120, 60, 60, 200};
        DrawRectangle(rec_stop_x, 0, TAB_REC_W, TAB_BAR_H, rec_bg);
        DrawRectangleLines(rec_stop_x, 2, TAB_REC_W - 1, TAB_BAR_H - 4, rec_outline);
        int sq = 10;
        DrawRectangle(rec_stop_x + (TAB_REC_W - sq) / 2,
                      (TAB_BAR_H - sq) / 2, sq, sq, stop_col);
    }

    /* Split buttons — vertical (side-by-side) then horizontal
       (top/bottom). Hidden once the active tab is already split. */
    if (show_splits) {
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

        /* Per-tab status glyph left of the label:
             - Spinner (cyan) while any pane has a command running
               (OSC 133;C without a matching D).
             - Activity dot (amber) for backgrounded tabs that have
               produced output since the user last focused them.
           Spinner takes precedence — a running command is the more
           informative signal. */
        int label_x = x + 10;
        bool any_running = false;
        for (int pi = 0; pi < g_tabs[i]->num_panes; pi++) {
            const Pane *_pp = &g_tabs[i]->panes[pi];
            if (_pp->scr && screen_command_running(_pp->scr)) {
                any_running = true;
                break;
            }
        }
        if (any_running) {
            /* Plain-ASCII 4-frame spinner — `|/-\` renders in every
               monospace font (braille / spinner glyphs aren't always
               in SF Mono / Consolas, and the missing-glyph fallback
               drew "?" instead). */
            static const char *frames[] = { "|", "/", "-", "\\" };
            int frame_idx = ((int)(GetTime() * 8.0)) % 4;
            if (frame_idx < 0) frame_idx += 4;
            const char *gly = frames[frame_idx];
            Color sp = {125, 207, 255, 255};
            Vector2 sz = MeasureTextEx(*f, gly, fs, 0);
            DrawTextEx(*f, gly,
                       (Vector2){label_x, (TAB_BAR_H - sz.y) / 2.0f},
                       fs, 0, sp);
            label_x += (int)sz.x + 6;
        } else if (!active && g_tabs[i]->activity) {
            int dy = TAB_BAR_H / 2;
            int dx = x + 10;
            DrawCircle(dx + 3, dy, 3.0f, (Color){255, 180, 60, 255});
            label_x = dx + 12;
        }
        const char *title = tab_label(g_tabs[i]);
        BeginScissorMode(label_x - 2, 0, tw - TAB_CLOSE_W - (label_x - x) - 4, TAB_BAR_H);
        Vector2 tsz = MeasureTextEx(*f, title, fs, 0);
        Vector2 tp  = { label_x, (TAB_BAR_H - tsz.y) / 2.0f };
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

/* Trim trailing whitespace + CR/LF from a NUL-terminated string. */
static void trim_end(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t' ||
                     s[n-1] == '\r' || s[n-1] == '\n'))
        s[--n] = 0;
}

/* Parse ~/.ssh/config into g_ssh_profiles. Reads HostName / User /
   Port / IdentityFile + the rbterm-specific # comment fields used
   by the SSH form. Wildcard stanzas (`Host *`, `Host !foo`) are
   skipped. Called every time the SSH form opens so live edits to
   the user's config show up without restarting rbterm. */
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

static void form_undo_clear_all(void);

/* Pre-fill the SSH connect form from a saved-host profile (or
   clear it if `prof` is NULL). Called when the user clicks a row
   in the saved-hosts sidebar. */
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
    form_undo_clear_all();
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
    form_undo_clear_all();
}

static void fonts_load(const char *current_path);

/* Open the SSH connect modal. Reloads ~/.ssh/config + the font
   list (so the per-host font picker is populated) and clears any
   leftover form state. */
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

/* Per-field undo / redo for the SSH form. Seven stacks: the six main
   text fields plus the per-host log-dir input. Each snapshot is a
   strdup'd copy of the field's value BEFORE a mutation. Cmd/Ctrl+Z
   pops undo and pushes the current value to redo; Cmd/Ctrl+Shift+Z
   is the inverse. Stacks are cleared on form open, New / clear, and
   apply-profile since those overwrite all fields. */
#define UNDO_CAP 64
#define UNDO_SLOT_COUNT (F_TEXT_FIELDS + 1)
#define UNDO_SLOT_LOGDIR F_TEXT_FIELDS
typedef struct {
    char *items[UNDO_CAP];
    int   count;
} UndoStack;
static UndoStack g_form_undo[UNDO_SLOT_COUNT];
static UndoStack g_form_redo[UNDO_SLOT_COUNT];

/* Free every snapshot in an undo stack and reset its count. */
static void undo_stack_clear(UndoStack *st) {
    for (int i = 0; i < st->count; i++) { free(st->items[i]); st->items[i] = NULL; }
    st->count = 0;
}

/* Push a strdup'd snapshot of `val` onto the stack. Deduplicates
   against the top so rejected / no-op edits don't pile up; evicts
   the oldest when the stack is full (cap = UNDO_CAP). */
static void undo_stack_push(UndoStack *st, const char *val) {
    /* Dedup against the top so rejected / no-op edits don't pile up. */
    if (st->count > 0 && strcmp(st->items[st->count - 1], val) == 0) return;
    if (st->count == UNDO_CAP) {
        free(st->items[0]);
        memmove(st->items, st->items + 1, sizeof(st->items[0]) * (UNDO_CAP - 1));
        st->count = UNDO_CAP - 1;
    }
    st->items[st->count++] = strdup(val);
}

static char *undo_stack_pop(UndoStack *st) {
    if (st->count == 0) return NULL;
    return st->items[--st->count];
}

/* Reset both undo and redo stacks for every SSH-form text field.
   Called on form open / clear / apply-profile, since those events
   wipe the whole form and any in-progress undo history is moot. */
static void form_undo_clear_all(void) {
    for (int i = 0; i < UNDO_SLOT_COUNT; i++) {
        undo_stack_clear(&g_form_undo[i]);
        undo_stack_clear(&g_form_redo[i]);
    }
}

static char *form_undo_slot_buf(int slot, size_t *cap) {
    if (slot == UNDO_SLOT_LOGDIR) { *cap = sizeof(g_form.log_dir); return g_form.log_dir; }
    return form_buf(F_NAME + slot, cap);
}

/* Map the currently-focused SSH-form text field to its undo-stack
   slot index. -1 when keyboard focus is on a non-text widget
   (button / list / dropdown). */
static int form_undo_current_slot(void) {
    if (g_form_logdir_focus) return UNDO_SLOT_LOGDIR;
    if (g_form.focus >= F_NAME && g_form.focus < F_NAME + F_TEXT_FIELDS)
        return g_form.focus - F_NAME;
    return -1;
}

/* Call before applying a mutation: snapshots the current value onto
   the undo stack and drops the redo stack. Dedup in push means it's
   safe to call even if the mutation is ultimately rejected. */
static void form_undo_capture(int slot) {
    if (slot < 0 || slot >= UNDO_SLOT_COUNT) return;
    size_t cap;
    char *buf = form_undo_slot_buf(slot, &cap);
    if (!buf) return;
    undo_stack_push(&g_form_undo[slot], buf);
    undo_stack_clear(&g_form_redo[slot]);
}

/* Apply one step: direction -1 = undo, +1 = redo. Moves the other
   side of history forward so Cmd+Z / Cmd+Shift+Z round-trip. */
static bool form_undo_apply(int slot, int dir) {
    if (slot < 0 || slot >= UNDO_SLOT_COUNT) return false;
    UndoStack *from = (dir < 0) ? &g_form_undo[slot] : &g_form_redo[slot];
    UndoStack *to   = (dir < 0) ? &g_form_redo[slot] : &g_form_undo[slot];
    char *popped = undo_stack_pop(from);
    if (!popped) return false;
    size_t cap;
    char *buf = form_undo_slot_buf(slot, &cap);
    if (!buf || cap == 0) { free(popped); return false; }
    undo_stack_push(to, buf);
    size_t n = strlen(popped);
    if (n >= cap) n = cap - 1;
    memcpy(buf, popped, n);
    buf[n] = 0;
    free(popped);
    g_form.sel_all = false;
    g_form.error[0] = 0;
    return true;
}

/* Compute the SSH-form modal layout: every clickable rect (text
   inputs, list panels, picker buttons, save/connect/cancel) for
   the current window size. Shared by draw + click hit-test. */
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

/* Is (x, y) inside `r`? Half-open rectangle so adjacent buttons
   share an edge cleanly. */
static bool rect_hit(Rect r, int x, int y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

/* Validate the SSH-form fields and try to connect. On success
   spawns a new SSH tab + closes the modal. On failure writes a
   libssh error message into the form's error line so the user
   can see what went wrong (bad host / auth fail / etc). */
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

/* Move keyboard focus through the SSH form's tab order by `delta`
   (+1 forward / -1 backward). Skips zero-sized rects so e.g. the
   Delete button is hidden until a saved host is selected. */
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

/* Dispatch a single frame's mouse activity inside the SSH form
   modal: hit-test buttons, text inputs, list rows, and the
   picker controls. Updates form state in-place; calls back into
   ssh_form_submit / ssh_form_save_to_config / ssh_form_clear etc.
   on the corresponding button hits. */
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
        Rect tab_dark, tab_light;
        int strip_h = theme_filter_strip_layout(
            L.theme_list.x, L.theme_list.y, L.theme_list.w,
            &tab_dark, &tab_light);
        if (rect_hit(tab_dark, mx, my)) {
            g_theme_filter = 0; g_form_theme_scroll = 0;
            g_form_focused_list = FORM_FOCUS_THEME; return;
        }
        if (rect_hit(tab_light, mx, my)) {
            g_theme_filter = 1; g_form_theme_scroll = 0;
            g_form_focused_list = FORM_FOCUS_THEME; return;
        }
        int row_h = 22;
        int rows_y = L.theme_list.y + strip_h;
        if (my < rows_y) { g_form_focused_list = FORM_FOCUS_THEME; return; }
        int v_idx = (my - rows_y) / row_h + g_form_theme_scroll;
        if (v_idx == 0) {
            g_form.theme[0] = 0;
            g_form_theme_idx = -1;
        } else {
            int seen = 1;  /* synthetic "(inherit default)" is index 0 */
            int tcount = themes_count();
            const Theme *ts = themes_all();
            for (int i = 0; i < tcount; i++) {
                bool light = theme_is_light(&ts[i]);
                if ((g_theme_filter == 1) != light) continue;
                if (seen == v_idx) {
                    strncpy(g_form.theme, ts[i].name, sizeof(g_form.theme) - 1);
                    g_form.theme[sizeof(g_form.theme) - 1] = 0;
                    g_form_theme_idx = i;
                    break;
                }
                seen++;
            }
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

/* Keyboard handler for the SSH form modal: Tab advances focus,
   Enter triggers the focused button (or submit on text-field),
   Esc cancels, plus per-text-field editing chords (Cmd+A
   select-all, Cmd+C/V copy/paste, Cmd+Z undo). */
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
        /* Undo / redo. */
        if (mod && IsKeyPressed(KEY_Z)) {
            form_undo_apply(UNDO_SLOT_LOGDIR, shift ? +1 : -1);
            return;
        }
        /* Copy current value. */
        if (mod && IsKeyPressed(KEY_C)) {
            if (g_form.log_dir[0]) SetClipboardText(g_form.log_dir);
            return;
        }
        /* Paste at end (no caret, so end is the only sensible target). */
        if (mod && IsKeyPressed(KEY_V)) {
            const char *clip = GetClipboardText();
            if (clip && *clip) {
                form_undo_capture(UNDO_SLOT_LOGDIR);
                for (const char *q = clip; *q; q++) {
                    unsigned char c = (unsigned char)*q;
                    if (c == '\r' || c == '\n' || c == '\t') continue;
                    if (c < 32 || c >= 127) continue;
                    if (len + 1 >= sizeof(g_form.log_dir)) break;
                    g_form.log_dir[len++] = (char)c;
                }
                g_form.log_dir[len] = 0;
            }
            return;
        }
        if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) {
            if (len > 0) {
                form_undo_capture(UNDO_SLOT_LOGDIR);
                g_form.log_dir[len - 1] = 0;
            }
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
            form_undo_capture(UNDO_SLOT_LOGDIR);
            g_form.log_dir[len++] = (char)cp;
            g_form.log_dir[len] = 0;
        }
        return;
    }

    /* Undo / redo (Ctrl+Z / Cmd+Z, Shift variant for redo). Only fires
       when a text field has focus — buttons and pickers don't have a
       meaningful undo. */
    if (mod && IsKeyPressed(KEY_Z) && is_text_field) {
        int slot = form_undo_current_slot();
        if (slot >= 0) form_undo_apply(slot, shift ? +1 : -1);
        return;
    }

    /* Select-all (Ctrl+A / Cmd+A). */
    if (mod && IsKeyPressed(KEY_A)) {
        if (is_text_field) g_form.sel_all = true;
        return;
    }

    /* Copy (Ctrl+C / Cmd+C) the focused text field's value. We don't
       track a caret, so there's no partial-selection case — copying
       always copies the whole field. Password is *not* copyable:
       the field is obscured by design, and shoving the plaintext
       onto the shared clipboard defeats that. */
    if (mod && IsKeyPressed(KEY_C) && is_text_field && g_form.focus != F_PASS) {
        size_t cap;
        char *buf = form_buf(g_form.focus, &cap);
        if (buf && *buf) SetClipboardText(buf);
        return;
    }

    /* Paste (Ctrl+V / Cmd+V) into whichever text field has focus. The
       GetCharPressed loop below ignores modified chords, so we inject
       clipboard bytes here explicitly. */
    if (mod && IsKeyPressed(KEY_V) && is_text_field) {
        const char *clip = GetClipboardText();
        if (clip && *clip) {
            size_t cap;
            char *buf = form_buf(g_form.focus, &cap);
            if (buf) {
                form_undo_capture(form_undo_current_slot());
                if (g_form.sel_all) { memset(buf, 0, cap); g_form.sel_all = false; }
                size_t len = strlen(buf);
                for (const char *q = clip; *q; q++) {
                    unsigned char c = (unsigned char)*q;
                    if (c == '\r' || c == '\n' || c == '\t') continue;
                    if (c < 32 || c >= 127) continue;
                    if (g_form.focus == F_PORT && !(c >= '0' && c <= '9')) continue;
                    if (len + 1 >= cap) break;
                    buf[len++] = (char)c;
                }
                buf[len] = 0;
                g_form.error[0] = 0;
            }
        }
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
                form_undo_capture(form_undo_current_slot());
                memset(buf, 0, cap);
                g_form.sel_all = false;
            } else if (len > 0) {
                form_undo_capture(form_undo_current_slot());
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
                form_undo_capture(form_undo_current_slot());
                memset(buf, 0, cap);
                len = 0;
                g_form.sel_all = false;
            }
            if (len + 1 >= (int)cap) continue;
            form_undo_capture(form_undo_current_slot());
            buf[len++] = (char)cp;
            buf[len] = 0;
            g_form.error[0] = 0;
        }
    }
    (void)L;
}

/* Render the entire SSH connect modal: panel, title, every text
   field with its label and (where focused) caret, the saved-host
   list, theme/font pickers, cursor-style + log-mode buttons,
   action buttons, and the status / error line at the bottom. */
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
        /* Dark / Light filter strip across the top of the list. */
        Rect tab_dark, tab_light;
        int strip_h = theme_filter_strip_layout(
            L.theme_list.x, L.theme_list.y, L.theme_list.w,
            &tab_dark, &tab_light);
        theme_filter_strip_draw(f, tab_dark, tab_light);

        int row_h = 22;
        int rows_y = L.theme_list.y + strip_h;
        int rows_h = L.theme_list.h - strip_h;
        const Theme *ts = themes_all();
        int tcount = themes_count();
        /* Filtered display list. Index 0 is always the synthetic
           "(inherit default)" row; subsequent entries follow only
           themes that match the active Dark/Light filter. */
        int filtered[1024];
        int n_filt = 1;
        filtered[0] = -1;  /* sentinel for "(inherit default)" */
        for (int i = 0; i < tcount && n_filt < (int)(sizeof(filtered)/sizeof(filtered[0])); i++) {
            bool light = theme_is_light(&ts[i]);
            if ((g_theme_filter == 1) == light) filtered[n_filt++] = i;
        }
        int visible = rows_h / row_h;
        int max_scroll = n_filt - visible;
        if (max_scroll < 0) max_scroll = 0;
        if (g_form_theme_scroll > max_scroll) g_form_theme_scroll = max_scroll;
        if (g_form_theme_scroll < 0) g_form_theme_scroll = 0;
        BeginScissorMode(L.theme_list.x + 2, rows_y,
                         L.theme_list.w - 4, rows_h - 2);
        for (int v = 0; v < n_filt; v++) {
            int real_i = filtered[v];
            int ry = rows_y + (v - g_form_theme_scroll) * row_h;
            if (ry + row_h < rows_y ||
                ry > rows_y + rows_h) continue;
            bool sel = (real_i < 0 && !g_form.theme[0]) ||
                       (real_i >= 0 && strcmp(ts[real_i].name, g_form.theme) == 0);
            if (sel) {
                DrawRectangle(L.theme_list.x + 2, ry,
                              L.theme_list.w - 4, row_h,
                              (Color){46, 62, 90, 220});
            }
            if (real_i < 0) {
                DrawTextEx(*f, "(inherit default)",
                           (Vector2){L.theme_list.x + 10, ry + 4},
                           13, 0, (Color){150, 155, 170, 255});
            } else {
                int swatch_w = 8, swatch_h = 11, swatch_gap = 1;
                int swatches_w = 8 * (swatch_w + swatch_gap);
                int sx = L.theme_list.x + L.theme_list.w - 6 - swatches_w;
                int sy = ry + (row_h - swatch_h) / 2;
                for (int k = 0; k < 8; k++) {
                    uint32_t c = ts[real_i].palette[k];
                    Color col = { (unsigned char)((c >> 16) & 0xff),
                                  (unsigned char)((c >> 8)  & 0xff),
                                  (unsigned char)( c        & 0xff), 255 };
                    DrawRectangle(sx + k * (swatch_w + swatch_gap), sy,
                                  swatch_w, swatch_h, col);
                }
                const char *bare = theme_display_split(ts[real_i].name, NULL, 0);
                DrawTextEx(*f, bare,
                           (Vector2){L.theme_list.x + 10, ry + 4},
                           13, 0,
                           sel ? (Color){230, 232, 240, 255}
                               : (Color){200, 205, 220, 255});
            }
        }
        EndScissorMode();
        if (n_filt > visible) {
            int track_x = L.theme_list.x + L.theme_list.w - 5;
            int bar_h = rows_h * visible / n_filt;
            if (bar_h < 24) bar_h = 24;
            int bar_y = rows_y +
                        (rows_h - bar_h) * g_form_theme_scroll /
                        (max_scroll > 0 ? max_scroll : 1);
            DrawRectangle(track_x, rows_y, 3, rows_h,
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
 * Organised into tabs (Font / Theme / Session / Window). Each tab
 * owns a subset of controls so the modal fits comfortably in a
 * reasonable window without overflow. New settings get slotted into
 * whichever tab makes sense — keep it boring and extensible. */

typedef enum {
    SETTINGS_TAB_FONT       = 0,
    SETTINGS_TAB_THEME      = 1,
    SETTINGS_TAB_CURSOR     = 2,
    SETTINGS_TAB_SESSION    = 3,
    SETTINGS_TAB_WINDOW     = 4,
    SETTINGS_TAB_RECORDING  = 5,
    SETTINGS_TAB_COUNT      = 6,
} SettingsTab;
static int g_settings_tab = SETTINGS_TAB_FONT;

static void fonts_load(const char *current_path);
/* Open the Settings modal. Reloads the on-disk font list so the
   font picker is populated with whatever's installed right now. */
static void settings_open(Renderer *r) {
    g_ui_mode = UI_SETTINGS;
    g_settings_status[0] = 0;
    fonts_load(r ? r->font_path : NULL);
}

/* Set the renderer font size + cascade through every tab so each
   pane resizes to the new cell dimensions. Also bumps the window
   minimum size so the user can't shrink below a usable grid. */
static void settings_apply_font_size(Renderer *r, int new_size) {
    if (renderer_set_font_size(r, new_size)) {
        tabs_resize_all(r, GetScreenWidth(), GetScreenHeight());
        SetWindowMinSize(r->cell_w * 20 + 2 * r->pad_x, r->cell_h * 5 + TAB_BAR_H + 2 * r->pad_y);
    }
}

typedef struct {
    Rect modal;
    Rect tab[SETTINGS_TAB_COUNT];  /* tab-bar buttons (always populated) */
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
    Rect rec_dir;    /* editable text box with recording-save directory */
    Rect repeat_initial; /* slider track — initial repeat delay */
    Rect repeat_rate;    /* slider track — per-repeat period */
    Rect startup_default;    /* startup-window-size picker: three buttons */
    Rect startup_fullscreen;
    Rect startup_maximized;
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

/* Quick filename heuristic — does this font's name contain any of
   the well-known monospace markers? Used to filter the disk-scan
   results so the picker isn't drowned in proportional fonts. */
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

/* qsort comparator — alphabetise font entries case-insensitively. */
static int cmp_font_name(const void *a, const void *b) {
    return strcasecmp(((const FontEntry *)a)->name,
                      ((const FontEntry *)b)->name);
}

/* Case-insensitive ends-with check — used to filter for .ttf /
   .otf / .ttc when scanning system font dirs. */
static bool ends_with_ci(const char *s, const char *suffix) {
    size_t ls = strlen(s), lsfx = strlen(suffix);
    if (ls < lsfx) return false;
    return strcasecmp(s + ls - lsfx, suffix) == 0;
}

/* Walk one font directory (recursing one level — most distros nest
   under font-name subdirs) and append every monospace-looking font
   to g_fonts. Silent on opendir failure so missing dirs don't spam. */
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

/* Scan every known font location (system dirs, bundled rbterm
   resources, embedded blobs) and rebuild g_fonts. Highlights the
   `current_path` in the picker if found. Idempotent — every
   settings_open / ssh_form_open call rebuilds the list. */
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

/* Switch the renderer to a picked font (disk file or embedded
   blob), keep the current size, and reflow every pane to the
   new cell metrics. */
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

/* Adjust the cell-grid padding (gap between window edge and the
   first cell), clamped to 0..64. Triggers a pane resize since
   usable cell rows/cols depend on padding. */
static void settings_apply_padding(Renderer *r, int new_pad) {
    if (new_pad < 0) new_pad = 0;
    if (new_pad > 64) new_pad = 64;
    r->pad_x = new_pad;
    r->pad_y = new_pad;
    tabs_resize_all(r, GetScreenWidth(), GetScreenHeight());
    SetWindowMinSize(r->cell_w * 20 + 2 * r->pad_x,
                     r->cell_h * 5 + TAB_BAR_H + 2 * r->pad_y);
}

/* Set the extra horizontal pixels per cell (letter spacing).
   Triggers a pane resize. */
static void settings_apply_spacing(Renderer *r, int new_extra) {
    renderer_set_cell_spacing(r, new_extra);
    tabs_resize_all(r, GetScreenWidth(), GetScreenHeight());
    SetWindowMinSize(r->cell_w * 20 + 2 * r->pad_x,
                     r->cell_h * 5 + TAB_BAR_H + 2 * r->pad_y);
}

static bool g_settings_dir_focus = false;
static bool g_settings_dir_sel_all = false;
/* Same pattern for the Recording-tab directory field. */
static bool g_settings_recdir_focus = false;
static bool g_settings_recdir_sel_all = false;

/* Compute the Settings modal layout: tab-bar buttons, content rects
   for the active tab only (Font / Theme / Session / Window),
   bottom-anchored Save-as-Default + Close. Shared by draw + click
   hit-test. */
static SettingsLayout settings_layout(int win_w, int win_h) {
    SettingsLayout L = {0};
    /* Modal is sized to the tallest tab's content so swapping tabs
       doesn't reshuffle the window. Clamp to fit the OS window. */
    int w = 600, h = 560;
    if (w > win_w - 40) w = win_w - 40;
    if (h > win_h - 40) h = win_h - 40;
    L.modal.x = (win_w - w) / 2;
    L.modal.y = (win_h - h) / 2;
    L.modal.w = w;
    L.modal.h = h;

    /* Tab bar row, directly under the 40px title bar. Four equal
       pills across the full inner width. */
    {
        int bar_y = L.modal.y + 40 + 6;
        int bar_h = 28;
        int gap = 4;
        int total_w = w - 2 * 14;
        int bw = (total_w - (SETTINGS_TAB_COUNT - 1) * gap) / SETTINGS_TAB_COUNT;
        int bx = L.modal.x + 14;
        for (int i = 0; i < SETTINGS_TAB_COUNT; i++)
            L.tab[i] = (Rect){ bx + i * (bw + gap), bar_y, bw, bar_h };
    }

    int btn = 32;
    int content_y = L.modal.y + 40 + 6 + 28 + 16;  /* title + tab bar + gap */
    int close_w = 90, close_h = 32, save_def_w = 150;
    int footer_y = L.modal.y + h - 22 - close_h;
    L.close = (Rect){ L.modal.x + w - 22 - close_w, footer_y, close_w, close_h };
    L.save_default = (Rect){ L.close.x - 8 - save_def_w, footer_y, save_def_w, close_h };

    /* Only the active tab gets content rects. Others stay zero-filled
       so click handlers can't hit them. */
    if (g_settings_tab == SETTINGS_TAB_FONT) {
        int font_row_y = content_y;
        L.font_val = (Rect){ L.modal.x + w - 214, font_row_y, 66, btn };
        L.dec      = (Rect){ L.modal.x + w - 138, font_row_y, btn, btn };
        L.inc      = (Rect){ L.modal.x + w - 60,  font_row_y, btn, btn };

        int font_list_y = font_row_y + btn + 16;
        L.font_list = (Rect){ L.modal.x + 140, font_list_y, w - 140 - 22, 160 };

        int pad_row_y = font_list_y + L.font_list.h + 16;
        L.pad_val  = (Rect){ L.modal.x + w - 214, pad_row_y, 66, btn };
        L.pad_dec  = (Rect){ L.modal.x + w - 138, pad_row_y, btn, btn };
        L.pad_inc  = (Rect){ L.modal.x + w - 60,  pad_row_y, btn, btn };

        int spc_row_y = pad_row_y + btn + 10;
        L.spc_val  = (Rect){ L.modal.x + w - 214, spc_row_y, 66, btn };
        L.spc_dec  = (Rect){ L.modal.x + w - 138, spc_row_y, btn, btn };
        L.spc_inc  = (Rect){ L.modal.x + w - 60,  spc_row_y, btn, btn };
    } else if (g_settings_tab == SETTINGS_TAB_THEME) {
        int theme_list_y = content_y;
        /* Theme tab now uses the full content height — cursor moved
           to its own tab. */
        int theme_h = footer_y - 22 - theme_list_y;
        if (theme_h < 120) theme_h = 120;
        L.theme_list = (Rect){ L.modal.x + 140, theme_list_y, w - 140 - 22, theme_h };
    } else if (g_settings_tab == SETTINGS_TAB_CURSOR) {
        int cur_row_y = content_y;
        int bwidth = w - 140 - 22;
        int gap_sty = 6;
        int bw = (bwidth - 3 * gap_sty) / 4;
        int bx = L.modal.x + 140;
        int bh = 30;
        L.cur_block = (Rect){ bx,                          cur_row_y, bw, bh };
        L.cur_under = (Rect){ bx + (bw + gap_sty),          cur_row_y, bw, bh };
        L.cur_bar   = (Rect){ bx + 2 * (bw + gap_sty),      cur_row_y, bw, bh };
        L.cur_blink = (Rect){ bx + 3 * (bw + gap_sty),      cur_row_y, bw, bh };
        /* Key-repeat sliders live on the Cursor tab too — they're
           keyboard-feel knobs, not session-state. */
        int slider_track_h = 8;
        int row_pitch = 22;
        int kr1_y = cur_row_y + bh + 24;
        L.repeat_initial = (Rect){ L.modal.x + 140, kr1_y, w - 140 - 22 - 60,
                                   slider_track_h };
        int kr2_y = kr1_y + row_pitch;
        L.repeat_rate    = (Rect){ L.modal.x + 140, kr2_y, w - 140 - 22 - 60,
                                   slider_track_h };
    } else if (g_settings_tab == SETTINGS_TAB_SESSION) {
        int log_row1_y = content_y;
        L.log_toggle = (Rect){ L.modal.x + w - 140, log_row1_y, 110, btn };

        int log_row2_y = log_row1_y + btn + 10;
        L.log_dir = (Rect){ L.modal.x + 140, log_row2_y, w - 140 - 22, btn };
    } else if (g_settings_tab == SETTINGS_TAB_WINDOW) {
        int sw_row_y = content_y;
        int bwidth = w - 140 - 22;
        int gap_sty = 6;
        int bw = (bwidth - gap_sty) / 2;
        int bx = L.modal.x + 140;
        int bh = 30;
        L.startup_default    = (Rect){ bx,                   sw_row_y, bw, bh };
        L.startup_maximized  = (Rect){ bx + (bw + gap_sty),  sw_row_y, bw, bh };
        /* Fullscreen removed — raylib's ToggleFullscreen is buggy on
           macOS here. Users wanting a distraction-free window use
           Maximized (own Space on macOS). */
        L.startup_fullscreen = (Rect){ 0, 0, 0, 0 };
    } else if (g_settings_tab == SETTINGS_TAB_RECORDING) {
        int row_y = content_y;
        L.rec_dir = (Rect){ L.modal.x + 140, row_y, w - 140 - 22, btn };
    }

    return L;
}

/* Map a slider track's mouse-x to a clamped int value. `vmin..vmax` is
   the usable range. Inclusive endpoints; thumb drawing and hit-test
   share this formula so they line up exactly. */
static int slider_value_from_x(Rect track, int mx, int vmin, int vmax) {
    float t = (float)(mx - track.x) / (float)track.w;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return vmin + (int)(t * (float)(vmax - vmin) + 0.5f);
}

/* Per-session slider drag state. Keyed by track rect since tracks are
   stable rects; we only drag one slider at a time so one slot is enough. */
static Rect g_slider_drag_track;  /* w==0 when no drag active */
static int *g_slider_drag_target;
static int  g_slider_drag_min;
static int  g_slider_drag_max;

/* One frame of mouse handling for the Settings modal. Continues an
   in-progress slider drag, hit-tests tab-bar buttons, then dispatches
   to the controls of the active tab (font +/-, font list rows,
   theme rows, cursor-style buttons, padding/spacing, log toggle/dir,
   key-repeat sliders, startup-window picker, save/close). */
static void settings_handle_mouse(Renderer *r, SettingsLayout L) {
    Vector2 mp = GetMousePosition();
    int mx = (int)mp.x, my = (int)mp.y;
    /* Continue an in-progress slider drag before anything else so the
       thumb can overshoot the track and the value still follows the
       mouse. Released? End the drag. */
    if (g_slider_drag_track.w > 0 && g_slider_drag_target) {
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            *g_slider_drag_target = slider_value_from_x(
                g_slider_drag_track, mx, g_slider_drag_min, g_slider_drag_max);
            input_set_repeat(g_app_settings.key_repeat_initial_ms,
                             g_app_settings.key_repeat_rate_ms);
            return;
        }
        g_slider_drag_track.w = 0;
        g_slider_drag_target = NULL;
    }
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
    /* Tab bar: switching tabs clears focus held by the previous tab's
       controls so keyboard input doesn't bleed into the new tab. */
    for (int i = 0; i < SETTINGS_TAB_COUNT; i++) {
        if (rect_hit(L.tab[i], mx, my)) {
            if (g_settings_tab != i) {
                g_settings_tab = i;
                g_settings_dir_focus = false;
                g_settings_recdir_focus = false;
                g_settings_focused_list = SETTINGS_FOCUS_NONE;
                g_slider_drag_track.w = 0;
                g_slider_drag_target = NULL;
            }
            return;
        }
    }
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
        Rect tab_dark, tab_light;
        int strip_h = theme_filter_strip_layout(
            L.theme_list.x, L.theme_list.y, L.theme_list.w,
            &tab_dark, &tab_light);
        if (rect_hit(tab_dark, mx, my)) {
            g_theme_filter = 0; g_theme_list_scroll = 0;
            g_settings_focused_list = SETTINGS_FOCUS_THEME;
            return;
        }
        if (rect_hit(tab_light, mx, my)) {
            g_theme_filter = 1; g_theme_list_scroll = 0;
            g_settings_focused_list = SETTINGS_FOCUS_THEME;
            return;
        }
        int row_h = 22;
        int rows_y = L.theme_list.y + strip_h;
        if (my < rows_y) { g_settings_focused_list = SETTINGS_FOCUS_THEME; return; }
        /* Visible row → filtered-index → real theme index. */
        int v_idx = (my - rows_y) / row_h + g_theme_list_scroll;
        const Theme *ts = themes_all();
        int tcount = themes_count();
        int seen = 0;
        for (int i = 0; i < tcount; i++) {
            bool light = theme_is_light(&ts[i]);
            if ((g_theme_filter == 1) != light) continue;
            if (seen == v_idx) {
                g_theme_list_selected = i;
                Tab *t = active_tab();
                Pane *p = t ? active_pane_of(t) : NULL;
                if (p && p->scr) screen_apply_theme(p->scr, &ts[i]);
                break;
            }
            seen++;
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
            /* Pick this style as the new default for fresh panes AND
               apply it immediately to the currently-focused pane.
               DECSCUSR (CSI N SP q) from running programs still wins
               on the pane it targets — this just sets the baseline. */
            g_app_settings.cursor_style = st;
            if (p && p->scr) screen_set_cursor_style(p->scr, st);
            return;
        }
    }
    if (rect_hit(L.log_toggle, mx, my)) {
        g_app_settings.log_enabled = !g_app_settings.log_enabled;
        refresh_tab_logs();
        return;
    }
    /* Key-repeat sliders. Press inside the track begins a drag; the
       value follows the mouse until release. We extend each track's
       hit-test a few px vertically so clicking the drawn thumb (which
       may overflow the track) still starts the drag. */
    {
        Rect r_init_hit = { L.repeat_initial.x, L.repeat_initial.y - 6,
                             L.repeat_initial.w, L.repeat_initial.h + 12 };
        Rect r_rate_hit = { L.repeat_rate.x,    L.repeat_rate.y    - 6,
                             L.repeat_rate.w,    L.repeat_rate.h    + 12 };
        if (rect_hit(r_init_hit, mx, my)) {
            g_slider_drag_track  = L.repeat_initial;
            g_slider_drag_target = &g_app_settings.key_repeat_initial_ms;
            g_slider_drag_min = 0; g_slider_drag_max = 1000;
            *g_slider_drag_target = slider_value_from_x(L.repeat_initial, mx, 0, 1000);
            input_set_repeat(g_app_settings.key_repeat_initial_ms,
                             g_app_settings.key_repeat_rate_ms);
            return;
        }
        if (rect_hit(r_rate_hit, mx, my)) {
            g_slider_drag_track  = L.repeat_rate;
            g_slider_drag_target = &g_app_settings.key_repeat_rate_ms;
            g_slider_drag_min = 5; g_slider_drag_max = 200;
            *g_slider_drag_target = slider_value_from_x(L.repeat_rate, mx, 5, 200);
            input_set_repeat(g_app_settings.key_repeat_initial_ms,
                             g_app_settings.key_repeat_rate_ms);
            return;
        }
    }
    if (rect_hit(L.log_dir, mx, my)) {
        g_settings_dir_focus = true;
        g_settings_dir_sel_all = false;
        return;
    }
    if (rect_hit(L.rec_dir, mx, my)) {
        g_settings_recdir_focus = true;
        g_settings_recdir_sel_all = false;
        return;
    }
    /* Startup window mode: Default or Maximized. Applied on next
       launch. Fullscreen option removed — raylib's ToggleFullscreen
       misbehaves on macOS. */
    {
        int pick = -1;
        if      (rect_hit(L.startup_default,   mx, my)) pick = STARTUP_WINDOW_DEFAULT;
        else if (rect_hit(L.startup_maximized, mx, my)) pick = STARTUP_WINDOW_MAXIMIZED;
        if (pick >= 0) {
            g_app_settings.startup_window = pick;
            snprintf(g_settings_status, sizeof(g_settings_status),
                     "Startup window mode set. Save as Default to persist.");
            return;
        }
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
    if (rect_hit(L.close, mx, my)) {
        g_ui_mode = UI_NORMAL;
        g_settings_dir_focus = false;
        g_settings_recdir_focus = false;
        g_settings_focused_list = SETTINGS_FOCUS_NONE;
        return;
    }
    /* Click elsewhere drops focus from the text input + list. */
    g_settings_dir_focus = false;
    g_settings_recdir_focus = false;
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

/* Keyboard handling for the Settings modal: Esc closes, Tab cycles
   focus, the log-dir text input has its own caret/edit chord set,
   and Up/Down navigates whichever list (font/theme) currently has
   keyboard focus. */
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
        if (g_settings_recdir_focus) { g_settings_recdir_focus = false; return; }
        if (g_settings_focused_list != SETTINGS_FOCUS_NONE) {
            g_settings_focused_list = SETTINGS_FOCUS_NONE;
            return;
        }
        g_ui_mode = UI_NORMAL; return;
    }

    /* Recording-tab directory text input. Same chord set as
       g_settings_dir_focus / log-dir editing. */
    if (g_settings_recdir_focus) {
        if (mod && IsKeyPressed(KEY_A)) { g_settings_recdir_sel_all = true; return; }
        if (mod && IsKeyPressed(KEY_C) && g_settings_recdir_sel_all
                && g_app_settings.rec_dir[0]) {
            SetClipboardText(g_app_settings.rec_dir);
            return;
        }
        if (mod && IsKeyPressed(KEY_V)) {
            const char *clip = GetClipboardText();
            if (clip && *clip) {
                if (g_settings_recdir_sel_all) {
                    g_app_settings.rec_dir[0] = 0;
                    g_settings_recdir_sel_all = false;
                }
                size_t len = strlen(g_app_settings.rec_dir);
                for (const char *q = clip; *q; q++) {
                    unsigned char c = (unsigned char)*q;
                    if (c == '\r' || c == '\n' || c == '\t') continue;
                    if (c < 32 || c >= 127) continue;
                    if (len + 1 >= sizeof(g_app_settings.rec_dir)) break;
                    g_app_settings.rec_dir[len++] = (char)c;
                }
                g_app_settings.rec_dir[len] = 0;
            }
            return;
        }
        size_t len = strlen(g_app_settings.rec_dir);
        if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) {
            if (g_settings_recdir_sel_all) {
                g_app_settings.rec_dir[0] = 0;
                g_settings_recdir_sel_all = false;
            } else if (len > 0) {
                g_app_settings.rec_dir[len - 1] = 0;
            }
        }
        if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
            g_settings_recdir_focus = false;
            return;
        }
        int cp;
        while ((cp = GetCharPressed()) != 0) {
            if (mod) continue;
            if (cp < 32 || cp >= 127) continue;
            if (g_settings_recdir_sel_all) {
                g_app_settings.rec_dir[0] = 0;
                len = 0;
                g_settings_recdir_sel_all = false;
            }
            if (len + 1 >= sizeof(g_app_settings.rec_dir)) continue;
            g_app_settings.rec_dir[len++] = (char)cp;
            g_app_settings.rec_dir[len] = 0;
        }
        return;
    }

    if (g_settings_dir_focus) {
        /* Text input on the log directory. */
        if (mod && IsKeyPressed(KEY_A)) { g_settings_dir_sel_all = true; return; }
        if (mod && IsKeyPressed(KEY_C) && g_settings_dir_sel_all
                && g_app_settings.log_dir[0]) {
            SetClipboardText(g_app_settings.log_dir);
            return;
        }
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

/* ---------- Save-recording modal ---------- */

typedef struct {
    Rect modal;
    Rect path;
    Rect fmt[REC_FMT_COUNT];
    Rect save_btn, close_btn, preview_btn;
} RecSaveLayout;

/* Compute the modal layout: path text field, six format buttons,
   action buttons. Shared by draw + click hit-test. */
static void draw_rec_save_modal(Renderer *r, int win_w, int win_h, RecSaveLayout L);

static RecSaveLayout rec_save_layout(int win_w, int win_h) {
    RecSaveLayout L = {0};
    /* Width sized so the typical "wrote NN KB <fmt> to /Users/<user>/<long>/rbterm-….<ext>"
       status line fits in full at 16pt mono. The status row sits
       above the button row (separate y), so it can use the full
       modal interior. Falls back to window-width-minus-margin on
       small windows; middle-ellipsis kicks in only if even that
       isn't enough. */
    int w = 1080, h = 280;
    if (w > win_w - 40) w = win_w - 40;
    if (h > win_h - 40) h = win_h - 40;
    L.modal.x = (win_w - w) / 2;
    L.modal.y = (win_h - h) / 2;
    L.modal.w = w;
    L.modal.h = h;
    int title_h = 38;
    int pad = 22;
    int label_w = 70;
    int field_x = L.modal.x + pad + label_w;
    int field_w = w - pad - label_w - pad;
    int row_y = L.modal.y + title_h + 22;
    L.path = (Rect){ field_x, row_y, field_w, 28 };
    int fmt_y = L.path.y + L.path.h + 18;
    int fmt_gap = 4;
    int fmt_w = (field_w - (REC_FMT_COUNT - 1) * fmt_gap) / REC_FMT_COUNT;
    int fmt_h = 30;
    for (int i = 0; i < REC_FMT_COUNT; i++)
        L.fmt[i] = (Rect){ field_x + i * (fmt_w + fmt_gap), fmt_y, fmt_w, fmt_h };
    /* Footer row, right-anchored: [Preview] [Close] [Save].
       Save / Preview keep the modal open so the user can export the
       same recording to multiple formats; Close (or Esc) dismisses
       and leaves the temp .cast in place. */
    int btn_h = 32;
    int row_y2 = L.modal.y + h - 22 - btn_h;
    int save_w = 110, close_w = 90, preview_w = 100;
    L.save_btn    = (Rect){ L.modal.x + w - 22 - save_w,        row_y2, save_w,    btn_h };
    L.close_btn   = (Rect){ L.save_btn.x - 8 - close_w,         row_y2, close_w,   btn_h };
    L.preview_btn = (Rect){ L.close_btn.x - 8 - preview_w,      row_y2, preview_w, btn_h };
    return L;
}

/* Look up bundled ffmpeg first (next to the rbterm executable on
   Linux/Windows, or in Contents/Resources/ on macOS .app bundles),
   then fall back to whatever is on PATH. Returns the resolved
   path in `out` (caller's buffer) or false if nothing's available. */
static bool find_ffmpeg(char *out, size_t cap) {
#ifndef __EMSCRIPTEN__
    const char *exe_dir = GetApplicationDirectory();
    if (exe_dir && *exe_dir) {
#ifdef __APPLE__
        snprintf(out, cap, "%s../Resources/ffmpeg", exe_dir);
        if (access(out, X_OK) == 0) return true;
#endif
#ifdef _WIN32
        snprintf(out, cap, "%sffmpeg.exe", exe_dir);
#else
        snprintf(out, cap, "%sffmpeg", exe_dir);
#endif
        if (access(out, X_OK) == 0) return true;
    }
#endif
    /* PATH fallback. */
    snprintf(out, cap, "ffmpeg");
    return true;
}

/* Spawn ffmpeg with stdin connected to a write pipe + stderr
   redirected to `err_log_path` so we can surface the actual error
   to the user when ffmpeg dies. Caller fcloses the FILE on
   completion and waits the child via waitpid. */
#ifndef _WIN32
#include <sys/wait.h>
#endif
static FILE *ffmpeg_open_pipe(const char *const argv[], pid_t *out_pid,
                              const char *err_log_path) {
#ifdef _WIN32
    (void)argv; (void)out_pid; (void)err_log_path;
    return NULL;   /* TODO: CreateProcess + anonymous pipe on Windows. */
#else
    int pfd[2];
    if (pipe(pfd) != 0) return NULL;
    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return NULL; }
    if (pid == 0) {
        dup2(pfd[0], 0);
        close(pfd[0]);
        close(pfd[1]);
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) { dup2(dn, 1); close(dn); }
        int errfd = err_log_path
            ? open(err_log_path, O_WRONLY | O_CREAT | O_TRUNC, 0600)
            : -1;
        if (errfd >= 0) { dup2(errfd, 2); close(errfd); }
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    close(pfd[0]);
    *out_pid = pid;
    FILE *fp = fdopen(pfd[1], "wb");
    if (!fp) close(pfd[1]);
    return fp;
#endif
}

/* Synchronous fork+exec — runs argv to completion and returns true
   on exit status 0. Stderr is captured to `err_log_path` if non-
   NULL so callers can surface ffmpeg's own message. */
static bool run_argv(const char *const argv[], const char *err_log_path) {
#ifdef _WIN32
    (void)argv; (void)err_log_path;
    return false;
#else
    pid_t pid = fork();
    if (pid == 0) {
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) { dup2(dn, 0); dup2(dn, 1); close(dn); }
        int errfd = err_log_path
            ? open(err_log_path, O_WRONLY | O_CREAT | O_TRUNC, 0600)
            : -1;
        if (errfd >= 0) { dup2(errfd, 2); close(errfd); }
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    if (pid < 0) return false;
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}

/* Read ffmpeg's stderr tail and pull out the most informative line.
   ffmpeg often prints `Conversion failed!` as the very last line —
   the actual cause ("Encoder libwebp not found", etc.) sits a few
   lines above. We scan a generous trailing window for a line that
   matches a known error pattern; failing that, fall back to the
   last non-empty line. */
static void read_tail(const char *path, char *buf, size_t cap) {
    if (cap == 0) return;
    buf[0] = 0;
    FILE *fp = fopen(path, "rb");
    if (!fp) return;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    /* Pull a wider window than `cap` so we can scan across multiple
       lines for the substantive error. */
    long want = 4096;
    if (sz < want) want = sz;
    char *win = malloc((size_t)want + 1);
    if (!win) { fclose(fp); return; }
    fseek(fp, sz - want, SEEK_SET);
    size_t got = fread(win, 1, (size_t)want, fp);
    fclose(fp);
    win[got] = 0;
    /* Tokenise into lines, scan for a "real" error line. */
    static const char *patterns[] = {
        "Encoder ", "encoder not found", "Encoder not found",
        "Unknown encoder", "not found", "No such file",
        "Invalid argument", "Cannot find", "Cannot open",
        "Permission denied", NULL
    };
    char *best = NULL;
    char *last_nonempty = NULL;
    char *p = win;
    while (*p) {
        char *end = strchr(p, '\n');
        if (end) *end = 0;
        if (*p) last_nonempty = p;
        for (int i = 0; patterns[i]; i++) {
            if (strstr(p, patterns[i])) { best = p; break; }
        }
        if (!end) break;
        p = end + 1;
    }
    const char *src = best ? best : (last_nonempty ? last_nonempty : "");
    snprintf(buf, cap, "%s", src);
    free(win);
    /* Strip trailing whitespace. */
    size_t n = strlen(buf);
    while (n > 0 && (buf[n - 1] == ' ' || buf[n - 1] == '\t' ||
                     buf[n - 1] == '\r' || buf[n - 1] == '\n')) {
        buf[--n] = 0;
    }
}

/* Walk a captured byte stream and emit a "what was visibly typed
   to the screen" version: ANSI / OSC / DCS escape sequences are
   dropped; CR resets the cursor to the start of the current line
   (so a typical "progress bar overwriting itself" replays as the
   final line); BS backs up one column; LF flushes the current
   line. The output is plain UTF-8 text — no control bytes other
   than '\n' and '\t'. */
typedef struct {
    char  *line;
    size_t line_cap;
    size_t cursor;     /* write position within the line buffer */
    size_t line_len;   /* high-water mark — we flush this many bytes */
    enum {
        STRIP_NORMAL,
        STRIP_ESC,         /* saw ESC, awaiting introducer */
        STRIP_CSI,         /* in CSI parameter run, awaiting final byte */
        STRIP_STR,         /* OSC/DCS/APC/PM/SOS — wait for BEL or ST */
        STRIP_STR_ESC,     /* in STR state, just saw ESC */
    } state;
    FILE *out;
} StripCtx;

static void strip_line_putc(StripCtx *c, unsigned char b) {
    /* Grow the line buffer so cursor + b fits. */
    if (c->cursor + 1 >= c->line_cap) {
        size_t nc = c->line_cap ? c->line_cap * 2 : 256;
        char *nb = realloc(c->line, nc);
        if (!nb) return;
        c->line = nb;
        c->line_cap = nc;
    }
    c->line[c->cursor++] = (char)b;
    if (c->cursor > c->line_len) c->line_len = c->cursor;
}

static void strip_flush_line(StripCtx *c) {
    if (c->line_len > 0) fwrite(c->line, 1, c->line_len, c->out);
    fputc('\n', c->out);
    c->cursor = 0;
    c->line_len = 0;
}

/* Feed one byte through the strip state machine. */
static void strip_feed(StripCtx *c, unsigned char b) {
    switch (c->state) {
    case STRIP_NORMAL:
        if (b == 0x1B) { c->state = STRIP_ESC; return; }
        if (b == '\n') { strip_flush_line(c); return; }
        if (b == '\r') { c->cursor = 0; return; }
        if (b == '\b') { if (c->cursor > 0) c->cursor--; return; }
        if (b == '\t') { strip_line_putc(c, '\t'); return; }
        if (b < 0x20)  return;     /* drop other C0 controls */
        if (b == 0x7F) return;     /* drop DEL */
        strip_line_putc(c, b);
        return;
    case STRIP_ESC:
        if (b == '[')                         { c->state = STRIP_CSI; return; }
        if (b == ']' || b == 'P' || b == '_'
                     || b == '^' || b == 'X') { c->state = STRIP_STR; return; }
        /* Two-byte ESC sequence — drop and resume. */
        c->state = STRIP_NORMAL;
        return;
    case STRIP_CSI:
        /* CSI body: parameter bytes 0x30..0x3F, intermediate 0x20..0x2F,
           terminated by a final byte 0x40..0x7E. */
        if (b >= 0x40 && b <= 0x7E) c->state = STRIP_NORMAL;
        return;
    case STRIP_STR:
        if (b == 0x07) { c->state = STRIP_NORMAL; return; }   /* BEL terminator */
        if (b == 0x1B) { c->state = STRIP_STR_ESC; return; }  /* possible ST */
        return;
    case STRIP_STR_ESC:
        c->state = (b == '\\') ? STRIP_NORMAL : STRIP_STR;
        return;
    }
}

/* Render src_cast → plain text at dst. Fast — single pass, no UI
   blocking. Returns false on file open failure. */
static bool rec_render_text(const char *src_cast, const char *dst,
                            char *err, size_t errsz) {
    char loaderr[128] = {0};
    CastFile *cf = cast_load(src_cast, loaderr, sizeof(loaderr));
    if (!cf) {
        snprintf(err, errsz, "cast_load: %s", loaderr[0] ? loaderr : "(unknown)");
        return false;
    }
    FILE *fp = fopen(dst, "wb");
    if (!fp) {
        snprintf(err, errsz, "open %s: %s", dst, strerror(errno));
        cast_free(cf);
        return false;
    }
    StripCtx c = {0};
    c.state = STRIP_NORMAL;
    c.out = fp;
    for (size_t i = 0; i < cf->count; i++) {
        const uint8_t *p = cf->events[i].data;
        size_t n = cf->events[i].n;
        for (size_t k = 0; k < n; k++) strip_feed(&c, p[k]);
    }
    /* Flush any trailing content that didn't end in LF. */
    if (c.line_len > 0) strip_flush_line(&c);
    free(c.line);
    cast_free(cf);
    if (fclose(fp) != 0) {
        snprintf(err, errsz, "close %s: %s", dst, strerror(errno));
        return false;
    }
    return true;
}

/* Native cast → frames → encoder/pipe. Replays the recorded byte
   stream into a hidden Screen, renders each frame to an off-screen
   RenderTexture using the existing renderer_draw, reads the pixels
   back, and either feeds them to the embedded GIF encoder or pipes
   them to ffmpeg (bundled or on PATH). Blocks the UI for the
   duration of conversion. */
static bool rec_render_native(RecFmt fmt, const char *src_cast, const char *dst,
                              char *err, size_t errsz) {
    if (err && errsz) err[0] = 0;
    if (fmt == REC_FMT_CAST) {
        if (strcmp(src_cast, dst) == 0) return true;
        if (rename(src_cast, dst) == 0) return true;
        snprintf(err, errsz, "rename failed: %s", strerror(errno));
        return false;
    }
    if (fmt == REC_FMT_TXT) {
        return rec_render_text(src_cast, dst, err, errsz);
    }
    if (!g_renderer) {
        snprintf(err, errsz, "renderer not initialised");
        return false;
    }

    CastFile *cf = cast_load(src_cast, err, errsz);
    if (!cf) return false;

    int cw = g_renderer->cell_w;
    int ch_ = g_renderer->cell_h;
    int px = g_renderer->pad_x;
    int py = g_renderer->pad_y;
    int width  = cf->cols * cw + 2 * px;
    int height = cf->rows * ch_ + 2 * py;
    /* RenderTexture sizes must be even on some drivers; round up. */
    if (width  & 1) width  += 1;
    if (height & 1) height += 1;

    /* Hidden Screen — no IO callbacks needed (no PTY, no clipboard). */
    ScreenIO sio = {0};
    Screen *scr = screen_new(cf->cols, cf->rows, 5000, sio);
    if (!scr) {
        snprintf(err, errsz, "screen_new failed");
        cast_free(cf);
        return false;
    }
    screen_set_cell_h_px(scr, ch_);

    RenderTexture2D rt = LoadRenderTexture(width, height);
    if (rt.id == 0) {
        snprintf(err, errsz, "couldn't allocate %dx%d render target", width, height);
        screen_free(scr);
        cast_free(cf);
        return false;
    }

    /* Output sink. Three direct paths + one fallback:
       - GIF  → native encoder (gif_encoder.c)
       - WebP → native encoder (webp_encoder.c, libwebp + libwebpmux)
       - MP4 / WebM → rawvideo straight to ffmpeg
       - APNG → render to a temp GIF, then ffmpeg gif → apng (libavcodec
                 has built-in apng so this works on stock brew ffmpeg). */
    bool two_pass = (fmt == REC_FMT_APNG);
    char gif_intermediate[PATH_MAX] = {0};
    GifEnc  *gif  = NULL;
    WebpEnc *webp = NULL;
    FILE   *pipe_fp = NULL;
    pid_t   pipe_pid = 0;
    int     fps = 15;
    int     delay_cs = 100 / fps;     /* gif delay is in 1/100s. */
    int     delay_ms = 1000 / fps;    /* webp delay is in ms. */
    char    ff_log[PATH_MAX];
    snprintf(ff_log, sizeof(ff_log), "/tmp/rbterm-ffmpeg-%d.log", (int)getpid());
    if (fmt == REC_FMT_GIF || two_pass) {
        const char *gif_dst = (fmt == REC_FMT_GIF) ? dst : gif_intermediate;
        if (two_pass) {
            snprintf(gif_intermediate, sizeof(gif_intermediate),
                     "/tmp/rbterm-conv-%d.gif", (int)getpid());
            gif_dst = gif_intermediate;
        }
        gif = gif_begin(gif_dst, width, height, delay_cs);
        if (!gif) {
            snprintf(err, errsz, "couldn't open %s for writing", gif_dst);
            UnloadRenderTexture(rt); screen_free(scr); cast_free(cf);
            return false;
        }
    } else if (fmt == REC_FMT_WEBP) {
        webp = webp_begin(dst, width, height, delay_ms);
        if (!webp) {
            snprintf(err, errsz, "couldn't open %s for webp encoding", dst);
            UnloadRenderTexture(rt); screen_free(scr); cast_free(cf);
            return false;
        }
    } else {
        char ff[PATH_MAX];
        find_ffmpeg(ff, sizeof(ff));
        char size_arg[32];
        snprintf(size_arg, sizeof(size_arg), "%dx%d", width, height);
        char rate_arg[16];
        snprintf(rate_arg, sizeof(rate_arg), "%d", fps);
        const char *fmt_args[16] = {0};
        int fmt_n = 0;
        if (fmt == REC_FMT_MP4) {
            fmt_args[fmt_n++] = "-pix_fmt"; fmt_args[fmt_n++] = "yuv420p";
            fmt_args[fmt_n++] = "-movflags"; fmt_args[fmt_n++] = "+faststart";
        } else if (fmt == REC_FMT_WEBM) {
            /* libvpx (VP8) ships with every brew/distro ffmpeg.
               It only accepts yuv420p — we have to pass that
               explicitly so ffmpeg inserts a colour-space
               conversion filter between our rgba frames and the
               encoder. Without it: "Invalid argument" / -22. */
            fmt_args[fmt_n++] = "-pix_fmt"; fmt_args[fmt_n++] = "yuv420p";
            fmt_args[fmt_n++] = "-c:v";    fmt_args[fmt_n++] = "libvpx";
            fmt_args[fmt_n++] = "-b:v";    fmt_args[fmt_n++] = "1M";
        }
        const char *argv[32];
        int n = 0;
        argv[n++] = ff;
        argv[n++] = "-y";
        argv[n++] = "-f"; argv[n++] = "rawvideo";
        argv[n++] = "-pix_fmt"; argv[n++] = "rgba";
        argv[n++] = "-s"; argv[n++] = size_arg;
        argv[n++] = "-framerate"; argv[n++] = rate_arg;
        argv[n++] = "-i"; argv[n++] = "-";
        for (int k = 0; k < fmt_n; k++) argv[n++] = fmt_args[k];
        argv[n++] = dst;
        argv[n] = NULL;
        pipe_fp = ffmpeg_open_pipe(argv, &pipe_pid, ff_log);
        if (!pipe_fp) {
            snprintf(err, errsz, "couldn't run ffmpeg (bundled: %s)", ff);
            UnloadRenderTexture(rt); screen_free(scr); cast_free(cf);
            return false;
        }
    }

    /* Render the frame sequence. We emit frames at a steady FPS;
       between events the previous frame is held (gif's per-frame
       delay does the right thing for free). */
    uint8_t *pixels = malloc((size_t)width * height * 4);
    if (!pixels) {
        snprintf(err, errsz, "out of memory for pixel buffer");
        if (gif) gif_end(gif);
        if (pipe_fp) { fclose(pipe_fp); waitpid(pipe_pid, NULL, 0); }
        UnloadRenderTexture(rt); screen_free(scr); cast_free(cf);
        return false;
    }
    /* Skip every blank frame before the first event so the gif
       opens on actual content instead of a couple of seconds of
       blank screen while the user was thinking. Frame 0 fires
       events[0] immediately. */
    double t0 = cf->events[0].t;
    if (t0 < 0.0) t0 = 0.0;
    double total = cf->duration_s - t0 + 0.3;
    if (total < 0.3) total = 0.3;
    int total_frames = (int)(total * fps) + 1;
    size_t ev_idx = 0;
    /* Drive the render in chunks, presenting a fresh modal frame
       between chunks so the user sees a live progress message + a
       spinning glyph. Without this the UI freezes for the full
       conversion duration and the OS overlays its "not responding"
       beachball. CHUNK_SZ trades latency-per-frame against UI
       responsiveness; 6 keeps the spinner readable. */
    const int CHUNK_SZ = 6;
    int f = 0;
    while (f < total_frames) {
        int chunk_end = f + CHUNK_SZ;
        if (chunk_end > total_frames) chunk_end = total_frames;
        for (; f < chunk_end; f++) {
            double frame_t = t0 + (double)f / fps;
            while (ev_idx < cf->count && cf->events[ev_idx].t <= frame_t) {
                screen_feed(scr, cf->events[ev_idx].data, cf->events[ev_idx].n);
                ev_idx++;
            }
            BeginTextureMode(rt);
                ClearBackground(BLACK);
                renderer_draw(g_renderer, scr, 0.0, false, NULL, 0, 0, -1, -1);
            EndTextureMode();
            Image img = LoadImageFromTexture(rt.texture);
            ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
            int row = width * 4;
            for (int y = 0; y < height; y++) {
                memcpy(pixels + (size_t)y * row,
                       (uint8_t *)img.data + (size_t)(height - 1 - y) * row,
                       (size_t)row);
            }
            UnloadImage(img);
            if (gif)       gif_add_frame(gif, pixels);
            else if (webp) webp_add_frame(webp, pixels);
            else if (pipe_fp) fwrite(pixels, 1, (size_t)width * height * 4, pipe_fp);
        }
        /* Live status + UI redraw between chunks. The save modal
           reads g_rec_save.status, so we update that and redraw the
           full app frame. The 4-glyph ASCII spinner ticks once per
           chunk so the user can tell something is happening even
           when the percentage doesn't change much. */
        static const char *spin = "|/-\\";
        int sidx = (f / CHUNK_SZ) & 3;
        int pct = (int)((double)f * 100.0 / (double)total_frames + 0.5);
        snprintf(g_rec_save.status, sizeof(g_rec_save.status),
                 "%c rendering %d / %d frames (%d%%)",
                 spin[sidx], f, total_frames, pct);
        BeginDrawing();
            ClearBackground((Color){0, 0, 0, 255});
            int win_w = GetScreenWidth();
            int win_h = GetScreenHeight();
            Tab *cur = active_tab();
            draw_tab_bar(g_renderer, win_w);
            if (cur) draw_tab_contents(g_renderer, cur, win_w, win_h,
                                       GetTime(), IsWindowFocused());
            RecSaveLayout RL = rec_save_layout(win_w, win_h);
            draw_rec_save_modal(g_renderer, win_w, win_h, RL);
        EndDrawing();
    }

    free(pixels);
    if (gif) gif_end(gif);
    if (webp) {
        if (!webp_end(webp)) {
            snprintf(err, errsz, "webp encode failed");
            UnloadRenderTexture(rt); screen_free(scr); cast_free(cf);
            return false;
        }
    }
    if (pipe_fp) {
        fclose(pipe_fp);
#ifndef _WIN32
        int status = 0;
        waitpid(pipe_pid, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            char tail[256] = {0};
            read_tail(ff_log, tail, sizeof(tail));
            unlink(ff_log);
            snprintf(err, errsz, "ffmpeg failed: %s",
                     tail[0] ? tail : "(no stderr captured)");
            UnloadRenderTexture(rt); screen_free(scr); cast_free(cf);
            return false;
        }
        unlink(ff_log);
#endif
    }
    /* Second pass: gif intermediate → final apng via ffmpeg.
       libavcodec ships an APNG encoder built-in on every brew /
       distro ffmpeg, so this doesn't need any extra codec. */
    if (two_pass && gif_intermediate[0]) {
        char ff[PATH_MAX];
        find_ffmpeg(ff, sizeof(ff));
        const char *argv[16];
        int n = 0;
        argv[n++] = ff;
        argv[n++] = "-y";
        argv[n++] = "-i"; argv[n++] = gif_intermediate;
        argv[n++] = "-plays"; argv[n++] = "1";   /* play once, no loop. */
        argv[n++] = dst;
        argv[n] = NULL;
        bool ok = run_argv(argv, ff_log);
        if (!ok) {
            char tail[256] = {0};
            read_tail(ff_log, tail, sizeof(tail));
            unlink(ff_log);
            unlink(gif_intermediate);
            snprintf(err, errsz, "ffmpeg %s pass: %s",
                     rec_fmt_ext(fmt), tail[0] ? tail : "(no stderr)");
            UnloadRenderTexture(rt); screen_free(scr); cast_free(cf);
            return false;
        }
        unlink(ff_log);
        unlink(gif_intermediate);
    }
    UnloadRenderTexture(rt);
    screen_free(scr);
    cast_free(cf);
    return true;
}

/* Public entry — preserves the old name so call sites keep working. */
static bool rec_convert(RecFmt fmt, const char *src_cast, const char *dst,
                        char *err, size_t errsz) {
    return rec_render_native(fmt, src_cast, dst, err, errsz);
}

/* Click-handling for the save modal. Tracks consecutive clicks on
   the path field for double-click-to-select-all (~0.45s gap). */
static void rec_save_handle_mouse(RecSaveLayout L) {
    static double last_click_t = -1.0;
    static int    click_count = 0;
    Vector2 mp = GetMousePosition();
    int mx = (int)mp.x, my = (int)mp.y;
    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) return;
    if (rect_hit(L.path, mx, my)) {
        double now = GetTime();
        if (last_click_t >= 0 && (now - last_click_t) < 0.45) click_count++;
        else click_count = 1;
        last_click_t = now;
        g_rec_save.path_focus = true;
        if (click_count >= 2 && g_rec_save.dst_path[0]) {
            g_rec_save.path_sel_all = true;
        } else {
            g_rec_save.path_sel_all = false;
        }
        return;
    }
    /* Click outside the path field — reset multi-click counter. */
    click_count = 0;
    last_click_t = -1.0;
    for (int i = 0; i < REC_FMT_COUNT; i++) {
        if (rect_hit(L.fmt[i], mx, my)) {
            g_rec_save.fmt = (RecFmt)i;
            rec_replace_ext(g_rec_save.dst_path, sizeof(g_rec_save.dst_path),
                            rec_fmt_ext((RecFmt)i));
            g_rec_save.status[0] = 0;
            return;
        }
    }
    if (rect_hit(L.preview_btn, mx, my)) {
        /* Render the chosen format to a temp file and hand it to
           the OS default opener. .cast opens whatever the user has
           registered (often a text editor); the rest get rendered
           as actual gif/mp4/webm/etc. via agg + ffmpeg. */
        char tmp[PATH_MAX];
        snprintf(tmp, sizeof(tmp), "/tmp/rbterm-preview-%d.%s",
                 (int)getpid(), rec_fmt_ext(g_rec_save.fmt));
        if (g_rec_save.fmt == REC_FMT_CAST) {
            /* Open the live src so previewing doesn't disturb the
               temp file the user might still want to save. */
            open_url(g_rec_save.src_path);
            snprintf(g_rec_save.status, sizeof(g_rec_save.status),
                     "opened %s in default app", g_rec_save.src_path);
        } else {
            char err[256];
            snprintf(g_rec_save.status, sizeof(g_rec_save.status),
                     "rendering preview — please wait…");
            if (!rec_convert(g_rec_save.fmt, g_rec_save.src_path, tmp,
                             err, sizeof(err))) {
                snprintf(g_rec_save.status, sizeof(g_rec_save.status),
                         "preview failed: %s", err);
                return;
            }
            open_url(tmp);
            snprintf(g_rec_save.status, sizeof(g_rec_save.status),
                     "previewing %s", tmp);
        }
        return;
    }
    if (rect_hit(L.save_btn, mx, my)) {
        fprintf(stderr, "rbterm: save → fmt=%s dst=%s src=%s\n",
                rec_fmt_ext(g_rec_save.fmt), g_rec_save.dst_path,
                g_rec_save.src_path);
        snprintf(g_rec_save.status, sizeof(g_rec_save.status),
                 "rendering %s — this can take a few seconds…",
                 rec_fmt_ext(g_rec_save.fmt));
        char err[256] = {0};
        if (!rec_convert(g_rec_save.fmt, g_rec_save.src_path,
                         g_rec_save.dst_path, err, sizeof(err))) {
            snprintf(g_rec_save.status, sizeof(g_rec_save.status),
                     "%s", err[0] ? err : "(no error message captured)");
            fprintf(stderr, "rbterm: save FAILED: %s\n", g_rec_save.status);
            return;
        }
        /* For .cast we just renamed the temp file; everything else
           leaves the source intact so the user can re-export to
           another format without re-recording. */
        if (g_rec_save.fmt == REC_FMT_CAST) {
            /* The src has moved to dst — point future operations at
               the new location so they still find data. */
            strncpy(g_rec_save.src_path, g_rec_save.dst_path,
                    sizeof(g_rec_save.src_path) - 1);
            g_rec_save.src_path[sizeof(g_rec_save.src_path) - 1] = 0;
        }
        /* Stat the freshly-written file and report a friendly size. */
        struct stat st;
        long long sz = 0;
        if (stat(g_rec_save.dst_path, &st) == 0) sz = (long long)st.st_size;
        const char *unit = "B"; double v = (double)sz;
        if (v >= 1024.0)        { v /= 1024.0;        unit = "KB"; }
        if (v >= 1024.0)        { v /= 1024.0;        unit = "MB"; }
        snprintf(g_rec_save.status, sizeof(g_rec_save.status),
                 "wrote %.0f %s %s to %s",
                 v, unit, rec_fmt_ext(g_rec_save.fmt), g_rec_save.dst_path);
        fprintf(stderr, "rbterm: %s\n", g_rec_save.status);
        return;
    }
    if (rect_hit(L.close_btn, mx, my)) {
        /* Keep the temp .cast at its original path on Close so a
           power-user can still rummage for it later. */
        fprintf(stderr, "rbterm: recording kept at %s\n", g_rec_save.src_path);
        g_ui_mode = UI_NORMAL;
        return;
    }
    /* Click anywhere else inside the modal drops focus. */
    g_rec_save.path_focus = false;
}

/* Keyboard input for the modal: edit the destination path field
   when focused, Esc closes (= Cancel). */
static void rec_save_handle_keys(void) {
    if (IsKeyPressed(KEY_ESCAPE)) {
        fprintf(stderr, "rbterm: recording kept at %s\n", g_rec_save.src_path);
        g_ui_mode = UI_NORMAL;
        return;
    }
    if (!g_rec_save.path_focus) return;
    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
#if defined(__APPLE__)
    bool cmd  = IsKeyDown(KEY_LEFT_SUPER)   || IsKeyDown(KEY_RIGHT_SUPER);
#else
    bool cmd  = false;
#endif
    bool mod = ctrl || cmd;
    size_t len = strlen(g_rec_save.dst_path);
    if (mod && IsKeyPressed(KEY_A)) {
        if (len > 0) g_rec_save.path_sel_all = true;
        return;
    }
    if (mod && IsKeyPressed(KEY_C)) {
        if (g_rec_save.dst_path[0]) SetClipboardText(g_rec_save.dst_path);
        return;
    }
    if (mod && IsKeyPressed(KEY_V)) {
        const char *clip = GetClipboardText();
        if (clip && *clip) {
            if (g_rec_save.path_sel_all) {
                g_rec_save.dst_path[0] = 0;
                g_rec_save.path_sel_all = false;
                len = 0;
            }
            for (const char *q = clip; *q; q++) {
                unsigned char c = (unsigned char)*q;
                if (c == '\r' || c == '\n' || c == '\t') continue;
                if (c < 32 || c >= 127) continue;
                if (len + 1 >= sizeof(g_rec_save.dst_path)) break;
                g_rec_save.dst_path[len++] = (char)c;
            }
            g_rec_save.dst_path[len] = 0;
        }
        return;
    }
    if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) {
        if (g_rec_save.path_sel_all) {
            g_rec_save.dst_path[0] = 0;
            g_rec_save.path_sel_all = false;
        } else if (len > 0) {
            g_rec_save.dst_path[len - 1] = 0;
        }
        return;
    }
    int cp;
    while ((cp = GetCharPressed()) != 0) {
        if (mod) continue;
        if (cp < 32 || cp >= 127) continue;
        if (g_rec_save.path_sel_all) {
            g_rec_save.dst_path[0] = 0;
            g_rec_save.path_sel_all = false;
            len = 0;
        }
        if (len + 1 >= sizeof(g_rec_save.dst_path)) continue;
        g_rec_save.dst_path[len++] = (char)cp;
        g_rec_save.dst_path[len] = 0;
    }
}

/* Render the save modal: panel + path field + format picker +
   Save / Discard / Cancel. */
static void draw_rec_save_modal(Renderer *r, int win_w, int win_h, RecSaveLayout L) {
    DrawRectangle(0, 0, win_w, win_h, (Color){0, 0, 0, 150});
    DrawRectangle(L.modal.x, L.modal.y, L.modal.w, L.modal.h,
                  (Color){30, 34, 46, 255});
    DrawRectangleLines(L.modal.x, L.modal.y, L.modal.w, L.modal.h,
                       (Color){125, 207, 255, 220});
    DrawRectangle(L.modal.x + 1, L.modal.y + 1, L.modal.w - 2, 38,
                  (Color){38, 42, 58, 255});
    Font *f = (Font *)r->font_data;
    char title[80];
    snprintf(title, sizeof(title), "Save recording (%.1fs)", g_rec_save.duration_s);
    DrawTextEx(*f, title,
               (Vector2){L.modal.x + 20, L.modal.y + 11},
               16, 0, (Color){230, 232, 240, 255});

    /* Path label + field. */
    DrawTextEx(*f, "Path",
               (Vector2){L.modal.x + 22, L.path.y + 8},
               14, 0, (Color){200, 205, 220, 255});
    DrawRectangle(L.path.x, L.path.y, L.path.w, L.path.h,
                  (Color){22, 25, 34, 255});
    DrawRectangleLines(L.path.x, L.path.y, L.path.w, L.path.h,
                       g_rec_save.path_focus ? (Color){125, 207, 255, 255}
                                             : (Color){70, 74, 90, 255});
    BeginScissorMode(L.path.x + 6, L.path.y, L.path.w - 12, L.path.h);
    if (g_rec_save.path_focus && g_rec_save.path_sel_all && g_rec_save.dst_path[0]) {
        Vector2 ssz = MeasureTextEx(*f, g_rec_save.dst_path, 13, 0);
        int sw = (int)ssz.x + 4;
        if (sw > L.path.w - 12) sw = L.path.w - 12;
        DrawRectangle(L.path.x + 6, L.path.y + 4, sw, L.path.h - 8,
                      (Color){64, 100, 150, 200});
    }
    DrawTextEx(*f, g_rec_save.dst_path,
               (Vector2){L.path.x + 8, L.path.y + 8},
               13, 0, (Color){230, 232, 240, 255});
    if (g_rec_save.path_focus && !g_rec_save.path_sel_all &&
        ((long long)(GetTime() * 2.0) & 1) == 0) {
        Vector2 dsz = MeasureTextEx(*f, g_rec_save.dst_path, 13, 0);
        DrawRectangle(L.path.x + 8 + (int)dsz.x + 1, L.path.y + 6, 8, 14,
                      (Color){125, 207, 255, 255});
    }
    EndScissorMode();

    /* Format picker. */
    DrawTextEx(*f, "Format",
               (Vector2){L.modal.x + 22, L.fmt[0].y + 8},
               14, 0, (Color){200, 205, 220, 255});
    static const char *labels[REC_FMT_COUNT] = { "cast", "txt", "gif", "mp4", "webm", "apng", "webp" };
    for (int i = 0; i < REC_FMT_COUNT; i++) {
        Rect rr = L.fmt[i];
        bool sel = (g_rec_save.fmt == (RecFmt)i);
        Color bg = sel ? (Color){46, 92, 150, 255} : (Color){34, 38, 52, 255};
        DrawRectangle(rr.x, rr.y, rr.w, rr.h, bg);
        DrawRectangleLines(rr.x, rr.y, rr.w, rr.h,
                           (Color){125, 207, 255, sel ? 255 : 120});
        Vector2 ts = MeasureTextEx(*f, labels[i], 13, 0);
        Color fg = (Color){230, 232, 240, 255};
        DrawTextEx(*f, labels[i],
                   (Vector2){rr.x + (rr.w - ts.x) / 2,
                             rr.y + (rr.h - ts.y) / 2},
                   13, 0, fg);
    }

    /* Action buttons. */
    struct { Rect r; const char *label; Color bg; Color fg; }
    btns[3] = {
        { L.preview_btn, "Preview",
          (Color){48, 60, 86, 255}, (Color){210, 225, 245, 255} },
        { L.close_btn,   "Close",
          (Color){48, 52, 66, 255}, (Color){210, 215, 230, 255} },
        { L.save_btn,    "Save",
          (Color){48, 78, 58, 255}, (Color){220, 240, 225, 255} },
    };
    for (int i = 0; i < 3; i++) {
        DrawRectangle(btns[i].r.x, btns[i].r.y, btns[i].r.w, btns[i].r.h, btns[i].bg);
        DrawRectangleLines(btns[i].r.x, btns[i].r.y, btns[i].r.w, btns[i].r.h,
                           (Color){150, 160, 180, 200});
        Vector2 ts = MeasureTextEx(*f, btns[i].label, 14, 0);
        DrawTextEx(*f, btns[i].label,
                   (Vector2){btns[i].r.x + (btns[i].r.w - ts.x) / 2,
                             btns[i].r.y + (btns[i].r.h - ts.y) / 2},
                   14, 0, btns[i].fg);
    }

    /* Status / hint line. The status row sits above the button row,
       so it can use the full modal interior (the buttons aren't
       next to it horizontally). Middle-ellipsis only kicks in if
       the message is *still* too wide to fit. */
    {
        int text_x = L.modal.x + 22;
        int text_y = L.save_btn.y - 26;
        int text_w = L.modal.w - 44;
        if (text_w < 100) text_w = 100;
        const char *msg = g_rec_save.status[0]
            ? g_rec_save.status
            : "Save keeps the modal open — export to multiple formats. Close dismisses.";
        Color col = g_rec_save.status[0]
            ? (Color){240, 200, 120, 255}
            : (Color){140, 150, 170, 255};
        /* Keep the font readable — no shrinking. Middle-ellipsis
           below handles overflow instead, so the user gets a
           legible "wrote NN KB gif to /Users/…/foo.gif" rather
           than a tiny 10pt full path. */
        int fs2 = 16;
        Vector2 sz = MeasureTextEx(*f, msg, (float)fs2, 0);
        char buf[512];
        const char *draw = msg;
        if (sz.x > text_w) {
            /* Middle-ellipsis: keep the start (status verb + size)
               and the tail (path/filename) so both ends stay
               legible. Grow the cut amount until the result fits. */
            size_t mlen = strlen(msg);
            for (size_t cut = 1; cut + 4 < mlen; cut++) {
                size_t keep = mlen - cut;
                size_t left = keep / 2;
                size_t right_len = keep - left;
                if (left + 4 + right_len >= sizeof(buf)) break;
                memcpy(buf, msg, left);
                memcpy(buf + left, "\xE2\x80\xA6", 3);   /* U+2026 … */
                memcpy(buf + left + 3, msg + mlen - right_len, right_len);
                buf[left + 3 + right_len] = 0;
                Vector2 ms = MeasureTextEx(*f, buf, (float)fs2, 0);
                if (ms.x <= text_w) { draw = buf; break; }
            }
        }
        BeginScissorMode(text_x, text_y - 2, text_w, fs2 + 6);
        DrawTextEx(*f, draw, (Vector2){text_x, text_y}, (float)fs2, 0, col);
        EndScissorMode();
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
    /* Each row is { chord, description } — two entries per visible row.
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
    if (g_help_tab == 0) {
        /* Navigation: tabs / splits / scroll / modals — all the
           "moving around" chords, no editing or finding. */
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
    } else if (g_help_tab == 1) {
        /* Edit & Search: selection, clipboard, find, plus the
           OSC-133-powered "select last output" / prompt-jump chords
           (cross-referenced — full setup lives on the Shell tab). */
        ROW("",                          "Selection + clipboard");
        ROW("Cmd+A",                     "Select all visible text in active pane");
        ROW("Cmd+C",                     "Copy selection");
        ROW("Cmd+V",                     "Paste");
        ROW("Click + drag",              "Select text");
        ROW("Double-click",              "Select word (smart trim of trailing punctuation)");
        ROW("Triple-click",              "Select line (joins wrapped rows)");
        ROW("",                          "Search");
        ROW("Cmd+F",                     "Open search bar on the active pane");
        ROW("Enter / Down / F3",         "Next match");
        ROW("Shift+Enter / Up / Shift+F3","Previous match");
        ROW("Cmd+A (in search)",         "Select all in search query");
        ROW("Click / Shift+click / drag","Position caret / extend / range select in search");
        ROW("Esc",                       "Close search, restore scroll position");
        ROW("",                          "Shell integration (needs OSC 133 — see Shell tab)");
        ROW("Cmd+Up / Cmd+Down",         "Jump to previous / next prompt");
        ROW("Cmd+Shift+L",               "Select last command's output");
    } else {
        /* Shell integration tab — narrative, not chords. Use chord
           column blank + desc column carrying the prose. */
        ROW("", "What is shell integration?");
        ROW("", "rbterm understands OSC 133 — a small protocol where your");
        ROW("", "shell tells the terminal where each prompt and command");
        ROW("", "starts/ends. Once it knows that structure, rbterm can:");
        ROW("", "");
        ROW("•",  "Paint a green/red badge in the left gutter next to each");
        ROW("",   "command (green = exit 0, red = nonzero).");
        ROW("•",  "Show a cyan spinner in the tab bar while a command is");
        ROW("",   "running, so you can glance over and see if your build is");
        ROW("",   "still going.");
        ROW("•",  "Jump prompt-to-prompt with Cmd+Up / Cmd+Down — no more");
        ROW("",   "hunting for the start of the previous compile in scrollback.");
        ROW("•",  "Select the last command's output with Cmd+Shift+L; then");
        ROW("",   "Cmd+C copies it to the clipboard.");
        ROW("",   "");
        ROW("",   "Setup (one time)");
        ROW("",   "Source the helper from your shell rc:");
        ROW("zsh:", "echo 'source <rbterm-repo>/tools/rbterm-shell-integration.zsh' >> ~/.zshrc");
        ROW("bash:", "echo 'source <rbterm-repo>/tools/rbterm-shell-integration.bash' >> ~/.bashrc");
        ROW("",     "");
        ROW("",     "Open a new pane (Cmd+T) so the new shell picks up the");
        ROW("",     "hook. Existing panes that didn't source the script");
        ROW("",     "stay un-instrumented — that's deliberate.");
        ROW("",     "");
        ROW("",     "Caveats");
        ROW("•",    "Inside tmux / vim / less, rbterm is on the alt screen");
        ROW("",     "with no scrollback, so marks aren't recorded there.");
        ROW("•",    "Other terminals also recognize OSC 133. Sourcing the");
        ROW("",     "helper is safe in iTerm2 / kitty / Wezterm too.");
    }
    ROW_END();
    #undef ROW
    #undef ROW_END

    int row_h = 22;
    int header_h = 50 + 32; /* title bar + tab bar */
    int side_pad = 28;
    int top_pad  = 18;
    int footer_h = 32;
    int needed_h = header_h + n * row_h + top_pad + footer_h;
    int w = 760, h = needed_h;
    if (w > win_w - 40) w = win_w - 40;
    if (h > win_h - 40) h = win_h - 40;
    int mx = (win_w - w) / 2;
    int my = (win_h - h) / 2;

    DrawRectangle(0, 0, win_w, win_h, (Color){0, 0, 0, 150});
    DrawRectangle(mx, my, w, h, (Color){30, 34, 46, 255});
    DrawRectangleLines(mx, my, w, h, (Color){125, 207, 255, 220});
    DrawRectangle(mx + 1, my + 1, w - 2, 38, (Color){38, 42, 58, 255});
    g_help_modal_rect = (Rect){mx, my, w, h};

    Font *f = (Font *)r->font_data;
    char title[80];
    snprintf(title, sizeof(title), "Help (modifier = %s)", MOD);
    DrawTextEx(*f, title,
               (Vector2){ mx + 20, my + 11 },
               16, 0, (Color){230, 232, 240, 255});

    /* Tab bar between title and content. */
    {
        const char *labels[HELP_TAB_COUNT] = {
            "Navigation", "Edit & Search", "Shell integration"
        };
        int bar_y = my + 44;
        int bar_h = 26;
        int bw = (w - 2 * 14 - 2 * 6) / HELP_TAB_COUNT;
        int bx = mx + 14;
        for (int i = 0; i < HELP_TAB_COUNT; i++) {
            Rect tr = { bx + i * (bw + 6), bar_y, bw, bar_h };
            g_help_tab_rects[i] = tr;
            bool on = (g_help_tab == i);
            DrawRectangle(tr.x, tr.y, tr.w, tr.h,
                          on ? (Color){46, 92, 150, 255}
                             : (Color){34, 38, 52, 255});
            DrawRectangleLines(tr.x, tr.y, tr.w, tr.h,
                               (Color){125, 207, 255, on ? 255 : 120});
            Vector2 ts = MeasureTextEx(*f, labels[i], 13, 0);
            DrawTextEx(*f, labels[i],
                       (Vector2){tr.x + (tr.w - ts.x) / 2,
                                 tr.y + (tr.h - ts.y) / 2},
                       13, 0, (Color){230, 232, 240, 255});
        }
    }

    int chord_w = (g_help_tab < 2) ? 240 : 60;
    BeginScissorMode(mx + 8, my + header_h, w - 16,
                     h - header_h - footer_h);
    int y = my + header_h;
    for (int i = 0; i < n; i++) {
        const char *chord = buf[i*2];
        const char *desc  = buf[i*2 + 1];
        if (g_help_tab < 2 && !chord[0]) {
            DrawTextEx(*f, desc,
                       (Vector2){ mx + side_pad, y + 3 },
                       14, 0, (Color){140, 200, 255, 255});
        } else {
            if (chord[0]) {
                DrawTextEx(*f, chord,
                           (Vector2){ mx + side_pad, y + 4 },
                           13, 0, (Color){200, 230, 255, 255});
            }
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

/* Render the Settings modal: panel + title bar, tab-bar buttons,
   the active tab's content (font controls / theme list / session
   logging + key repeat / startup-window mode), the bottom-anchored
   Save-as-Default + Close, and the status / "saved" line above
   them. */
static void draw_settings(Renderer *r, int win_w, int win_h, SettingsLayout L) {
    /* No backdrop dim — the user wants to keep terminal contents
       visible at full brightness while changing settings (mainly so
       theme/font/cursor changes preview live behind the modal). */
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

    /* Tab bar. Active tab gets an accent fill; others sit dim. */
    {
        const char *labels[SETTINGS_TAB_COUNT] = {
            "Font", "Theme", "Cursor", "Session", "Window", "Recording"
        };
        for (int i = 0; i < SETTINGS_TAB_COUNT; i++) {
            Rect tr = L.tab[i];
            bool on = (g_settings_tab == i);
            DrawRectangle(tr.x, tr.y, tr.w, tr.h,
                          on ? (Color){46, 92, 150, 255}
                             : (Color){34, 38, 52, 255});
            DrawRectangleLines(tr.x, tr.y, tr.w, tr.h,
                               (Color){125, 207, 255, on ? 255 : 120});
            Vector2 ts = MeasureTextEx(*f, labels[i], 13, 0);
            DrawTextEx(*f, labels[i],
                       (Vector2){tr.x + (tr.w - ts.x) / 2,
                                 tr.y + (tr.h - ts.y) / 2},
                       13, 0, (Color){230, 232, 240, 255});
        }
    }

    /* Glyph metrics reused across every +/- button in the Font tab. */
    Vector2 ms = MeasureTextEx(*f, "-", 18, 0);
    Vector2 ps = MeasureTextEx(*f, "+", 18, 0);

    if (g_settings_tab == SETTINGS_TAB_FONT) {
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
    DrawTextEx(*f, "-",
               (Vector2){L.dec.x + (L.dec.w - ms.x) / 2,
                         L.dec.y + (L.dec.h - ms.y) / 2},
               18, 0, (Color){230, 232, 240, 255});
    DrawRectangle(L.inc.x, L.inc.y, L.inc.w, L.inc.h, (Color){46, 52, 70, 255});
    DrawRectangleLines(L.inc.x, L.inc.y, L.inc.w, L.inc.h, (Color){125, 207, 255, 180});
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

    }  /* end Font tab */
    if (g_settings_tab == SETTINGS_TAB_THEME) {
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
        /* Dark / Light filter strip across the top of the list. */
        Rect tab_dark, tab_light;
        int strip_h = theme_filter_strip_layout(
            L.theme_list.x, L.theme_list.y, L.theme_list.w,
            &tab_dark, &tab_light);
        theme_filter_strip_draw(f, tab_dark, tab_light);

        int trow_h = 22;
        int rows_y = L.theme_list.y + strip_h;
        int rows_h = L.theme_list.h - strip_h;
        const Theme *ts = themes_all();
        /* Build the filtered index list once per frame. */
        int filtered[1024];
        int n_filt = 0;
        for (int i = 0; i < tcount && n_filt < (int)(sizeof(filtered)/sizeof(filtered[0])); i++) {
            bool light = theme_is_light(&ts[i]);
            if ((g_theme_filter == 1) == light) filtered[n_filt++] = i;
        }
        int tvisible = rows_h / trow_h;
        int tmax_scroll = n_filt - tvisible;
        if (tmax_scroll < 0) tmax_scroll = 0;
        if (g_theme_list_scroll > tmax_scroll) g_theme_list_scroll = tmax_scroll;
        if (g_theme_list_scroll < 0) g_theme_list_scroll = 0;
        BeginScissorMode(L.theme_list.x + 2, rows_y,
                         L.theme_list.w - 4, rows_h - 2);
        for (int v = 0; v < n_filt; v++) {
            int i = filtered[v];
            int ry = rows_y + (v - g_theme_list_scroll) * trow_h;
            if (ry + trow_h < rows_y ||
                ry > rows_y + rows_h) continue;
            bool sel = (i == g_theme_list_selected);
            if (sel) {
                DrawRectangle(L.theme_list.x + 2, ry,
                              L.theme_list.w - 4, trow_h,
                              (Color){46, 62, 90, 220});
            }
            /* Colour swatches on the right. */
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
            const char *bare = theme_display_split(ts[i].name, NULL, 0);
            DrawTextEx(*f, bare,
                       (Vector2){L.theme_list.x + 10, ry + 4},
                       14, 0,
                       sel ? (Color){230, 232, 240, 255}
                           : (Color){200, 205, 220, 255});
        }
        EndScissorMode();
        if (n_filt > tvisible) {
            int track_x = L.theme_list.x + L.theme_list.w - 5;
            int bar_h = rows_h * tvisible / n_filt;
            if (bar_h < 24) bar_h = 24;
            int bar_y = rows_y +
                        (rows_h - bar_h) * g_theme_list_scroll /
                        (tmax_scroll > 0 ? tmax_scroll : 1);
            DrawRectangle(track_x, rows_y, 3, rows_h,
                          (Color){40, 45, 58, 255});
            DrawRectangle(track_x, bar_y, 3, bar_h,
                          (Color){110, 130, 170, 255});
        }
    }

    }  /* end Theme tab */
    if (g_settings_tab == SETTINGS_TAB_CURSOR) {
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

    /* Key-repeat sliders. Two rows: initial delay, then per-repeat
       period. Track is a thin inset bar; the thumb is a tall pill
       overlapping the track. Current value renders to the right.
       Lives on the Cursor tab — keyboard-feel knobs sit naturally
       alongside the cursor-style picker. */
    {
        struct SliderSpec {
            Rect        track;
            const char *label;
            int         val, vmin, vmax;
            const char *unit;
        } rows[2] = {
            { L.repeat_initial, "Repeat delay",
              g_app_settings.key_repeat_initial_ms, 0, 1000, "ms" },
            { L.repeat_rate,    "Repeat rate",
              g_app_settings.key_repeat_rate_ms,    5, 200, "ms" },
        };
        for (int i = 0; i < 2; i++) {
            Rect tr = rows[i].track;
            DrawTextEx(*f, rows[i].label,
                       (Vector2){L.modal.x + 22, tr.y - 3},
                       14, 0, (Color){200, 205, 220, 255});
            DrawRectangle(tr.x, tr.y + tr.h / 2 - 2, tr.w, 4,
                          (Color){46, 52, 70, 255});
            float t = (float)(rows[i].val - rows[i].vmin)
                    / (float)(rows[i].vmax - rows[i].vmin);
            if (t < 0.0f) t = 0.0f; if (t > 1.0f) t = 1.0f;
            int tx = tr.x + (int)(t * (float)tr.w + 0.5f);
            DrawRectangle(tr.x, tr.y + tr.h / 2 - 2, tx - tr.x, 4,
                          (Color){125, 207, 255, 220});
            DrawRectangle(tx - 5, tr.y - 4, 10, tr.h + 8,
                          (Color){220, 228, 245, 255});
            DrawRectangleLines(tx - 5, tr.y - 4, 10, tr.h + 8,
                               (Color){125, 207, 255, 220});
            char vbuf[32];
            snprintf(vbuf, sizeof(vbuf), "%d %s", rows[i].val, rows[i].unit);
            DrawTextEx(*f, vbuf,
                       (Vector2){tr.x + tr.w + 8, tr.y - 3},
                       13, 0, (Color){180, 190, 210, 255});
        }
    }

    }  /* end Cursor tab */
    if (g_settings_tab == SETTINGS_TAB_FONT) {
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

    }  /* end Font tab (padding + spacing) */
    if (g_settings_tab == SETTINGS_TAB_SESSION) {
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

    }  /* end Session tab */
    if (g_settings_tab == SETTINGS_TAB_WINDOW) {
    /* Startup window mode row. Three mutually-exclusive buttons;
       applied on next launch so we don't fight the user's current
       window state. Save as Default persists the pick. */
    {
        DrawTextEx(*f, "On launch",
                   (Vector2){L.modal.x + 22, L.startup_default.y + 8},
                   14, 0, (Color){200, 205, 220, 255});
        struct { Rect r; const char *label; int mode; } opts[] = {
            { L.startup_default,    "Default",    STARTUP_WINDOW_DEFAULT },
#ifdef __APPLE__
            { L.startup_maximized,  "Own Space",  STARTUP_WINDOW_MAXIMIZED },
#else
            { L.startup_maximized,  "Maximized",  STARTUP_WINDOW_MAXIMIZED },
#endif
        };
        for (int i = 0; i < 2; i++) {
            Rect rr = opts[i].r;
            bool on = (g_app_settings.startup_window == opts[i].mode);
            DrawRectangle(rr.x, rr.y, rr.w, rr.h,
                          on ? (Color){46, 92, 150, 255} : (Color){34, 38, 52, 255});
            DrawRectangleLines(rr.x, rr.y, rr.w, rr.h,
                               (Color){125, 207, 255, on ? 255 : 150});
            Vector2 bsz = MeasureTextEx(*f, opts[i].label, 13, 0);
            DrawTextEx(*f, opts[i].label,
                       (Vector2){rr.x + (rr.w - bsz.x) / 2,
                                 rr.y + (rr.h - bsz.y) / 2},
                       13, 0, (Color){230, 232, 240, 255});
        }
    }

    }  /* end Window tab */
    if (g_settings_tab == SETTINGS_TAB_RECORDING) {
    /* Recording → default save folder. The path is used both as the
       initial drop for stop-recording temp .cast files and as the
       default destination shown in the save modal. */
    DrawTextEx(*f, "Save folder",
               (Vector2){L.modal.x + 22, L.rec_dir.y + 8},
               14, 0, (Color){200, 205, 220, 255});
    DrawRectangle(L.rec_dir.x, L.rec_dir.y, L.rec_dir.w, L.rec_dir.h,
                  (Color){22, 25, 34, 255});
    DrawRectangleLines(L.rec_dir.x, L.rec_dir.y, L.rec_dir.w, L.rec_dir.h,
                       g_settings_recdir_focus ? (Color){125, 207, 255, 255}
                                               : (Color){70, 74, 90, 255});
    BeginScissorMode(L.rec_dir.x + 6, L.rec_dir.y,
                     L.rec_dir.w - 12, L.rec_dir.h);
    if (g_settings_recdir_focus && g_settings_recdir_sel_all
        && g_app_settings.rec_dir[0]) {
        Vector2 ssz = MeasureTextEx(*f, g_app_settings.rec_dir, 13, 0);
        int sw = (int)ssz.x + 4;
        if (sw > L.rec_dir.w - 12) sw = L.rec_dir.w - 12;
        DrawRectangle(L.rec_dir.x + 6, L.rec_dir.y + 4, sw,
                      L.rec_dir.h - 8, (Color){64, 100, 150, 200});
    }
    DrawTextEx(*f, g_app_settings.rec_dir,
               (Vector2){L.rec_dir.x + 8, L.rec_dir.y + 8},
               13, 0, (Color){230, 232, 240, 255});
    if (g_settings_recdir_focus && !g_settings_recdir_sel_all
        && ((long long)(GetTime() * 2.0) & 1) == 0) {
        Vector2 dsz = MeasureTextEx(*f, g_app_settings.rec_dir, 13, 0);
        DrawRectangle(L.rec_dir.x + 8 + (int)dsz.x + 1,
                      L.rec_dir.y + 8, 8, 14,
                      (Color){125, 207, 255, 255});
    }
    EndScissorMode();
    DrawTextEx(*f, "New recordings drop a temp .cast here. Save as Default to persist.",
               (Vector2){L.modal.x + 22, L.rec_dir.y + L.rec_dir.h + 10},
               12, 0, (Color){140, 150, 170, 255});
    }  /* end Recording tab */

    /* Save-as-Default button. */
    DrawRectangle(L.save_default.x, L.save_default.y,
                  L.save_default.w, L.save_default.h,
                  (Color){48, 78, 58, 255});
    DrawRectangleLines(L.save_default.x, L.save_default.y,
                       L.save_default.w, L.save_default.h,
                       (Color){150, 220, 170, 200});
    Vector2 sdsz = MeasureTextEx(*f, "Save", 14, 0);
    DrawTextEx(*f, "Save",
               (Vector2){L.save_default.x + (L.save_default.w - sdsz.x) / 2,
                         L.save_default.y + (L.save_default.h - sdsz.y) / 2},
               14, 0, (Color){220, 240, 225, 255});

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

#ifdef _WIN32
/* main.c compiles with NOUSER which strips shellapi.h from the
   windows.h pull. Forward-declare the two bits we need — on x86_64
   / ARM64 there's only one calling convention, so no need to spell
   out __stdcall / WINAPI. */
__declspec(dllimport) void *__stdcall ShellExecuteW(
    void *hwnd, const wchar_t *verb, const wchar_t *file,
    const wchar_t *params, const wchar_t *dir, int show);
#define SW_SHOWNORMAL 1
#endif

/* Platform-independent "open this URL in the user's default handler".
   Fork+execvp on Unix so the URL is passed as argv (no shell injection).
   Windows uses ShellExecuteW which launches the registered protocol
   handler directly. */
static void open_url(const char *url) {
    if (!url || !*url) return;
#ifdef _WIN32
    wchar_t wurl[2048];
    int n = MultiByteToWideChar(CP_UTF8, 0, url, -1,
                                wurl, (int)(sizeof(wurl)/sizeof(wurl[0])));
    if (n <= 0) return;
    ShellExecuteW(NULL, L"open", wurl, NULL, NULL, SW_SHOWNORMAL);
#else
    pid_t pid = fork();
    if (pid == 0) {
        /* Detach from the rbterm process group so Ctrl-C etc. don't
           hit the browser. */
        setsid();
        #ifdef __APPLE__
            execlp("open", "open", url, (char *)NULL);
        #else
            execlp("xdg-open", "xdg-open", url, (char *)NULL);
        #endif
        _exit(127);
    }
    /* We don't want to wait() — if the user clicks many links we'd
       accumulate children. SIGCHLD handler in main.c already reaps. */
#endif
}

/* ---------- Main ---------- */

/* Print the --help message to stdout. */
static void usage(void) {
    printf("rbterm — terminal emulator\n"
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
    input_set_repeat(g_app_settings.key_repeat_initial_ms,
                     g_app_settings.key_repeat_rate_ms);

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
#elif defined(_WIN32)
    /* Windows: HIGHDPI makes raylib size the OpenGL framebuffer to
       physical pixels while GetScreenWidth() returns logical pixels.
       When the DPI scale is non-1 (especially inside VMs + Parallels)
       the two coordinate systems diverge and the right-cluster tab
       buttons land outside the visible viewport. Stick with 1:1
       pixel mapping — slightly less crisp on HiDPI panels but the
       layout is always correct. */
    unsigned int cfg_flags = FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT;
#else
    unsigned int cfg_flags = FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT
                           | FLAG_WINDOW_HIGHDPI;
#endif
    if (init_opacity < 0.999f) cfg_flags |= FLAG_WINDOW_TRANSPARENT;
    if (init_undecorated)      cfg_flags |= FLAG_WINDOW_UNDECORATED;
    SetConfigFlags(cfg_flags);
    SetTraceLogCallback(rl_trace_log);
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
    /* Apply startup window layout (configured under Settings). Must
       come after InitWindow but before the first frame so the user
       doesn't see the default 800x500 briefly before the transition.
       On macOS, MAXIMIZED means native fullscreen in its own Space
       so three-finger swipe crosses between it and other desktops. */
#ifndef __EMSCRIPTEN__
    if (g_app_settings.startup_window == STARTUP_WINDOW_MAXIMIZED) {
#ifdef __APPLE__
        mac_enter_native_fullscreen();
#else
        MaximizeWindow();
#endif
    }
    /* STARTUP_WINDOW_FULLSCREEN is accepted by config parsing for
       back-compat but is now treated as DEFAULT — raylib's
       ToggleFullscreen misbehaves on our macOS GLFW build. */
#endif

#ifdef __APPLE__
    /* Strip Cmd+W from AppKit's File > Close Window menu item so our
       in-app handler can use the chord to close just the active tab. */
    mac_disable_close_menu_item();
    /* AppKit intercepts Ctrl+Tab before GLFW can see it. A local
       NSEvent monitor swallows the key and latches a flag we read
       each frame via mac_consume_ctrl_tab(). */
    mac_install_ctrl_tab_monitor();
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
        /* Clear activity on whichever tab is currently foregrounded
           — whether it got there by click, chord, or the dirty dance
           in tab_close. Simpler than patching every site that writes
           g_active. */
        if (g_active >= 0 && g_active < g_num_tabs)
            g_tabs[g_active]->activity = false;
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
                        rec_write(p, readbuf, (size_t)n);
                        drained += (size_t)n;
                        if (drained >= drain_cap) break;
                        continue;
                    }
                    if (n < 0) pane_dead[pi] = true;
                    break;
                }
                if (!pty_alive(p->pty)) pane_dead[pi] = true;
                /* Activity on a background tab lights the tab-bar
                   indicator until the user switches to it. */
                if (drained > 0 && i != g_active) t->activity = true;
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
                else if (h.on_rec_start) {
                    if (!g_rec.active) {
                        Tab *_t = active_tab();
                        Pane *_p = _t ? active_pane_of(_t) : NULL;
                        if (_p) rec_start(_p);
                    }
                }
                else if (h.on_rec_stop) {
                    rec_stop();
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
                    else {
                        g_active = h.tab_idx;
                        /* Arm a potential drag — the hold-handler below
                           promotes this to a real drag once the cursor
                           moves past the threshold. */
                        g_tab_press_idx = h.tab_idx;
                        g_tab_press_mx  = (int)mp.x;
                        g_tab_dragging  = false;
                    }
                }
                cur = active_tab();
            }
        }

        /* Tab-reorder drag. Handled outside the in_tab_bar guard so
           the drag survives the cursor leaving the bar briefly (e.g.
           user overshoots vertically). Released anywhere ends it. */
        if (!modal_open && g_tab_press_idx >= 0) {
            if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                int mx = (int)mp.x;
                if (!g_tab_dragging && (mx - g_tab_press_mx > 6
                                     || g_tab_press_mx - mx > 6)) {
                    g_tab_dragging = true;
                }
                if (g_tab_dragging) {
                    int tw = tab_width_for(win_w_now);
                    int tab_start = TAB_SSH_W;
                    int target = (mx - tab_start) / tw;
                    if (target < 0) target = 0;
                    if (target >= g_num_tabs) target = g_num_tabs - 1;
                    if (target != g_tab_press_idx && target >= 0) {
                        Tab *moving = g_tabs[g_tab_press_idx];
                        if (target > g_tab_press_idx) {
                            for (int i = g_tab_press_idx; i < target; i++)
                                g_tabs[i] = g_tabs[i + 1];
                        } else {
                            for (int i = g_tab_press_idx; i > target; i--)
                                g_tabs[i] = g_tabs[i - 1];
                        }
                        g_tabs[target] = moving;
                        g_tab_press_idx = target;
                        g_active = target;
                        cur = active_tab();
                    }
                }
            } else {
                g_tab_press_idx = -1;
                g_tab_dragging  = false;
            }
        }

        if (!modal_open && !in_tab_bar && cur) {
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
                        /* Search bar mouse handling: click positions
                           the caret, shift-click extends selection,
                           drag selects a subset. Must run before the
                           grid-select path or the click would also
                           start a terminal selection. */
                        if (p->search.active) {
                            int bx, by, bw, bh;
                            search_bar_rect(pr.x, pr.y, pr.w, &bx, &by, &bw, &bh);
                            int mx = (int)mp.x, my = (int)mp.y;
                            bool inside_bar = (mx >= bx && mx < bx + bw &&
                                               my >= by && my < by + bh);
                            Font *ff = (Font *)r.font_data;
                            int text_left = bx + SEARCH_TEXT_OFF;
                            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && inside_bar) {
                                bool sh = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
                                int rel_x = mx - text_left;
                                int new_caret = search_x_to_caret(*ff, p->search.query, rel_x);
                                if (sh) {
                                    if (p->search.sel_anchor < 0) p->search.sel_anchor = p->search.caret;
                                    p->search.caret = new_caret;
                                } else {
                                    p->search.sel_anchor = new_caret;  /* drag anchor */
                                    p->search.caret = new_caret;
                                }
                                p->search.mouse_down = true;
                                goto pane_click_done;
                            }
                            if (p->search.mouse_down && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                                int rel_x = mx - text_left;
                                p->search.caret = search_x_to_caret(*ff, p->search.query, rel_x);
                                goto pane_click_done;
                            }
                            if (p->search.mouse_down && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                                p->search.mouse_down = false;
                                if (p->search.sel_anchor == p->search.caret)
                                    p->search.sel_anchor = -1;
                                goto pane_click_done;
                            }
                        }
                        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                            /* Cmd/Ctrl+click on an OSC 8 hyperlink opens
                               it in the system default handler and
                               doesn't start a selection. */
                            if (ui_key_down()) {
                                Cell cc = screen_view_cell(p->scr, mcol, mrow);
                                const char *u = screen_link_url(p->scr, cc.link_id);
                                if (u && *u) {
                                    open_url(u);
                                    goto pane_click_done;
                                }
                                /* Plain-text URL fallback when the
                                   cell isn't OSC-8-tagged. */
                                int uc0, uc1;
                                if (url_at_view_pos(p->scr, mcol, mrow, &uc0, &uc1)) {
                                    char *turl = url_copy_span(p->scr, mrow, uc0, uc1);
                                    if (turl) {
                                        open_url(turl);
                                        free(turl);
                                        goto pane_click_done;
                                    }
                                }
                            }
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
                        pane_click_done: ;
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

        /* Save-recording modal. */
        if (g_ui_mode == UI_REC_SAVE) {
            RecSaveLayout RL = rec_save_layout(win_w_now, win_h_now);
            rec_save_handle_mouse(RL);
            rec_save_handle_keys();
            cur = active_tab();
            if (!cur) break;
            BeginDrawing();
            ClearBackground((Color){0, 0, 0, 255});
            draw_tab_bar(&r, win_w_now);
            draw_tab_contents(&r, cur, win_w_now, win_h_now, GetTime(), IsWindowFocused());
            if (g_ui_mode == UI_REC_SAVE) {
                draw_rec_save_modal(&r, win_w_now, win_h_now, RL);
            }
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
            /* Hit-test against the actual panel rect that
               draw_help_modal published last frame — different tabs
               have different content heights, so a fixed-size
               fallback would wrongly mark the empty space above/
               below the panel as "inside". On the very first open
               the rect is zero (draw hasn't run yet); g_help_just_
               opened below eats that frame's click anyway. */
            Rect hr = g_help_modal_rect;
            Vector2 mp_help = GetMousePosition();
            bool inside = (hr.w > 0 && mp_help.x >= hr.x &&
                           mp_help.x < hr.x + hr.w &&
                           mp_help.y >= hr.y &&
                           mp_help.y < hr.y + hr.h);
            if (g_help_just_opened) {
                /* Eat the press from the same frame that opened the
                   modal — the help button sits outside the modal
                   panel, so the click-outside check would otherwise
                   close it instantly. */
                g_help_just_opened = false;
            } else if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && inside) {
                /* Tab switch — uses rects populated by draw_help_modal
                   on the previous frame. */
                int mxp = (int)mp_help.x, myp = (int)mp_help.y;
                for (int _i = 0; _i < HELP_TAB_COUNT; _i++) {
                    Rect tr = g_help_tab_rects[_i];
                    if (tr.w > 0 && mxp >= tr.x && mxp < tr.x + tr.w &&
                        myp >= tr.y && myp < tr.y + tr.h) {
                        g_help_tab = _i;
                        break;
                    }
                }
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
            else if (IsKeyPressed(KEY_F)) {
                /* Cmd/Ctrl+F: open the search bar on the active pane.
                   Honest behaviour: inside tmux / vim / less rbterm
                   is on the alt screen and has no scrollback, so
                   search only covers the current screenful. */
                Tab *_ct = active_tab();
                Pane *_ap = _ct ? active_pane_of(_ct) : NULL;
                if (_ap) search_open(_ap);
            }
            /* OSC 133 navigation. Cmd/Ctrl+Up jumps to the previous
               prompt (an A-marked row above the current view), Down
               jumps to the next. Walks abs_rows from the visible
               anchor outward. Silent no-op when no marks exist. */
            else if (IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_DOWN)) {
                bool back = IsKeyPressed(KEY_UP);
                Tab *_ct = active_tab();
                Pane *_ap = _ct ? active_pane_of(_ct) : NULL;
                if (_ap && _ap->scr) {
                    Screen *sc = _ap->scr;
                    int total = screen_total_rows(sc);
                    int sb_len = screen_scrollback_len(sc);
                    int rows = screen_rows(sc);
                    int off = screen_view_offset(sc);
                    /* Anchor = abs_row at the top of the current view. */
                    int anchor = sb_len - off;
                    int target = -1;
                    if (back) {
                        for (int r = anchor - 1; r >= 0; r--) {
                            if (screen_pmark_at_abs(sc, r, NULL) == 'A') { target = r; break; }
                        }
                    } else {
                        for (int r = anchor + 1; r < total; r++) {
                            if (screen_pmark_at_abs(sc, r, NULL) == 'A') { target = r; break; }
                        }
                    }
                    if (target >= 0) {
                        /* Place the target row near the top of the
                           viewport (abs_row maps to vy = abs_row - sb_len + view_off). */
                        int want_off = sb_len - target;
                        if (want_off < 0) want_off = 0;
                        if (want_off > sb_len) want_off = sb_len;
                        screen_scroll_reset(sc);
                        if (want_off > 0) screen_scroll_view(sc, want_off);
                        (void)rows;
                    }
                }
            }
            /* Cmd/Ctrl+Shift+L: select the most recent command's
               output (between the latest C mark and the next A/D
               or end of buffer). Then Cmd+C copies it. */
            else if (shift_held && IsKeyPressed(KEY_L)) {
                Tab *_ct = active_tab();
                Pane *_ap = _ct ? active_pane_of(_ct) : NULL;
                if (_ap && _ap->scr) {
                    Screen *sc = _ap->scr;
                    int total = screen_total_rows(sc);
                    int sb_len = screen_scrollback_len(sc);
                    int rows = screen_rows(sc);
                    int cols = screen_cols(sc);
                    int c_row = -1;
                    for (int r = total - 1; r >= 0; r--) {
                        if (screen_pmark_at_abs(sc, r, NULL) == 'C') { c_row = r; break; }
                    }
                    if (c_row >= 0) {
                        int end_row = total;  /* exclusive */
                        for (int r = c_row + 1; r < total; r++) {
                            uint8_t mk = screen_pmark_at_abs(sc, r, NULL);
                            if (mk == 'A' || mk == 'D') { end_row = r; break; }
                        }
                        if (end_row > c_row) {
                            /* Scroll so c_row is on screen (top), then
                               translate abs rows into view rows. */
                            int want_off = sb_len - c_row;
                            if (want_off < 0) want_off = 0;
                            if (want_off > sb_len) want_off = sb_len;
                            screen_scroll_reset(sc);
                            if (want_off > 0) screen_scroll_view(sc, want_off);
                            int new_off = screen_view_offset(sc);
                            int va = c_row - sb_len + new_off;
                            int vb = (end_row - 1) - sb_len + new_off;
                            if (va < 0) va = 0;
                            if (vb >= rows) vb = rows - 1;
                            _ap->sel.active = true;
                            _ap->sel.dragging = false;
                            _ap->sel.a_col = 0;
                            _ap->sel.a_row = va;
                            _ap->sel.b_col = cols - 1;
                            _ap->sel.b_row = vb;
                        }
                    }
                }
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

        /* Ctrl+Tab / Ctrl+Shift+Tab cycles tabs. Real Ctrl — Cmd+Tab
           stays the macOS app switcher. On Linux/Windows GLFW
           delivers Ctrl+Tab to IsKeyPressed normally; on macOS AppKit
           swallows it, so an NSEvent monitor (installed at startup)
           latches a flag we drain here each frame. */
        int cycle_dir = 0;   /* +1 forward, -1 backward */
#ifdef __APPLE__
        int mct = mac_consume_ctrl_tab();
        if (mct == 1) cycle_dir = +1;
        else if (mct == 2) cycle_dir = -1;
#else
        if ((IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL))
            && IsKeyPressed(KEY_TAB)) {
            cycle_dir = (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT))
                        ? -1 : +1;
        }
#endif
        if (cycle_dir != 0 && g_num_tabs > 1) {
            g_active = (cycle_dir > 0)
                       ? (g_active + 1) % g_num_tabs
                       : (g_active - 1 + g_num_tabs) % g_num_tabs;
            cur = active_tab();
        }

        if (!cur) break;

        Pane *ap = active_pane_of(cur);
        InputActions acts = {0};
        size_t in_n = 0;
        if (ap && ap->search.active) {
            /* Search bar owns the keyboard — consume raylib's key
               events here so they don't also land in the shell. */
            search_handle_input(ap);
        } else if (ap) {
            in_n = input_poll(ap->scr, inputbuf, sizeof(inputbuf), &acts);
        }
        if (ap && acts.scroll_rows != 0) {
            /* Keep the selection anchored to the content it started on
               while the user scrolls through history. screen_scroll_view
               clamps at the scrollback bounds, so we read the before/
               after view_offset to know how much the viewport actually
               moved and shift the selection rows by the same amount. */
            int before = screen_view_offset(ap->scr);
            screen_scroll_view(ap->scr, acts.scroll_rows);
            int after  = screen_view_offset(ap->scr);
            int delta  = after - before;
            if (delta != 0 && ap->sel.active && !ap->sel.dragging) {
                ap->sel.a_row += delta;
                ap->sel.b_row += delta;
            }
        }
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
