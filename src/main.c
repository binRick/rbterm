#include "raylib.h"
#include "cast.h"
#include "gif_encoder.h"
#include "webp_encoder.h"
#include "hud.h"
#include "rec_effects.h"
#include "shape.h"
#include <stdarg.h>
#ifdef RBTERM_SSH
#include <pthread.h>
#endif
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
  #include <io.h>           /* _access */
  #define strcasecmp  _stricmp
  #define strncasecmp _strnicmp
  /* MSVC's <io.h> _access doesn't accept X_OK — Windows has no
     execute bit. Map X_OK to 0 (F_OK = "exists"); good enough for
     our find_ffmpeg probe where the goal is "is there a file at
     this path". `access` itself is provided as an alias to _access
     by Microsoft's CRT compatibility headers. */
  #ifndef X_OK
    #define X_OK 0
  #endif
  /* Windows lacks pid_t. Our subprocess helpers are no-ops on
     Windows (the bodies are #ifndef _WIN32 guarded), but the
     function signatures still mention pid_t. Aliasing it to int
     lets the prototypes parse without dragging in a fork model
     we don't use here. */
  typedef int pid_t;
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
  bool mac_pick_open_file(char *out, size_t cap);
  bool mac_pick_save_file(const char *suggested, char *out, size_t cap);
  bool mac_pick_open_directory(const char *prompt_title, char *out, size_t cap);
  /* Quake-style global hotkey — installed when STARTUP_WINDOW_BORDERLESS
     is in force. Cmd+` toggles rbterm visibility from any app. */
  void mac_install_quake_hotkey(void);
  int  mac_consume_quake_toggle(void);
  void mac_toggle_quake_visibility(void);
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
    STARTUP_WINDOW_FULLSCREEN = 1,   /* (back-compat, treated as DEFAULT) */
    STARTUP_WINDOW_MAXIMIZED  = 2,   /* macOS: native fullscreen / "Own Space" */
    STARTUP_WINDOW_SMALL      = 3,   /* fixed 720x480 centred */
    STARTUP_WINDOW_MEDIUM     = 4,   /* fixed 1024x720 centred */
    STARTUP_WINDOW_LARGE      = 5,   /* fixed 1280x900 centred */
    STARTUP_WINDOW_FILL       = 6,   /* fill the current monitor (still windowed) */
    STARTUP_WINDOW_BORDERLESS = 7,   /* borderless windowed-fullscreen (no own-Space) */
} StartupWindowMode;

typedef enum {
    HUD_POS_TOP_RIGHT    = 0,
    HUD_POS_TOP_LEFT     = 1,
    HUD_POS_BOTTOM_RIGHT = 2,
    HUD_POS_BOTTOM_LEFT  = 3,
} HudPosition;

/* Per-HUD-field index into the AppSettings.hud_show / hud_color /
   hud_size parallel arrays. Keep the integer values stable since we
   persist the indices to disk. */
typedef enum {
    HUD_FIELD_HOST = 0,
    HUD_FIELD_IP   = 1,
    HUD_FIELD_LOAD = 2,
    HUD_FIELD_MEM  = 3,
    HUD_FIELD_DISK = 4,
    HUD_FIELD_COUNT = 5,
} HudField;

/* Eight-colour preset palette for HUD fields. Storing an index
   instead of an RGB triple keeps the click-to-cycle UX cheap and
   the persisted config tiny. Indices are stable across releases. */
#define HUD_PALETTE_COUNT 8

typedef struct {
    bool log_enabled;
    char log_dir[PATH_MAX];
    int  key_repeat_initial_ms;  /* delay before first repeat fires (held key) */
    int  key_repeat_rate_ms;     /* period between subsequent repeats */
    int  cursor_style;           /* CursorStyle enum — default for new panes */
    char cursor_color[16];       /* "#rrggbb" or empty (= use natural cursor colour, the cell's fg) */
    int  startup_window;         /* StartupWindowMode */
    char rec_dir[PATH_MAX];      /* default folder for saved recordings */
    bool show_hud;               /* master enable for the system-info overlay */
    int  hud_pos;                /* HudPosition — which corner of the pane */
    bool hud_show[HUD_FIELD_COUNT];   /* per-field visibility */
    int  hud_color[HUD_FIELD_COUNT];  /* index into the preset palette (0..HUD_PALETTE_COUNT-1) */
    int  hud_size[HUD_FIELD_COUNT];   /* font size in points, clamped 10..18 */
    bool hud_show_cpu;           /* CPU sparkline (last 60 sec) under the text slab */
    bool hud_collapsed;          /* user rolled the slab up to a chevron tab */

    /* Launch entries — what tabs to open when rbterm starts. Empty
       count = fall back to the default behaviour (one local shell).
       Otherwise we open one tab per entry, in order. SSH entries
       resolve `host` against ~/.ssh/config via libssh's normal
       parsing; the user's saved-host appearance overrides apply.
       Persisted as `launch.<i>=local`, `launch.<i>=ssh:<alias>`,
       or `launch.<i>=session:<name>`. `launch_active` is the index
       of the entry whose tab should be focused once the launch
       sweep finishes (clamped at boot to a valid index). */
#define LAUNCH_ENTRY_MAX 16
#define LAUNCH_KIND_LOCAL   0
#define LAUNCH_KIND_SSH     1
#define LAUNCH_KIND_SESSION 2
    struct {
        int  kind;          /* 0 = local, 1 = ssh, 2 = session */
        char host[128];     /* ssh alias OR session name; "" for local */
    } launch[LAUNCH_ENTRY_MAX];
    int  launch_count;
    int  launch_active;     /* row index that becomes the active tab on startup */

    /* Default visual effects applied to every new pane's live render.
       SSH panes inherit this then have any matching `# rbterm-effects-*`
       directive from ~/.ssh/config layered on top. The `speed` field
       is unused for live rendering — it only matters during recording
       playback — but we keep it in the struct so the same RecEffects
       type works in both contexts. */
    RecEffects effects;

    /* OpenType ligature shaping (Settings → Font → Ligatures). When
       on the renderer routes each row through HarfBuzz before drawing
       and substitutes ligature glyphs (=>, !=, >=, …). Only applies
       to fonts loaded from embedded blobs (the bundled set);
       disk-path fonts don't shape because we don't keep their bytes
       around. */
    bool ligatures;

    /* Default folder for `Cmd+Shift+S` / camera-button screenshots.
       Populated to ~/Desktop on first run; configurable via the
       Recording settings tab. Created on first save with mkdir_p. */
    char screenshot_dir[PATH_MAX];
} AppSettings;

/* SSH key inventory — populated by ssh_keys_rescan from ~/.ssh.
   Each entry is one private/public pair (we list them by the
   public-key file's stem so id_ed25519.pub → name "id_ed25519"). */
#define SSH_KEYS_MAX 32
typedef struct {
    char name[64];        /* file stem, e.g. "id_ed25519" */
    char algo[16];        /* "ed25519" / "rsa" / "ecdsa" / "dsa" */
    char fingerprint[80]; /* short ssh-keygen -lf output, may be empty */
    char pubpath[PATH_MAX]; /* absolute path to .pub */
    char privpath[PATH_MAX];/* absolute path to private (may not exist) */
    bool has_private;
    time_t mtime;         /* .pub file mtime — used for newest-first sort */
} SshKeyEntry;
static SshKeyEntry g_ssh_keys[SSH_KEYS_MAX];
static int         g_ssh_keys_count = 0;

/* Generate-key form (Settings → Keys "+ Generate" button). */
typedef struct {
    int   type_idx;       /* 0 = ed25519, 1 = rsa */
    char  name[64];       /* file stem; written to ~/.ssh/<name> */
    char  pass[256];      /* optional passphrase */
    int   focus_field;    /* 0 = name, 1 = pass */
    char  status[256];    /* error / success line */
    bool  open;
    bool  sel_all;        /* Cmd+A — next text input replaces field */
} SshKeyGenForm;
static SshKeyGenForm g_keygen_form;

/* Per-row install dropdown — same shape as the Settings → Launch
   host picker. -1 = closed, otherwise the key index whose host
   list is visible. */
static int g_keys_install_dropdown = -1;
static int g_keys_install_scroll   = 0;
/* Index of the key whose delete-confirmation modal is visible.
   -1 = no modal. */
static int g_keys_delete_idx       = -1;
/* Status line shown below the keys list (e.g. "Generated id_rbterm",
   "ssh-copy-id failed: …"). */
static char g_keys_status[256];

/* Deferred-SSH-launch queue. main() opens local entries before
   the first frame so the window appears immediately; SSH entries
   land here and the main loop drains one per frame so the user
   sees their window populate progressively rather than staring
   at a black screen for 1-2 s × N hosts. */
static struct {
    char host[128];
    bool is_active;        /* this entry's row was Settings → Launch's "active" pick */
} g_launch_pending[16];
static int g_launch_pending_count = 0;
static int g_launch_pending_pos   = 0;

/* Parallel SSH connect storage for startup launches. The actual
   struct + state live further down (declared after SshProfile so
   each worker can hold a snapshot). See SshLaunchWorker. */

/* Per-instance HUD configuration. Used both as the per-SSH-host
   override (Tab/SshProfile/SshForm) and as the value returned by
   hud_effective(active_tab) — render code reads from it uniformly
   so it doesn't need to know whether the values came from the
   global settings or a host override. */
typedef struct {
    bool override;            /* only meaningful inside SshProfile/Tab/SshForm:
                                 false → fall back to g_app_settings */
    bool show;
    int  pos;                 /* HudPosition */
    bool field_show[HUD_FIELD_COUNT];
    int  field_color[HUD_FIELD_COUNT];
    int  field_size[HUD_FIELD_COUNT];
    bool show_cpu;
} HudConfig;

/* Indexed palette used for HUD field colours. Keep the order stable
   — the index is what gets saved to disk. */
static const Color HUD_PALETTE[HUD_PALETTE_COUNT] = {
    {205, 215, 230, 230},   /* 0 default light grey */
    {255, 255, 255, 240},   /* 1 white */
    {120, 220, 255, 235},   /* 2 cyan */
    {140, 230, 160, 235},   /* 3 green */
    {250, 220, 110, 235},   /* 4 yellow */
    {250, 170, 110, 235},   /* 5 orange */
    {245, 170, 220, 235},   /* 6 pink */
    {180, 175, 245, 235},   /* 7 lavender */
};
static AppSettings g_app_settings;
static char        g_settings_status[160]; /* status line in the settings modal */

/* Pull a HudConfig view of the global app settings. Used as the
   fallback for tabs that don't override and as the seed when a
   user enables the per-host override on a fresh form. */
static HudConfig hud_config_from_app_settings(void) {
    HudConfig c = {0};
    c.override = false;
    c.show     = g_app_settings.show_hud;
    c.pos      = g_app_settings.hud_pos;
    for (int i = 0; i < HUD_FIELD_COUNT; i++) {
        c.field_show[i]  = g_app_settings.hud_show[i];
        c.field_color[i] = g_app_settings.hud_color[i];
        c.field_size[i]  = g_app_settings.hud_size[i];
    }
    c.show_cpu = g_app_settings.hud_show_cpu;
    return c;
}
/* hud_effective(tab) is defined after the Tab struct since it
   reads tab->ssh_hud / tab->is_ssh; declared here so call sites
   in earlier static helpers compile. */

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
        } else if (strcmp(k, "screenshot_dir") == 0) {
            strncpy(g_app_settings.screenshot_dir, v,
                    sizeof(g_app_settings.screenshot_dir) - 1);
            g_app_settings.screenshot_dir[
                sizeof(g_app_settings.screenshot_dir) - 1] = 0;
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
        } else if (strcmp(k, "cursor_color") == 0) {
            strncpy(g_app_settings.cursor_color, v,
                    sizeof(g_app_settings.cursor_color) - 1);
            g_app_settings.cursor_color[
                sizeof(g_app_settings.cursor_color) - 1] = 0;
        } else if (strcmp(k, "startup_window") == 0) {
            if      (!strcmp(v, "maximized"))  g_app_settings.startup_window = STARTUP_WINDOW_MAXIMIZED;
            else if (!strcmp(v, "small"))      g_app_settings.startup_window = STARTUP_WINDOW_SMALL;
            else if (!strcmp(v, "medium"))     g_app_settings.startup_window = STARTUP_WINDOW_MEDIUM;
            else if (!strcmp(v, "large"))      g_app_settings.startup_window = STARTUP_WINDOW_LARGE;
            else if (!strcmp(v, "fill"))       g_app_settings.startup_window = STARTUP_WINDOW_FILL;
            else if (!strcmp(v, "borderless")) g_app_settings.startup_window = STARTUP_WINDOW_BORDERLESS;
            /* "fullscreen" is accepted for back-compat but demoted to
               borderless — the original option ran ToggleFullscreen
               which misbehaves on macOS; borderless gives the same
               distraction-free effect without the bugs. */
            else if (!strcmp(v, "fullscreen")) g_app_settings.startup_window = STARTUP_WINDOW_BORDERLESS;
            else                               g_app_settings.startup_window = STARTUP_WINDOW_DEFAULT;
        } else if (strcmp(k, "launch_active") == 0) {
            int vi = atoi(v);
            if (vi < 0) vi = 0;
            g_app_settings.launch_active = vi;
        } else if (strncmp(k, "effects.", 8) == 0) {
            /* effects.<name>=<value> — pixel-effect default, parsed
               via the shared helper so config-file and SSH-stanza
               readers handle the same set of keys identically. */
            rec_effects_set(&g_app_settings.effects, k + 8, v);
        } else if (strcmp(k, "ligatures") == 0) {
            g_app_settings.ligatures = (strcmp(v, "true") == 0 ||
                                        strcmp(v, "1") == 0);
        } else if (strncmp(k, "launch.", 7) == 0) {
            /* launch.<i> = local | ssh:<alias> | session:<name>.
               Index is informational for humans editing the file;
               we always append in the order seen so re-saving
               rewrites the slots cleanly. */
            if (g_app_settings.launch_count < LAUNCH_ENTRY_MAX) {
                int i = g_app_settings.launch_count++;
                if (strncmp(v, "ssh:", 4) == 0) {
                    g_app_settings.launch[i].kind = LAUNCH_KIND_SSH;
                    strncpy(g_app_settings.launch[i].host, v + 4,
                            sizeof(g_app_settings.launch[i].host) - 1);
                    g_app_settings.launch[i].host[
                        sizeof(g_app_settings.launch[i].host) - 1] = 0;
                } else if (strncmp(v, "session:", 8) == 0) {
                    g_app_settings.launch[i].kind = LAUNCH_KIND_SESSION;
                    strncpy(g_app_settings.launch[i].host, v + 8,
                            sizeof(g_app_settings.launch[i].host) - 1);
                    g_app_settings.launch[i].host[
                        sizeof(g_app_settings.launch[i].host) - 1] = 0;
                } else {
                    g_app_settings.launch[i].kind = LAUNCH_KIND_LOCAL;
                    g_app_settings.launch[i].host[0] = 0;
                }
            }
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
    if (g_app_settings.screenshot_dir[0]) fprintf(fp, "screenshot_dir=%s\n", g_app_settings.screenshot_dir);
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
    if (g_app_settings.cursor_color[0])
        fprintf(fp, "cursor_color=%s\n", g_app_settings.cursor_color);
    {
        const char *sw = "default";
        switch (g_app_settings.startup_window) {
        case STARTUP_WINDOW_MAXIMIZED:  sw = "maximized";  break;
        case STARTUP_WINDOW_SMALL:      sw = "small";      break;
        case STARTUP_WINDOW_MEDIUM:     sw = "medium";     break;
        case STARTUP_WINDOW_LARGE:      sw = "large";      break;
        case STARTUP_WINDOW_FILL:       sw = "fill";       break;
        case STARTUP_WINDOW_BORDERLESS: sw = "borderless"; break;
        default: break;
        }
        fprintf(fp, "startup_window=%s\n", sw);
    }
    /* Launch entries — one line per slot, in order. We rewrite all
       of them on save so deletes propagate cleanly. */
    for (int i = 0; i < g_app_settings.launch_count; i++) {
        if (g_app_settings.launch[i].kind == LAUNCH_KIND_SSH) {
            fprintf(fp, "launch.%d=ssh:%s\n", i, g_app_settings.launch[i].host);
        } else if (g_app_settings.launch[i].kind == LAUNCH_KIND_SESSION) {
            fprintf(fp, "launch.%d=session:%s\n", i, g_app_settings.launch[i].host);
        } else {
            fprintf(fp, "launch.%d=local\n", i);
        }
    }
    if (g_app_settings.launch_count > 0) {
        fprintf(fp, "launch_active=%d\n", g_app_settings.launch_active);
    }
    /* Visual effects — only emitted when something has been customised
       so a vanilla install's config stays compact. */
    if (!rec_effects_is_default(&g_app_settings.effects)) {
        for (int i = 0; rec_effects_keys[i]; i++) {
            char buf[32];
            if (rec_effects_format(&g_app_settings.effects,
                                   rec_effects_keys[i], buf, sizeof(buf))) {
                fprintf(fp, "effects.%s=%s\n", rec_effects_keys[i], buf);
            }
        }
    }
    if (g_app_settings.ligatures) {
        fprintf(fp, "ligatures=true\n");
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
    g_app_settings.show_hud              = true;
    g_app_settings.hud_pos               = HUD_POS_BOTTOM_RIGHT;
    for (int i = 0; i < HUD_FIELD_COUNT; i++) {
        g_app_settings.hud_show[i]  = true;
        g_app_settings.hud_color[i] = 0;     /* default light grey */
        g_app_settings.hud_size[i]  = 12;    /* sensible default; range 10..18 */
    }
    g_app_settings.hud_show_cpu = true;
    g_app_settings.hud_collapsed = false;
    rec_effects_defaults(&g_app_settings.effects);
    const char *home = getenv("HOME");
#ifdef _WIN32
    if (!home || !*home) home = getenv("USERPROFILE");
#endif
    if (home && *home) {
        snprintf(g_app_settings.log_dir, sizeof(g_app_settings.log_dir),
                 "%s/.rbterm/logs", home);
        snprintf(g_app_settings.rec_dir, sizeof(g_app_settings.rec_dir),
                 "%s/Downloads", home);
        snprintf(g_app_settings.screenshot_dir, sizeof(g_app_settings.screenshot_dir),
                 "%s/Desktop", home);
    } else {
        strncpy(g_app_settings.log_dir, "./rbterm-logs",
                sizeof(g_app_settings.log_dir) - 1);
        strncpy(g_app_settings.rec_dir, "./",
                sizeof(g_app_settings.rec_dir) - 1);
        strncpy(g_app_settings.screenshot_dir, "./",
                sizeof(g_app_settings.screenshot_dir) - 1);
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
    /* Debounce: instead of recomputing matches on every keystroke
       (slow when scrollback has hundreds of thousands of lines),
       defer the recompute until the user has paused typing. The
       timestamp records when the deferred work should fire; 0
       means "no recompute pending". */
    double pending_recompute_at;
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
    FILE *log_fp;                /* raw session log (every PTY byte) */
    char  log_path[PATH_MAX];
    /* Hover-only "maximize / restore" rollover button — top-right
       corner of each pane, drawn while the mouse is over that
       pane. Cleared (w=0) every frame; the click handler reads
       whatever the previous frame published. Stored as four ints
       because Rect's typedef lives further down. */
    int   maximize_btn_x, maximize_btn_y;
    int   maximize_btn_w, maximize_btn_h;
    /* Clean .txt transcript — written via the screen's
       scrollback-push callback. Each row that scrolls out of the
       live grid is emitted as UTF-8 plain text (auto-wrap flag
       controls whether to terminate the line with \n). Alt-screen
       output (vim/less/tmux) never pushes to scrollback so it's
       skipped automatically — that's exactly what we want. */
    FILE *clean_fp;
    char  clean_path[PATH_MAX];

    /* HUD: top-right overlay with hostname, IP, load avg, free mem,
       free disk. For local panes the main loop fills these via cheap
       syscalls; for SSH panes a dedicated probe thread on the libssh
       session writes them under hud_lock and the render path reads
       them lock-free via memcpy from a snapshot. */
    char   hud_hostname[64];
    char   hud_ip[48];
    double hud_load1;            /* 1-min load average; -1 = unknown */
    long   hud_mem_free_mb;      /* -1 = unknown */
    int    hud_disk_free_pct;    /* -1 = unknown */
    double hud_updated_at;       /* GetTime() when stats last refreshed */
    double hud_next_poll_at;     /* GetTime() at which the next local poll should fire */

    /* CPU usage history — circular buffer of the last 60 samples (60
       sec at 1 Hz). hud_cpu_pct[hud_cpu_head] is the most recent. We
       store percentages 0..100 (or -1 for "unknown" before the first
       reading). */
    #define HUD_CPU_HISTORY 60
    int    hud_cpu_pct[HUD_CPU_HISTORY];
    int    hud_cpu_head;
    bool   hud_cpu_inited;
    /* Previous CPU tick totals so we can compute % busy as a delta
       between consecutive polls — single-point readings of CPU
       counters are meaningless. */
    unsigned long long hud_cpu_prev_busy;
    unsigned long long hud_cpu_prev_total;

    /* Active SFTP upload (one at a time per pane). NULL when no
       upload is in flight or the toast has expired. The display
       name + status are pulled from the PtyUpload via
       pty_upload_status / pty_upload_name. */
    PtyUpload *upload;
    /* Wall-clock time the upload finished (success or fail). 0
       while in flight. Used to keep the toast on screen for a few
       seconds after completion before releasing the handle. */
    double     upload_done_at;

    /* Download mirror of the upload slot. Both can run
       concurrently; the toast block renders both stacked. */
    PtyDownload *download;
    double       download_done_at;

    /* Per-pane visual effects (CRT / phosphor / VHS / etc.) applied
       to the live render. New panes seed from g_app_settings.effects;
       SSH panes also overlay any `# rbterm-effects-*` directives from
       the matching ~/.ssh/config stanza. The shader pass is owned by
       rec_effects.c and short-circuits when every effect is at its
       neutral value, so the cost when disabled is one struct check
       per frame. */
    RecEffects effects;

    /* Lazy-allocated render target the shader writes to. .id == 0
       means "not allocated yet" (zero-init from memset is safe). The
       size tracks the last-drawn pane rect so resize triggers a
       reallocation. Freed in pane_free; reused across frames. */
    RenderTexture2D fx_rt;
    /* Previous-frame output for the phosphor-decay shader stage.
       Live-render path ping-pongs between fx_rt and fx_prev each
       frame: this frame writes to whichever is "current", reads
       from the other as `texture1` so trails stay readable. Both
       are freed together. */
    RenderTexture2D fx_prev;
    int             fx_rt_w, fx_rt_h;
} Pane;

typedef enum {
    SPLIT_NONE = 0,
    SPLIT_VERTICAL   = 1,   /* child[0] left, child[1] right   — splitter is a vertical line */
    SPLIT_HORIZONTAL = 2    /* child[0] top,  child[1] bottom  — splitter is a horizontal line */
} SplitMode;

/* Recursive split layout: each node is either a leaf (split == SPLIT_NONE,
   pane != NULL) or an internal split (split != SPLIT_NONE, child[0/1] !=
   NULL, pane == NULL). Pane is heap-allocated under each leaf so its
   address (handed to Screen as io.user) stays stable across collapses
   and tree mutations. */
typedef struct PaneNode PaneNode;
struct PaneNode {
    PaneNode *parent;
    SplitMode split;             /* SPLIT_NONE on leaves */
    float     ratio;             /* internal: 0.15..0.85, fraction given to child[0] */
    PaneNode *child[2];          /* internal */
    bool      splitter_drag;     /* internal: true while user drags this splitter */
    Pane     *pane;              /* leaf */
};

typedef struct {
    PaneNode *root;              /* tree of leaves + splits */
    PaneNode *active;            /* current focused leaf */
    /* When non-NULL, only this leaf renders — and it gets the
       full content rect of the tab. Other leaves stay alive
       (PTYs keep draining, output keeps flowing) so toggling
       back snaps to the up-to-date state. Edge stubs around the
       maximized rect indicate which sides have hidden siblings.
       tab_split / tab_close_leaf clear this. */
    PaneNode *maximized_leaf;
    bool dead;
    /* User-set tab name. When non-empty, tab_label() prefixes the
       auto-derived label (OSC 0/2 title or cwd basename) as
       `<tab_name> · <auto>` so the user's mnemonic stays visible
       even when the shell rewrites the title every prompt. Set
       interactively via Cmd+R, or seeded from an SSH profile's
       display_name. */
    char  tab_name[128];
    bool  is_ssh;
    char  ssh_target[256];       /* user@host[:port] for SSH tabs */
    /* `Host <alias>` from ~/.ssh/config that opened this tab. Empty
       for ad-hoc connections. Used by ssh_form_open to round-trip
       to the *exact* saved profile when two aliases share the same
       (HostName, User, Port). */
    char  ssh_alias[128];
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
    /* Per-host accent colour for the tab in the tab bar. Empty = no
       override (uses the default bg). Stored as a "#rrggbb" string so
       it round-trips cleanly through ssh_config and is easy to
       hand-edit. Applied at draw_tab_bar time only — the terminal
       contents themselves remain whatever theme is in force. */
    char  ssh_color[16];
    /* Per-host cursor colour ("#rrggbb"); empty = inherit
       Settings → Cursor's choice (which itself may be empty,
       meaning "use the natural cursor colour"). */
    char  ssh_cursor_color[16];
    /* Per-host HUD configuration. ssh_hud.override == false means
       fall back to g_app_settings; render code goes through
       hud_effective(t) so it doesn't have to branch. */
    HudConfig ssh_hud;
    /* Per-host visual-effect overrides. ssh_effects_override mirrors
       the parsed `# rbterm-effects-*` directives — when set, every
       pane on this tab gets ssh_effects copied in instead of the
       global default; splits inherit cleanly because pane_apply_*
       reads from the Tab. */
    bool       ssh_effects_override;
    RecEffects ssh_effects;
    /* Per-host startup commands, sent to the shell right after the
       channel opens. Empty = no-op. See ssh_form's matching fields. */
    char  ssh_init_cwd[256];
    char  ssh_init_cmd[256];
    /* Predefined split layout for this SSH host. After the first
       pane is up, tab_open_ssh parses ssh_layout, performs the
       splits in DFS pre-order, and sends per-leaf cwd/cmd to each
       new pane. Empty layout = single pane, init_cwd/init_cmd
       fallback. Mirrors SshProfile/SshForm. */
    char  ssh_layout[256];
    char  ssh_pane_cwds[8][256];
    char  ssh_pane_cmds[8][256];
    /* Background-tab activity: set when any pane of a non-active tab
       receives PTY output. Cleared when the tab becomes active. */
    bool  activity;
} Tab;

#define MAX_TABS 16
/* Per-pane scrollback ceiling (lines). Stored as compact rows
   (per-cell uint32 codepoints + run-length-encoded styles) so a
   typical unstyled ASCII line costs ~80–150 bytes instead of the
   prior ~2 KB. Cap of 1 000 000 lets a `find /` (~700 k entries)
   sit fully in history without truncation. The buffer grows on
   demand from a small seed (64 lines, doubling), so quiet panes
   stay cheap. Once the cap is reached the ring overwrites oldest. */
#define SCROLLBACK_LINES 1000000
#define TAB_BAR_H 30
#define TAB_MIN_W 100
#define TAB_MAX_W 240
#define TAB_CLOSE_W 22
#define TAB_PLUS_W  30
#define TAB_GEAR_W  30
#define TAB_HELP_W  28
#define TAB_SPLIT_W 28          /* one split button (two of them — vertical + horizontal) */
#define TAB_REC_W   26          /* one record button (start + stop, both always visible) */
#define TAB_SNAP_W  28          /* screenshot of the active pane → PNG to disk */
#define TAB_SSH_W   48
#define TAB_UPLOAD_W 28         /* SFTP upload button (only visible on SSH tabs) */
#define TAB_DOWNLOAD_W 28       /* SFTP download button (only visible on SSH tabs) */
#define TAB_BCAST_W 30          /* Broadcast-input toggle (only visible when 2+ panes) */

/* ---------- PaneNode tree helpers ---------- */

typedef struct {
    int x, y, w, h;
} PaneRect;

#define SPLITTER_PX   2
#define SPLITTER_GRAB 6

static void pane_free(Pane *p);

static PaneNode *pane_node_new_leaf(void) {
    PaneNode *n = calloc(1, sizeof(PaneNode));
    n->pane = calloc(1, sizeof(Pane));
    n->ratio = 0.5f;
    return n;
}

static void pane_node_free_recursive(PaneNode *n) {
    if (!n) return;
    if (n->split == SPLIT_NONE) {
        if (n->pane) {
            pane_free(n->pane);
            free(n->pane);
        }
    } else {
        pane_node_free_recursive(n->child[0]);
        pane_node_free_recursive(n->child[1]);
    }
    free(n);
}

static int pane_tree_count(const PaneNode *n) {
    if (!n) return 0;
    if (n->split == SPLIT_NONE) return 1;
    return pane_tree_count(n->child[0]) + pane_tree_count(n->child[1]);
}

static PaneNode *pane_tree_first_leaf(PaneNode *n) {
    if (!n) return NULL;
    while (n->split != SPLIT_NONE) n = n->child[0];
    return n;
}

static PaneNode *pane_tree_last_leaf(PaneNode *n) {
    if (!n) return NULL;
    while (n->split != SPLIT_NONE) n = n->child[1];
    return n;
}

/* Tree-order next/prev leaf — used by Cmd-cycle-pane chords. NULL at
   the ends of the tree. */
static PaneNode *pane_tree_next_leaf(PaneNode *leaf) {
    if (!leaf) return NULL;
    PaneNode *cur = leaf;
    while (cur->parent && cur == cur->parent->child[1]) cur = cur->parent;
    if (!cur->parent) return NULL;
    return pane_tree_first_leaf(cur->parent->child[1]);
}

static PaneNode *pane_tree_prev_leaf(PaneNode *leaf) {
    if (!leaf) return NULL;
    PaneNode *cur = leaf;
    while (cur->parent && cur == cur->parent->child[0]) cur = cur->parent;
    if (!cur->parent) return NULL;
    return pane_tree_last_leaf(cur->parent->child[0]);
}

/* Wraps next/prev around so cycling never lands on NULL. */
static PaneNode *pane_tree_cycle_leaf(PaneNode *root, PaneNode *leaf, int dir) {
    if (!root) return NULL;
    PaneNode *next = (dir > 0) ? pane_tree_next_leaf(leaf)
                               : pane_tree_prev_leaf(leaf);
    if (next) return next;
    return (dir > 0) ? pane_tree_first_leaf(root) : pane_tree_last_leaf(root);
}

/* Compute child rects for an internal node within `outer`. */
static void pane_tree_split_children(const PaneNode *n, PaneRect outer,
                                     PaneRect *a_out, PaneRect *b_out) {
    float ratio = n->ratio;
    if (ratio < 0.15f) ratio = 0.15f;
    if (ratio > 0.85f) ratio = 0.85f;
    if (n->split == SPLIT_VERTICAL) {
        int left_w = (int)((outer.w - SPLITTER_PX) * ratio);
        if (left_w < 0) left_w = 0;
        a_out->x = outer.x; a_out->y = outer.y;
        a_out->w = left_w;  a_out->h = outer.h;
        b_out->x = outer.x + left_w + SPLITTER_PX;
        b_out->y = outer.y;
        b_out->w = outer.w - left_w - SPLITTER_PX;
        b_out->h = outer.h;
        if (b_out->w < 0) b_out->w = 0;
    } else { /* SPLIT_HORIZONTAL */
        int top_h = (int)((outer.h - SPLITTER_PX) * ratio);
        if (top_h < 0) top_h = 0;
        a_out->x = outer.x; a_out->y = outer.y;
        a_out->w = outer.w; a_out->h = top_h;
        b_out->x = outer.x;
        b_out->y = outer.y + top_h + SPLITTER_PX;
        b_out->w = outer.w;
        b_out->h = outer.h - top_h - SPLITTER_PX;
        if (b_out->h < 0) b_out->h = 0;
    }
}

/* Outer rect of `target` (any node) within the tree rooted at `root`.
   Returns false if not found. */
static bool pane_tree_node_rect_walk(const PaneNode *n, const PaneNode *target,
                                     PaneRect outer, PaneRect *out) {
    if (!n) return false;
    if (n == target) { *out = outer; return true; }
    if (n->split == SPLIT_NONE) return false;
    PaneRect ra, rb;
    pane_tree_split_children(n, outer, &ra, &rb);
    if (pane_tree_node_rect_walk(n->child[0], target, ra, out)) return true;
    return pane_tree_node_rect_walk(n->child[1], target, rb, out);
}

static PaneRect pane_tree_terminal_outer(int win_w, int win_h) {
    PaneRect r = { 0, TAB_BAR_H, win_w, win_h - TAB_BAR_H };
    if (r.h < 0) r.h = 0;
    return r;
}

/* Find the leaf containing (mx, my) inside `outer`. */
static PaneNode *pane_tree_at(PaneNode *n, PaneRect outer, int mx, int my) {
    if (!n) return NULL;
    if (mx < outer.x || mx >= outer.x + outer.w ||
        my < outer.y || my >= outer.y + outer.h) return NULL;
    if (n->split == SPLIT_NONE) return n;
    PaneRect ra, rb;
    pane_tree_split_children(n, outer, &ra, &rb);
    PaneNode *r = pane_tree_at(n->child[0], ra, mx, my);
    if (r) return r;
    return pane_tree_at(n->child[1], rb, mx, my);
}

/* Find an internal node whose splitter rect (padded by `grab`)
   contains (mx, my). Optionally returns its outer rect (needed for
   drag math). */
static PaneNode *pane_tree_splitter_at(PaneNode *n, PaneRect outer,
                                       int mx, int my, int grab,
                                       PaneRect *outer_out) {
    if (!n || n->split == SPLIT_NONE) return NULL;
    PaneRect ra, rb;
    pane_tree_split_children(n, outer, &ra, &rb);
    int gx, gy, gw, gh;
    if (n->split == SPLIT_VERTICAL) {
        gx = ra.x + ra.w - grab;
        gy = ra.y;
        gw = SPLITTER_PX + 2 * grab;
        gh = ra.h;
    } else {
        gx = ra.x;
        gy = ra.y + ra.h - grab;
        gw = ra.w;
        gh = SPLITTER_PX + 2 * grab;
    }
    if (mx >= gx && mx < gx + gw && my >= gy && my < gy + gh) {
        if (outer_out) *outer_out = outer;
        return n;
    }
    PaneNode *r = pane_tree_splitter_at(n->child[0], ra, mx, my, grab, outer_out);
    if (r) return r;
    return pane_tree_splitter_at(n->child[1], rb, mx, my, grab, outer_out);
}

/* Replace `leaf` in the tree with a fresh internal split node whose
   first child is the original leaf and second child is a fresh leaf.
   Returns the new (right/bottom) leaf so the caller can spawn a PTY
   into it. */
static PaneNode *pane_node_split_leaf(Tab *t, PaneNode *leaf, SplitMode mode) {
    if (!t || !leaf || leaf->split != SPLIT_NONE) return NULL;
    PaneNode *split = calloc(1, sizeof(PaneNode));
    PaneNode *new_leaf = pane_node_new_leaf();
    split->split = mode;
    split->ratio = 0.5f;
    split->child[0] = leaf;
    split->child[1] = new_leaf;
    split->parent = leaf->parent;
    if (leaf->parent) {
        int slot = (leaf->parent->child[0] == leaf) ? 0 : 1;
        leaf->parent->child[slot] = split;
    } else {
        t->root = split;
    }
    leaf->parent = split;
    new_leaf->parent = split;
    return new_leaf;
}

/* Close a leaf and collapse its parent into the surviving sibling.
   Returns true if the tab itself should now die (the leaf was the
   only pane). */
static bool pane_node_close_leaf(Tab *t, PaneNode *leaf) {
    if (!t || !leaf || leaf->split != SPLIT_NONE) return false;
    PaneNode *parent = leaf->parent;
    if (!parent) {
        return true;   /* caller marks tab dead */
    }
    PaneNode *sibling = (parent->child[0] == leaf) ? parent->child[1]
                                                   : parent->child[0];
    /* Free the leaf (its Pane and the node itself). */
    pane_node_free_recursive(leaf);
    parent->child[0] = parent->child[1] = NULL;
    /* Promote `sibling` into `parent`'s slot. */
    sibling->parent = parent->parent;
    if (parent->parent) {
        int slot = (parent->parent->child[0] == parent) ? 0 : 1;
        parent->parent->child[slot] = sibling;
    } else {
        t->root = sibling;
    }
    free(parent);
    return false;
}

/* Maximum number of leaves in an SSH host's predefined split layout.
   8 is a soft cap — covers everything from a single pane to a
   3-deep recursion (3 splits = 4 leaves) plus headroom. Picked over
   "unlimited" so the descriptor strings stay one-digit-per-leaf. */
#define SSH_LAYOUT_MAX_PANES 8

/* ---------- Layout descriptor: serialize/parse + replay ----------

   The layout descriptor encodes a split tree as a string. Grammar:

       expr  := leaf | split
       leaf  := DIGIT (single-digit pane index, 0..7)
       split := ('V' | 'H') ratio '(' expr ',' expr ')'
       ratio := '0' '.' DIGIT DIGIT          (e.g. "0.50")

   Examples:
       "0"                             single pane
       "V0.50(0,1)"                    side-by-side
       "V0.50(H0.50(0,1),2)"           three panes, 2-over-1 on the left
       "H0.40(V0.30(0,1),V0.60(2,3))"  4-pane 2x2 grid

   Indices number leaves in DFS pre-order so the serializer and
   parser agree without an explicit numbering pass.

   Serializer + parser cap at SSH_LAYOUT_MAX_PANES (8) leaves and
   reject deeper recursion. Malformed input is treated as empty —
   the host opens with a single pane. */

/* Walk `t->root` in DFS pre-order, emitting the layout descriptor
   and capturing each leaf's cwd into cwds_out (one per leaf). The
   cmd array is left untouched — capturing what's "running" is out
   of scope for v1; the caller initialises cmds_out to whatever
   default it wants. Returns the leaf count, or -1 on overflow. */
static int layout_serialize_walk(const PaneNode *n, char *buf, size_t cap,
                                 size_t *off,
                                 char cwds_out[][256],
                                 int *next_idx) {
    if (!n) return -1;
    if (n->split == SPLIT_NONE) {
        if (*next_idx >= SSH_LAYOUT_MAX_PANES) return -1;
        int written = snprintf(buf + *off, cap - *off, "%d", *next_idx);
        if (written < 0 || (size_t)written >= cap - *off) return -1;
        *off += (size_t)written;
        if (n->pane && n->pane->cwd[0]) {
            strncpy(cwds_out[*next_idx], n->pane->cwd, 255);
            cwds_out[*next_idx][255] = 0;
        } else {
            cwds_out[*next_idx][0] = 0;
        }
        (*next_idx)++;
        return 0;
    }
    /* Internal split. */
    char head = (n->split == SPLIT_VERTICAL) ? 'V' : 'H';
    float ratio = n->ratio;
    if (ratio < 0.15f) ratio = 0.15f;
    if (ratio > 0.85f) ratio = 0.85f;
    int written = snprintf(buf + *off, cap - *off, "%c%.2f(", head, ratio);
    if (written < 0 || (size_t)written >= cap - *off) return -1;
    *off += (size_t)written;
    if (layout_serialize_walk(n->child[0], buf, cap, off, cwds_out, next_idx) < 0) return -1;
    if (*off + 1 >= cap) return -1;
    buf[(*off)++] = ',';
    buf[*off] = 0;
    if (layout_serialize_walk(n->child[1], buf, cap, off, cwds_out, next_idx) < 0) return -1;
    if (*off + 1 >= cap) return -1;
    buf[(*off)++] = ')';
    buf[*off] = 0;
    return 0;
}

/* Captures `t`'s layout into `out` and `cwds_out`. Returns the leaf
   count on success, 0 on overflow / no tree. Caller pre-zeroes
   cwds_out + cmds_out (we don't touch cmds). */
static int layout_serialize(const Tab *t, char *out, size_t cap,
                            char cwds_out[][256]) {
    if (!t || !t->root || cap < 2) return 0;
    out[0] = 0;
    size_t off = 0;
    int next = 0;
    if (layout_serialize_walk(t->root, out, cap, &off, cwds_out, &next) < 0) {
        out[0] = 0;
        return 0;
    }
    return next;
}

/* Parsed layout tree node — internal-only is { split, ratio, two
   children }; leaves carry their pane index. Recursive-descent
   parser allocates these on the heap; the replay walker frees the
   tree as it goes. Capping at SSH_LAYOUT_MAX_PANES leaves keeps
   total nodes <= 15. */
typedef struct LayoutNode LayoutNode;
struct LayoutNode {
    bool is_leaf;
    int  leaf_idx;          /* leaves only */
    SplitMode split;        /* internal only */
    float ratio;
    LayoutNode *child[2];
};

static void layout_node_free(LayoutNode *n) {
    if (!n) return;
    if (!n->is_leaf) {
        layout_node_free(n->child[0]);
        layout_node_free(n->child[1]);
    }
    free(n);
}

/* Returns NULL on parse failure. *cur is advanced past consumed
   chars on success. Caller frees the result with layout_node_free. */
static LayoutNode *layout_parse_expr(const char **cur, int depth) {
    if (depth > SSH_LAYOUT_MAX_PANES) return NULL;   /* runaway */
    const char *p = *cur;
    if (!p || !*p) return NULL;
    if (*p >= '0' && *p <= '9') {
        /* Leaf: single decimal digit. (We only support 0..7 so a
           single digit is enough.) */
        LayoutNode *n = calloc(1, sizeof(LayoutNode));
        n->is_leaf = true;
        n->leaf_idx = *p - '0';
        if (n->leaf_idx >= SSH_LAYOUT_MAX_PANES) { free(n); return NULL; }
        *cur = p + 1;
        return n;
    }
    if (*p != 'V' && *p != 'H') return NULL;
    SplitMode sp = (*p == 'V') ? SPLIT_VERTICAL : SPLIT_HORIZONTAL;
    p++;
    /* Ratio: 0.NN */
    if (*p != '0' || p[1] != '.') return NULL;
    char *end = NULL;
    float ratio = strtof(p, &end);
    if (!end || end == p) return NULL;
    if (ratio < 0.15f) ratio = 0.15f;
    if (ratio > 0.85f) ratio = 0.85f;
    p = end;
    if (*p != '(') return NULL;
    p++;
    *cur = p;
    LayoutNode *left = layout_parse_expr(cur, depth + 1);
    if (!left) return NULL;
    p = *cur;
    if (*p != ',') { layout_node_free(left); return NULL; }
    p++;
    *cur = p;
    LayoutNode *right = layout_parse_expr(cur, depth + 1);
    if (!right) { layout_node_free(left); return NULL; }
    p = *cur;
    if (*p != ')') { layout_node_free(left); layout_node_free(right); return NULL; }
    p++;
    *cur = p;
    LayoutNode *n = calloc(1, sizeof(LayoutNode));
    n->is_leaf = false;
    n->split = sp;
    n->ratio = ratio;
    n->child[0] = left;
    n->child[1] = right;
    return n;
}

/* Parse a complete layout descriptor. Returns NULL on malformed
   input (caller should treat as "no layout"). */
static LayoutNode *layout_parse(const char *str) {
    if (!str || !*str) return NULL;
    const char *cur = str;
    LayoutNode *n = layout_parse_expr(&cur, 0);
    if (!n) return NULL;
    /* Must consume the entire string. */
    while (*cur == ' ' || *cur == '\t' || *cur == '\n' || *cur == '\r') cur++;
    if (*cur != 0) { layout_node_free(n); return NULL; }
    return n;
}

/* Verify the parsed tree's leaf indices form 0..N-1 with no
   duplicates (the serializer always assigns DFS pre-order, but we
   want to be tolerant of hand-edited layouts). Returns N >= 1 on
   success, 0 on failure. */
static int layout_count_leaves(const LayoutNode *n, bool seen[SSH_LAYOUT_MAX_PANES]) {
    if (!n) return 0;
    if (n->is_leaf) {
        if (n->leaf_idx < 0 || n->leaf_idx >= SSH_LAYOUT_MAX_PANES) return 0;
        if (seen[n->leaf_idx]) return 0;
        seen[n->leaf_idx] = true;
        return 1;
    }
    int a = layout_count_leaves(n->child[0], seen);
    if (a == 0) return 0;
    int b = layout_count_leaves(n->child[1], seen);
    if (b == 0) return 0;
    return a + b;
}

/* Returns the HudConfig the renderer should consult for `t`. SSH
   tab with override flag set → use the per-host config; otherwise
   fall back to the global app settings. Returned by value so call
   sites can read fields uniformly. */
static HudConfig hud_effective(const Tab *t) {
    if (t && t->is_ssh && t->ssh_hud.override) return t->ssh_hud;
    return hud_config_from_app_settings();
}

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

/* Broadcast input mode: when true, every keystroke (and paste) fans
   out to every leaf of the active tab's pane tree instead of going
   to just the focused pane. Auto-disarms when the user switches
   tabs so a stray Cmd+T doesn't drop a local shell into the
   broadcast group. Toggle via Cmd+Shift+I or the tab-bar button. */
static bool g_broadcast_active = false;

/* Long-press menu on the [+] tab-bar button. Quick click opens a
   local shell (legacy behaviour); pressing and holding for
   PLUS_HOLD_MS surfaces a drag-out menu with three primary items:
   Local shell, SSH host, Session. SSH host and Session expand a
   submenu (saved hosts / saved sessions) on hover; releasing over
   a submenu item fires it directly. Releasing on a primary item
   when its submenu is empty falls back to the corresponding
   discovery page (SSH form / Settings → Sessions). */
#define PLUS_HOLD_MS  350
#define PLUS_MENU_W   180
#define PLUS_MENU_H   28
#define PLUS_SUBMENU_W 220
#define PLUS_PRIMARY_LOCAL   0
#define PLUS_PRIMARY_SSH     1
#define PLUS_PRIMARY_SESSION 2
static bool   g_plus_pressing    = false;
static double g_plus_press_at    = 0.0;
static bool   g_plus_menu_active = false;
static int    g_plus_submenu     = -1;   /* -1 = none, else PLUS_PRIMARY_* of the expanded row */
/* Hover-debounce state for the submenu. When the cursor moves
   diagonally from a primary toward its open submenu, it can cross
   the row of a different primary on the way — without debounce
   the submenu flips mid-move and the user clicks the wrong list.
   We track which primary the cursor is currently over and how
   long it's been there; the submenu only switches after the
   cursor lingers on the new primary for PLUS_HOVER_DEBOUNCE_MS. */
static int    g_plus_hover_primary    = -1;
static double g_plus_hover_started_at = 0.0;
#define PLUS_HOVER_DEBOUNCE_MS 200

/* Tab-rename overlay state. Cmd+R captures the keyboard for the
   active tab's label; the user types into g_tab_rename_buf and
   commits with Enter (Esc cancels). Drawn in draw_tab_bar over
   the active tab's slot. */
static bool g_tab_rename_active = false;
static int  g_tab_rename_idx    = -1;
static char g_tab_rename_buf[128];
static int  g_tab_rename_caret  = 0;

/* Modal SSH connection form. When non-NORMAL, the terminal is locked:
   keystrokes edit form fields and mouse clicks focus them instead of
   going to the active tab. Layout is computed on the fly in
   ssh_form_layout() so draw and hit-test share one source of truth. */
typedef enum {
    UI_NORMAL = 0, UI_SSH_FORM, UI_SETTINGS, UI_HELP, UI_REC_SAVE,
    UI_SESSION_DESIGNER,
    UI_SFTP_UPLOAD, UI_SFTP_DOWNLOAD,
    UI_LOGS
} UiMode;
typedef enum {
    F_NAME = 0, F_HOST, F_PORT, F_USER, F_PASS, F_KEY,
    F_INIT_CWD, F_INIT_CMD, F_DNAME,
    F_NEW, F_CONNECT, F_DELETE, F_CLONE, F_SAVE, F_CANCEL,
    F_COUNT
} SshField;
#define F_TEXT_FIELDS 9    /* name, host, port, user, pass, key, init_cwd, init_cmd, display_name */
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
    char color[16];          /* "#rrggbb" tab accent colour; empty = none */
    char cursor_color[16];   /* "#rrggbb" cursor colour; empty = inherit */
    HudConfig hud;           /* per-host HUD override (override flag gates use) */
    /* Per-host visual-effect override. effects_override flips on the
       first time the user touches anything in the form's effects
       panel; without it the SSH stanza is written without
       `# rbterm-effects-*` lines and the host inherits the global
       default. */
    bool       effects_override;
    RecEffects effects;
    char key[512];
    /* Per-host startup commands. After the SSH session opens we
       send `cd <init_cwd>; <init_cmd>\\r` to the remote shell so
       new tabs land in a known location and optionally launch a
       program (vim, tmux attach, etc.). Empty = no-op. */
    char init_cwd[256];
    char init_cmd[256];
    /* Manual tab name persisted with the profile. Empty = use the
       auto-derived label only. */
    char display_name[128];
    /* Predefined split layout. Mirror of SshProfile.layout — see
       there for the descriptor-string grammar. Filled by the
       "Save Layout" button (capturing the active SSH tab's tree)
       and by ssh_form_apply_profile when an existing host is
       picked. Layout is replayed after connect by tab_open_ssh. */
    char layout[256];
    char pane_cwds[8][256];
    char pane_cmds[8][256];
    int  focus;              /* SshField */
    bool sel_all;            /* focused text field's contents are fully selected */
    char error[256];
    char layout_status[128]; /* feedback line under the Save Layout button */
} SshForm;
static UiMode  g_ui_mode = UI_NORMAL;
static SshForm g_form;

/* SFTP upload form state. Lives behind UI_SFTP_UPLOAD; reset by
   upload_form_open. The local file is set by the system file
   picker (or "Choose…" inside the modal); remote_path is editable
   text and defaults to "~/" the first time the modal opens. */
typedef struct {
    char  local_path[4096];
    char  remote_path[4096];
    bool  remote_focus;
    char  status[256];   /* error from pty_upload_start, if any */
} SftpUploadForm;
static SftpUploadForm g_upload_form;

/* SFTP download form state. The user navigates a remote directory,
   filters by name, and picks a file. We hold the listing in memory
   until the modal closes or a different dir is loaded. */
typedef struct {
    char  remote_dir[4096];     /* current dir being viewed */
    char  filter[128];          /* live-filter substring */
    bool  filter_focus;         /* true while keystrokes edit the filter */
    bool  dir_focus;            /* true while keystrokes edit remote_dir */
    int   selected;             /* index in the filtered view, -1 = none */
    int   scroll;               /* row offset for the listing */
    char  status[256];          /* errors / "loading…" */

    /* Heap-allocated listing of `remote_dir`. Refreshed each time
       the user navigates or hits Refresh. NULL means we haven't
       loaded yet (or it's still loading). */
    PtyDirEntry *entries;
    int          entry_count;
} SftpDownloadForm;
static SftpDownloadForm g_download_form;

typedef struct {
    int x, y, w, h;
} Rect;

/* ---------- Visual-effect picker shared widgets ----------------------------
   Both the rec-save modal, the Settings → Effects tab, and the SSH
   form's per-host effects panel show the same six sliders + Phosphor
   picker. The enum + label helpers are declared here so every layout
   below (SshFormLayout, SettingsLayout, RecSaveLayout) can size its
   slider arrays from EFX_SLIDER_COUNT. */

typedef enum {
    EFX_SLIDER_CRT = 0,
    EFX_SLIDER_BLOOM,
    EFX_SLIDER_GRAIN,
    EFX_SLIDER_VHS,
    EFX_SLIDER_GLITCH,
    EFX_SLIDER_HALFTONE,
    EFX_SLIDER_COUNT
} EfxSlider;

#define EFX_SPEED_COUNT 5     /* 0.5× / 1× / 2× / 4× / 8× — rec-save only */

/* Slider order on screen: left column top-down, then right column. */
static const EfxSlider k_efx_left_col[3]  = { EFX_SLIDER_CRT,   EFX_SLIDER_BLOOM,  EFX_SLIDER_GRAIN };
static const EfxSlider k_efx_right_col[3] = { EFX_SLIDER_VHS,   EFX_SLIDER_GLITCH, EFX_SLIDER_HALFTONE };

static const float k_speed_values[EFX_SPEED_COUNT] = { 0.5f, 1.0f, 2.0f, 4.0f, 8.0f };
static const char *k_speed_labels[EFX_SPEED_COUNT] = { "0.5x", "1x", "2x", "4x", "8x" };

static const char *efx_slider_label(EfxSlider s) {
    switch (s) {
    case EFX_SLIDER_CRT:      return "CRT";
    case EFX_SLIDER_BLOOM:    return "Bloom";
    case EFX_SLIDER_GRAIN:    return "Grain";
    case EFX_SLIDER_VHS:      return "VHS";
    case EFX_SLIDER_GLITCH:   return "Glitch";
    case EFX_SLIDER_HALFTONE: return "Halftone";
    default:                  return "?";
    }
}

static float *efx_slider_value(RecEffects *e, EfxSlider s) {
    switch (s) {
    case EFX_SLIDER_CRT:      return &e->crt;
    case EFX_SLIDER_BLOOM:    return &e->bloom;
    case EFX_SLIDER_GRAIN:    return &e->grain;
    case EFX_SLIDER_VHS:      return &e->vhs;
    case EFX_SLIDER_GLITCH:   return &e->glitch;
    case EFX_SLIDER_HALFTONE: return &e->halftone;
    default:                  return NULL;
    }
}

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
    char color[16];          /* "#rrggbb" tab accent; empty = none */
    char cursor_color[16];   /* "#rrggbb" cursor colour; empty = inherit */
    HudConfig hud;           /* per-host HUD override */
    /* Per-host visual-effect overrides. `effects_override` flips to
       true the first time any `# rbterm-effects-*` directive is seen
       inside the stanza; once flipped, all RecEffects fields apply
       to new SSH panes opened against this host instead of the
       global default. */
    bool       effects_override;
    RecEffects effects;
    /* Run-on-connect commands. Sent verbatim to the remote shell
       right after the channel opens. */
    char init_cwd[256];
    char init_cmd[256];
    /* User-assigned tab name persisted with this profile. Seeds
       Tab.tab_name on connect; overrides any auto-derived label
       in the tab bar (with the auto label appended as a suffix). */
    char display_name[128];
    /* Predefined split layout for this host. `layout` is a
       descriptor string like `V0.50(H0.50(0,1),2)` where letters
       are split orientations, decimals are 0.15..0.85 ratios, and
       integers are leaf indices into pane_cwds[] / pane_cmds[].
       Empty layout means "single pane, fall back to init_cwd /
       init_cmd". When the layout has N leaves, the first N entries
       in pane_cwds / pane_cmds are sent to the matching pane after
       its PTY opens. See SSH_LAYOUT_*. */
    char layout[256];
    char pane_cwds[8][256];
    char pane_cmds[8][256];
} SshProfile;


/* Curated palette for SSH tab accents. 8 saturated colours that all
   read well as a tab background plus a "none" sentinel handled
   separately. Hex stored on disk so users can hand-edit
   ssh_config and choose any colour they like. */
static const char * const SSH_COLOR_PRESETS[] = {
    "#c84a4a",   /* red — prod / danger */
    "#d68a3a",   /* amber — staging */
    "#5aa84a",   /* green — dev / safe */
    "#3a8aa8",   /* teal */
    "#3a7bff",   /* blue */
    "#7a4ad6",   /* purple */
    "#c84aa0",   /* pink */
    "#5a6275",   /* slate — neutral */
};
#define SSH_COLOR_PRESET_COUNT (int)(sizeof(SSH_COLOR_PRESETS) / sizeof(SSH_COLOR_PRESETS[0]))

/* Parse "#rrggbb" or "rrggbb" into an opaque Color. Returns false on
   malformed input — caller falls back to the default bg. */
static bool parse_hex_color(const char *hex, Color *out) {
    if (!hex || !*hex) return false;
    if (*hex == '#') hex++;
    if (strlen(hex) < 6) return false;
    unsigned int rr = 0, gg = 0, bb = 0;
    if (sscanf(hex, "%2x%2x%2x", &rr, &gg, &bb) != 3) return false;
    out->r = (unsigned char)rr;
    out->g = (unsigned char)gg;
    out->b = (unsigned char)bb;
    out->a = 255;
    return true;
}

#define SSH_PROFILES_MAX 128
static SshProfile g_ssh_profiles[SSH_PROFILES_MAX];

#ifdef RBTERM_SSH
/* Parallel SSH connect for startup launches. Each pending SSH entry
   gets a worker thread that runs pty_open_ssh concurrently with the
   others; the main loop polls completion flags and integrates each
   finished Pty into a fresh Tab. Total wait drops from the sum of
   per-host handshake latencies to the max. Windows builds compile
   pty_ssh_stub.c (no real SSH) and don't have <pthread.h>, so the
   whole worker apparatus is gated on RBTERM_SSH. */
typedef struct {
    pthread_t    th;
    int          started;
    volatile int done;
    int          integrated;
    char         alias[128];
    bool         is_active;
    /* Connect inputs (copies — g_ssh_profiles can be reloaded
       under us when the SSH form opens during startup). */
    char         user[96];
    char         host[256];
    char         keyfile[PATH_MAX];
    /* Password is form-only (saved profiles never carry one). The
       worker scrubs this slot the moment pty_open_ssh returns. */
    char         password[256];
    int          port;
    int          cols, rows;
    /* Full profile snapshot for post-connect Tab setup
       (theme/cursor/font/HUD/effects/init/layout). */
    SshProfile   prof;
    bool         prof_valid;
    /* Placeholder Tab created at kick time. The integration step
       attaches the worker's Pty to this Tab once handshake done. */
    Tab         *placeholder;
    /* Result. */
    Pty         *pty;
    char         err[256];
} SshLaunchWorker;
#define LAUNCH_WORKERS_MAX 16
static SshLaunchWorker g_launch_workers[LAUNCH_WORKERS_MAX];
static int  g_launch_workers_count  = 0;
static bool g_launch_workers_kicked = false;
/* Forward decl — ssh_launch_kick lives near the worker thread
   function (file-bottom) but is called from the SSH form's Connect
   path and the [+] menu's SSH branch (both file-middle). */
static bool ssh_launch_kick(const SshProfile *prof, const char *alias,
                            const char *password, bool is_active,
                            int cols, int rows);
/* Background SSH ops (forward decls). Each runs the previously-
   synchronous helper on a short-lived pthread so the main thread
   can keep redrawing while libssh handshakes / pki_generate runs.
   Result strings land in the right visible status field via
   bg_ssh_integrate(), called once per main-loop iteration. */
static bool bg_test_auth_kick(int cols, int rows);
static bool bg_key_install_kick(const SshProfile *prof,
                                const char *pubkey,
                                const char *key_label,
                                const char *host_label);
static bool bg_key_generate_kick(const char *type_name,
                                 const char *file_stem,
                                 const char *passphrase);
static bool bg_ssh_integrate(void);
/* Busy predicates so the UI can show "Testing..." / "Installing..."
   / "Generating..." while a thread is in flight, and so re-clicks
   on the trigger short-circuit instead of stacking. */
static bool bg_test_auth_busy(void);
static bool bg_key_install_busy(void);
static bool bg_key_generate_busy(void);
#endif /* RBTERM_SSH */
static int        g_ssh_profile_count = 0;
static int        g_ssh_list_scroll = 0;   /* in rows */
static int        g_ssh_list_selected = -1; /* highlighted row, -1 = none */

/* ---------- Sessions: multi-host pre-defined splits ---------- */

/* A Session is a saved tab template: a recursive split tree where
   each leaf carries its own kind (local shell vs SSH host), so
   one tab can host multiple SSH connections side-by-side. The user
   designs sessions interactively in the Session Designer modal,
   they're persisted to ~/.config/rbterm/sessions.ini, and listed
   in Settings → Sessions where each row has Open / Edit / Delete.

   Reuses the same descriptor-string grammar as the per-host SSH
   layout (V0.50(H0.50(0,1),2)) for the tree shape; per-leaf
   metadata lives in parallel arrays indexed by DFS-pre-order leaf
   number. */
#define SESSION_NAME_MAX     128
#define SESSION_LAYOUT_MAX   256
#define SESSION_MAX_LEAVES   8
#define SESSION_HOST_MAX     128
#define SESSION_CWD_MAX      256
#define SESSION_CMD_MAX      256

typedef enum {
    SESSION_LEAF_LOCAL = 0,
    SESSION_LEAF_SSH   = 1,
} SessionLeafKind;

typedef struct SessionNode SessionNode;
struct SessionNode {
    SessionNode *parent;
    SplitMode    split;          /* SPLIT_NONE for leaves */
    float        ratio;          /* internal nodes only */
    SessionNode *child[2];
    /* Leaf-only metadata. Internal nodes leave these zero. */
    SessionLeafKind kind;
    char         host[SESSION_HOST_MAX];
    char         cwd[SESSION_CWD_MAX];
    char         cmd[SESSION_CMD_MAX];
};

typedef struct {
    char         name[SESSION_NAME_MAX];
    SessionNode *root;
    /* DFS pre-order leaf index that gets focus when this session
       opens as a Tab. Captured from the user's selected leaf in
       the designer canvas at save time; defaults to 0 (first leaf)
       for legacy sessions saved before this field existed. */
    int          default_idx;
} Session;

/* Walk the SessionNode tree in DFS pre-order and return the leaf
   index of `target`, or -1 if target isn't a leaf in the tree. The
   ordering matches session_collect_leaves / session_serialize_walk
   so the index round-trips through save/load and through the live
   PaneNode tree built by session_replay_recurse. */
static int session_node_dfs_leaf_index(const SessionNode *root,
                                       const SessionNode *target);

#define SESSIONS_MAX 32
static Session g_sessions[SESSIONS_MAX];
static int     g_sessions_count = 0;

/* SessionNode helpers — mirror the runtime PaneNode API but
   manipulate the saved descriptor instead of the live tree. */

static SessionNode *session_node_new_leaf(void) {
    SessionNode *n = calloc(1, sizeof(SessionNode));
    n->ratio = 0.5f;
    n->kind = SESSION_LEAF_LOCAL;
    return n;
}

static void session_node_free(SessionNode *n) {
    if (!n) return;
    if (n->split != SPLIT_NONE) {
        session_node_free(n->child[0]);
        session_node_free(n->child[1]);
    }
    free(n);
}

static int session_count_leaves(const SessionNode *n) {
    if (!n) return 0;
    if (n->split == SPLIT_NONE) return 1;
    return session_count_leaves(n->child[0]) +
           session_count_leaves(n->child[1]);
}

static SessionNode *session_first_leaf(SessionNode *n) {
    if (!n) return NULL;
    while (n->split != SPLIT_NONE) n = n->child[0];
    return n;
}

static SessionNode *session_next_leaf(SessionNode *leaf) {
    if (!leaf) return NULL;
    SessionNode *cur = leaf;
    while (cur->parent && cur == cur->parent->child[1]) cur = cur->parent;
    if (!cur->parent) return NULL;
    return session_first_leaf(cur->parent->child[1]);
}

static int session_node_dfs_leaf_index(const SessionNode *root,
                                       const SessionNode *target) {
    if (!root || !target || target->split != SPLIT_NONE) return -1;
    int idx = 0;
    SessionNode *cur = session_first_leaf((SessionNode *)root);
    while (cur) {
        if (cur == target) return idx;
        cur = session_next_leaf(cur);
        idx++;
    }
    return -1;
}

/* Replace `leaf` with an internal split node whose children are the
   original leaf and a fresh second leaf. Returns the new leaf so
   the caller can inspect / fill it. */
static SessionNode *session_split_leaf(Session *s, SessionNode *leaf,
                                       SplitMode mode) {
    if (!s || !leaf || leaf->split != SPLIT_NONE) return NULL;
    if (mode != SPLIT_VERTICAL && mode != SPLIT_HORIZONTAL) return NULL;
    if (session_count_leaves(s->root) >= SESSION_MAX_LEAVES) return NULL;
    SessionNode *split = calloc(1, sizeof(SessionNode));
    SessionNode *new_leaf = session_node_new_leaf();
    split->split = mode;
    split->ratio = 0.5f;
    split->child[0] = leaf;
    split->child[1] = new_leaf;
    split->parent = leaf->parent;
    if (leaf->parent) {
        int slot = (leaf->parent->child[0] == leaf) ? 0 : 1;
        leaf->parent->child[slot] = split;
    } else {
        s->root = split;
    }
    leaf->parent = split;
    new_leaf->parent = split;
    return new_leaf;
}

/* Close a leaf and collapse its parent into the surviving sibling.
   Returns the surviving leaf for selection update; NULL when the
   leaf was the only one (caller should refuse — designer shouldn't
   allow zero-leaf sessions). */
static SessionNode *session_close_leaf(Session *s, SessionNode *leaf) {
    if (!s || !leaf || leaf->split != SPLIT_NONE) return NULL;
    SessionNode *parent = leaf->parent;
    if (!parent) return NULL;
    SessionNode *sibling = (parent->child[0] == leaf) ? parent->child[1]
                                                      : parent->child[0];
    session_node_free(leaf);
    parent->child[0] = parent->child[1] = NULL;
    sibling->parent = parent->parent;
    if (parent->parent) {
        int slot = (parent->parent->child[0] == parent) ? 0 : 1;
        parent->parent->child[slot] = sibling;
    } else {
        s->root = sibling;
    }
    free(parent);
    return session_first_leaf(sibling);
}

/* Serialize the SessionNode tree into the same descriptor-string
   format as ~/.ssh/config layouts: V0.50(H0.50(0,1),2) etc. */
static int session_serialize_walk(const SessionNode *n, char *buf, size_t cap,
                                  size_t *off, int *next_idx) {
    if (!n) return -1;
    if (n->split == SPLIT_NONE) {
        if (*next_idx >= SESSION_MAX_LEAVES) return -1;
        int written = snprintf(buf + *off, cap - *off, "%d", *next_idx);
        if (written < 0 || (size_t)written >= cap - *off) return -1;
        *off += (size_t)written;
        (*next_idx)++;
        return 0;
    }
    char head = (n->split == SPLIT_VERTICAL) ? 'V' : 'H';
    float ratio = n->ratio;
    if (ratio < 0.15f) ratio = 0.15f;
    if (ratio > 0.85f) ratio = 0.85f;
    int written = snprintf(buf + *off, cap - *off, "%c%.2f(", head, ratio);
    if (written < 0 || (size_t)written >= cap - *off) return -1;
    *off += (size_t)written;
    if (session_serialize_walk(n->child[0], buf, cap, off, next_idx) < 0) return -1;
    if (*off + 1 >= cap) return -1;
    buf[(*off)++] = ',';
    buf[*off] = 0;
    if (session_serialize_walk(n->child[1], buf, cap, off, next_idx) < 0) return -1;
    if (*off + 1 >= cap) return -1;
    buf[(*off)++] = ')';
    buf[*off] = 0;
    return 0;
}

/* Walk the tree in DFS pre-order, filling per-leaf metadata arrays
   matching the indices the descriptor uses. Caller pre-zeroes the
   arrays. */
static void session_collect_leaves(const SessionNode *n,
                                   SessionLeafKind *kinds,
                                   char hosts[][SESSION_HOST_MAX],
                                   char cwds[][SESSION_CWD_MAX],
                                   char cmds[][SESSION_CMD_MAX],
                                   int  *next_idx) {
    if (!n) return;
    if (n->split == SPLIT_NONE) {
        if (*next_idx >= SESSION_MAX_LEAVES) return;
        int i = (*next_idx)++;
        kinds[i] = n->kind;
        snprintf(hosts[i], SESSION_HOST_MAX, "%s", n->host);
        snprintf(cwds[i],  SESSION_CWD_MAX,  "%s", n->cwd);
        snprintf(cmds[i],  SESSION_CMD_MAX,  "%s", n->cmd);
        return;
    }
    session_collect_leaves(n->child[0], kinds, hosts, cwds, cmds, next_idx);
    session_collect_leaves(n->child[1], kinds, hosts, cwds, cmds, next_idx);
}

static void session_apply_leaves(SessionNode *n,
                                 const SessionLeafKind *kinds,
                                 const char hosts[][SESSION_HOST_MAX],
                                 const char cwds[][SESSION_CWD_MAX],
                                 const char cmds[][SESSION_CMD_MAX],
                                 int  *next_idx, int max_idx) {
    if (!n) return;
    if (n->split == SPLIT_NONE) {
        int i = (*next_idx)++;
        if (i >= max_idx) return;
        n->kind = kinds[i];
        snprintf(n->host, SESSION_HOST_MAX, "%s", hosts[i]);
        snprintf(n->cwd,  SESSION_CWD_MAX,  "%s", cwds[i]);
        snprintf(n->cmd,  SESSION_CMD_MAX,  "%s", cmds[i]);
        return;
    }
    session_apply_leaves(n->child[0], kinds, hosts, cwds, cmds, next_idx, max_idx);
    session_apply_leaves(n->child[1], kinds, hosts, cwds, cmds, next_idx, max_idx);
}

/* Same recursive-descent grammar as layout_parse but materialises
   SessionNode internal/leaf nodes. Used to re-hydrate sessions
   loaded from sessions.ini. */
static SessionNode *session_parse_expr(const char **cur, int depth) {
    if (depth > SESSION_MAX_LEAVES) return NULL;
    const char *p = *cur;
    if (!p || !*p) return NULL;
    if (*p >= '0' && *p <= '9') {
        SessionNode *n = session_node_new_leaf();
        int idx = *p - '0';
        if (idx >= SESSION_MAX_LEAVES) { session_node_free(n); return NULL; }
        /* Stash the leaf index temporarily in cwd[0..1] for the
           caller to use during apply_leaves; cleared afterwards. */
        n->cwd[0] = (char)('0' + idx);
        n->cwd[1] = 0;
        *cur = p + 1;
        return n;
    }
    if (*p != 'V' && *p != 'H') return NULL;
    SplitMode sp = (*p == 'V') ? SPLIT_VERTICAL : SPLIT_HORIZONTAL;
    p++;
    if (*p != '0' || p[1] != '.') return NULL;
    char *end = NULL;
    float ratio = strtof(p, &end);
    if (!end || end == p) return NULL;
    if (ratio < 0.15f) ratio = 0.15f;
    if (ratio > 0.85f) ratio = 0.85f;
    p = end;
    if (*p != '(') return NULL;
    p++;
    *cur = p;
    SessionNode *left = session_parse_expr(cur, depth + 1);
    if (!left) return NULL;
    p = *cur;
    if (*p != ',') { session_node_free(left); return NULL; }
    p++;
    *cur = p;
    SessionNode *right = session_parse_expr(cur, depth + 1);
    if (!right) { session_node_free(left); return NULL; }
    p = *cur;
    if (*p != ')') { session_node_free(left); session_node_free(right); return NULL; }
    p++;
    *cur = p;
    SessionNode *n = calloc(1, sizeof(SessionNode));
    n->split = sp;
    n->ratio = ratio;
    n->child[0] = left;  left->parent  = n;
    n->child[1] = right; right->parent = n;
    return n;
}

static SessionNode *session_parse_layout(const char *str) {
    if (!str || !*str) return NULL;
    const char *cur = str;
    SessionNode *n = session_parse_expr(&cur, 0);
    if (!n) return NULL;
    while (*cur == ' ' || *cur == '\t' || *cur == '\n' || *cur == '\r') cur++;
    if (*cur != 0) { session_node_free(n); return NULL; }
    return n;
}

/* Forward decls for the sessions block — helpers all live further
   down. */
static void trim_end(char *s);
static bool rect_hit(Rect r, int x, int y);
static void ssh_profiles_load(void);
static void pane_init_click_state(Pane *p);
static ScreenIO pane_io(Pane *p);
static void ssh_send_init_line(Pane *p, const char *cwd, const char *cmd);
static void tab_log_open_all(Tab *t);

/* ---------- Sessions: persistence ---------- */

/* Format on disk (~/.config/rbterm/sessions.ini):
 *
 *   [session.prod-monitoring]
 *   layout = V0.50(H0.50(0,1),2)
 *   pane.0 = ssh:mia
 *   pane.0.cwd = /var/log
 *   pane.0.cmd = tail -f *.log
 *   pane.1 = local
 *   pane.1.cwd = ~/work
 *   pane.2 = ssh:win
 *
 * Round-trippable; the writer always emits a fresh file from
 * g_sessions[] so hand-edits to fields the designer doesn't manage
 * (currently none beyond what's listed above) would be lost. */
static void sessions_load(void) {
    /* Free any existing sessions first. */
    for (int i = 0; i < g_sessions_count; i++) {
        session_node_free(g_sessions[i].root);
        g_sessions[i].root = NULL;
        g_sessions[i].name[0] = 0;
    }
    g_sessions_count = 0;

    char path[PATH_MAX];
    expand_home_path("~/.config/rbterm/sessions.ini", path, sizeof(path));
    FILE *fp = fopen(path, "r");
    if (!fp) return;

    /* Per-session staging — we collect the layout + per-leaf metadata
       under the current section, then materialise a SessionNode tree
       on the next [section] header (or EOF). */
    char        cur_name[SESSION_NAME_MAX] = {0};
    char        cur_layout[SESSION_LAYOUT_MAX] = {0};
    int         cur_default = 0;
    SessionLeafKind cur_kinds[SESSION_MAX_LEAVES] = {0};
    char        cur_hosts[SESSION_MAX_LEAVES][SESSION_HOST_MAX] = {{0}};
    char        cur_cwds[SESSION_MAX_LEAVES][SESSION_CWD_MAX]   = {{0}};
    char        cur_cmds[SESSION_MAX_LEAVES][SESSION_CMD_MAX]   = {{0}};

    #define FLUSH_CURRENT() do {                                              \
        if (cur_name[0] && cur_layout[0] &&                                   \
            g_sessions_count < SESSIONS_MAX) {                                \
            SessionNode *root = session_parse_layout(cur_layout);             \
            if (root) {                                                       \
                int idx = 0;                                                  \
                int max = session_count_leaves(root);                         \
                if (max > SESSION_MAX_LEAVES) max = SESSION_MAX_LEAVES;       \
                session_apply_leaves(root, cur_kinds, cur_hosts,              \
                                     cur_cwds, cur_cmds, &idx, max);          \
                Session *s = &g_sessions[g_sessions_count++];                 \
                snprintf(s->name, sizeof(s->name), "%s", cur_name);           \
                s->root = root;                                               \
                s->default_idx = (cur_default >= 0 && cur_default < max)      \
                                 ? cur_default : 0;                           \
            }                                                                 \
        }                                                                     \
        cur_name[0] = 0;                                                      \
        cur_layout[0] = 0;                                                    \
        cur_default = 0;                                                      \
        for (int _i = 0; _i < SESSION_MAX_LEAVES; _i++) {                     \
            cur_kinds[_i] = SESSION_LEAF_LOCAL;                               \
            cur_hosts[_i][0] = 0;                                             \
            cur_cwds[_i][0]  = 0;                                             \
            cur_cmds[_i][0]  = 0;                                             \
        }                                                                     \
    } while (0)

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == ';' || *p == '\n' || *p == 0) continue;
        trim_end(p);
        if (*p == '[') {
            FLUSH_CURRENT();
            const char *prefix = "[session.";
            size_t plen = strlen(prefix);
            if (strncmp(p, prefix, plen) != 0) continue;
            const char *q = p + plen;
            const char *close = strchr(q, ']');
            if (!close) continue;
            size_t nlen = (size_t)(close - q);
            if (nlen >= sizeof(cur_name)) nlen = sizeof(cur_name) - 1;
            memcpy(cur_name, q, nlen);
            cur_name[nlen] = 0;
            continue;
        }
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = p;
        char *val = eq + 1;
        /* Trim trailing whitespace on key, leading on val. */
        size_t klen = strlen(key);
        while (klen > 0 && (key[klen-1] == ' ' || key[klen-1] == '\t')) {
            key[--klen] = 0;
        }
        while (*val == ' ' || *val == '\t') val++;

        if (!cur_name[0]) continue;
        if (strcmp(key, "layout") == 0) {
            snprintf(cur_layout, sizeof(cur_layout), "%s", val);
        } else if (strcmp(key, "default") == 0) {
            cur_default = atoi(val);
        } else if (strncmp(key, "pane.", 5) == 0) {
            const char *r = key + 5;
            if (*r < '0' || *r > '0' + (SESSION_MAX_LEAVES - 1)) continue;
            int idx = *r - '0';
            r++;
            if (*r == 0) {
                /* `pane.N = local | ssh:<alias>` */
                if (strcmp(val, "local") == 0) {
                    cur_kinds[idx] = SESSION_LEAF_LOCAL;
                    cur_hosts[idx][0] = 0;
                } else if (strncmp(val, "ssh:", 4) == 0) {
                    cur_kinds[idx] = SESSION_LEAF_SSH;
                    snprintf(cur_hosts[idx], SESSION_HOST_MAX, "%s", val + 4);
                }
            } else if (strcmp(r, ".cwd") == 0) {
                snprintf(cur_cwds[idx], SESSION_CWD_MAX, "%s", val);
            } else if (strcmp(r, ".cmd") == 0) {
                snprintf(cur_cmds[idx], SESSION_CMD_MAX, "%s", val);
            }
        }
    }
    FLUSH_CURRENT();
    #undef FLUSH_CURRENT
    fclose(fp);
}

static void sessions_save(void) {
    char path[PATH_MAX];
    expand_home_path("~/.config/rbterm/sessions.ini", path, sizeof(path));
    /* Ensure parent dir exists. */
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = 0; mkdir_p(dir); }
    FILE *fp = fopen(path, "w");
    if (!fp) {
        fprintf(stderr, "rbterm: sessions_save: %s: %s\n",
                path, strerror(errno));
        return;
    }
    fprintf(fp,
            "# Auto-generated by rbterm (Session Designer).\n"
            "# Each [session.NAME] block describes a multi-pane tab\n"
            "# template. layout uses the same descriptor grammar as\n"
            "# ~/.ssh/config's # rbterm-layout: comments — V/H + ratio\n"
            "# + nested expressions; integers are leaf indices into\n"
            "# the pane.N entries below.\n\n");
    for (int i = 0; i < g_sessions_count; i++) {
        Session *s = &g_sessions[i];
        if (!s->name[0] || !s->root) continue;
        char layout[SESSION_LAYOUT_MAX];
        layout[0] = 0;
        size_t off = 0;
        int next = 0;
        if (session_serialize_walk(s->root, layout, sizeof(layout), &off, &next) < 0)
            continue;
        SessionLeafKind kinds[SESSION_MAX_LEAVES] = {0};
        char hosts[SESSION_MAX_LEAVES][SESSION_HOST_MAX] = {{0}};
        char cwds[SESSION_MAX_LEAVES][SESSION_CWD_MAX]   = {{0}};
        char cmds[SESSION_MAX_LEAVES][SESSION_CMD_MAX]   = {{0}};
        int idx = 0;
        session_collect_leaves(s->root, kinds, hosts, cwds, cmds, &idx);
        fprintf(fp, "[session.%s]\n", s->name);
        fprintf(fp, "layout = %s\n", layout);
        if (s->default_idx > 0) fprintf(fp, "default = %d\n", s->default_idx);
        for (int k = 0; k < idx; k++) {
            if (kinds[k] == SESSION_LEAF_SSH && hosts[k][0]) {
                fprintf(fp, "pane.%d = ssh:%s\n", k, hosts[k]);
            } else {
                fprintf(fp, "pane.%d = local\n", k);
            }
            if (cwds[k][0]) fprintf(fp, "pane.%d.cwd = %s\n", k, cwds[k]);
            if (cmds[k][0]) fprintf(fp, "pane.%d.cmd = %s\n", k, cmds[k]);
        }
        fprintf(fp, "\n");
    }
    fclose(fp);
}

/* ---------- Sessions: in-memory list helpers ---------- */

static int sessions_find_by_name(const char *name) {
    if (!name || !*name) return -1;
    for (int i = 0; i < g_sessions_count; i++) {
        if (strcmp(g_sessions[i].name, name) == 0) return i;
    }
    return -1;
}

/* Replace a session's tree (used by the designer when committing).
   The passed-in tree is adopted; the previous tree is freed. Caller
   must not free `new_root` after this returns. */
static void session_replace_tree(int idx, const char *name,
                                 SessionNode *new_root, int default_idx) {
    if (idx < 0 || idx >= SESSIONS_MAX) return;
    if (idx == g_sessions_count) g_sessions_count++;
    Session *s = &g_sessions[idx];
    if (s->root) session_node_free(s->root);
    snprintf(s->name, sizeof(s->name), "%s", name);
    s->root = new_root;
    s->default_idx = default_idx;
}

/* Remove a session by index, shifting later entries down. */
static void sessions_remove(int idx) {
    if (idx < 0 || idx >= g_sessions_count) return;
    if (g_sessions[idx].root) session_node_free(g_sessions[idx].root);
    for (int i = idx; i < g_sessions_count - 1; i++) {
        g_sessions[i] = g_sessions[i + 1];
    }
    g_sessions_count--;
    g_sessions[g_sessions_count].root = NULL;
    g_sessions[g_sessions_count].name[0] = 0;
}

/* ---------- Sessions: layout helpers (canvas hit-test + rect walk) ---------- */

/* Same shape as pane_tree_split_children but for SessionNode trees,
   with a 2-px gap between siblings so the canvas reads as discrete
   rectangles instead of a continuous grid. */
static void session_node_split_children(const SessionNode *n, PaneRect outer,
                                        PaneRect *a_out, PaneRect *b_out) {
    const int gap = 2;
    float ratio = n->ratio;
    if (ratio < 0.15f) ratio = 0.15f;
    if (ratio > 0.85f) ratio = 0.85f;
    if (n->split == SPLIT_VERTICAL) {
        int left_w = (int)((outer.w - gap) * ratio);
        if (left_w < 0) left_w = 0;
        a_out->x = outer.x; a_out->y = outer.y;
        a_out->w = left_w;  a_out->h = outer.h;
        b_out->x = outer.x + left_w + gap;
        b_out->y = outer.y;
        b_out->w = outer.w - left_w - gap;
        b_out->h = outer.h;
    } else {
        int top_h = (int)((outer.h - gap) * ratio);
        if (top_h < 0) top_h = 0;
        a_out->x = outer.x; a_out->y = outer.y;
        a_out->w = outer.w; a_out->h = top_h;
        b_out->x = outer.x;
        b_out->y = outer.y + top_h + gap;
        b_out->w = outer.w;
        b_out->h = outer.h - top_h - gap;
    }
}

/* Find the leaf containing (mx, my) inside `outer`, or NULL. */
static SessionNode *session_node_at(SessionNode *n, PaneRect outer,
                                    int mx, int my) {
    if (!n) return NULL;
    if (mx < outer.x || mx >= outer.x + outer.w ||
        my < outer.y || my >= outer.y + outer.h) return NULL;
    if (n->split == SPLIT_NONE) return n;
    PaneRect ra, rb;
    session_node_split_children(n, outer, &ra, &rb);
    SessionNode *r = session_node_at(n->child[0], ra, mx, my);
    if (r) return r;
    return session_node_at(n->child[1], rb, mx, my);
}

/* Return the internal node whose splitter strip (padded by `grab`)
   contains (mx, my). Optionally returns its outer rect for drag math.
   Mirrors pane_tree_splitter_at but for SessionNode trees + the
   designer's 2-px gap. */
static SessionNode *session_node_splitter_at(SessionNode *n, PaneRect outer,
                                             int mx, int my, int grab,
                                             PaneRect *outer_out) {
    if (!n || n->split == SPLIT_NONE) return NULL;
    PaneRect ra, rb;
    session_node_split_children(n, outer, &ra, &rb);
    const int gap = 2;
    int gx, gy, gw, gh;
    if (n->split == SPLIT_VERTICAL) {
        gx = ra.x + ra.w - grab;
        gy = ra.y;
        gw = gap + 2 * grab;
        gh = ra.h;
    } else {
        gx = ra.x;
        gy = ra.y + ra.h - grab;
        gw = ra.w;
        gh = gap + 2 * grab;
    }
    if (mx >= gx && mx < gx + gw && my >= gy && my < gy + gh) {
        if (outer_out) *outer_out = outer;
        return n;
    }
    SessionNode *r = session_node_splitter_at(n->child[0], ra, mx, my, grab, outer_out);
    if (r) return r;
    return session_node_splitter_at(n->child[1], rb, mx, my, grab, outer_out);
}

/* ---------- Sessions: designer modal ---------- */

typedef enum {
    SDF_NONE = 0,
    SDF_NAME,
    SDF_CWD,
    SDF_CMD,
} SessionDesignerField;

static Session              g_sd_session;        /* working copy */
static int                  g_sd_idx = -1;       /* -1 = new */
static SessionNode         *g_sd_selected = NULL;
static SessionDesignerField g_sd_focus = SDF_NONE;
static bool                 g_sd_host_dropdown = false;
static int                  g_sd_host_scroll = 0;
static char                 g_sd_status[256];
static bool                 g_sd_open_after_save = false;   /* Save & Open path */
/* Splitter drag state (designer-only). One drag at a time, so a
   single pair of globals beats per-node bookkeeping. The outer
   rect is captured at drag-start so the math stays consistent
   even if the canvas size changes mid-drag. */
static SessionNode         *g_sd_drag_node = NULL;
static PaneRect             g_sd_drag_outer;

typedef struct {
    Rect modal;
    Rect name_field;
    Rect canvas;
    Rect btn_split_v;
    Rect btn_split_h;
    Rect btn_close_pane;
    /* Inspector panel — rects for kind toggle, host dropdown,
       cwd / cmd text fields. */
    Rect ins_kind_local;
    Rect ins_kind_ssh;
    Rect ins_host;          /* dropdown trigger / display */
    Rect ins_host_list;     /* expanded list when open */
    Rect ins_cwd;
    Rect ins_cmd;
    /* Bottom action row. */
    Rect btn_save;
    Rect btn_save_open;
    Rect btn_cancel;
} SessionDesignerLayout;

static SessionDesignerLayout session_designer_layout(int win_w, int win_h) {
    SessionDesignerLayout L = {0};
    int w = 760, h = 620;
    if (w > win_w - 40) w = win_w - 40;
    if (h > win_h - 40) h = win_h - 40;
    L.modal.x = (win_w - w) / 2;
    L.modal.y = (win_h - h) / 2;
    L.modal.w = w;
    L.modal.h = h;

    int pad = 22;
    int title_h = 38;
    int inner_x = L.modal.x + pad;
    int inner_w = w - 2 * pad;

    /* Name field at the top of the content area. */
    int name_y = L.modal.y + title_h + 14;
    int label_w = 70;
    L.name_field = (Rect){ inner_x + label_w, name_y,
                           inner_w - label_w, 28 };

    /* Canvas + side panel split: 60/40. */
    int row_y = name_y + 28 + 18;
    int canvas_w = (inner_w * 60) / 100 - 8;
    int side_w   = inner_w - canvas_w - 16;
    int row_h_total = h - (row_y - L.modal.y) - 60;   /* leave room for action row */
    L.canvas = (Rect){ inner_x, row_y, canvas_w, row_h_total };

    /* Side panel: toolbar at top, then inspector. */
    int side_x = inner_x + canvas_w + 16;
    int btn_h = 30;
    int btn_w = (side_w - 8) / 2;
    L.btn_split_v   = (Rect){ side_x,                     row_y, btn_w, btn_h };
    L.btn_split_h   = (Rect){ side_x + btn_w + 8,         row_y, btn_w, btn_h };
    L.btn_close_pane = (Rect){ side_x,                    row_y + btn_h + 6,
                               side_w, btn_h };

    int ins_y = row_y + 2 * (btn_h + 6) + 18;
    int kw = (side_w - 8) / 2;
    L.ins_kind_local = (Rect){ side_x,           ins_y, kw, btn_h };
    L.ins_kind_ssh   = (Rect){ side_x + kw + 8,  ins_y, kw, btn_h };
    ins_y += btn_h + 12;
    L.ins_host = (Rect){ side_x, ins_y, side_w, btn_h };
    L.ins_host_list = (Rect){ side_x, ins_y + btn_h + 2, side_w, 6 * 22 + 4 };
    ins_y += btn_h + 12;
    L.ins_cwd = (Rect){ side_x, ins_y, side_w, 26 };
    ins_y += 26 + 8;
    L.ins_cmd = (Rect){ side_x, ins_y, side_w, 26 };

    /* Action row across the bottom. */
    int abh = 32;
    int aby = L.modal.y + h - 22 - abh;
    L.btn_cancel    = (Rect){ L.modal.x + w - 22 - 90,     aby, 90,  abh };
    L.btn_save_open = (Rect){ L.btn_cancel.x - 8 - 130,    aby, 130, abh };
    L.btn_save      = (Rect){ L.btn_save_open.x - 8 - 80,  aby, 80,  abh };
    return L;
}

/* Forward decl — logs browser is defined near the bottom of the
   file but referenced from the Cmd+Shift+O chord and Settings →
   Logging click handler (both above the definition). */
static void logs_open(void);

static void session_designer_open_for(int idx) {
    /* Free any previous working copy. */
    if (g_sd_session.root) session_node_free(g_sd_session.root);
    memset(&g_sd_session, 0, sizeof(g_sd_session));
    g_sd_idx = idx;
    /* New session → auto-focus Name so the user can type a name
       immediately. Editing existing leaves focus off so a stray
       keystroke doesn't mutate the saved name. */
    g_sd_focus = (idx < 0) ? SDF_NAME : SDF_NONE;
    g_sd_host_dropdown = false;
    g_sd_host_scroll = 0;
    g_sd_status[0] = 0;
    g_sd_open_after_save = false;

    if (idx >= 0 && idx < g_sessions_count) {
        /* Edit existing — deep-copy the tree. */
        snprintf(g_sd_session.name, sizeof(g_sd_session.name),
                 "%s", g_sessions[idx].name);
        g_sd_session.default_idx = g_sessions[idx].default_idx;
        char layout[SESSION_LAYOUT_MAX];
        layout[0] = 0;
        size_t off = 0;
        int next = 0;
        session_serialize_walk(g_sessions[idx].root, layout, sizeof(layout),
                               &off, &next);
        g_sd_session.root = session_parse_layout(layout);
        if (g_sd_session.root) {
            SessionLeafKind kinds[SESSION_MAX_LEAVES] = {0};
            char hosts[SESSION_MAX_LEAVES][SESSION_HOST_MAX] = {{0}};
            char cwds[SESSION_MAX_LEAVES][SESSION_CWD_MAX]   = {{0}};
            char cmds[SESSION_MAX_LEAVES][SESSION_CMD_MAX]   = {{0}};
            int ix = 0;
            session_collect_leaves(g_sessions[idx].root, kinds, hosts, cwds, cmds, &ix);
            int ax = 0;
            session_apply_leaves(g_sd_session.root, kinds, hosts, cwds, cmds,
                                 &ax, ix);
        }
    } else {
        /* Fresh session: one local-shell leaf, empty name (the
           placeholder hint in the field invites the user to type
           one). */
        g_sd_session.name[0] = 0;
        g_sd_session.default_idx = 0;
        g_sd_session.root = session_node_new_leaf();
    }
    /* Pre-select the saved default leaf so the canvas highlights
       what'll get focus on open — the user can click another pane
       to change it. */
    g_sd_selected = session_first_leaf(g_sd_session.root);
    {
        int want = g_sd_session.default_idx;
        SessionNode *cur = g_sd_selected;
        for (int k = 0; cur && k < want; k++) cur = session_next_leaf(cur);
        if (cur) g_sd_selected = cur;
    }
    /* Refresh saved-host list so the dropdown is fresh. */
    ssh_profiles_load();
    g_ui_mode = UI_SESSION_DESIGNER;
}

/* Forward decl for the open-from-session path (defined further down). */
static Tab *tab_open_from_session(const Session *s, int cols, int rows);

static bool session_designer_commit(bool also_open, int cols_for_open, int rows_for_open) {
    if (!g_sd_session.root) {
        snprintf(g_sd_status, sizeof(g_sd_status), "Nothing to save.");
        return false;
    }
    if (!g_sd_session.name[0]) {
        snprintf(g_sd_status, sizeof(g_sd_status), "Session needs a name.");
        return false;
    }
    /* Reject duplicate names when creating a new session. */
    int existing = sessions_find_by_name(g_sd_session.name);
    int dst = g_sd_idx;
    if (dst < 0) {
        if (existing >= 0) {
            snprintf(g_sd_status, sizeof(g_sd_status),
                     "A session named '%s' already exists.", g_sd_session.name);
            return false;
        }
        if (g_sessions_count >= SESSIONS_MAX) {
            snprintf(g_sd_status, sizeof(g_sd_status),
                     "Session limit reached (%d).", SESSIONS_MAX);
            return false;
        }
        dst = g_sessions_count;
    } else if (existing >= 0 && existing != dst) {
        snprintf(g_sd_status, sizeof(g_sd_status),
                 "A session named '%s' already exists.", g_sd_session.name);
        return false;
    }
    /* Capture the default-focused leaf from the user's current
       selection before we hand the tree off — replace_tree adopts
       the pointer and tracks the index by value. */
    int sel_idx = session_node_dfs_leaf_index(g_sd_session.root, g_sd_selected);
    if (sel_idx < 0) sel_idx = 0;
    /* Deep-copy g_sd_session.root into the slot — we adopt the tree
       directly, so clear g_sd_session.root afterwards to avoid a
       double-free when the modal closes. */
    session_replace_tree(dst, g_sd_session.name, g_sd_session.root, sel_idx);
    g_sd_session.root = NULL;
    g_sd_selected = NULL;
    sessions_save();
    if (also_open) {
        const Session *s = &g_sessions[dst];
        Tab *t = tab_open_from_session(s, cols_for_open, rows_for_open);
        if (!t) {
            snprintf(g_sd_status, sizeof(g_sd_status),
                     "Saved, but failed to open the session.");
            g_ui_mode = UI_NORMAL;
            return false;
        }
    }
    g_ui_mode = UI_NORMAL;
    return true;
}

static void session_designer_handle_mouse(SessionDesignerLayout L,
                                          int cols_for_open, int rows_for_open) {
    Vector2 mp = GetMousePosition();
    int mx = (int)mp.x, my = (int)mp.y;

    /* Wheel scroll inside the host dropdown. */
    if (g_sd_host_dropdown && rect_hit(L.ins_host_list, mx, my)) {
        float wheel = GetMouseWheelMove();
        if (wheel != 0.0f) {
            g_sd_host_scroll -= (int)(wheel * 3.0f);
            if (g_sd_host_scroll < 0) g_sd_host_scroll = 0;
        }
    }

    /* Splitter drag — runs every frame so the ratio updates while
       the button is held, regardless of where the cursor moves to. */
    if (g_sd_drag_node) {
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            const int gap = 2;
            float nr;
            if (g_sd_drag_node->split == SPLIT_VERTICAL) {
                int span = g_sd_drag_outer.w - gap;
                nr = (span > 0)
                     ? (float)(mx - g_sd_drag_outer.x) / (float)span
                     : 0.5f;
            } else {
                int span = g_sd_drag_outer.h - gap;
                nr = (span > 0)
                     ? (float)(my - g_sd_drag_outer.y) / (float)span
                     : 0.5f;
            }
            if (nr < 0.15f) nr = 0.15f;
            if (nr > 0.85f) nr = 0.85f;
            g_sd_drag_node->ratio = nr;
            return;
        }
        g_sd_drag_node = NULL;  /* Button released — end drag. */
    }

    /* Splitter hit-test on a fresh press. Wider grab zone so the
       2-px visual gap is comfortable to grab. */
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
        g_sd_session.root && rect_hit(L.canvas, mx, my)) {
        PaneRect outer = { L.canvas.x, L.canvas.y, L.canvas.w, L.canvas.h };
        PaneRect hit_outer = {0};
        SessionNode *hit_split =
            session_node_splitter_at(g_sd_session.root, outer,
                                     mx, my, SPLITTER_GRAB, &hit_outer);
        if (hit_split) {
            g_sd_drag_node = hit_split;
            g_sd_drag_outer = hit_outer;
            return;
        }
    }

    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) return;

    /* Host dropdown — handle FIRST so a click on a row doesn't
       fall through to other rects. */
    if (g_sd_host_dropdown) {
        if (rect_hit(L.ins_host_list, mx, my)) {
            int row_h = 22;
            int idx = (my - L.ins_host_list.y) / row_h + g_sd_host_scroll;
            if (g_sd_selected) {
                if (idx == 0) {
                    /* "(none)" sentinel — clears host. */
                    g_sd_selected->host[0] = 0;
                } else if (idx - 1 < g_ssh_profile_count) {
                    snprintf(g_sd_selected->host, SESSION_HOST_MAX,
                             "%s", g_ssh_profiles[idx - 1].name);
                    g_sd_selected->kind = SESSION_LEAF_SSH;
                }
            }
            g_sd_host_dropdown = false;
            return;
        }
        if (!rect_hit(L.ins_host, mx, my)) g_sd_host_dropdown = false;
    }

    /* Click on the canvas → select that leaf. */
    if (rect_hit(L.canvas, mx, my) && g_sd_session.root) {
        PaneRect outer = { L.canvas.x, L.canvas.y, L.canvas.w, L.canvas.h };
        SessionNode *hit = session_node_at(g_sd_session.root, outer, mx, my);
        if (hit) g_sd_selected = hit;
        return;
    }

    /* Toolbar. */
    if (rect_hit(L.btn_split_v, mx, my)) {
        if (g_sd_selected) {
            SessionNode *nl = session_split_leaf(&g_sd_session, g_sd_selected,
                                                 SPLIT_VERTICAL);
            if (nl) g_sd_selected = nl;
            else snprintf(g_sd_status, sizeof(g_sd_status),
                          "Cannot split — leaf cap %d.", SESSION_MAX_LEAVES);
        }
        return;
    }
    if (rect_hit(L.btn_split_h, mx, my)) {
        if (g_sd_selected) {
            SessionNode *nl = session_split_leaf(&g_sd_session, g_sd_selected,
                                                 SPLIT_HORIZONTAL);
            if (nl) g_sd_selected = nl;
            else snprintf(g_sd_status, sizeof(g_sd_status),
                          "Cannot split — leaf cap %d.", SESSION_MAX_LEAVES);
        }
        return;
    }
    if (rect_hit(L.btn_close_pane, mx, my)) {
        if (g_sd_selected && session_count_leaves(g_sd_session.root) > 1) {
            SessionNode *succ = session_close_leaf(&g_sd_session, g_sd_selected);
            g_sd_selected = succ ? succ : session_first_leaf(g_sd_session.root);
        } else {
            snprintf(g_sd_status, sizeof(g_sd_status),
                     "Can't close the last pane.");
        }
        return;
    }

    /* Inspector — kind toggle. */
    if (g_sd_selected && rect_hit(L.ins_kind_local, mx, my)) {
        g_sd_selected->kind = SESSION_LEAF_LOCAL;
        g_sd_selected->host[0] = 0;
        return;
    }
    if (g_sd_selected && rect_hit(L.ins_kind_ssh, mx, my)) {
        g_sd_selected->kind = SESSION_LEAF_SSH;
        return;
    }
    /* Host dropdown trigger. */
    if (g_sd_selected && rect_hit(L.ins_host, mx, my)) {
        g_sd_host_dropdown = !g_sd_host_dropdown;
        if (g_sd_host_dropdown) {
            ssh_profiles_load();
            g_sd_host_scroll = 0;
        }
        return;
    }
    /* Text-field focus selection. */
    if (rect_hit(L.name_field, mx, my)) { g_sd_focus = SDF_NAME; return; }
    if (g_sd_selected && rect_hit(L.ins_cwd, mx, my)) { g_sd_focus = SDF_CWD; return; }
    if (g_sd_selected && rect_hit(L.ins_cmd, mx, my)) { g_sd_focus = SDF_CMD; return; }

    /* Action buttons. */
    if (rect_hit(L.btn_save, mx, my)) {
        session_designer_commit(false, 0, 0);
        return;
    }
    if (rect_hit(L.btn_save_open, mx, my)) {
        session_designer_commit(true, cols_for_open, rows_for_open);
        return;
    }
    if (rect_hit(L.btn_cancel, mx, my)) {
        if (g_sd_session.root) {
            session_node_free(g_sd_session.root);
            g_sd_session.root = NULL;
        }
        g_sd_selected = NULL;
        g_ui_mode = UI_NORMAL;
        return;
    }
    /* Click anywhere else inside the modal drops field focus. */
    if (rect_hit(L.modal, mx, my)) g_sd_focus = SDF_NONE;
}

static char *session_designer_focused_buf(size_t *cap) {
    if (g_sd_focus == SDF_NAME) {
        *cap = sizeof(g_sd_session.name);
        return g_sd_session.name;
    }
    if (g_sd_focus == SDF_CWD && g_sd_selected) {
        *cap = SESSION_CWD_MAX;
        return g_sd_selected->cwd;
    }
    if (g_sd_focus == SDF_CMD && g_sd_selected) {
        *cap = SESSION_CMD_MAX;
        return g_sd_selected->cmd;
    }
    *cap = 0;
    return NULL;
}

static void session_designer_handle_keys(int cols_for_open, int rows_for_open) {
    if (IsKeyPressed(KEY_ESCAPE)) {
        if (g_sd_host_dropdown) { g_sd_host_dropdown = false; return; }
        if (g_sd_session.root) {
            session_node_free(g_sd_session.root);
            g_sd_session.root = NULL;
        }
        g_sd_selected = NULL;
        g_ui_mode = UI_NORMAL;
        return;
    }
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
        if (g_sd_focus == SDF_NAME) {
            g_sd_focus = SDF_NONE;
            return;
        }
        session_designer_commit(false, cols_for_open, rows_for_open);
        return;
    }
    size_t cap = 0;
    char *buf = session_designer_focused_buf(&cap);
    if (!buf || cap == 0) return;
    size_t len = strlen(buf);
    if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) {
        if (len > 0) buf[len - 1] = 0;
        return;
    }
    int ch;
    while ((ch = GetCharPressed()) != 0) {
        if (ch < 32 || ch >= 127) continue;
        if (len + 1 >= cap) break;
        buf[len++] = (char)ch;
        buf[len] = 0;
    }
    (void)cols_for_open; (void)rows_for_open;
}

/* Recursive canvas paint — colours leaves by kind, highlights the
   selected leaf, prints the leaf's label inside its rect when it's
   tall enough to read. */
static void session_canvas_paint(Renderer *r, const SessionNode *n,
                                 PaneRect outer, const SessionNode *selected) {
    if (!n) return;
    if (n->split == SPLIT_NONE) {
        Color base = (n->kind == SESSION_LEAF_SSH)
                        ? (Color){46, 70, 110, 255}
                        : (Color){38, 56, 46, 255};
        if (n == selected) {
            base.r = (unsigned char)(base.r + 30 > 255 ? 255 : base.r + 30);
            base.g = (unsigned char)(base.g + 30 > 255 ? 255 : base.g + 30);
            base.b = (unsigned char)(base.b + 30 > 255 ? 255 : base.b + 30);
        }
        DrawRectangle(outer.x, outer.y, outer.w, outer.h, base);
        Color border = (n == selected) ? (Color){125, 207, 255, 255}
                                       : (Color){70, 80, 100, 200};
        DrawRectangleLinesEx(
            (Rectangle){(float)outer.x, (float)outer.y,
                        (float)outer.w, (float)outer.h},
            (n == selected) ? 3.0f : 1.0f, border);
        if (outer.h >= 24 && outer.w >= 60 && r->font_data) {
            const char *label;
            char buf[160];
            if (n->kind == SESSION_LEAF_SSH) {
                snprintf(buf, sizeof(buf), "ssh:%s",
                         n->host[0] ? n->host : "(pick host)");
                label = buf;
            } else {
                label = "local shell";
            }
            Font *f = (Font *)r->font_data;
            DrawTextEx(*f, label,
                       (Vector2){outer.x + 8, outer.y + 6},
                       13, 0, (Color){220, 230, 245, 255});
            if (outer.h >= 50 && (n->cwd[0] || n->cmd[0])) {
                char sub[200];
                snprintf(sub, sizeof(sub), "%s%s%s",
                         n->cwd[0] ? n->cwd : "",
                         (n->cwd[0] && n->cmd[0]) ? "  " : "",
                         n->cmd[0] ? n->cmd : "");
                DrawTextEx(*f, sub,
                           (Vector2){outer.x + 8, outer.y + 22},
                           11, 0, (Color){170, 180, 200, 220});
            }
        }
        return;
    }
    PaneRect ra, rb;
    session_node_split_children(n, outer, &ra, &rb);
    session_canvas_paint(r, n->child[0], ra, selected);
    session_canvas_paint(r, n->child[1], rb, selected);
}

static void draw_session_designer(Renderer *r, int win_w, int win_h,
                                  SessionDesignerLayout L) {
    Font *f = (Font *)r->font_data;
    /* Backdrop. */
    DrawRectangle(0, 0, win_w, win_h, (Color){0, 0, 0, 160});
    DrawRectangle(L.modal.x, L.modal.y, L.modal.w, L.modal.h,
                  (Color){26, 28, 38, 255});
    DrawRectangleLines(L.modal.x, L.modal.y, L.modal.w, L.modal.h,
                       (Color){80, 90, 110, 255});
    DrawTextEx(*f, "Session Designer",
               (Vector2){L.modal.x + 20, L.modal.y + 11},
               16, 0, (Color){230, 232, 240, 255});

    /* Name field. */
    DrawTextEx(*f, "Name",
               (Vector2){L.modal.x + 22, L.name_field.y + 7},
               13, 0, (Color){180, 185, 200, 255});
    bool name_focus = (g_sd_focus == SDF_NAME);
    DrawRectangle(L.name_field.x, L.name_field.y, L.name_field.w, L.name_field.h,
                  (Color){22, 25, 34, 255});
    DrawRectangleLines(L.name_field.x, L.name_field.y, L.name_field.w, L.name_field.h,
                       name_focus ? (Color){125, 207, 255, 255}
                                  : (Color){70, 74, 90, 255});
    const char *name_shown = g_sd_session.name[0]
                                ? g_sd_session.name
                                : "(type a name)";
    DrawTextEx(*f, name_shown,
               (Vector2){L.name_field.x + 8, L.name_field.y + 7},
               14, 0, g_sd_session.name[0]
                          ? (Color){230, 232, 240, 255}
                          : (Color){110, 115, 130, 255});
    /* Caret when the field is focused — blinks at 2 Hz. */
    if (name_focus && ((long long)(GetTime() * 2.0) & 1) == 0) {
        Vector2 ns = MeasureTextEx(*f, g_sd_session.name, 14, 0);
        int cx = (int)(L.name_field.x + 8 + ns.x + 1);
        DrawRectangle(cx, L.name_field.y + 6, 2, 16,
                      (Color){125, 207, 255, 255});
    }

    /* Canvas. */
    DrawRectangle(L.canvas.x, L.canvas.y, L.canvas.w, L.canvas.h,
                  (Color){12, 14, 22, 255});
    DrawRectangleLines(L.canvas.x, L.canvas.y, L.canvas.w, L.canvas.h,
                       (Color){70, 80, 100, 255});
    if (g_sd_session.root) {
        PaneRect outer = { L.canvas.x + 2, L.canvas.y + 2,
                           L.canvas.w - 4, L.canvas.h - 4 };
        session_canvas_paint(r, g_sd_session.root, outer, g_sd_selected);
    }

    /* Toolbar — split V / H / close pane. */
    struct { Rect r; const char *lbl; bool danger; } tools[] = {
        { L.btn_split_v,    "+ Split V",   false },
        { L.btn_split_h,    "+ Split H",   false },
        { L.btn_close_pane, "× Close pane", true  },
    };
    for (size_t i = 0; i < sizeof(tools) / sizeof(tools[0]); i++) {
        Rect tr = tools[i].r;
        Color bg = tools[i].danger ? (Color){62, 30, 32, 255}
                                   : (Color){38, 48, 66, 255};
        Color outline = tools[i].danger ? (Color){200, 80, 80, 220}
                                        : (Color){125, 207, 255, 200};
        DrawRectangle(tr.x, tr.y, tr.w, tr.h, bg);
        DrawRectangleLines(tr.x, tr.y, tr.w, tr.h, outline);
        Vector2 sz = MeasureTextEx(*f, tools[i].lbl, 13, 0);
        DrawTextEx(*f, tools[i].lbl,
                   (Vector2){tr.x + (tr.w - sz.x) / 2,
                             tr.y + (tr.h - sz.y) / 2},
                   13, 0, (Color){230, 232, 240, 255});
    }

    /* Inspector header. */
    DrawTextEx(*f, "Selected pane",
               (Vector2){L.ins_kind_local.x, L.ins_kind_local.y - 18},
               13, 0, (Color){180, 185, 200, 255});

    /* Kind toggle. */
    bool is_ssh = g_sd_selected && g_sd_selected->kind == SESSION_LEAF_SSH;
    struct { Rect r; const char *lbl; bool active; } kinds[] = {
        { L.ins_kind_local, "Local",  !is_ssh },
        { L.ins_kind_ssh,   "SSH",    is_ssh  },
    };
    for (int i = 0; i < 2; i++) {
        Rect kr = kinds[i].r;
        Color bg = kinds[i].active ? (Color){46, 92, 150, 255}
                                   : (Color){34, 38, 52, 255};
        DrawRectangle(kr.x, kr.y, kr.w, kr.h, bg);
        DrawRectangleLines(kr.x, kr.y, kr.w, kr.h,
                           kinds[i].active ? (Color){125, 207, 255, 255}
                                            : (Color){70, 74, 90, 255});
        Vector2 sz = MeasureTextEx(*f, kinds[i].lbl, 13, 0);
        DrawTextEx(*f, kinds[i].lbl,
                   (Vector2){kr.x + (kr.w - sz.x) / 2,
                             kr.y + (kr.h - sz.y) / 2},
                   13, 0, (Color){230, 232, 240, 255});
    }

    /* Host dropdown trigger (visible only when SSH). */
    {
        Rect hr = L.ins_host;
        Color bg = is_ssh ? (Color){22, 25, 34, 255} : (Color){18, 20, 28, 255};
        Color outline = is_ssh ? (Color){70, 74, 90, 255}
                               : (Color){50, 54, 66, 255};
        DrawRectangle(hr.x, hr.y, hr.w, hr.h, bg);
        DrawRectangleLines(hr.x, hr.y, hr.w, hr.h, outline);
        const char *lbl = is_ssh
            ? (g_sd_selected->host[0] ? g_sd_selected->host : "(pick a saved host)")
            : "(host selection — switch to SSH)";
        DrawTextEx(*f, lbl,
                   (Vector2){hr.x + 10, hr.y + 6},
                   13, 0, is_ssh ? (Color){230, 232, 240, 255}
                                 : (Color){110, 115, 130, 255});
        DrawTextEx(*f, "v",
                   (Vector2){hr.x + hr.w - 22, hr.y + 6},
                   13, 0, (Color){200, 215, 240, 255});
    }

    /* Cwd + cmd. */
    struct { Rect r; SessionDesignerField field; const char *label;
             const char *value; const char *hint; } fields[] = {
        { L.ins_cwd, SDF_CWD, "Cwd",
          g_sd_selected ? g_sd_selected->cwd : "",
          "(optional working directory)" },
        { L.ins_cmd, SDF_CMD, "Cmd",
          g_sd_selected ? g_sd_selected->cmd : "",
          "(optional startup command)" },
    };
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        Rect fr = fields[i].r;
        bool focused = g_sd_focus == fields[i].field;
        DrawRectangle(fr.x, fr.y, fr.w, fr.h, (Color){22, 25, 34, 255});
        DrawRectangleLines(fr.x, fr.y, fr.w, fr.h,
                           focused ? (Color){125, 207, 255, 255}
                                   : (Color){70, 74, 90, 255});
        DrawTextEx(*f, fields[i].label,
                   (Vector2){fr.x - 36, fr.y + 6},
                   12, 0, (Color){180, 185, 200, 255});
        const char *shown = fields[i].value[0] ? fields[i].value : fields[i].hint;
        Color tc = fields[i].value[0] ? (Color){230, 232, 240, 255}
                                      : (Color){110, 115, 130, 255};
        DrawTextEx(*f, shown,
                   (Vector2){fr.x + 8, fr.y + 6},
                   13, 0, tc);
        if (focused && ((long long)(GetTime() * 2.0) & 1) == 0) {
            Vector2 vsz = MeasureTextEx(*f, fields[i].value, 13, 0);
            int cx = (int)(fr.x + 8 + vsz.x + 1);
            DrawRectangle(cx, fr.y + 5, 2, 16, (Color){125, 207, 255, 255});
        }
    }

    /* Bottom action row. */
    struct { Rect r; const char *lbl; bool primary; } actions[] = {
        { L.btn_save,      "Save",        false },
        { L.btn_save_open, "Save & Open", true  },
        { L.btn_cancel,    "Cancel",      false },
    };
    for (size_t i = 0; i < sizeof(actions) / sizeof(actions[0]); i++) {
        Rect ar = actions[i].r;
        Color bg = actions[i].primary ? (Color){46, 92, 150, 255}
                                      : (Color){48, 52, 66, 255};
        Color outline = actions[i].primary ? (Color){125, 207, 255, 255}
                                           : (Color){90, 96, 116, 255};
        DrawRectangle(ar.x, ar.y, ar.w, ar.h, bg);
        DrawRectangleLines(ar.x, ar.y, ar.w, ar.h, outline);
        Vector2 sz = MeasureTextEx(*f, actions[i].lbl, 14, 0);
        DrawTextEx(*f, actions[i].lbl,
                   (Vector2){ar.x + (ar.w - sz.x) / 2,
                             ar.y + (ar.h - sz.y) / 2},
                   14, 0, (Color){230, 235, 248, 255});
    }

    /* Status line above the action row. */
    if (g_sd_status[0]) {
        DrawTextEx(*f, g_sd_status,
                   (Vector2){L.modal.x + 22, L.btn_save.y - 22},
                   12, 0, (Color){240, 180, 100, 255});
    }

    /* Host dropdown — drawn last so it overlaps everything below. */
    if (g_sd_host_dropdown) {
        Rect hl = L.ins_host_list;
        DrawRectangle(hl.x, hl.y, hl.w, hl.h, (Color){22, 25, 34, 250});
        DrawRectangleLines(hl.x, hl.y, hl.w, hl.h, (Color){125, 207, 255, 220});
        int row_h = 22;
        int rows = hl.h / row_h;
        BeginScissorMode(hl.x + 2, hl.y + 2, hl.w - 4, hl.h - 4);
        /* Row 0: "(none)" sentinel. Then saved profiles. */
        for (int i = 0; i < rows + 1; i++) {
            int row_idx = i + g_sd_host_scroll;
            if (row_idx > g_ssh_profile_count) break;
            int ry = hl.y + (i) * row_h;
            const char *name = (row_idx == 0)
                                  ? "(none)"
                                  : g_ssh_profiles[row_idx - 1].name;
            Color tc = (row_idx == 0) ? (Color){170, 175, 190, 255}
                                      : (Color){230, 232, 240, 255};
            DrawTextEx(*f, name,
                       (Vector2){hl.x + 10, ry + 4},
                       13, 0, tc);
        }
        EndScissorMode();
    }
}

/* ---------- Sessions: open a saved session as a Tab ---------- */

/* Lazy-allocate a Pane's Screen (without a Pty). Used by
   tab_open_from_session to set up the placeholder rendering grid
   for each leaf before its PTY is opened. */
static void pane_ensure_screen(Pane *p, int cols, int rows) {
    if (!p) return;
    pane_init_click_state(p);
    if (!p->scr) {
        p->scr = screen_new(cols, rows, SCROLLBACK_LINES, pane_io(p));
        p->effects = g_app_settings.effects;
    }
}

/* Open the actual PTY for a session leaf — local shell or SSH host
   per kind. The Pane's Screen is assumed already set up by
   pane_ensure_screen so the renderer can paint while we connect.
   Synchronous for SSH leaves in v1; the user sees splits appear in
   sequence as each handshake finishes. (Async per-leaf parallel
   connect is a future step that'd reuse the existing
   SshLaunchWorker apparatus.) */
static bool session_open_leaf_pty(Pane *p,
                                  SessionLeafKind kind,
                                  const char *host_alias,
                                  const char *cwd, const char *cmd,
                                  int cols, int rows) {
    if (!p || p->pty) return p && p->pty;
    if (kind == SESSION_LEAF_SSH && host_alias && host_alias[0]) {
#ifdef RBTERM_SSH
        const SshProfile *prof = NULL;
        for (int i = 0; i < g_ssh_profile_count; i++) {
            if (strcmp(g_ssh_profiles[i].name, host_alias) == 0) {
                prof = &g_ssh_profiles[i]; break;
            }
        }
        char err[256] = {0};
        Pty *pty = pty_open_ssh(
            prof && prof->user[0]     ? prof->user     : NULL,
            prof && prof->hostname[0] ? prof->hostname : host_alias,
            prof ? (prof->port > 0 ? prof->port : 22) : 22,
            NULL,
            prof && prof->identity[0] ? prof->identity : NULL,
            cols, rows, err, sizeof(err));
        if (!pty) {
            fprintf(stderr,
                    "rbterm: session ssh '%s' failed: %s\n",
                    host_alias, err[0] ? err : "(unknown)");
            /* Surface the failure into the pane so the user sees it
               in-place rather than just a blank tile. */
            char msg[512];
            int n = snprintf(msg, sizeof(msg),
                             "\x1b[2J\x1b[H\x1b[1;31mFailed to connect to %s\x1b[0m\r\n"
                             "\x1b[2;37m%s\x1b[0m\r\n",
                             host_alias, err[0] ? err : "(no error)");
            if (n > 0 && n < (int)sizeof(msg))
                screen_feed(p->scr, (const uint8_t *)msg, (size_t)n);
            snprintf(p->title, sizeof(p->title), "Failed: %s", host_alias);
            return false;
        }
        p->pty = pty;
        snprintf(p->title, sizeof(p->title), "%s", host_alias);
#else
        (void)cols; (void)rows;
        char msg[256];
        int n = snprintf(msg, sizeof(msg),
                         "\x1b[2J\x1b[H\x1b[1;31mSSH not built into this rbterm.\x1b[0m\r\n");
        if (n > 0) screen_feed(p->scr, (const uint8_t *)msg, (size_t)n);
        return false;
#endif
    } else {
        const char *want_cwd = (cwd && cwd[0]) ? cwd : NULL;
        p->pty = pty_open(cols, rows, want_cwd);
        if (!p->pty) {
            fprintf(stderr, "rbterm: session local pane: pty_open failed\n");
            return false;
        }
        if (want_cwd) {
            strncpy(p->cwd, want_cwd, sizeof(p->cwd) - 1);
            p->cwd[sizeof(p->cwd) - 1] = 0;
        }
        snprintf(p->title, sizeof(p->title), "shell");
    }
    /* Send cwd / cmd as init lines. For SSH the cwd doubles as a
       cd command (libssh's cwd doesn't follow ssh's HostName); for
       local panes it was already the spawn cwd, so cd-ing into it
       is a no-op but echoes the path which matches the user's
       expectation. */
    if (cwd && cwd[0])
        ssh_send_init_line(p, cwd, NULL);
    if (cmd && cmd[0])
        ssh_send_init_line(p, NULL, cmd);
    return true;
}

/* Recursive replay walker. Mirrors layout_replay_walk's shape but
   uses per-leaf kind/host metadata from the Session instead of
   re-dialing the parent tab's host. */
static void session_replay_recurse(Tab *t, const SessionNode *sn,
                                   PaneNode *current_leaf, int *next_idx,
                                   const SessionLeafKind *kinds,
                                   const char hosts[][SESSION_HOST_MAX],
                                   const char cwds[][SESSION_CWD_MAX],
                                   const char cmds[][SESSION_CMD_MAX],
                                   int cols, int rows) {
    if (!sn || !current_leaf) return;
    if (sn->split == SPLIT_NONE) {
        int i = (*next_idx)++;
        if (i >= SESSION_MAX_LEAVES) return;
        session_open_leaf_pty(current_leaf->pane, kinds[i], hosts[i],
                              cwds[i], cmds[i], cols, rows);
        return;
    }
    /* Structural split. Allocate the new leaf with a Screen but no
       Pty yet; the recursion fills its PTY when it reaches a leaf. */
    PaneNode *new_leaf = pane_node_split_leaf(t, current_leaf, sn->split);
    if (!new_leaf) return;
    if (new_leaf->parent) new_leaf->parent->ratio = sn->ratio;
    pane_ensure_screen(new_leaf->pane, cols, rows);
    snprintf(new_leaf->pane->title, sizeof(new_leaf->pane->title), "...");
    /* Show a banner in the new pane while we wait. */
    const char *wait =
        "\x1b[2J\x1b[H\x1b[1;36mPreparing session pane...\x1b[0m\r\n";
    screen_feed(new_leaf->pane->scr, (const uint8_t *)wait, strlen(wait));
    session_replay_recurse(t, sn->child[0], current_leaf, next_idx,
                           kinds, hosts, cwds, cmds, cols, rows);
    session_replay_recurse(t, sn->child[1], new_leaf, next_idx,
                           kinds, hosts, cwds, cmds, cols, rows);
}

/* Build a Tab from a saved Session. The first leaf gets opened
   inline (with a Screen but no Pty), the recursive walker creates
   the rest of the splits and opens each leaf's PTY in DFS pre-
   order. Synchronous for now — multi-host parallel connect is the
   natural follow-up. Returns NULL on failure (out of tab slots,
   empty session, or first PTY open fails). */
static Tab *tab_open_from_session(const Session *s, int cols, int rows) {
    if (!s || !s->root) return NULL;
    SessionLeafKind kinds[SESSION_MAX_LEAVES] = {0};
    char hosts[SESSION_MAX_LEAVES][SESSION_HOST_MAX] = {{0}};
    char cwds[SESSION_MAX_LEAVES][SESSION_CWD_MAX]   = {{0}};
    char cmds[SESSION_MAX_LEAVES][SESSION_CMD_MAX]   = {{0}};
    int count = 0;
    session_collect_leaves(s->root, kinds, hosts, cwds, cmds, &count);
    if (count == 0) return NULL;
    if (g_num_tabs >= MAX_TABS) return NULL;

    Tab *t = calloc(1, sizeof(Tab));
    PaneNode *leaf = pane_node_new_leaf();
    t->root = leaf;
    t->active = leaf;
    pane_ensure_screen(leaf->pane, cols, rows);
    snprintf(leaf->pane->title, sizeof(leaf->pane->title), "...");
    snprintf(t->tab_name, sizeof(t->tab_name), "%s", s->name);
    /* Mark as SSH if any leaf is SSH — the SFTP / broadcast / etc.
       UIs key off this flag and most session usage will be
       SSH-heavy anyway. */
    for (int k = 0; k < count; k++) {
        if (kinds[k] == SESSION_LEAF_SSH) { t->is_ssh = true; break; }
    }
    g_tabs[g_num_tabs] = t;
    g_active = g_num_tabs;
    g_num_tabs++;

    int next_idx = 0;
    session_replay_recurse(t, s->root, leaf, &next_idx,
                           kinds, hosts, cwds, cmds, cols, rows);
    /* Walk the live tree in DFS pre-order — same order as
       session_collect_leaves / session_replay_recurse — and pick
       the leaf the user marked as default in the designer. Falls
       back to the first leaf for legacy sessions or out-of-range
       indices. */
    PaneNode *active = pane_tree_first_leaf(t->root);
    int want = (s->default_idx >= 0 && s->default_idx < count) ? s->default_idx : 0;
    for (int k = 0; active && k < want; k++) active = pane_tree_next_leaf(active);
    t->active = active ? active : pane_tree_first_leaf(t->root);
    tab_log_open_all(t);
    return t;
}

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
    /* Tab accent colour row: 8 preset swatches plus a "none" sentinel
       at index SSH_COLOR_PRESET_COUNT. */
    Rect color_swatch[9];
    /* Cursor colour row — same shape, slot 8 = "default". */
    Rect cur_color_swatch[9];
    /* HUD tab — per-host override of the HUD config. */
    Rect hud_override_btn;      /* "Use host overrides" toggle */
    Rect hud_toggle;            /* show / hide HUD on this host */
    Rect hud_pos_tl, hud_pos_tr, hud_pos_bl, hud_pos_br;
    Rect hud_show_btn[HUD_FIELD_COUNT];
    Rect hud_color_btn[HUD_FIELD_COUNT];
    Rect hud_size_dec[HUD_FIELD_COUNT];
    Rect hud_size_val[HUD_FIELD_COUNT];
    Rect hud_size_inc[HUD_FIELD_COUNT];
    Rect hud_cpu_toggle;
    /* Effects tab — per-host visual-effect override. Layout matches
       the Settings → Effects tab: an Override toggle, six sliders in
       two columns, a Decay slider, a Phosphor pill row, and a Preset
       row. */
    Rect efx_override_btn;
    Rect efx_slider[EFX_SLIDER_COUNT];
    Rect efx_decay;
    Rect efx_phos[PHOSPHOR_COUNT];
    Rect efx_preset[EFX_PRESET_COUNT];
    /* Form-tab buttons across the top of the modal — Connection /
       Appearance / Logging / HUD / Effects. */
    Rect form_tab[5];
    Rect newbtn;
    Rect testbtn;               /* dry-run auth without opening a tab */
    Rect connect;
    Rect delbtn;                /* zero-sized when not deletable */
    Rect clonebtn;              /* zero-sized when no saved host is selected */
    Rect save;
    Rect cancel;
    /* Key-file dropdown trigger (right edge of F_KEY field) and the
       expanded list rect when open. List is sized for up to 6 visible
       rows; scroll if more. */
    Rect key_pick_btn;
    Rect key_pick_list;
    /* Save-current-layout button (Connection tab). Captures the active
       SSH tab's split tree + per-pane cwd into ~/.ssh/config. */
    Rect save_layout;
} SshFormLayout;

/* Per-list scroll state for the SSH form (independent of the Settings
   modal's scrolls so each picker remembers its own position). */
static int g_form_theme_scroll = 0;
static int g_form_font_scroll  = 0;
static bool g_form_key_dropdown = false;
static int  g_form_key_scroll   = 0;

/* SSH form is split into tabs the same way the Settings modal is.
   Connection holds the text fields users always edit; the rest are
   appearance / logging / HUD knobs that they don't usually touch
   on every connect. */
typedef enum {
    SSH_FORM_TAB_CONNECTION = 0,
    SSH_FORM_TAB_APPEARANCE = 1,
    SSH_FORM_TAB_LOGGING    = 2,
    SSH_FORM_TAB_HUD        = 3,
    SSH_FORM_TAB_EFFECTS    = 4,
    SSH_FORM_TAB_COUNT      = 5,
} SshFormTabId;
static int g_ssh_form_tab = SSH_FORM_TAB_CONNECTION;

/* True for one frame after the help modal opens, so the same click
   that triggered the open doesn't immediately dismiss it via the
   "click outside" check. */
static bool g_help_just_opened = false;
/* 0 = Navigation, 1 = Edit & Search, 2 = Shell integration. Persists
   between opens so the user lands on whichever tab they last
   looked at. */
static int  g_help_tab = 0;
/* Sub-tab within the Navigation top-level tab, since the chord
   list is large enough that a single page is hard to scan. Order:
   0 = Tabs, 1 = Panes, 2 = Scroll & font, 3 = Modals. */
#define HELP_NAV_SUBTAB_COUNT 4
static int  g_help_nav_subtab = 0;
/* Edit & Search top-level tab also splits cleanly:
   0 = Selection (clipboard + click selection)
   1 = Search (find-bar chords). */
#define HELP_EDIT_SUBTAB_COUNT 2
static int  g_help_edit_subtab = 0;
/* Shell integration top-level tab splits by shell, since the
   one-line setup snippet is the part the user actually copies
   and it's different per shell. The narrative + caveats render
   identically on both panels — only the sourcing snippet
   differs. */
#define HELP_SHELL_SUBTAB_COUNT 2
static int  g_help_shell_subtab = 0;
/* Filled by draw_help_modal each frame so the main-loop click
   handler can hit-test the tab buttons AND know the actual panel
   rect (which depends on the active tab's row count, so a fixed
   fallback would over-claim "inside" for short tabs). */
#define HELP_TAB_COUNT 3
static Rect g_help_tab_rects[HELP_TAB_COUNT];
static Rect g_help_nav_subtab_rects[HELP_NAV_SUBTAB_COUNT];
static Rect g_help_edit_subtab_rects[HELP_EDIT_SUBTAB_COUNT];
static Rect g_help_shell_subtab_rects[HELP_SHELL_SUBTAB_COUNT];
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

/* Slider drag state for the SSH form's Effects tab. */
static bool g_form_efx_drag = false;
static int  g_form_efx_drag_idx = 0;

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

/* Forward decls for font-picker grouping helpers (defined down with
   the loader). Both pickers (Settings and SSH form) call into these
   to decide which display-rows are headers and which are font rows. */
static int         font_display_row_count(void);
static int         font_display_to_idx(int row);
static int         font_idx_to_display(int idx);
static const char *font_header_at_row(int row);

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
    bool  ligatures;       /* true = font ships with programming ligatures (Fira
                              Code-class). The picker groups these at the top of
                              the list so users can find them at a glance. The
                              flag is informational only until HarfBuzz shaping
                              lands — rbterm doesn't substitute glyph clusters
                              today, but the font *would* light up when it does. */
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

/* Forward decl — defined alongside pane_log_close below; used
   from pane_log_open to wire the screen's scrollback callback. */
static void pane_clean_log_row_cb(void *user, const Cell *row,
                                  int cols, int wrapped);

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
    if (pane_tree_count(t->root) > 1 || pane_idx > 0) {
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
    /* Parallel clean .txt transcript — same name with .txt suffix
       instead of .log. Wired to the screen's scrollback-push
       callback so rows finalized by scroll-up land here as plain
       UTF-8 (no ANSI escapes). */
    snprintf(p->clean_path, sizeof(p->clean_path), "%s", p->log_path);
    {
        size_t pl = strlen(p->clean_path);
        if (pl >= 4 && strcmp(p->clean_path + pl - 4, ".log") == 0) {
            strcpy(p->clean_path + pl - 4, ".txt");
        }
    }
    p->clean_fp = fopen(p->clean_path, "ab");
    if (!p->clean_fp) {
        fprintf(stderr, "rbterm: can't open clean log %s: %s\n",
                p->clean_path, strerror(errno));
        p->clean_path[0] = 0;
    } else if (p->scr) {
        screen_set_scrollback_callback(p->scr, pane_clean_log_row_cb, p);
    }
}

/* Close a pane's open log file (if any). Idempotent. */
static void pane_log_close(Pane *p) {
    if (!p) return;
    if (p->log_fp) { fclose(p->log_fp); p->log_fp = NULL; }
    if (p->clean_fp) {
        /* Flush whatever is currently visible on the live grid as
           a "tail" to the clean log, so the last screenful (which
           never scrolled out) isn't missing from the transcript. */
        if (p->scr) {
            int rows = screen_rows(p->scr);
            int cols = screen_cols(p->scr);
            for (int y = 0; y < rows; y++) {
                int last = -1;
                for (int x = cols - 1; x >= 0; x--) {
                    Cell c = screen_view_cell(p->scr, x, y);
                    if (c.cp != 0 && c.cp != ' ' && !(c.attrs & ATTR_WIDE_CONT)) { last = x; break; }
                }
                if (last < 0) continue;
                for (int x = 0; x <= last; x++) {
                    Cell c = screen_view_cell(p->scr, x, y);
                    if (c.attrs & ATTR_WIDE_CONT) continue;
                    uint32_t cp = c.cp ? c.cp : ' ';
                    if (cp < 0x80) {
                        fputc((int)cp, p->clean_fp);
                    } else if (cp < 0x800) {
                        fputc(0xC0 | (cp >> 6),     p->clean_fp);
                        fputc(0x80 | (cp & 0x3F),   p->clean_fp);
                    } else if (cp < 0x10000) {
                        fputc(0xE0 | (cp >> 12),         p->clean_fp);
                        fputc(0x80 | ((cp >> 6) & 0x3F), p->clean_fp);
                        fputc(0x80 | (cp & 0x3F),        p->clean_fp);
                    } else {
                        fputc(0xF0 | (cp >> 18),          p->clean_fp);
                        fputc(0x80 | ((cp >> 12) & 0x3F), p->clean_fp);
                        fputc(0x80 | ((cp >> 6) & 0x3F),  p->clean_fp);
                        fputc(0x80 | (cp & 0x3F),         p->clean_fp);
                    }
                }
                fputc('\n', p->clean_fp);
            }
        }
        fclose(p->clean_fp);
        p->clean_fp = NULL;
    }
    if (p->scr) {
        screen_set_scrollback_callback(p->scr, NULL, NULL);
    }
}

/* Scrollback-push callback. Each finalized row arrives here as
   resolved Cell[cols]; we walk the cells, emit codepoints as
   UTF-8, and either continue the line (if auto-wrapped) or
   terminate it with \n. Skips trailing default-blank cells so
   half-empty rows don't tail with a wall of spaces. */
static void pane_clean_log_row_cb(void *user, const Cell *row,
                                  int cols, int wrapped) {
    Pane *p = (Pane *)user;
    if (!p || !p->clean_fp) return;
    int last = -1;
    for (int x = cols - 1; x >= 0; x--) {
        if (row[x].cp != 0 && row[x].cp != ' ' &&
            !(row[x].attrs & ATTR_WIDE_CONT)) { last = x; break; }
    }
    for (int x = 0; x <= last; x++) {
        if (row[x].attrs & ATTR_WIDE_CONT) continue;
        uint32_t cp = row[x].cp ? row[x].cp : ' ';
        if (cp < 0x80) {
            fputc((int)cp, p->clean_fp);
        } else if (cp < 0x800) {
            fputc(0xC0 | (cp >> 6),     p->clean_fp);
            fputc(0x80 | (cp & 0x3F),   p->clean_fp);
        } else if (cp < 0x10000) {
            fputc(0xE0 | (cp >> 12),         p->clean_fp);
            fputc(0x80 | ((cp >> 6) & 0x3F), p->clean_fp);
            fputc(0x80 | (cp & 0x3F),        p->clean_fp);
        } else {
            fputc(0xF0 | (cp >> 18),          p->clean_fp);
            fputc(0x80 | ((cp >> 12) & 0x3F), p->clean_fp);
            fputc(0x80 | ((cp >> 6) & 0x3F),  p->clean_fp);
            fputc(0x80 | (cp & 0x3F),         p->clean_fp);
        }
    }
    /* Auto-wrap means the row terminated by hitting cols, not by
       a logical line break — keep the next row on the same
       logical line by NOT writing \n. */
    if (!wrapped) fputc('\n', p->clean_fp);
    fflush(p->clean_fp);
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
    if (!t || !t->root) return;
    int i = 0;
    for (PaneNode *leaf = pane_tree_first_leaf(t->root); leaf;
         leaf = pane_tree_next_leaf(leaf), i++) {
        pane_log_open(t, leaf->pane, i);
    }
}
/* Symmetric — close every pane's log in a tab. */
static void tab_log_close_all(Tab *t) {
    if (!t || !t->root) return;
    for (PaneNode *leaf = pane_tree_first_leaf(t->root); leaf;
         leaf = pane_tree_next_leaf(leaf)) {
        pane_log_close(leaf->pane);
    }
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
    /* Leave p->title empty (zero-initialised by tab_open's calloc).
       tab_label naturally falls through to the cwd basename, or
       "rbterm" if the cwd isn't yet detected — gives a clean
       branded title the moment the window appears, without the
       cold-start "shell" leak. */
    p->pty = pty_open(cols, rows, cwd);
    if (!p->pty) return false;
    p->scr = screen_new(cols, rows, SCROLLBACK_LINES, pane_io(p));
    /* Seed the cursor style from the rbterm-wide default. DECSCUSR
       sequences from the shell (or programs that set their own style)
       overwrite this on the per-Screen level later. */
    if (g_app_settings.cursor_style != CURSOR_STYLE_DEFAULT) {
        screen_set_cursor_style(p->scr, (CursorStyle)g_app_settings.cursor_style);
    }
    if (g_app_settings.cursor_color[0]) {
        Color cc;
        if (parse_hex_color(g_app_settings.cursor_color, &cc)) {
            uint32_t rgb = ((uint32_t)cc.r << 16) | ((uint32_t)cc.g << 8) | cc.b;
            screen_set_cursor_color(p->scr, rgb);
        }
    }
    if (cwd && *cwd) {
        strncpy(p->cwd, cwd, sizeof(p->cwd) - 1);
        p->cwd[sizeof(p->cwd) - 1] = 0;
    }
    /* Seed visual effects from the global default. SSH panes get
       per-host overrides layered on by tab_open_ssh. */
    p->effects = g_app_settings.effects;
    return true;
}

/* Attach a pre-built Pty to a fresh Pane. Used by both
   pane_open_ssh (synchronous connect) and the parallel-launch path,
   where the worker thread already produced the Pty. */
static void pane_attach_pty(Pane *p, Pty *pty, int cols, int rows) {
    pane_init_click_state(p);
    p->pty = pty;
    p->scr = screen_new(cols, rows, SCROLLBACK_LINES, pane_io(p));
    p->effects = g_app_settings.effects;
}

/* Like pane_open_local but the PTY is an SSH session. On failure
   (handshake / auth / channel) writes the libssh error message
   into `err` and returns false. */
static bool pane_open_ssh(Pane *p, const char *user, const char *host, int port,
                          const char *password, const char *keyfile,
                          int cols, int rows, char *err, size_t errsz) {
    Pty *pty = pty_open_ssh(user, host, port, password, keyfile,
                            cols, rows, err, errsz);
    if (!pty) return false;
    pane_attach_pty(p, pty, cols, rows);
    return true;
}

static void open_url(const char *url);

/* Asciinema recording — global singleton. We tap the PTY drain in
   the main loop and write timestamped events to one .cast file per
   captured pane (asciinema v2 spec). Multi-pane tabs record every
   leaf so split workflows produce per-pane outputs the user can
   diff or replay independently. The capture set is fixed at start
   time (so a split *during* recording doesn't add a new file mid-
   stream — same trade-off as recording staying tied to the
   originally-active tab). */
typedef struct {
    bool   active;
    /* Per-pane capture slots, populated DFS pre-order at rec_start. */
    Pane  *panes[8];
    FILE  *fps[8];
    char   paths[8][PATH_MAX];
    int    count;
    double start_time;
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
    /* Up to 8 source casts captured during the recording. src_paths[0]
       is the legacy single-pane slot; on multi-pane tabs every leaf
       gets its own slot. The save action loops over `src_count` and
       emits one output per pane. */
    char       src_paths[8][PATH_MAX];
    int        src_count;
    /* Convenience alias for code paths that only use the first
       capture (e.g. preview / single-output formats). */
    char       src_path[PATH_MAX];
    double     duration_s;
    char       dst_path[PATH_MAX];   /* user-editable destination */
    RecFmt     fmt;
    bool       path_focus;
    bool       path_sel_all;
    char       status[256];
    /* Per-recording effect choices. Reset to defaults each Stop. */
    RecEffects effects;
    /* True while the user is dragging one of the slider thumbs in the
       effects panel. Index identifies which slider (see EFX_SLIDER_*
       enum near the modal layout). */
    bool       slider_drag;
    int        slider_drag_idx;
    /* Closed-caption-style overlay of user input. When on, the
       renderer paints a translucent strip near the bottom of each
       frame showing the keystrokes the user typed (rendered as
       printable chars + ⏎ for Enter, ⌫ for Backspace, etc). */
    bool       show_captions;
} RecSave;
static RecSave g_rec_save;

/* Free everything a pane owns: log file, PTY, screen, search-match
   arrays. Idempotent — safe to call on a partially-initialised
   pane (e.g. after a failed pane_open_*). */
static void pane_free(Pane *p) {
    if (!p) return;
    /* Stop a recording if any of its captured panes is going away —
       the matching file pointer would dangle otherwise. */
    if (g_rec.active) {
        for (int _r = 0; _r < g_rec.count; _r++) {
            if (g_rec.panes[_r] == p) { rec_stop(); break; }
        }
    }
    /* Cancel + join any in-flight SFTP transfers before tearing
       down the PTY (the worker threads hold the session lock). */
    if (p->upload)   { pty_upload_release(p->upload);     p->upload = NULL; }
    if (p->download) { pty_download_release(p->download); p->download = NULL; }
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
    if (p->fx_rt.id) {
        UnloadRenderTexture(p->fx_rt);
        p->fx_rt.id = 0;
    }
    if (p->fx_prev.id) {
        UnloadRenderTexture(p->fx_prev);
        p->fx_prev.id = 0;
    }
    p->fx_rt_w = p->fx_rt_h = 0;
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
   when the user starts recording mid-session. Preserves cell text,
   cursor position, and the cell's SGR styling (bold / italic /
   underline / fg / bg), so a colourised prompt or syntax-
   highlighted output replays in the right colours. */
static void rec_emit_initial_snapshot(FILE *fp, Pane *p) {
    if (!p || !p->scr) return;
    Screen *s = p->scr;
    int cols = screen_cols(s);
    int rows = screen_rows(s);
    /* Build the byte stream into a buffer, then JSON-escape it
       through the existing rec_json_escape so the syntax matches
       any other "o" event line. */
    /* Buffer needs room for the prefix + per-row CSI + worst-case
       4-byte UTF-8 per cell + ~32 bytes of SGR per cell when
       styling changes + the final cursor restore + safety margin.
       Per-cell SGR upper bound (`\x1b[38;2;R;G;Bm` + bg + attrs)
       runs ~40 B; multiplying by every cell is overkill in
       practice (typical row has a handful of style runs) but
       under-budgeting silently truncated the last rows in earlier
       versions. */
    size_t cap = (size_t)cols * rows * 48 + (size_t)rows * 24 + 1024;
    uint8_t *buf = malloc(cap);
    if (!buf) return;
    size_t n = 0;
    /* Per-row tracker of "what SGR have we already emitted?" so
       consecutive cells with the same style don't re-emit. Reset
       at the start of every row because we always emit a CSI H
       which doesn't reset SGR — but we DO want the first cell to
       paint its style explicitly. Sentinel values guarantee a
       miss on the first compare. */
    uint16_t last_attrs = 0xFFFF;
    uint32_t last_fg    = 0xDEADBEEF;
    uint32_t last_bg    = 0xDEADBEEF;
    #define APPEND_LIT(s_lit) do { \
        size_t _l = sizeof(s_lit) - 1; \
        if (n + _l < cap) { memcpy(buf + n, (s_lit), _l); n += _l; } \
    } while (0)
    #define APPEND_FMT(...) do { \
        int _w = snprintf((char *)buf + n, cap - n, __VA_ARGS__); \
        if (_w > 0 && (size_t)_w < cap - n) n += (size_t)_w; \
    } while (0)
    /* Reset attrs, clear screen, home cursor, disable auto-wrap
       (DECAWM off) for the duration of the snapshot. Without that,
       writing the very last character of any row leaves wrap_next
       set; while CSI H normally clears it, some VT impls (and any
       intervening parser quirk) can scroll the top row to
       scrollback if a stray byte slips through before our next
       cursor position command. Re-enable auto-wrap before
       restoring the cursor so live-event playback behaves as
       expected. */
    const char *prefix = "\x1b[0m\x1b[?7l\x1b[2J\x1b[H";
    size_t plen = strlen(prefix);
    if (n + plen < cap) { memcpy(buf + n, prefix, plen); n += plen; }
    for (int y = 0; y < rows; y++) {
        /* Position cursor at the start of this row. The leading \r
           is redundant with CSI but it's a single byte of insurance
           that any future parser oddity won't leave the cursor on
           the wrong column. */
        int wrote = snprintf((char *)buf + n, cap - n, "\x1b[%d;1H\r", y + 1);
        if (wrote < 0 || (size_t)wrote >= cap - n) break;
        n += (size_t)wrote;
        /* Find last non-blank column so we don't emit huge tail-runs of spaces. */
        int last = -1;
        for (int x = cols - 1; x >= 0; x--) {
            Cell c = screen_view_cell(s, x, y);
            uint32_t cp = c.cp;
            if (cp != 0 && cp != ' ' && !(c.attrs & ATTR_WIDE_CONT)) { last = x; break; }
        }
        for (int x = 0; x <= last && n + 64 < cap; x++) {
            Cell c = screen_view_cell(s, x, y);
            if (c.attrs & ATTR_WIDE_CONT) continue;
            /* Emit a fresh SGR run when this cell's style differs
               from the most recently emitted one. Reset (`\x1b[0m`)
               + per-attribute toggles is verbose but unambiguous;
               playback parsers all handle it the same way and the
               size cost is bounded by the number of style runs in
               the snapshot. ul_color and OSC 8 link_id aren't
               re-emitted (uncommon in shell prompts; future work
               if anyone hits a case). */
            if (c.attrs != last_attrs || c.fg != last_fg || c.bg != last_bg) {
                APPEND_LIT("\x1b[0m");
                if (c.attrs & ATTR_BOLD)      APPEND_LIT("\x1b[1m");
                if (c.attrs & ATTR_DIM)       APPEND_LIT("\x1b[2m");
                if (c.attrs & ATTR_ITALIC)    APPEND_LIT("\x1b[3m");
                if (c.attrs & ATTR_UNDERLINE) APPEND_LIT("\x1b[4m");
                if (c.attrs & ATTR_REVERSE)   APPEND_LIT("\x1b[7m");
                if (c.attrs & ATTR_STRIKE)    APPEND_LIT("\x1b[9m");
                if (!(c.attrs & ATTR_DEFAULT_FG)) {
                    if (c.attrs & ATTR_FG_INDEX) {
                        APPEND_FMT("\x1b[38;5;%dm", (int)(c.fg & 0xFF));
                    } else {
                        APPEND_FMT("\x1b[38;2;%d;%d;%dm",
                                   (int)((c.fg >> 16) & 0xFF),
                                   (int)((c.fg >> 8)  & 0xFF),
                                   (int)( c.fg        & 0xFF));
                    }
                }
                if (!(c.attrs & ATTR_DEFAULT_BG)) {
                    if (c.attrs & ATTR_BG_INDEX) {
                        APPEND_FMT("\x1b[48;5;%dm", (int)(c.bg & 0xFF));
                    } else {
                        APPEND_FMT("\x1b[48;2;%d;%d;%dm",
                                   (int)((c.bg >> 16) & 0xFF),
                                   (int)((c.bg >> 8)  & 0xFF),
                                   (int)( c.bg        & 0xFF));
                    }
                }
                last_attrs = c.attrs;
                last_fg    = c.fg;
                last_bg    = c.bg;
            }
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
    #undef APPEND_LIT
    #undef APPEND_FMT
    /* Reset SGR before re-enabling auto-wrap and restoring the
       cursor. Without this, the LAST cell's style (e.g. a blue
       `~` from the user's prompt) stays active on the playback
       screen and bleeds into the first real PTY events — every
       byte the user types would inherit the prompt's colour
       until the shell emits its own SGR. The live session
       doesn't have this problem because the shell typically
       emits `\x1b[0m` after each colour run, but the snapshot
       captures cell colours not shell state, so we must reset
       explicitly. */
    int curx = screen_cursor_x(s) + 1;
    int cury = screen_cursor_y(s) + 1;
    int wrote = snprintf((char *)buf + n, cap - n,
                         "\x1b[0m\x1b[?7h\x1b[%d;%dH", cury, curx);
    if (wrote > 0 && (size_t)wrote < cap - n) n += (size_t)wrote;

    /* Emit as a single t=0 event. */
    fprintf(fp, "[0.000000, \"o\", \"");
    rec_json_escape(fp, buf, n);
    fputs("\"]\n", fp);
    fflush(fp);
    free(buf);
}

/* Status line for the most recent screenshot. The tab bar surfaces
   this as a small pill next to the camera button for ~3 seconds so
   the user gets immediate confirmation + the saved path without
   needing to look at stderr. Defined here (early in the file) so
   the tab-bar draw can show the most recent message. The actual
   screenshot_active_pane function lives further down — after the
   PaneRect / pane_rect declarations it depends on. */
static char   g_snap_status[PATH_MAX];
static double g_snap_status_at;

/* Forward declaration so the tab-bar click handler at the bottom
   of main() can dispatch into the screenshot path. The body lives
   below pane_rect's definition because it needs to compute the
   active pane's on-screen rect. */
static bool screenshot_active_pane(Renderer *r);

/* Begin recording every leaf of `t`'s pane tree. Writes one v2 .cast
   per leaf, seeds each with that leaf's current screen state so
   playback opens on what the user sees, and pins the leaf pointers
   so the capture set is fixed for the lifetime of the recording.
   Returns false if no leaves are recordable or any file-open fails
   (in which case anything we did open is closed before returning). */
static bool rec_start(Tab *t) {
    if (!t || !t->root || g_rec.active) return false;
    char dir[PATH_MAX];
    const char *want = g_app_settings.rec_dir[0]
                       ? g_app_settings.rec_dir : "~/Downloads";
    expand_home_path(want, dir, sizeof(dir));
    mkdir_p(dir);
    char stamp[64];
    time_t now = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv);

    /* Walk leaves in DFS pre-order — same numbering the layout
       descriptor uses, so save filenames line up with rbterm's
       per-pane mental model. */
    int count = 0;
    for (PaneNode *leaf = pane_tree_first_leaf(t->root); leaf;
         leaf = pane_tree_next_leaf(leaf)) {
        Pane *p = leaf->pane;
        if (!p || !p->scr) continue;
        if (count >= (int)(sizeof(g_rec.panes) / sizeof(g_rec.panes[0]))) break;
        /* Single-pane recordings keep the legacy "one filename per
           recording" feel; multi-pane add a -p<I> suffix so they
           land alongside each other in the recording dir. */
        bool many = pane_tree_count(t->root) >= 2;
        if (many) {
            snprintf(g_rec.paths[count], sizeof(g_rec.paths[count]),
                     "%s/rbterm-%s-p%d.cast", dir, stamp, count);
        } else {
            snprintf(g_rec.paths[count], sizeof(g_rec.paths[count]),
                     "%s/rbterm-%s.cast", dir, stamp);
        }
        FILE *fp = fopen(g_rec.paths[count], "wb");
        if (!fp) {
            fprintf(stderr, "rbterm: rec_start: %s: %s\n",
                    g_rec.paths[count], strerror(errno));
            /* Roll back any earlier opens. */
            for (int k = 0; k < count; k++) {
                if (g_rec.fps[k]) { fclose(g_rec.fps[k]); g_rec.fps[k] = NULL; }
                g_rec.panes[k] = NULL;
            }
            return false;
        }
        int cols = screen_cols(p->scr);
        int rows = screen_rows(p->scr);
        fprintf(fp,
                "{\"version\": 2, \"width\": %d, \"height\": %d, "
                "\"timestamp\": %lld}\n",
                cols, rows, (long long)now);
        rec_emit_initial_snapshot(fp, p);
        g_rec.panes[count] = p;
        g_rec.fps[count]   = fp;
        count++;
    }
    if (count == 0) return false;
    g_rec.active = true;
    g_rec.count = count;
    g_rec.start_time = GetTime();
    fprintf(stderr, "rbterm: recording %d pane%s → %s%s\n",
            count, count == 1 ? "" : "s",
            g_rec.paths[0], count > 1 ? " (+ siblings)" : "");
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

/* Close every captured cast and pop the post-record save modal so
   the user can pick a destination path + format. Multi-pane
   recordings stage all source paths so the save action can emit a
   matching set of outputs. */
static void rec_stop(void) {
    if (!g_rec.active) return;
    double dur = GetTime() - g_rec.start_time;
    for (int i = 0; i < g_rec.count; i++) {
        if (g_rec.fps[i]) { fclose(g_rec.fps[i]); g_rec.fps[i] = NULL; }
    }
    fprintf(stderr, "rbterm: recording stopped (%.1fs, %d pane%s)\n",
            dur, g_rec.count, g_rec.count == 1 ? "" : "s");
    memset(&g_rec_save, 0, sizeof(g_rec_save));
    g_rec_save.src_count = g_rec.count;
    for (int i = 0; i < g_rec.count; i++) {
        strncpy(g_rec_save.src_paths[i], g_rec.paths[i],
                sizeof(g_rec_save.src_paths[i]) - 1);
    }
    /* src_path is the legacy single-source convenience alias used
       by preview / non-multi-aware code paths. Always points at
       slot 0 so existing call sites keep working. */
    if (g_rec.count > 0) {
        strncpy(g_rec_save.src_path, g_rec.paths[0],
                sizeof(g_rec_save.src_path) - 1);
    }
    g_rec_save.duration_s = dur;
    /* MP4 is the default — most users want a shareable video file,
       and the .cast format is mostly useful for re-rendering or
       debugging. The dst_path's extension gets rewritten below
       (after src_path is copied in) so the file lands as .mp4. */
    g_rec_save.fmt = REC_FMT_MP4;
    /* Seed the save modal's effects from whatever the recorded pane
       was rendering with — typically the user picked a preset
       (Nostromo, VHS, etc.) before hitting Rec, so the saved file
       should match the look they were seeing live. Falls back to
       the global default for multi-pane recordings (where there's
       no single source pane to copy from). */
    if (g_rec.count == 1 && g_rec.panes[0]) {
        g_rec_save.effects = g_rec.panes[0]->effects;
    } else {
        g_rec_save.effects = g_app_settings.effects;
    }
    /* Default destination = the first cast's path with the
       extension rewritten to match the default format (mp4). The
       save action strips the -pN suffix when it loops over panes. */
    strncpy(g_rec_save.dst_path, g_rec_save.src_path,
            sizeof(g_rec_save.dst_path) - 1);
    rec_replace_ext(g_rec_save.dst_path, sizeof(g_rec_save.dst_path),
                    rec_fmt_ext(g_rec_save.fmt));
    g_ui_mode = UI_REC_SAVE;
    g_rec.active = false;
    g_rec.count = 0;
    for (int i = 0; i < (int)(sizeof(g_rec.panes) / sizeof(g_rec.panes[0])); i++) {
        g_rec.panes[i] = NULL;
    }
}

/* Append a chunk of PTY-output bytes as one asciinema event row in
   the matching pane's slot. No-op when no recording is active or
   `p` isn't one of the captured panes. */
static void rec_write(Pane *p, const uint8_t *buf, size_t n) {
    if (!g_rec.active || n == 0) return;
    double t = GetTime() - g_rec.start_time;
    for (int i = 0; i < g_rec.count; i++) {
        if (g_rec.panes[i] != p) continue;
        FILE *fp = g_rec.fps[i];
        if (!fp) return;
        fprintf(fp, "[%.6f, \"o\", \"", t);
        rec_json_escape(fp, buf, n);
        fputs("\"]\n", fp);
        fflush(fp);
        return;
    }
}

/* Mirror user-input bytes into the cast as `[t, "i", "..."]` events
   so the save renderer can show them as caption-style overlays.
   Bypassed silently when no recording is active or `p` isn't one
   of the captured panes. Same flush-on-write discipline as
   rec_write so a forced quit doesn't lose the most recent input. */
static void rec_input(Pane *p, const uint8_t *buf, size_t n) {
    if (!g_rec.active || n == 0) return;
    double t = GetTime() - g_rec.start_time;
    for (int i = 0; i < g_rec.count; i++) {
        if (g_rec.panes[i] != p) continue;
        FILE *fp = g_rec.fps[i];
        if (!fp) return;
        fprintf(fp, "[%.6f, \"i\", \"", t);
        rec_json_escape(fp, buf, n);
        fputs("\"]\n", fp);
        fflush(fp);
        return;
    }
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
        if (cur_t && cur_t->active && cur_t->active->pane &&
            cur_t->active->pane->cwd[0]) {
            inherit_cwd = cur_t->active->pane->cwd;
        }
    }
    Tab *t = calloc(1, sizeof(Tab));
    PaneNode *leaf = pane_node_new_leaf();
    if (!pane_open_local(leaf->pane, cols, rows, inherit_cwd)) {
        pane_node_free_recursive(leaf);
        free(t);
        return NULL;
    }
    t->root = leaf;
    t->active = leaf;
    g_tabs[g_num_tabs] = t;
    g_active = g_num_tabs;
    g_num_tabs++;
    tab_log_open_all(t);
    return t;
}

static void pane_apply_tab_appearance(const Tab *t, Pane *p);

/* Forward decl — tab_split lives below tab_open_ssh but the layout
   replay walker needs it. */
static bool tab_split(Tab *t, SplitMode mode, int cols, int rows,
                      char *err, size_t errsz);

/* Send `cd "<cwd>"; <cmd>\r` to a pane's PTY. Either argument can
   be empty / NULL; an empty pair is a no-op. Used by both the
   legacy single-pane init path and the per-leaf layout replay. */
static void ssh_send_init_line(Pane *p, const char *cwd, const char *cmd) {
    if (!p || !p->pty) return;
    bool have_cwd = (cwd && cwd[0]);
    bool have_cmd = (cmd && cmd[0]);
    if (!have_cwd && !have_cmd) return;
    char buf[1024];
    int n = 0;
    if (have_cwd) {
        n = snprintf(buf, sizeof(buf), "cd \"%s\"", cwd);
        if (have_cmd && n > 0 && n < (int)sizeof(buf)) {
            int m = snprintf(buf + n, sizeof(buf) - n, "; %s", cmd);
            if (m > 0) n += m;
        }
    } else {
        n = snprintf(buf, sizeof(buf), "%s", cmd);
    }
    if (n > 0 && n < (int)sizeof(buf) - 1) {
        buf[n++] = '\r';
        pty_write(p->pty, (const uint8_t *)buf, (size_t)n);
    }
}

/* Walk the parsed layout tree, performing splits in DFS pre-order.
   `current_leaf` is the actual tree leaf that maps to `node`; on a
   split, the original leaf becomes child[0] and the new leaf
   (returned by tab_split) becomes child[1]. After reaching a layout
   leaf, pane-N's cwd/cmd are sent. */
static bool layout_replay_walk(Tab *t, const LayoutNode *node,
                               PaneNode *current_leaf,
                               char cwds[][256], char cmds[][256],
                               int cols, int rows) {
    if (!t || !node || !current_leaf) return false;
    if (node->is_leaf) {
        ssh_send_init_line(current_leaf->pane,
                           cwds[node->leaf_idx],
                           cmds[node->leaf_idx]);
        return true;
    }
    /* Split the existing leaf. tab_split focuses the new leaf and
       does the SSH re-dial; we restore active afterwards. */
    PaneNode *prev_active = t->active;
    t->active = current_leaf;
    char err[256] = {0};
    if (!tab_split(t, node->split, cols, rows, err, sizeof(err))) {
        if (err[0]) fprintf(stderr,
                            "rbterm: layout replay split failed: %s\n", err);
        t->active = prev_active;
        return false;
    }
    /* tab_split sets the parent's ratio to 0.5; honour the layout's
       requested ratio. */
    if (current_leaf->parent) current_leaf->parent->ratio = node->ratio;
    /* The new internal node's children: child[0] is the original
       leaf, child[1] is the freshly-split one. */
    PaneNode *new_leaf = current_leaf->parent->child[1];
    bool ok = layout_replay_walk(t, node->child[0], current_leaf,
                                 cwds, cmds, cols, rows);
    if (ok) ok = layout_replay_walk(t, node->child[1], new_leaf,
                                    cwds, cmds, cols, rows);
    t->active = prev_active;
    return ok;
}

/* Build a Tab around an SSH connection. When `pre_pty` is non-NULL
   the network handshake is assumed to have already happened on a
   worker thread — we just attach the Pty and finish setup. When
   pre_pty is NULL the call blocks on pty_open_ssh inline. */
static Tab *tab_open_ssh_ex(const char *user, const char *host, int port,
                            const char *password, const char *keyfile,
                            const char *theme, int cursor_style,
                            const char *font, int font_size,
                            const char *log_dir, int log_mode,
                            const char *color, const char *cursor_color,
                            const HudConfig *hud,
                            const RecEffects *effects,
                            const char *init_cwd, const char *init_cmd,
                            const char *layout,
                            const char (*pane_cwds)[256],
                            const char (*pane_cmds)[256],
                            int cols, int rows,
                            Pty *pre_pty,
                            char *err, size_t errsz) {
    if (g_num_tabs >= MAX_TABS) return NULL;
    Tab *t = calloc(1, sizeof(Tab));
    PaneNode *leaf = pane_node_new_leaf();
    t->root = leaf;
    t->active = leaf;
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
    if (color)    { strncpy(t->ssh_color, color, sizeof(t->ssh_color) - 1); t->ssh_color[sizeof(t->ssh_color) - 1] = 0; }
    if (cursor_color) {
        strncpy(t->ssh_cursor_color, cursor_color, sizeof(t->ssh_cursor_color) - 1);
        t->ssh_cursor_color[sizeof(t->ssh_cursor_color) - 1] = 0;
    }
    if (hud) t->ssh_hud = *hud;
    if (effects) {
        t->ssh_effects_override = true;
        t->ssh_effects          = *effects;
    }
    if (init_cwd) {
        strncpy(t->ssh_init_cwd, init_cwd, sizeof(t->ssh_init_cwd) - 1);
        t->ssh_init_cwd[sizeof(t->ssh_init_cwd) - 1] = 0;
    }
    if (init_cmd) {
        strncpy(t->ssh_init_cmd, init_cmd, sizeof(t->ssh_init_cmd) - 1);
        t->ssh_init_cmd[sizeof(t->ssh_init_cmd) - 1] = 0;
    }
    t->ssh_port = port;
    Pane *first_pane = leaf->pane;
    snprintf(first_pane->title, sizeof(first_pane->title), "%s", t->ssh_target);
    if (pre_pty) {
        pane_attach_pty(first_pane, pre_pty, cols, rows);
    } else if (!pane_open_ssh(first_pane, user, host, port, password, keyfile,
                              cols, rows, err, errsz)) {
        pane_node_free_recursive(t->root);
        free(t); return NULL;
    }
    pane_apply_tab_appearance(t, first_pane);
    /* Stash layout fields on the Tab — useful for diagnostics, and
       so a future reconnect path could re-apply without rebuilding
       the Tab struct. */
    if (layout) {
        strncpy(t->ssh_layout, layout, sizeof(t->ssh_layout) - 1);
        t->ssh_layout[sizeof(t->ssh_layout) - 1] = 0;
    }
    if (pane_cwds) {
        for (int i = 0; i < SSH_LAYOUT_MAX_PANES; i++) {
            strncpy(t->ssh_pane_cwds[i], pane_cwds[i],
                    sizeof(t->ssh_pane_cwds[i]) - 1);
            t->ssh_pane_cwds[i][sizeof(t->ssh_pane_cwds[i]) - 1] = 0;
        }
    }
    if (pane_cmds) {
        for (int i = 0; i < SSH_LAYOUT_MAX_PANES; i++) {
            strncpy(t->ssh_pane_cmds[i], pane_cmds[i],
                    sizeof(t->ssh_pane_cmds[i]) - 1);
            t->ssh_pane_cmds[i][sizeof(t->ssh_pane_cmds[i]) - 1] = 0;
        }
    }
    /* Layout replay vs legacy init send. If the host has a
       predefined layout AND it parses cleanly, the layout owns the
       per-pane init: pane-0's cwd/cmd is sent to the first pane,
       and tab_split is called for each non-leaf node to spawn the
       remaining panes. Otherwise fall back to the single-pane
       init_cwd/init_cmd path. */
    LayoutNode *layout_root = NULL;
    if (layout && layout[0]) layout_root = layout_parse(layout);
    bool seen[SSH_LAYOUT_MAX_PANES] = {0};
    int leaf_count = layout_root ? layout_count_leaves(layout_root, seen) : 0;
    if (layout_root && leaf_count > 0) {
        layout_replay_walk(t, layout_root, leaf,
                           t->ssh_pane_cwds, t->ssh_pane_cmds, cols, rows);
        layout_node_free(layout_root);
        /* Restore focus to the first leaf. */
        t->active = pane_tree_first_leaf(t->root);
    } else {
        if (layout_root) layout_node_free(layout_root);
        ssh_send_init_line(first_pane, t->ssh_init_cwd, t->ssh_init_cmd);
    }
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

/* Build a placeholder Tab for an in-flight SSH connect — visible
   in the tab bar immediately, so the user gets feedback that the
   handshake is in progress. The Pane has a Screen but no PTY; the
   pane area shows a "Connecting to <alias>..." banner via
   screen_feed. The drain / cwd-poll / hud-poll loops skip panes
   with a NULL pty so this is safe. The matching `tab_attach_ssh_*`
   helper attaches the worker's Pty when the handshake completes. */
static Tab *tab_open_ssh_placeholder(const SshProfile *prof,
                                     const char *alias, bool is_active,
                                     int cols, int rows) {
    if (g_num_tabs >= MAX_TABS) return NULL;
    Tab *t = calloc(1, sizeof(Tab));
    PaneNode *leaf = pane_node_new_leaf();
    t->root = leaf;
    t->active = leaf;
    t->is_ssh = true;
    /* Stash connection metadata so split + reconnect paths can use
       it later (and so the SSH-form auto-select round-trips). */
    if (alias) {
        strncpy(t->ssh_alias, alias, sizeof(t->ssh_alias) - 1);
        t->ssh_alias[sizeof(t->ssh_alias) - 1] = 0;
    }
    if (prof) {
        const char *user = prof->user[0] ? prof->user : NULL;
        const char *host = prof->hostname[0] ? prof->hostname : alias;
        int         port = prof->port > 0 ? prof->port : 22;
        if (user) {
            if (port == 22) snprintf(t->ssh_target, sizeof(t->ssh_target), "%s@%s", user, host);
            else            snprintf(t->ssh_target, sizeof(t->ssh_target), "%s@%s:%d", user, host, port);
        } else {
            if (port == 22) snprintf(t->ssh_target, sizeof(t->ssh_target), "%s", host);
            else            snprintf(t->ssh_target, sizeof(t->ssh_target), "%s:%d", host, port);
        }
        if (host) { strncpy(t->ssh_host, host, sizeof(t->ssh_host) - 1); t->ssh_host[sizeof(t->ssh_host) - 1] = 0; }
        if (user) { strncpy(t->ssh_user, user, sizeof(t->ssh_user) - 1); t->ssh_user[sizeof(t->ssh_user) - 1] = 0; }
        if (prof->identity[0]) { strncpy(t->ssh_key, prof->identity, sizeof(t->ssh_key) - 1); t->ssh_key[sizeof(t->ssh_key) - 1] = 0; }
        t->ssh_port = port;
        if (prof->display_name[0]) {
            strncpy(t->tab_name, prof->display_name, sizeof(t->tab_name) - 1);
            t->tab_name[sizeof(t->tab_name) - 1] = 0;
        }
    } else {
        snprintf(t->ssh_target, sizeof(t->ssh_target), "%s", alias ? alias : "ssh");
        if (alias) {
            strncpy(t->ssh_host, alias, sizeof(t->ssh_host) - 1);
            t->ssh_host[sizeof(t->ssh_host) - 1] = 0;
        }
        t->ssh_port = 22;
    }
    Pane *p = leaf->pane;
    pane_init_click_state(p);
    p->scr = screen_new(cols, rows, SCROLLBACK_LINES, pane_io(p));
    p->effects = g_app_settings.effects;
    /* Banner inside the pane area. ANSI: clear + home, bold cyan
       caption, dim line below it. The shell's first redraw blows
       this away as soon as the PTY is attached. */
    char banner[512];
    int n = snprintf(banner, sizeof(banner),
                     "\x1b[2J\x1b[H\x1b[1;36mConnecting to %s...\x1b[0m\r\n"
                     "\x1b[2;37m(libssh handshake in progress)\x1b[0m\r\n",
                     t->ssh_target);
    if (n > 0 && n < (int)sizeof(banner)) {
        screen_feed(p->scr, (const uint8_t *)banner, (size_t)n);
    }
    snprintf(p->title, sizeof(p->title), "Connecting to %s",
             t->ssh_target);
    p->title_dirty = true;
    g_tabs[g_num_tabs] = t;
    if (is_active) g_active = g_num_tabs;
    g_num_tabs++;
    tab_log_open_all(t);
    return t;
}

/* Attach a worker-produced Pty to a placeholder Tab and finish all
   the post-connect setup tab_open_ssh_ex would normally do
   inline (appearance, layout/init, font). Mirrors the second half
   of tab_open_ssh_ex but on an existing Tab. */
static void tab_attach_ssh_finalize(Tab *t, Pty *pty, const SshProfile *prof,
                                    int cols, int rows) {
    if (!t || !pty || !t->active || !t->active->pane) return;
    Pane *p = t->active->pane;
    /* Clear the placeholder banner so the shell's first paint
       lands on a fresh grid. */
    screen_feed(p->scr, (const uint8_t *)"\x1b[2J\x1b[H", 6);
    p->pty = pty;
    /* Refresh title to the SSH target (the shell will rewrite via
       OSC 0/2 once it draws). */
    snprintf(p->title, sizeof(p->title), "%s", t->ssh_target);
    p->title_dirty = true;
    /* Lift profile fields onto the Tab the same way tab_open_ssh_ex
       would have. */
    if (prof) {
        if (prof->theme[0])   { strncpy(t->ssh_theme, prof->theme, sizeof(t->ssh_theme) - 1); t->ssh_theme[sizeof(t->ssh_theme) - 1] = 0; }
        t->ssh_cursor_style = prof->cursor_style;
        if (prof->font[0])    { strncpy(t->ssh_font, prof->font, sizeof(t->ssh_font) - 1); t->ssh_font[sizeof(t->ssh_font) - 1] = 0; }
        t->ssh_font_size = prof->font_size;
        if (prof->log_dir[0]) { strncpy(t->ssh_log_dir, prof->log_dir, sizeof(t->ssh_log_dir) - 1); t->ssh_log_dir[sizeof(t->ssh_log_dir) - 1] = 0; }
        t->ssh_log_mode = prof->log_mode;
        if (prof->color[0])   { strncpy(t->ssh_color, prof->color, sizeof(t->ssh_color) - 1); t->ssh_color[sizeof(t->ssh_color) - 1] = 0; }
        if (prof->cursor_color[0]) {
            strncpy(t->ssh_cursor_color, prof->cursor_color, sizeof(t->ssh_cursor_color) - 1);
            t->ssh_cursor_color[sizeof(t->ssh_cursor_color) - 1] = 0;
        }
        if (prof->hud.override) t->ssh_hud = prof->hud;
        if (prof->effects_override) {
            t->ssh_effects_override = true;
            t->ssh_effects = prof->effects;
        }
        if (prof->init_cwd[0]) { strncpy(t->ssh_init_cwd, prof->init_cwd, sizeof(t->ssh_init_cwd) - 1); t->ssh_init_cwd[sizeof(t->ssh_init_cwd) - 1] = 0; }
        if (prof->init_cmd[0]) { strncpy(t->ssh_init_cmd, prof->init_cmd, sizeof(t->ssh_init_cmd) - 1); t->ssh_init_cmd[sizeof(t->ssh_init_cmd) - 1] = 0; }
        if (prof->layout[0])   { strncpy(t->ssh_layout, prof->layout, sizeof(t->ssh_layout) - 1); t->ssh_layout[sizeof(t->ssh_layout) - 1] = 0; }
        for (int i = 0; i < SSH_LAYOUT_MAX_PANES; i++) {
            strncpy(t->ssh_pane_cwds[i], prof->pane_cwds[i], sizeof(t->ssh_pane_cwds[i]) - 1);
            t->ssh_pane_cwds[i][sizeof(t->ssh_pane_cwds[i]) - 1] = 0;
            strncpy(t->ssh_pane_cmds[i], prof->pane_cmds[i], sizeof(t->ssh_pane_cmds[i]) - 1);
            t->ssh_pane_cmds[i][sizeof(t->ssh_pane_cmds[i]) - 1] = 0;
        }
    }
    pane_apply_tab_appearance(t, p);
    /* Layout replay (if any) or legacy single-pane init send. */
    LayoutNode *layout_root = NULL;
    if (t->ssh_layout[0]) layout_root = layout_parse(t->ssh_layout);
    bool seen[SSH_LAYOUT_MAX_PANES] = {0};
    int leaf_count = layout_root ? layout_count_leaves(layout_root, seen) : 0;
    if (layout_root && leaf_count > 0) {
        layout_replay_walk(t, layout_root, t->root,
                           t->ssh_pane_cwds, t->ssh_pane_cmds, cols, rows);
        layout_node_free(layout_root);
        t->active = pane_tree_first_leaf(t->root);
    } else {
        if (layout_root) layout_node_free(layout_root);
        ssh_send_init_line(p, t->ssh_init_cwd, t->ssh_init_cmd);
    }
    /* Per-host font / size — apply globally. */
    if (g_renderer) {
        bool resized = false;
        if (t->ssh_font[0]) {
            const EmbeddedFont *ef = embedded_font_lookup(t->ssh_font);
            bool ok = ef
                ? renderer_set_font_data(g_renderer, ef->data,
                                         (int)ef->data_size,
                                         ef->ext, t->ssh_font)
                : renderer_set_font_path(g_renderer, t->ssh_font);
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
}

/* Wrapper for the legacy synchronous-connect call sites. Forwards
   to tab_open_ssh_ex with pre_pty=NULL so pty_open_ssh is invoked
   inline (blocking). */
static Tab *tab_open_ssh(const char *user, const char *host, int port,
                         const char *password, const char *keyfile,
                         const char *theme, int cursor_style,
                         const char *font, int font_size,
                         const char *log_dir, int log_mode,
                         const char *color, const char *cursor_color,
                         const HudConfig *hud,
                         const RecEffects *effects,
                         const char *init_cwd, const char *init_cmd,
                         const char *layout,
                         const char (*pane_cwds)[256],
                         const char (*pane_cmds)[256],
                         int cols, int rows,
                         char *err, size_t errsz) {
    return tab_open_ssh_ex(user, host, port, password, keyfile,
                           theme, cursor_style, font, font_size,
                           log_dir, log_mode, color, cursor_color,
                           hud, effects, init_cwd, init_cmd,
                           layout, pane_cwds, pane_cmds,
                           cols, rows, NULL, err, errsz);
}

/* Close tab at index `idx`: free all panes, scrub stashed SSH
   creds, free the Tab struct, then shift remaining tabs down to
   keep the array dense. Clamps g_active. */
static void tab_close(int idx) {
    if (idx < 0 || idx >= g_num_tabs) return;
    Tab *t = g_tabs[idx];
#ifdef RBTERM_SSH
    /* If a worker still considers this tab its placeholder, null
       the back-reference so integration can detect the abandon
       and release the resulting Pty without dereferencing a freed
       Tab. */
    for (int wi = 0; wi < g_launch_workers_count; wi++) {
        if (g_launch_workers[wi].placeholder == t) {
            g_launch_workers[wi].placeholder = NULL;
        }
    }
#endif
    pane_node_free_recursive(t->root);
    t->root = NULL;
    t->active = NULL;
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
   tab itself is NULL. Self-corrects when t->active is stale (e.g.
   was the last leaf to close). */
static inline Pane *active_pane_of(Tab *t) {
    if (!t) return NULL;
    if (!t->active || t->active->split != SPLIT_NONE) {
        t->active = pane_tree_first_leaf(t->root);
    }
    return t->active ? t->active->pane : NULL;
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
    if (t->ssh_cursor_color[0]) {
        Color cc;
        if (parse_hex_color(t->ssh_cursor_color, &cc)) {
            uint32_t rgb = ((uint32_t)cc.r << 16) | ((uint32_t)cc.g << 8) | cc.b;
            screen_set_cursor_color(p->scr, rgb);
        }
    }
    /* Per-host effects override — applied once on pane creation;
       splits inherit because tab_split also calls this helper.
       Without an override the pane keeps whatever it picked up
       from g_app_settings.effects in pane_open_*. */
    if (t->ssh_effects_override) {
        p->effects = t->ssh_effects;
    }
}

/* Split the currently-active leaf of `t`. If the tab was opened via
   SSH, re-dial the same host into the new pane; otherwise open a
   local shell. cols/rows are the current full-window cell dims; the
   pane's real size is fitted by the main loop's tabs_resize_all
   call. Returns true on success. */
static bool tab_split(Tab *t, SplitMode mode, int cols, int rows,
                      char *err, size_t errsz) {
    if (!t || !t->active) return false;
    if (mode != SPLIT_VERTICAL && mode != SPLIT_HORIZONTAL) return false;
    /* Splitting changes the tree shape — drop maximize so the
       new sibling becomes visible too. */
    t->maximized_leaf = NULL;
    PaneNode *target = t->active;
    PaneNode *new_leaf = pane_node_split_leaf(t, target, mode);
    if (!new_leaf) return false;
    Pane *np = new_leaf->pane;
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
        /* New split inherits cwd from the leaf it was split off. */
        const char *split_cwd = target->pane->cwd[0] ? target->pane->cwd : NULL;
        ok = pane_open_local(np, cols, rows, split_cwd);
    }
    if (!ok) {
        /* Roll back: collapse the brand-new internal node, restoring
           the original leaf in its place. */
        pane_node_close_leaf(t, new_leaf);
        return false;
    }
    pane_apply_tab_appearance(t, np);
    t->active = new_leaf;
    /* Use the leaf's tree-order index for a stable per-pane log filename. */
    int idx = 0;
    for (PaneNode *l = pane_tree_first_leaf(t->root); l && l != new_leaf;
         l = pane_tree_next_leaf(l)) idx++;
    pane_log_open(t, np, idx);
    return true;
}

/* Toggle maximize on a tab's active leaf. Maximizing snaps the
   leaf to the full content area (the live grid resizes via the
   main loop's tabs_resize_all on the next frame); unmaximizing
   restores the recursive split layout. Stays a no-op for
   single-pane tabs since there's nothing to maximize.

   Clears every leaf's selection on toggle — selection state is
   in cell coords, and the imminent resize either moves those
   cells under different content (maximize) or hides them
   entirely (restore). Without this clear, a leftover selection
   from before the toggle paints a blue rectangle over the new
   layout where the old cells used to live. */
static void tab_toggle_maximize(Tab *t) {
    if (!t || !t->root) return;
    if (pane_tree_count(t->root) < 2) return;
    if (t->maximized_leaf) {
        t->maximized_leaf = NULL;
    } else {
        t->maximized_leaf = t->active;
    }
    for (PaneNode *_l = pane_tree_first_leaf(t->root); _l;
         _l = pane_tree_next_leaf(_l)) {
        Pane *_p = _l->pane;
        if (!_p) continue;
        _p->sel.active   = false;
        _p->sel.dragging = false;
    }
}

/* Close one leaf of a tab. If it's the only leaf, mark the tab dead
   (the caller collects via the tab_close sweep). */
static void tab_close_leaf(Tab *t, PaneNode *leaf) {
    if (!t || !leaf || leaf->split != SPLIT_NONE) return;
    /* Closing any leaf collapses the tree shape — clearest fix
       is to drop maximize state so the user lands back on the
       full layout. */
    t->maximized_leaf = NULL;
    bool was_active = (leaf == t->active);
    /* Pick a successor leaf BEFORE we mutate the tree. */
    PaneNode *succ = NULL;
    if (was_active) {
        succ = pane_tree_next_leaf(leaf);
        if (!succ) succ = pane_tree_prev_leaf(leaf);
    }
    if (pane_node_close_leaf(t, leaf)) {
        /* Was the only leaf in the tab. */
        t->dead = true;
        return;
    }
    if (was_active) {
        t->active = succ ? succ : pane_tree_first_leaf(t->root);
    }
    /* Force a retitle so the tab label / window title pick up the
       new active leaf. */
    Pane *ap = active_pane_of(t);
    if (ap) ap->title_dirty = true;
}

/* Close the active pane. If the tab only has one pane, defer to
   tab_close for the whole tab. */
static void pane_close_active(int tab_idx) {
    if (tab_idx < 0 || tab_idx >= g_num_tabs) return;
    Tab *t = g_tabs[tab_idx];
    if (pane_tree_count(t->root) < 2) { tab_close(tab_idx); return; }
    tab_close_leaf(t, t->active);
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
    p->search.pending_recompute_at = 0.0;
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
    p->search.pending_recompute_at = 0.0;
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
        /* Defer the actual scrollback walk by 350 ms of typing
           inactivity. With a million-line history a recompute can
           take tens of ms; running it on every keystroke makes the
           query field feel laggy. The fire-time gets pushed
           forward each time the query changes, so a fast typer
           never triggers an interim recompute. */
        S->pending_recompute_at = GetTime() + 0.350;
    }
    /* Fire the deferred recompute when the user has paused. */
    if (S->pending_recompute_at > 0.0 &&
        GetTime() >= S->pending_recompute_at) {
        S->pending_recompute_at = 0.0;
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
/* Slot in per-tab static buffer space so multiple tab labels can
   coexist within one frame. Returns the buffer for `t`'s slot. */
static char *tab_label_buf(const Tab *t) {
    static char buf[MAX_TABS][320];
    static int  slot = 0;
    int idx = -1;
    for (int i = 0; i < g_num_tabs; i++) if (g_tabs[i] == t) { idx = i; break; }
    if (idx < 0) idx = slot++ % MAX_TABS;
    return buf[idx];
}

/* The "auto" label: what the tab would show with no manual name —
   shell title, cwd basename, or a fallback. tab_label() prefixes
   the manual name (when set) onto whatever this returns. */
static const char *tab_label_auto(const Tab *t) {
    if (!t || !t->active || !t->active->pane) return "rbterm";
    const Pane *p = t->active->pane;
    if (t->is_ssh) {
        char *out = tab_label_buf(t);
        if (p->title[0]) {
            snprintf(out, 320, "%s %s", t->ssh_target, p->title);
            return out;
        }
        if (!p->cwd[0]) {
            return t->ssh_target;
        }
        const char *dir_label = p->cwd;
        const char *b = strrchr(p->cwd, '/');
        if (strcmp(p->cwd, "/") == 0) dir_label = "/";
        else if (b) dir_label = (*(b + 1) ? b + 1 : b);
        snprintf(out, 320, "%s %s", t->ssh_target, dir_label);
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
    /* Cold-start fallback: nothing known yet (no OSC 0/2 title, cwd
       not yet detected). Use the brand so the macOS title bar +
       Cmd-Tab label read as "rbterm" instead of a generic "shell". */
    return "rbterm";
}

/* The "short" suffix shown after a manual tab_name. Just whatever
   the shell currently advertises (OSC 0/2 title, or cwd basename),
   without the SSH-target prefix — the manual name is the identity
   now, so doubling up on host info would just be clutter. */
static const char *tab_label_suffix(const Tab *t) {
    if (!t || !t->active || !t->active->pane) return "";
    const Pane *p = t->active->pane;
    if (p->title[0]) return p->title;
    if (p->cwd[0]) {
        const char *home = getenv("HOME");
        if (home && *home) {
            size_t hn = strlen(home);
            if (strncmp(p->cwd, home, hn) == 0 &&
                (p->cwd[hn] == 0 || p->cwd[hn] == '/' || p->cwd[hn] == '\\'))
                return p->cwd[hn] == 0 ? "~" : (p->cwd + hn);
        }
        if (strcmp(p->cwd, "/") == 0) return "/";
        const char *b = strrchr(p->cwd, '/');
        if (b && *(b + 1)) return b + 1;
        return p->cwd;
    }
    return "";
}

/* Public label: the user's manual tab name (if any) followed by
   the auto-derived label as a suffix. The interpunct keeps the two
   visually distinct and matches what other terminals do. */
static const char *tab_label(const Tab *t) {
    if (!t) return "rbterm";
    if (!t->tab_name[0]) return tab_label_auto(t);
    const char *suf = tab_label_suffix(t);
    if (!suf || !suf[0]) return t->tab_name;
    char *out = tab_label_buf(t);
    snprintf(out, 320, "%s · %s", t->tab_name, suf);
    return out;
}

/* ---------- Pane layout ---------- */

/* PaneRect / SPLITTER_PX / SPLITTER_GRAB and the tree-walking helpers
   live up at the top of the file (next to the PaneNode definition). */

/* On-screen rect of a leaf within a tab's tree, given the current
   window size. Returns false if the leaf isn't in the tab.
   When the tab has a maximized leaf, the maximized one occupies
   the full content area and any other leaf returns false (they're
   not visible). */
static bool leaf_rect(const Tab *t, const PaneNode *leaf, int win_w, int win_h,
                      PaneRect *out) {
    if (!t || !leaf) return false;
    PaneRect outer = pane_tree_terminal_outer(win_w, win_h);
    if (t->maximized_leaf) {
        if (leaf != t->maximized_leaf) return false;
        if (out) *out = outer;
        return true;
    }
    return pane_tree_node_rect_walk(t->root, leaf, outer, out);
}

/* Pick the best leaf to focus when the user presses a directional
   pane-switch chord (Cmd+Opt+arrows). Direction is a (dx, dy) unit
   vector — exactly one component non-zero.

   Two-pass scoring matches tmux / iTerm2 convention:
     1. Prefer panes that *overlap* on the perpendicular axis with
        the active pane (i.e. share rows for ←/→, share columns for
        ↑/↓). Pick the closest such pane along the primary axis.
        This is what the user means by "the pane directly below me".
     2. If nothing overlaps, fall back to the candidate with the
        smallest Manhattan-ish score. Weighting perpendicular
        distance 2× lets a pane in the same general band beat one
        that's slightly closer along the primary axis but in a
        different region.

   Returns NULL if no candidate is in the requested direction. */
static PaneNode *pane_focus_directional(Tab *t, int win_w, int win_h,
                                        int dx, int dy) {
    if (!t || !t->active) return NULL;
    PaneRect ar;
    if (!leaf_rect(t, t->active, win_w, win_h, &ar)) return NULL;
    int acx = ar.x + ar.w / 2;
    int acy = ar.y + ar.h / 2;
    PaneNode *best_overlap = NULL, *best_any = NULL;
    int best_overlap_primary = (1 << 30);
    int best_any_score        = (1 << 30);
    for (PaneNode *leaf = pane_tree_first_leaf(t->root); leaf;
         leaf = pane_tree_next_leaf(leaf)) {
        if (leaf == t->active) continue;
        PaneRect br;
        if (!leaf_rect(t, leaf, win_w, win_h, &br)) continue;
        int bcx = br.x + br.w / 2;
        int bcy = br.y + br.h / 2;
        if (dx < 0 && bcx >= acx) continue;
        if (dx > 0 && bcx <= acx) continue;
        if (dy < 0 && bcy >= acy) continue;
        if (dy > 0 && bcy <= acy) continue;
        int primary, perp;
        bool overlap;
        if (dx != 0) {
            primary = (dx > 0 ? (bcx - acx) : (acx - bcx));
            perp    = (bcy > acy) ? (bcy - acy) : (acy - bcy);
            /* Vertical overlap: any shared rows with active? */
            overlap = !(br.y + br.h <= ar.y || ar.y + ar.h <= br.y);
        } else {
            primary = (dy > 0 ? (bcy - acy) : (acy - bcy));
            perp    = (bcx > acx) ? (bcx - acx) : (acx - bcx);
            /* Horizontal overlap: any shared columns? */
            overlap = !(br.x + br.w <= ar.x || ar.x + ar.w <= br.x);
        }
        if (overlap && primary < best_overlap_primary) {
            best_overlap_primary = primary;
            best_overlap = leaf;
        }
        int score = primary + perp * 2;
        if (score < best_any_score) {
            best_any_score = score;
            best_any = leaf;
        }
    }
    return best_overlap ? best_overlap : best_any;
}

/* On-screen rect of an internal split node's splitter strip. */
static bool internal_splitter_rect(const Tab *t, const PaneNode *node,
                                   int win_w, int win_h, PaneRect *out) {
    if (!t || !node || node->split == SPLIT_NONE) return false;
    PaneRect outer;
    if (!pane_tree_node_rect_walk(t->root, node,
                                  pane_tree_terminal_outer(win_w, win_h),
                                  &outer)) return false;
    PaneRect ra, rb;
    pane_tree_split_children(node, outer, &ra, &rb);
    if (node->split == SPLIT_VERTICAL) {
        out->x = ra.x + ra.w;
        out->y = ra.y;
        out->w = SPLITTER_PX;
        out->h = ra.h;
    } else {
        out->x = ra.x;
        out->y = ra.y + ra.h;
        out->w = ra.w;
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

/* Capture the active pane to a PNG and pop it open in the system
   image viewer. Returns true on success; false (and a stderr log
   line) on failure — typically a write-permission issue with the
   target directory. The pane is re-rendered to an off-screen
   RenderTexture so the screenshot is a clean grid (no tab bar, no
   modals, no HUD) regardless of what's currently overlaying it. */
static bool screenshot_active_pane(Renderer *r) {
    Tab *t = active_tab();
    if (!t || !r) return false;
    Pane *p = active_pane_of(t);
    if (!p || !p->scr) return false;
    int win_w = GetScreenWidth();
    int win_h = GetScreenHeight();
    PaneRect pr;
    if (!leaf_rect(t, t->active, win_w, win_h, &pr)) return false;
    if (pr.w <= 0 || pr.h <= 0) return false;
    /* RenderTexture sizes must be even on some drivers; round up. */
    int rw = pr.w, rh = pr.h;
    if (rw & 1) rw++;
    if (rh & 1) rh++;
    RenderTexture2D rt = LoadRenderTexture(rw, rh);
    if (rt.id == 0) {
        fprintf(stderr, "rbterm: screenshot: couldn't allocate %dx%d RT\n", rw, rh);
        return false;
    }
    uint32_t bg = screen_default_bg(p->scr);
    Color bgc = { (unsigned char)((bg >> 16) & 0xff),
                  (unsigned char)((bg >> 8)  & 0xff),
                  (unsigned char)( bg        & 0xff), 255 };
    BeginTextureMode(rt);
        ClearBackground(bgc);
        renderer_draw(r, p->scr, GetTime(), true, &p->sel,
                      0, 0, -1, -1);
    EndTextureMode();

    Image img = LoadImageFromTexture(rt.texture);
    ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    /* RT textures store bottom-up; flip vertically so the PNG isn't
       upside-down when opened in Preview. */
    ImageFlipVertical(&img);

    /* Resolve the destination directory and timestamp the filename. */
    char dir[PATH_MAX];
    expand_home_path(g_app_settings.screenshot_dir[0]
                       ? g_app_settings.screenshot_dir : "~/Desktop",
                     dir, sizeof(dir));
    mkdir_p(dir);
    char ts[32];
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    if (lt) strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S", lt);
    else    snprintf(ts, sizeof(ts), "%lld", (long long)now);
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/rbterm-%s.png", dir, ts);

    bool ok = ExportImage(img, path);
    UnloadImage(img);
    UnloadRenderTexture(rt);

    if (ok) {
        snprintf(g_snap_status, sizeof(g_snap_status),
                 "Screenshot → %s", path);
        g_snap_status_at = GetTime();
        fprintf(stderr, "rbterm: %s\n", g_snap_status);
        /* Open the PNG in the user's default image viewer for
           immediate "view" without leaving rbterm. */
        open_url(path);
    } else {
        snprintf(g_snap_status, sizeof(g_snap_status),
                 "Screenshot failed: couldn't write %s", path);
        g_snap_status_at = GetTime();
        fprintf(stderr, "rbterm: %s\n", g_snap_status);
    }
    return ok;
}

/* Find which leaf of the active tab a window-pixel coordinate falls
   into (or NULL if neither). Used for click-to-focus. */
static PaneNode *pane_at(Tab *t, int win_w, int win_h, int mx, int my) {
    if (!t) return NULL;
    return pane_tree_at(t->root, pane_tree_terminal_outer(win_w, win_h),
                        mx, my);
}

/* Walk every internal split node, calling cb for each. */
static void pane_tree_visit_splits(PaneNode *n, PaneRect outer,
                                   void (*cb)(PaneNode *, PaneRect, void *),
                                   void *user) {
    if (!n || n->split == SPLIT_NONE) return;
    PaneRect ra, rb;
    pane_tree_split_children(n, outer, &ra, &rb);
    cb(n, outer, user);
    pane_tree_visit_splits(n->child[0], ra, cb, user);
    pane_tree_visit_splits(n->child[1], rb, cb, user);
}

/* Draw every pane of one tab (plus the splitter bars between them). */
static void draw_tab_contents(Renderer *r, Tab *t, int win_w, int win_h,
                              double time_sec, bool focused) {
    if (!t) return;
    Vector2 mpos = GetMousePosition();
    bool want_url_cursor = false;
    for (PaneNode *leaf = pane_tree_first_leaf(t->root); leaf;
         leaf = pane_tree_next_leaf(leaf)) {
        Pane *p = leaf->pane;
        if (!p || !p->scr) continue;
        PaneRect pr;
        if (!leaf_rect(t, leaf, win_w, win_h, &pr)) continue;
        bool pane_focused = focused && (leaf == t->active);
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
        uint32_t bg = screen_default_bg(p->scr);
        Color bgc = { (unsigned char)((bg >> 16) & 0xff),
                      (unsigned char)((bg >> 8)  & 0xff),
                      (unsigned char)( bg        & 0xff), 255 };
        bool use_pane_fx = rec_effects_any_visual(&p->effects)
                           && rec_effects_shader_ready();
        if (use_pane_fx) {
            /* Lazy-allocate (or resize) the per-pane RTs.
                 fx_rt   — terminal grid renders here, then the shader
                           pass reads from it.
                 fx_prev — last frame's *output*, sampled as `u_prev`
                           when decay > 0 so trails are visible. We
                           ping-pong by swapping these two each frame:
                           this frame writes into fx_prev (it now
                           becomes the new output), next frame uses
                           the old output as its trail source. */
            if (p->fx_rt.id == 0 || p->fx_rt_w != pr.w || p->fx_rt_h != pr.h) {
                if (p->fx_rt.id)   UnloadRenderTexture(p->fx_rt);
                if (p->fx_prev.id) UnloadRenderTexture(p->fx_prev);
                p->fx_rt   = LoadRenderTexture(pr.w, pr.h);
                p->fx_prev = LoadRenderTexture(pr.w, pr.h);
                p->fx_rt_w = pr.w;
                p->fx_rt_h = pr.h;
                /* Clear both so the first decay sample doesn't pull
                   garbage into the trail. */
                if (p->fx_prev.id) {
                    BeginTextureMode(p->fx_prev);
                        ClearBackground(BLACK);
                    EndTextureMode();
                }
            }
            if (p->fx_rt.id == 0) {
                /* Allocation failed — fall back to direct draw. */
                use_pane_fx = false;
            }
        }
        if (use_pane_fx) {
            /* Render the terminal grid into fx_rt (the "raw" buffer). */
            BeginTextureMode(p->fx_rt);
                ClearBackground(bgc);
                renderer_draw(r, p->scr, time_sec, pane_focused, &p->sel,
                              0, 0, hcol, hrow);
            EndTextureMode();
            /* Then run the effects shader, sampling fx_rt as texture0
               and fx_prev (last frame's output) as texture1 / u_prev,
               writing to fx_prev. After the swap below, what we just
               wrote becomes the new "last frame" for the *next* tick. */
            rec_effects_set_uniforms_for(&p->effects, pr.w, pr.h, time_sec,
                                         &p->fx_prev.texture);
            if (rec_effects_begin_shader_mode()) {
                /* Render the shader output into a *fresh* RT so we
                   can also blit it to the screen. We use fx_prev as
                   the destination, then swap so it becomes
                   "current". (Both RTs are the same size; ping-
                   pong is safe.) */
                BeginTextureMode(p->fx_prev);
                    ClearBackground(BLACK);
                    DrawTextureRec(p->fx_rt.texture,
                                   (Rectangle){ 0.0f, 0.0f,
                                                (float)p->fx_rt.texture.width,
                                               -(float)p->fx_rt.texture.height },
                                   (Vector2){ 0.0f, 0.0f }, WHITE);
                EndTextureMode();
                rec_effects_end_shader_mode();
                /* Blit the just-produced output to the window at the
                   pane's on-screen position. fx_prev now holds what
                   the user sees this frame. */
                DrawTextureRec(p->fx_prev.texture,
                               (Rectangle){ 0.0f, 0.0f,
                                            (float)p->fx_prev.texture.width,
                                           -(float)p->fx_prev.texture.height },
                               (Vector2){ (float)pr.x, (float)pr.y }, WHITE);
                /* Swap so next frame's u_prev sample reads what we
                   just produced (and we write into the other slot). */
                RenderTexture2D tmp = p->fx_rt;
                p->fx_rt   = p->fx_prev;
                p->fx_prev = tmp;
            }
        } else {
            DrawRectangle(pr.x, pr.y, pr.w, pr.h, bgc);
            renderer_draw(r, p->scr, time_sec, pane_focused, &p->sel,
                          pr.x, pr.y, hcol, hrow);
            /* Free both cached RTs if effects were just turned off so
               we don't sit on per-pane VRAM after the user disables
               the look. */
            if (p->fx_rt.id || p->fx_prev.id) {
                if (p->fx_rt.id)   UnloadRenderTexture(p->fx_rt);
                if (p->fx_prev.id) UnloadRenderTexture(p->fx_prev);
                p->fx_rt.id = p->fx_prev.id = 0;
                p->fx_rt_w = p->fx_rt_h = 0;
            }
        }

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
    /* Broadcast-mode border. A 3px red outline on every leaf in the
       active tab when broadcast is on, so the user can never lose
       track of which panes are receiving fanned input. Drawn after
       pane content + search overlays so it sits clearly on top, but
       before the splitter strip so the border doesn't paint over
       the splitter line. */
    if (g_broadcast_active && t == active_tab() &&
        pane_tree_count(t->root) >= 2) {
        for (PaneNode *_l = pane_tree_first_leaf(t->root); _l;
             _l = pane_tree_next_leaf(_l)) {
            PaneRect pr;
            if (!leaf_rect(t, _l, win_w, win_h, &pr)) continue;
            Color bc = (Color){240, 100, 110, 230};
            DrawRectangleLinesEx(
                (Rectangle){(float)pr.x, (float)pr.y, (float)pr.w, (float)pr.h},
                3.0f, bc);
        }
    }
    /* Active-pane border. 2px cyan outline around the focused leaf
       so multi-pane tabs make it obvious which pane will receive
       the next keystroke. Only fires on multi-pane tabs (no point
       outlining a single-pane tab — there's nothing to disambiguate)
       and is suppressed while broadcast is on (the red border above
       already covers the focus signal). Same draw stage as the
       broadcast border so it sits over content but under splitters. */
    if (!g_broadcast_active && t == active_tab() && t->active &&
        pane_tree_count(t->root) >= 2) {
        PaneRect pr;
        if (leaf_rect(t, t->active, win_w, win_h, &pr)) {
            Color bc = focused ? (Color){125, 207, 255, 230}
                               : (Color){90, 130, 165, 180};
            DrawRectangleLinesEx(
                (Rectangle){(float)pr.x, (float)pr.y, (float)pr.w, (float)pr.h},
                2.0f, bc);
        }
    }
    /* Maximize-mode edge stubs. When a pane is maximized, walk
       up its ancestor chain and OR together the sides on which a
       hidden sibling lives — then paint a short colored bar on
       each of those edges so the user knows which directions the
       hidden panes are in. */
    if (t->maximized_leaf && pane_tree_count(t->root) >= 2) {
        int sides = 0;  /* bit0 top, bit1 right, bit2 bottom, bit3 left */
        for (PaneNode *n = t->maximized_leaf; n && n->parent; n = n->parent) {
            PaneNode *par = n->parent;
            bool is_first = (par->child[0] == n);
            if (par->split == SPLIT_VERTICAL) {
                sides |= is_first ? 0x02 : 0x08;
            } else if (par->split == SPLIT_HORIZONTAL) {
                sides |= is_first ? 0x04 : 0x01;
            }
        }
        PaneRect pr;
        if (leaf_rect(t, t->maximized_leaf, win_w, win_h, &pr)) {
            Color sb = (Color){125, 207, 255, 220};
            int len_h = pr.w / 4;
            int len_v = pr.h / 4;
            int thick = 3;
            if (sides & 0x01) {  /* top */
                DrawRectangle(pr.x + (pr.w - len_h) / 2, pr.y,
                              len_h, thick, sb);
            }
            if (sides & 0x02) {  /* right */
                DrawRectangle(pr.x + pr.w - thick, pr.y + (pr.h - len_v) / 2,
                              thick, len_v, sb);
            }
            if (sides & 0x04) {  /* bottom */
                DrawRectangle(pr.x + (pr.w - len_h) / 2, pr.y + pr.h - thick,
                              len_h, thick, sb);
            }
            if (sides & 0x08) {  /* left */
                DrawRectangle(pr.x, pr.y + (pr.h - len_v) / 2,
                              thick, len_v, sb);
            }
        }
    }
    /* Rollover maximize / restore button — top-right corner of
       each visible pane while the mouse is over it. The arrow
       points 45° toward the pane's centre when not maximized
       (a "fold-into-pane" cue) and points outward in the
       opposite direction when maximized. Click handler reads
       p->maximize_btn_* fields the next frame. Multi-pane only;
       a single pane has nothing to maximize. */
    {
        bool multi = pane_tree_count(t->root) >= 2;
        for (PaneNode *_l = pane_tree_first_leaf(t->root); _l;
             _l = pane_tree_next_leaf(_l)) {
            Pane *_p = _l->pane;
            if (!_p) continue;
            _p->maximize_btn_x = _p->maximize_btn_y = 0;
            _p->maximize_btn_w = _p->maximize_btn_h = 0;
            if (!multi) continue;
            PaneRect pr;
            if (!leaf_rect(t, _l, win_w, win_h, &pr)) continue;
            int btn_sz = 20;
            int bx = pr.x + pr.w - btn_sz - 6;
            int by = pr.y + 6;
            int mx = (int)mpos.x, my = (int)mpos.y;
            bool over_pane = (mx >= pr.x && mx < pr.x + pr.w &&
                              my >= pr.y && my < pr.y + pr.h);
            bool over_btn  = (mx >= bx && mx < bx + btn_sz &&
                              my >= by && my < by + btn_sz);
            if (!over_pane && !over_btn) continue;
            _p->maximize_btn_x = bx;
            _p->maximize_btn_y = by;
            _p->maximize_btn_w = btn_sz;
            _p->maximize_btn_h = btn_sz;
            /* Soft background pill so the icon is readable
               whatever cell colour sits underneath. */
            Color bg   = (Color){0, 0, 0, over_btn ? 200 : 130};
            Color line = (Color){200, 230, 255, over_btn ? 255 : 200};
            DrawRectangle(bx, by, btn_sz, btn_sz, bg);
            DrawRectangleLines(bx, by, btn_sz, btn_sz, line);
            bool is_max = (t->maximized_leaf == _l);
            int ax1, ay1, ax2, ay2;
            if (!is_max) {
                /* Arrow ↗ pointing OUT of the pane: "expand to
                   fill the window". Tip at the upper-right of
                   the button, tail at the lower-left. */
                ax1 = bx + 4;           ay1 = by + btn_sz - 4;
                ax2 = bx + btn_sz - 4;  ay2 = by + 4;
            } else {
                /* Arrow ↙ pointing INTO the pane: "contract back
                   to its split position". Tip at lower-left, tail
                   at upper-right. */
                ax1 = bx + btn_sz - 4;  ay1 = by + 4;
                ax2 = bx + 4;           ay2 = by + btn_sz - 4;
            }
            DrawLineEx((Vector2){(float)ax1, (float)ay1},
                       (Vector2){(float)ax2, (float)ay2},
                       2.0f, line);
            /* Arrowhead — two short legs at the tip end (ax2,ay2). */
            int dx = (ax2 > ax1) ? 1 : -1;
            int dy = (ay2 > ay1) ? 1 : -1;
            DrawLineEx((Vector2){(float)ax2, (float)ay2},
                       (Vector2){(float)(ax2 - dx * 6), (float)ay2},
                       2.0f, line);
            DrawLineEx((Vector2){(float)ax2, (float)ay2},
                       (Vector2){(float)ax2, (float)(ay2 - dy * 6)},
                       2.0f, line);
        }
    }

    /* One splitter bar per internal node, walked iteratively so we
       don't recurse and don't allocate a closure. Skipped entirely
       when a pane is maximized — without that, the splitter
       lines paint OVER the maximized pane at the positions of
       the original splits, which looks like leftover dividers
       from before the maximize. */
    if (!t->maximized_leaf) {
        PaneRect outer = pane_tree_terminal_outer(win_w, win_h);
        PaneNode *stack[64]; PaneRect rstack[64]; int sp_top = 0;
        if (t->root) { stack[sp_top] = t->root; rstack[sp_top] = outer; sp_top++; }
        while (sp_top > 0) {
            sp_top--;
            PaneNode *n = stack[sp_top]; PaneRect o = rstack[sp_top];
            if (!n || n->split == SPLIT_NONE) continue;
            PaneRect ra, rb;
            pane_tree_split_children(n, o, &ra, &rb);
            PaneRect sp;
            if (n->split == SPLIT_VERTICAL) {
                sp.x = ra.x + ra.w; sp.y = ra.y;
                sp.w = SPLITTER_PX; sp.h = ra.h;
            } else {
                sp.x = ra.x; sp.y = ra.y + ra.h;
                sp.w = ra.w; sp.h = SPLITTER_PX;
            }
            DrawRectangle(sp.x, sp.y, sp.w, sp.h, (Color){60, 60, 75, 255});
            if (sp_top + 1 < 64) { stack[sp_top] = n->child[0]; rstack[sp_top] = ra; sp_top++; }
            if (sp_top + 1 < 64) { stack[sp_top] = n->child[1]; rstack[sp_top] = rb; sp_top++; }
        }
    }

    /* HUD overlay — corner of each pane. Per-field colour and size
       are individually configurable (Settings → HUD), so we don't
       use hud_format's single-string output anymore — each field is
       formatted, measured, and drawn separately so it can carry its
       own size + colour. Slab dimensions are computed from the
       widest measured line and the sum of per-line heights.

       Click-to-roll: clicking the slab toggles g_app_settings.hud_collapsed.
       When collapsed, the slab eases down to a tiny chevron tab the
       user can click to expand again. Useful when the HUD covers
       interesting terminal text. */
    static float s_hud_phase = 1.0f;   /* 0 = collapsed, 1 = fully expanded */
    {
        float target = g_app_settings.hud_collapsed ? 0.0f : 1.0f;
        s_hud_phase += (target - s_hud_phase) * 0.25f;
        if (s_hud_phase < 0.001f) s_hud_phase = 0.0f;
        if (s_hud_phase > 0.999f) s_hud_phase = 1.0f;
    }
    bool hud_hover = false;
    HudConfig hud = hud_effective(t);
    /* HUD: one slab per window, sourced from the active pane. With
       recursive splits, repeating the slab in every leaf would tile
       the screen; the active pane is the only one whose stats the
       user is actively reading. The slab anchors to the terminal
       area below the tab bar, not to the active leaf's rect, so it
       lives in the same screen corner regardless of split layout. */
    if (hud.show && t->active && t->active->pane) {
        const int x_pad  = 8;
        const int y_pad  = 6;
        const int margin = 8;
        Vector2 _mp = GetMousePosition();
        bool _click = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
        do {
            Pane *p = t->active->pane;
            PaneRect pr = pane_tree_terminal_outer(win_w, win_h);

            /* Build the per-field string + style table. Skipped fields
               just have show=false. */
            char host_s[64], ip_s[48], load_s[24], mem_s[24], disk_s[24];
            snprintf(host_s, sizeof(host_s), "%s",
                     p->hud_hostname[0] ? p->hud_hostname : "?");
            snprintf(ip_s,   sizeof(ip_s),   "%s",
                     p->hud_ip[0] ? p->hud_ip : "?");
            if (p->hud_load1 < 0)
                snprintf(load_s, sizeof(load_s), "load ?");
            else
                snprintf(load_s, sizeof(load_s), "load %.2f", p->hud_load1);
            if (p->hud_mem_free_mb < 0)
                snprintf(mem_s, sizeof(mem_s), "mem ?");
            else if (p->hud_mem_free_mb < 1024)
                snprintf(mem_s, sizeof(mem_s), "mem %ld MB", p->hud_mem_free_mb);
            else
                snprintf(mem_s, sizeof(mem_s), "mem %.1f GB",
                         p->hud_mem_free_mb / 1024.0);
            if (p->hud_disk_free_pct < 0)
                snprintf(disk_s, sizeof(disk_s), "disk ?");
            else
                snprintf(disk_s, sizeof(disk_s), "disk %d%% free",
                         p->hud_disk_free_pct);
            const char *texts[HUD_FIELD_COUNT] = {
                host_s, ip_s, load_s, mem_s, disk_s
            };

            int max_w = 0;
            int total_h = 0;
            int line_h[HUD_FIELD_COUNT] = {0};
            int line_w[HUD_FIELD_COUNT] = {0};
            int rendered_count = 0;
            for (int fi = 0; fi < HUD_FIELD_COUNT; fi++) {
                if (!hud.field_show[fi]) continue;
                int fs = hud.field_size[fi];
                if (fs < 10) fs = 10;
                if (fs > 18) fs = 18;
                int w = MeasureText(texts[fi], fs);
                int h = fs + 2;   /* 2px line-leading on top of font size */
                line_w[fi] = w;
                line_h[fi] = h;
                if (w > max_w) max_w = w;
                total_h += h;
                rendered_count++;
            }
            /* Reserve a 30px-tall sparkline if CPU history exists and
               the user hasn't disabled it. The sparkline width is the
               wider of the slab text or a 100px minimum. */
            const int spark_h = 30;
            const int spark_min_w = 100;
            bool draw_spark = hud.show_cpu && p->hud_cpu_inited;
            if (rendered_count == 0 && !draw_spark) continue;

            int spark_w = (max_w > spark_min_w) ? max_w : spark_min_w;
            int slab_w_full = (draw_spark ? spark_w : max_w) + x_pad * 2;
            int slab_h_full = total_h + (draw_spark ? spark_h + 4 : 0) + y_pad * 2;
            /* When collapsed, the slab shrinks vertically to ~14px
               and shows just a chevron. Animate via s_hud_phase. */
            const int collapsed_h = 14;
            int slab_w = slab_w_full;
            int slab_h;
            if (s_hud_phase >= 1.0f) {
                slab_h = slab_h_full;
            } else if (s_hud_phase <= 0.0f) {
                slab_h = collapsed_h;
            } else {
                slab_h = (int)(collapsed_h + (slab_h_full - collapsed_h) * s_hud_phase);
            }
            int slab_x, slab_y;
            switch (hud.pos) {
            case HUD_POS_TOP_LEFT:
                slab_x = pr.x + margin;
                slab_y = pr.y + margin;
                break;
            case HUD_POS_BOTTOM_RIGHT:
                slab_x = pr.x + pr.w - slab_w - margin;
                slab_y = pr.y + pr.h - slab_h - margin;
                break;
            case HUD_POS_BOTTOM_LEFT:
                slab_x = pr.x + margin;
                slab_y = pr.y + pr.h - slab_h - margin;
                break;
            case HUD_POS_TOP_RIGHT:
            default:
                slab_x = pr.x + pr.w - slab_w - margin;
                slab_y = pr.y + margin;
                break;
            }
            /* Hover + click handling. While the slab is at least
               partly visible, hovering inside it changes the cursor
               to a pointer; clicking toggles the collapsed state. */
            bool inside_slab = _mp.x >= slab_x && _mp.x < slab_x + slab_w
                            && _mp.y >= slab_y && _mp.y < slab_y + slab_h;
            if (inside_slab) hud_hover = true;
            if (inside_slab && _click) {
                g_app_settings.hud_collapsed = !g_app_settings.hud_collapsed;
            }

            DrawRectangle(slab_x, slab_y, slab_w, slab_h,
                          (Color){10, 12, 18, 175});
            DrawRectangleLines(slab_x, slab_y, slab_w, slab_h,
                               (Color){80, 90, 110, 100});

            /* Roll-up content draws at full opacity but is clipped
               to the current slab height so as the slab shrinks the
               text rolls up cleanly behind the slab edge. */
            BeginScissorMode(slab_x, slab_y, slab_w, slab_h);
            int yy = slab_y + y_pad;
            if (s_hud_phase > 0.05f) {
                for (int fi = 0; fi < HUD_FIELD_COUNT; fi++) {
                    if (!hud.field_show[fi]) continue;
                    int fs = hud.field_size[fi];
                    if (fs < 10) fs = 10;
                    if (fs > 18) fs = 18;
                    int ci = hud.field_color[fi];
                    if (ci < 0 || ci >= HUD_PALETTE_COUNT) ci = 0;
                    DrawText(texts[fi], slab_x + x_pad, yy, fs,
                             HUD_PALETTE[ci]);
                    yy += line_h[fi];
                }
            } else {
                /* Collapsed handle: draw a small chevron (▾) so the
                   user knows where to click to expand again. */
                const char *chevron = "v hud";
                int csz_w = MeasureText(chevron, 10);
                int cx = slab_x + (slab_w - csz_w) / 2;
                int cy = slab_y + (slab_h - 10) / 2;
                DrawText(chevron, cx, cy, 10,
                         (Color){180, 200, 230, 220});
            }

            /* CPU sparkline. Walks the ring buffer in chronological
               order (oldest sample at the left, newest at the right)
               and draws one filled bar per sample. Inside the same
               scissor as text so it rolls up with the rest. */
            if (draw_spark && s_hud_phase > 0.05f) {
                yy += 4;
                int gx = slab_x + x_pad;
                int gy = yy;
                int gw = spark_w;
                int gh = spark_h;
                /* Subtle backing so the sparkline reads against the
                   slab even when bars are short. */
                DrawRectangle(gx, gy, gw, gh, (Color){0, 0, 0, 100});
                /* Per-bar layout — leave 1px gap so individual
                   samples are distinguishable. */
                int bw = gw / HUD_CPU_HISTORY;
                if (bw < 1) bw = 1;
                int extra = gw - bw * HUD_CPU_HISTORY;
                for (int s = 0; s < HUD_CPU_HISTORY; s++) {
                    /* Walk oldest-to-newest. head is the most-recent
                       slot, so the oldest is (head + 1) % CAP. */
                    int idx = (p->hud_cpu_head + 1 + s) % HUD_CPU_HISTORY;
                    int pct = p->hud_cpu_pct[idx];
                    if (pct < 0) continue;       /* unwritten slot */
                    int bh = (gh * pct) / 100;
                    if (bh < 1) bh = 1;
                    int bx = gx + s * bw + (s < extra ? s : extra);
                    int by = gy + gh - bh;
                    /* Colour ramp: green at low load, yellow mid,
                       red high. Cheap interpolation between three
                       waypoints. */
                    Color bc;
                    if (pct < 50) {
                        float t = pct / 50.0f;
                        bc = (Color){(unsigned char)(120 + (int)(120 * t)),
                                     (unsigned char)(220 - (int)(20 * t)),
                                     120, 220};
                    } else {
                        float t = (pct - 50) / 50.0f;
                        bc = (Color){(unsigned char)(240),
                                     (unsigned char)(200 - (int)(140 * t)),
                                     (unsigned char)(80  - (int)(40  * t)), 230};
                    }
                    DrawRectangle(bx, by, bw, bh, bc);
                }
                /* Latest-value tag in the corner of the sparkline so
                   the user has an exact number to read. */
                int latest = p->hud_cpu_pct[p->hud_cpu_head];
                if (latest >= 0) {
                    char cb[16];
                    snprintf(cb, sizeof(cb), "cpu %d%%", latest);
                    DrawText(cb, gx + 4, gy + 2, 10,
                             (Color){220, 224, 232, 220});
                }
            }
            EndScissorMode();
        } while (0);
    }

    /* SFTP transfer toasts — bottom-left of each pane. Upload (if
       any) draws first, download stacks above it. Reads atomic
       state from the worker so this is safe to call every frame
       without locking. ASCII-only labels because raylib's bundled
       font doesn't carry ↑/✓/✗ codepoints. */
    for (PaneNode *leaf = pane_tree_first_leaf(t->root); leaf;
         leaf = pane_tree_next_leaf(leaf)) {
        Pane *p = leaf->pane;
        if (!p) continue;
        struct { void *handle; const char *prefix; bool is_upload; } slots[2] = {
            { p->upload,   "up",  true  },
            { p->download, "dn",  false },
        };
        PaneRect pr;
        if (!leaf_rect(t, leaf, win_w, win_h, &pr)) continue;
        int fs = 12, x_pad = 8, y_pad = 4, margin = 8;
        int slab_h = fs + y_pad * 2;
        int stack_offset = 0;
        for (int si = 0; si < 2; si++) {
            if (!slots[si].handle) continue;
            uint64_t done = 0, total = 0;
            char err[256] = {0};
            int st;
            const char *name;
            if (slots[si].is_upload) {
                st   = pty_upload_status((PtyUpload *)slots[si].handle,
                                         &done, &total, err, sizeof(err));
                name = pty_upload_name((PtyUpload *)slots[si].handle);
            } else {
                st   = pty_download_status((PtyDownload *)slots[si].handle,
                                           &done, &total, err, sizeof(err));
                name = pty_download_name((PtyDownload *)slots[si].handle);
            }
            char label[320];
            Color outline = (Color){80, 90, 110, 255};
            if (st == 0) {
                int pct = (total > 0) ? (int)((done * 100ULL) / total) : 0;
                if (pct > 99) pct = 99;
                snprintf(label, sizeof(label), "[%s] %s  %d%%",
                         slots[si].prefix, name, pct);
                outline = (Color){125, 207, 255, 220};
            } else if (st == 1) {
                double mb = (double)total / (1024.0 * 1024.0);
                if (mb >= 1.0) snprintf(label, sizeof(label), "[ok] %s  %.1f MB",
                                        name, mb);
                else           snprintf(label, sizeof(label), "[ok] %s  %llu KB",
                                        name, (unsigned long long)(total / 1024));
                outline = (Color){140, 230, 160, 220};
            } else {
                snprintf(label, sizeof(label), "[err] %s: %s",
                         name, err[0] ? err : "failed");
                outline = (Color){240, 120, 120, 220};
            }
            int tw = MeasureText(label, fs);
            int slab_w = tw + x_pad * 2;
            int slab_x = pr.x + margin;
            int slab_y = pr.y + pr.h - slab_h - margin - stack_offset;
            DrawRectangle(slab_x, slab_y, slab_w, slab_h,
                          (Color){10, 12, 18, 200});
            DrawRectangleLines(slab_x, slab_y, slab_w, slab_h, outline);
            DrawText(label, slab_x + x_pad, slab_y + y_pad, fs,
                     (Color){230, 235, 245, 255});
            stack_offset += slab_h + 4;
        }
    }

    SetMouseCursor(hud_hover ? MOUSE_CURSOR_POINTING_HAND
                  : want_url_cursor ? MOUSE_CURSOR_POINTING_HAND
                                    : MOUSE_CURSOR_DEFAULT);
    (void)win_w; (void)win_h;
    (void)time_sec;
}

/* Resize every pane of every tab to fit its current pane rectangle.
   Called on window resize, split toggles, and font-size changes. */
static void tabs_resize_all(const Renderer *r, int win_w, int win_h) {
    for (int i = 0; i < g_num_tabs; i++) {
        Tab *t = g_tabs[i];
        for (PaneNode *leaf = pane_tree_first_leaf(t->root); leaf;
             leaf = pane_tree_next_leaf(leaf)) {
            Pane *p = leaf->pane;
            if (!p || !p->scr) continue;
            PaneRect pr;
            if (!leaf_rect(t, leaf, win_w, win_h, &pr)) continue;
            int cols, rows;
            pane_dims(r, &pr, &cols, &rows);
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
    bool on_snap;          /* take screenshot of active pane */
    bool on_upload;        /* SFTP upload — only present on SSH tabs */
    bool on_download;      /* SFTP download — only present on SSH tabs */
    bool on_bcast;         /* Broadcast-input toggle — only when multi-pane */
} TabBarHit;

/* Split buttons are always available now that splits are recursive —
   the kept-as-stub function preserves the existing call sites. */
static bool split_buttons_visible(void) {
    return true;
}

/* True iff the active tab is an SSH session — controls visibility of
   the SFTP upload + download tab-bar buttons. */
static bool upload_button_visible(void) {
    if (g_active < 0 || g_active >= g_num_tabs) return false;
    Tab *t = g_tabs[g_active];
    return t && t->is_ssh;
}
static bool download_button_visible(void) {
    return upload_button_visible();
}

/* Broadcast button only renders when the active tab has 2+ panes
   — broadcasting to a single pane is just typing, so the button
   would be useless real estate. */
static bool bcast_button_visible(void) {
    if (g_active < 0 || g_active >= g_num_tabs) return false;
    Tab *t = g_tabs[g_active];
    return t && pane_tree_count(t->root) >= 2;
}

/* Compute the per-tab pixel width in the tab bar from the
   available room (window width minus the SSH/help/+/-/split
   buttons). Splits the leftover width evenly across the open
   tabs, clamped to TAB_MIN_W..TAB_MAX_W. */
static int tab_width_for(int win_w) {
    int split_w  = split_buttons_visible() ? 2 * TAB_SPLIT_W : 0;
    int upload_w = upload_button_visible()  ? TAB_UPLOAD_W   : 0;
    int dl_w     = download_button_visible() ? TAB_DOWNLOAD_W : 0;
    int bcast_w  = bcast_button_visible()    ? TAB_BCAST_W   : 0;
    int avail = win_w - TAB_PLUS_W - TAB_GEAR_W - 2 * TAB_REC_W - TAB_SNAP_W
                - TAB_HELP_W - split_w - upload_w - dl_w - bcast_w - TAB_SSH_W;
    if (g_num_tabs <= 0) return TAB_MIN_W;
    int w = avail / g_num_tabs;
    if (w > TAB_MAX_W) w = TAB_MAX_W;
    if (w < TAB_MIN_W) w = TAB_MIN_W;
    return w;
}

/* Layout: [ssh] [+] | tab1 | tab2 | ... | [gear] [rec-start] [rec-stop] [split-v] [split-h] [?]
   The split pair disappears entirely when the active tab is split.
   The "+" was on the far right; moved next to [ssh] so the two
   "open something" buttons live together. */
static TabBarHit tab_bar_hit_test(int win_w, int mx, int my) {
    TabBarHit h = { -1, false, false, false, false, false, false, false, false, false, false, false, false, false };
    if (my < 0 || my >= TAB_BAR_H) return h;
    bool show_splits = split_buttons_visible();
    bool show_up     = upload_button_visible();
    bool show_dl     = download_button_visible();
    bool show_bcast  = bcast_button_visible();
    int help_x      = win_w - TAB_HELP_W;
    int split_h_x   = show_splits ? help_x - TAB_SPLIT_W     : help_x;
    int split_v_x   = show_splits ? split_h_x - TAB_SPLIT_W  : help_x;
    int bcast_x     = show_bcast ? split_v_x - TAB_BCAST_W : split_v_x;
    int snap_x      = bcast_x - TAB_SNAP_W;
    int rec_stop_x  = snap_x - TAB_REC_W;
    int rec_start_x = rec_stop_x - TAB_REC_W;
    int upload_x    = show_up ? rec_start_x - TAB_UPLOAD_W : rec_start_x;
    int dl_x        = show_dl ? upload_x - TAB_DOWNLOAD_W  : upload_x;
    int gear_x      = dl_x - TAB_GEAR_W;
    int plus_x      = TAB_SSH_W;
    int tab_start   = TAB_SSH_W + TAB_PLUS_W;
    if (mx < TAB_SSH_W)                       { h.on_ssh  = true; return h; }
    if (mx < tab_start)                       { h.on_plus = true; return h; }
    (void)plus_x;
    if (mx >= help_x)       { h.on_help    = true; return h; }
    if (show_splits && mx >= split_h_x) { h.on_split_h = true; return h; }
    if (show_splits && mx >= split_v_x) { h.on_split_v = true; return h; }
    if (show_bcast && mx >= bcast_x)    { h.on_bcast    = true; return h; }
    if (mx >= snap_x)       { h.on_snap      = true; return h; }
    if (mx >= rec_stop_x)   { h.on_rec_stop  = true; return h; }
    if (mx >= rec_start_x)  { h.on_rec_start = true; return h; }
    if (show_up && mx >= upload_x) { h.on_upload = true; return h; }
    if (show_dl && mx >= dl_x)     { h.on_download = true; return h; }
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

    /* "+" button — sits immediately right of the [ssh] button so
       the two "open a new tab" actions are next to each other. */
    int plus_x = TAB_SSH_W;
    DrawRectangle(plus_x, 0, TAB_PLUS_W, TAB_BAR_H, (Color){38, 48, 66, 255});
    DrawRectangleLines(plus_x, 2, TAB_PLUS_W - 1, TAB_BAR_H - 4,
                       (Color){125, 207, 255, 200});
    Vector2 psz = MeasureTextEx(*f, "+", 18, 0);
    DrawTextEx(*f, "+",
               (Vector2){ plus_x + (TAB_PLUS_W - psz.x) / 2.0f,
                          (TAB_BAR_H - psz.y) / 2.0f },
               18, 0, (Color){230, 240, 255, 255});

    /* Help button (?) anchored top-right. */
    int help_x = win_w - TAB_HELP_W;
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
    bool show_up     = upload_button_visible();
    bool show_dl     = download_button_visible();
    bool show_bcast  = bcast_button_visible();
    int split_h_x   = show_splits ? help_x - TAB_SPLIT_W    : help_x;
    int split_v_x   = show_splits ? split_h_x - TAB_SPLIT_W : help_x;
    int bcast_x     = show_bcast ? split_v_x - TAB_BCAST_W : split_v_x;
    int snap_x      = bcast_x - TAB_SNAP_W;
    int rec_stop_x  = snap_x - TAB_REC_W;
    int rec_start_x = rec_stop_x - TAB_REC_W;
    int upload_x    = show_up ? rec_start_x - TAB_UPLOAD_W : rec_start_x;
    int dl_x        = show_dl ? upload_x - TAB_DOWNLOAD_W  : upload_x;
    int gear_x      = dl_x - TAB_GEAR_W;

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

    /* BROADCAST pill — same shape as REC, sits just left of either
       the rec pill (when both are active) or the gear (when broadcast
       is the only one). Pulses gently so it stays salient even when
       the user's reading the terminal. */
    if (g_broadcast_active) {
        const char *blabel = "BROADCAST";
        Vector2 blsz = MeasureTextEx(*f, blabel, 13, 0);
        int bpill_pad = 8;
        int bpill_w = (int)blsz.x + bpill_pad * 2;
        int bpill_anchor = gear_x;
        if (g_rec.active) {
            /* Slot it left of the REC pill. We approximate the
               width of REC since we don't keep its rect around;
               80 px is conservative for "REC mm:ss". */
            bpill_anchor = gear_x - 80 - 12;
        }
        int bpill_x = bpill_anchor - bpill_w - 6;
        if (bpill_x < TAB_SSH_W + 4) bpill_x = TAB_SSH_W + 4;
        /* Solid bg — cursor-blink-style animation isn't a dirty
           trigger so it would freeze on whichever phase the last
           redraw landed in. Solid red still reads "active". */
        Color bpill_bg = (Color){70, 22, 28, 255};
        Color bpill_outline = (Color){240, 100, 110, 230};
        Color btext_col = (Color){255, 220, 220, 255};
        DrawRectangle(bpill_x, 4, bpill_w, TAB_BAR_H - 8, bpill_bg);
        DrawRectangleLines(bpill_x, 4, bpill_w, TAB_BAR_H - 8, bpill_outline);
        DrawTextEx(*f, blabel,
                   (Vector2){bpill_x + bpill_pad,
                             (TAB_BAR_H - blsz.y) / 2.0f},
                   13, 0, btext_col);
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

    /* Screenshot button — small camera icon drawn from primitives
       (rounded body + viewfinder bump + lens circle). One click
       captures the active pane to a PNG and pops it open in the
       system image viewer. Same x slot whether or not a recording
       is in progress. */
    {
        Color cam_bg      = (Color){38, 48, 66, 255};
        Color cam_outline = (Color){125, 207, 255, 200};
        Color cam_glyph   = (Color){220, 235, 255, 255};
        DrawRectangle(snap_x, 0, TAB_SNAP_W, TAB_BAR_H, cam_bg);
        DrawRectangleLines(snap_x, 2, TAB_SNAP_W - 1, TAB_BAR_H - 4, cam_outline);
        float cx = snap_x + TAB_SNAP_W / 2.0f;
        float cy = TAB_BAR_H / 2.0f;
        float bw = 14.0f, bh = 9.0f;
        /* Camera body. */
        DrawRectangle((int)(cx - bw / 2.0f), (int)(cy - bh / 2.0f + 1),
                      (int)bw, (int)bh, cam_glyph);
        /* Viewfinder bump on top-left of the body. */
        DrawRectangle((int)(cx - bw / 2.0f + 2), (int)(cy - bh / 2.0f - 2),
                      4, 3, cam_glyph);
        /* Lens (dark circle + tiny white highlight). */
        DrawCircle((int)cx, (int)(cy + 1), 3.2f, cam_bg);
        DrawCircle((int)cx, (int)cy, 1.0f, cam_glyph);
    }

    /* SFTP upload button — only on SSH tabs. A small "↑" arrow
       glyph drawn from line primitives so we don't depend on the
       active font having an arrow codepoint. Click opens
       UI_SFTP_UPLOAD with the system file picker pre-fired. */
    if (show_up) {
        Color up_bg = (Color){38, 48, 66, 255};
        Color up_outline = (Color){125, 207, 255, 200};
        Color up_glyph = (Color){220, 235, 255, 255};
        DrawRectangle(upload_x, 0, TAB_UPLOAD_W, TAB_BAR_H, up_bg);
        DrawRectangleLines(upload_x, 2, TAB_UPLOAD_W - 1, TAB_BAR_H - 4, up_outline);
        float cx = upload_x + TAB_UPLOAD_W / 2.0f;
        float cy = TAB_BAR_H / 2.0f;
        float size = TAB_BAR_H * 0.42f;
        /* Vertical shaft, then a chevron at the top. */
        DrawLineEx((Vector2){cx, cy + size * 0.55f},
                   (Vector2){cx, cy - size * 0.55f}, 2.0f, up_glyph);
        DrawLineEx((Vector2){cx - size * 0.45f, cy - size * 0.10f},
                   (Vector2){cx, cy - size * 0.55f}, 2.0f, up_glyph);
        DrawLineEx((Vector2){cx + size * 0.45f, cy - size * 0.10f},
                   (Vector2){cx, cy - size * 0.55f}, 2.0f, up_glyph);
    }

    /* SFTP download button — same shape, chevron pointing down. */
    if (show_dl) {
        Color dl_bg = (Color){38, 48, 66, 255};
        Color dl_outline = (Color){125, 207, 255, 200};
        Color dl_glyph = (Color){220, 235, 255, 255};
        DrawRectangle(dl_x, 0, TAB_DOWNLOAD_W, TAB_BAR_H, dl_bg);
        DrawRectangleLines(dl_x, 2, TAB_DOWNLOAD_W - 1, TAB_BAR_H - 4, dl_outline);
        float cx = dl_x + TAB_DOWNLOAD_W / 2.0f;
        float cy = TAB_BAR_H / 2.0f;
        float size = TAB_BAR_H * 0.42f;
        DrawLineEx((Vector2){cx, cy - size * 0.55f},
                   (Vector2){cx, cy + size * 0.55f}, 2.0f, dl_glyph);
        DrawLineEx((Vector2){cx - size * 0.45f, cy + size * 0.10f},
                   (Vector2){cx, cy + size * 0.55f}, 2.0f, dl_glyph);
        DrawLineEx((Vector2){cx + size * 0.45f, cy + size * 0.10f},
                   (Vector2){cx, cy + size * 0.55f}, 2.0f, dl_glyph);
    }

    /* Broadcast-input toggle. The icon is a small radio-tower glyph:
       a centre dot with three concentric arcs above/below to suggest
       transmission. When active the whole button glows red so the
       user can't miss that input is fanning out. */
    if (show_bcast) {
        bool on = g_broadcast_active;
        Color bc_bg      = on ? (Color){90, 30, 40, 255} : (Color){38, 48, 66, 255};
        Color bc_outline = on ? (Color){240, 100, 110, 255}
                              : (Color){125, 207, 255, 200};
        Color bc_glyph   = on ? (Color){255, 220, 220, 255}
                              : (Color){220, 235, 255, 255};
        DrawRectangle(bcast_x, 0, TAB_BCAST_W, TAB_BAR_H, bc_bg);
        DrawRectangleLines(bcast_x, 2, TAB_BCAST_W - 1, TAB_BAR_H - 4, bc_outline);
        float cx = bcast_x + TAB_BCAST_W / 2.0f;
        float cy = TAB_BAR_H / 2.0f;
        /* Centre dot. */
        DrawCircle((int)cx, (int)cy, 2.0f, bc_glyph);
        /* Two arcs of "wave fronts" on either side. We approximate
           with thin DrawCircleSector calls — raylib's API needs a
           start/end angle in degrees. Left arcs: 110..160, right
           arcs: 20..70 (so the open mouths face outward). */
        DrawRing((Vector2){cx, cy}, 4.0f, 5.5f, 110.0f, 160.0f, 20, bc_glyph);
        DrawRing((Vector2){cx, cy}, 7.0f, 8.5f, 110.0f, 160.0f, 20, bc_glyph);
        DrawRing((Vector2){cx, cy}, 4.0f, 5.5f, 20.0f, 70.0f, 20, bc_glyph);
        DrawRing((Vector2){cx, cy}, 7.0f, 8.5f, 20.0f, 70.0f, 20, bc_glyph);
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

    /* Tabs fill the space between the [ssh][+] cluster on the left
       and the gear/rec/split/help cluster on the right. */
    int tab_start = TAB_SSH_W + TAB_PLUS_W;
    int tw = tab_width_for(win_w);

    for (int i = 0; i < g_num_tabs; i++) {
        int x = tab_start + i * tw;
        bool active = (i == g_active);
        Color bg = active ? (Color){46, 52, 70, 255} : (Color){28, 32, 44, 255};
        Color fg = active ? (Color){230, 230, 240, 255} : (Color){150, 150, 165, 255};
        /* Per-host accent: SSH tabs with a configured colour use it
           as the tab background, dimmed for inactive tabs so the
           active one still pops out of the bar. The top accent
           stripe (drawn below) and label colour stay constant. */
        Color host_tint;
        if (g_tabs[i]->is_ssh && g_tabs[i]->ssh_color[0] &&
            parse_hex_color(g_tabs[i]->ssh_color, &host_tint)) {
            if (active) {
                bg = host_tint;
            } else {
                /* Mix the tint 35% with the dim default so the
                   inactive tab still looks "off" but still hints
                   at the host identity. */
                bg.r = (unsigned char)((host_tint.r * 35 + 28 * 65) / 100);
                bg.g = (unsigned char)((host_tint.g * 35 + 32 * 65) / 100);
                bg.b = (unsigned char)((host_tint.b * 35 + 44 * 65) / 100);
                bg.a = 255;
            }
        }
        DrawRectangle(x, 0, tw, TAB_BAR_H, bg);
        if (active) {
            /* Strong active-tab cue: thick green outline around
               the tab header plus the cyan top stripe. The
               outline contrasts with the cyan/amber accents the
               rest of the UI uses, so it reads as "this is the
               active tab" at a glance. */
            Color act_outline = (Color){80, 220, 130, 255};
            DrawRectangleLinesEx(
                (Rectangle){(float)x, 0.0f, (float)tw, (float)TAB_BAR_H},
                2.0f, act_outline);
            DrawRectangle(x, 0, tw, 2, (Color){125, 207, 255, 255});
        }

        /* Per-tab status glyph left of the label:
             - Spinner (cyan) while any pane has a command running
               (OSC 133;C without a matching D).
             - Activity dot (amber) for backgrounded tabs that have
               produced output since the user last focused them.
           Spinner takes precedence — a running command is the more
           informative signal. */
        int label_x = x + 10;
        bool any_running = false;
        for (PaneNode *_leaf = pane_tree_first_leaf(g_tabs[i]->root); _leaf;
             _leaf = pane_tree_next_leaf(_leaf)) {
            const Pane *_pp = _leaf->pane;
            if (_pp && _pp->scr && screen_command_running(_pp->scr)) {
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
        bool renaming = g_tab_rename_active && i == g_tab_rename_idx;
        BeginScissorMode(label_x - 2, 0, tw - TAB_CLOSE_W - (label_x - x) - 4, TAB_BAR_H);
        if (renaming) {
            const char *title = g_tab_rename_buf;
            float rfs = fs + 2.0f;
            Vector2 tsz = MeasureTextEx(*f, title, rfs, 0);
            Vector2 tp  = { label_x, (TAB_BAR_H - tsz.y) / 2.0f };
            DrawTextEx(*f, title, tp, rfs, 0,
                       (Color){255, 230, 150, 255});
            if (((long long)(GetTime() * 2.0) & 1) == 0) {
                int cx = (int)(tp.x + tsz.x + 1);
                DrawRectangle(cx, 6, 2, TAB_BAR_H - 12, (Color){255, 230, 150, 255});
            }
        } else if (g_tabs[i]->tab_name[0]) {
            /* Manual name: bigger, brighter. The auto suffix
               trails in the regular fs/fg so the eye locks onto
               the user's mnemonic first. */
            float mfs = fs + 2.0f;
            const char *manual = g_tabs[i]->tab_name;
            const char *suf = tab_label_suffix(g_tabs[i]);
            Vector2 msz = MeasureTextEx(*f, manual, mfs, 0);
            Color manual_col = active ? (Color){255, 220, 140, 255}
                                      : (Color){200, 175, 120, 255};
            Vector2 mp_t = { label_x, (TAB_BAR_H - msz.y) / 2.0f };
            DrawTextEx(*f, manual, mp_t, mfs, 0, manual_col);
            if (suf && suf[0]) {
                /* Separator + suffix at the original size, dimmer. */
                char trail[256];
                snprintf(trail, sizeof(trail), "  · %s", suf);
                Color suf_col = active ? (Color){180, 185, 200, 255}
                                       : (Color){110, 115, 130, 255};
                DrawTextEx(*f, trail,
                           (Vector2){mp_t.x + msz.x,
                                     (TAB_BAR_H - msz.y) / 2.0f + (msz.y - fs) * 0.5f},
                           fs, 0, suf_col);
            }
        } else {
            const char *title = tab_label(g_tabs[i]);
            Vector2 tsz = MeasureTextEx(*f, title, fs, 0);
            Vector2 tp  = { label_x, (TAB_BAR_H - tsz.y) / 2.0f };
            DrawTextEx(*f, title, tp, fs, 0, fg);
        }
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

/* Hover tooltip for the top-bar buttons. Drawn AFTER
   draw_tab_contents so the pane background painted below the tab
   bar doesn't cover it. Single tooltip per frame. */
static void draw_tab_bar_tooltip(Renderer *r, int win_w) {
    if (g_ui_mode != UI_NORMAL) return;
    Vector2 mp_tt = GetMousePosition();
    if (mp_tt.y < 0 || mp_tt.y >= TAB_BAR_H) return;
#ifdef __APPLE__
    const char *MOD = "Cmd";
#else
    const char *MOD = "Ctrl";
#endif
    /* Re-derive button x positions exactly the way tab_bar_hit_test
       does so the tooltip lines up with whichever rect produced the
       hover. */
    bool show_splits = split_buttons_visible();
    bool show_up     = upload_button_visible();
    bool show_dl     = download_button_visible();
    bool show_bcast  = bcast_button_visible();
    int help_x      = win_w - TAB_HELP_W;
    int split_h_x   = show_splits ? help_x - TAB_SPLIT_W     : help_x;
    int split_v_x   = show_splits ? split_h_x - TAB_SPLIT_W  : help_x;
    int bcast_x     = show_bcast ? split_v_x - TAB_BCAST_W : split_v_x;
    int snap_x      = bcast_x - TAB_SNAP_W;
    int rec_stop_x  = snap_x - TAB_REC_W;
    int rec_start_x = rec_stop_x - TAB_REC_W;
    int upload_x    = show_up ? rec_start_x - TAB_UPLOAD_W : rec_start_x;
    int dl_x        = show_dl ? upload_x - TAB_DOWNLOAD_W  : upload_x;
    int gear_x      = dl_x - TAB_GEAR_W;

    const char *label = NULL;
    int btn_x = -1, btn_w = 0;
    int mx = (int)mp_tt.x;
    char buf[96];
    if (mx >= 0 && mx < TAB_SSH_W) {
        snprintf(buf, sizeof(buf), "Connect to SSH host  (%s+Shift+T)", MOD);
        label = buf; btn_x = 0; btn_w = TAB_SSH_W;
    } else if (mx < TAB_SSH_W + TAB_PLUS_W) {
        snprintf(buf, sizeof(buf), "New tab  (%s+T)", MOD);
        label = buf; btn_x = TAB_SSH_W; btn_w = TAB_PLUS_W;
    } else if (mx >= help_x) {
        label = "Keyboard shortcuts";
        btn_x = help_x; btn_w = TAB_HELP_W;
    } else if (show_splits && mx >= split_h_x) {
        snprintf(buf, sizeof(buf), "Split horizontally  (%s+Shift+D)", MOD);
        label = buf; btn_x = split_h_x; btn_w = TAB_SPLIT_W;
    } else if (show_splits && mx >= split_v_x) {
        snprintf(buf, sizeof(buf), "Split vertically  (%s+D)", MOD);
        label = buf; btn_x = split_v_x; btn_w = TAB_SPLIT_W;
    } else if (show_bcast && mx >= bcast_x) {
        snprintf(buf, sizeof(buf),
                 g_broadcast_active
                   ? "Broadcast input: ON — click to stop  (%s+Shift+I)"
                   : "Broadcast input to all panes  (%s+Shift+I)",
                 MOD);
        label = buf; btn_x = bcast_x; btn_w = TAB_BCAST_W;
    } else if (mx >= snap_x) {
        snprintf(buf, sizeof(buf),
                 "Screenshot active pane  (%s+Shift+S)", MOD);
        label = buf; btn_x = snap_x; btn_w = TAB_SNAP_W;
    } else if (mx >= rec_stop_x) {
        label = g_rec.active ? "Stop recording"
                             : "(Stop) — start a recording first";
        btn_x = rec_stop_x; btn_w = TAB_REC_W;
    } else if (mx >= rec_start_x) {
        label = g_rec.active ? "(Start) — already recording"
                             : "Record active pane to .cast";
        btn_x = rec_start_x; btn_w = TAB_REC_W;
    } else if (show_up && mx >= upload_x) {
        label = "Upload file via SFTP";
        btn_x = upload_x; btn_w = TAB_UPLOAD_W;
    } else if (show_dl && mx >= dl_x) {
        label = "Download file via SFTP";
        btn_x = dl_x; btn_w = TAB_DOWNLOAD_W;
    } else if (mx >= gear_x) {
        snprintf(buf, sizeof(buf), "Settings  (%s+,)", MOD);
        label = buf; btn_x = gear_x; btn_w = TAB_GEAR_W;
    }
    if (!label || btn_x < 0) return;
    Font *f = (Font *)r->font_data;
    int tt_pad_x = 8, tt_pad_y = 5;
    int tt_fs = 12;
    Vector2 tsz = MeasureTextEx(*f, label, tt_fs, 0);
    int tt_w = (int)tsz.x + tt_pad_x * 2;
    int tt_h = (int)tsz.y + tt_pad_y * 2;
    int tt_x = btn_x + (btn_w - tt_w) / 2;
    if (tt_x < 4) tt_x = 4;
    if (tt_x + tt_w > win_w - 4) tt_x = win_w - 4 - tt_w;
    int tt_y = TAB_BAR_H + 4;
    DrawRectangle(tt_x, tt_y, tt_w, tt_h, (Color){10, 12, 18, 235});
    DrawRectangleLines(tt_x, tt_y, tt_w, tt_h,
                       (Color){125, 207, 255, 200});
    DrawTextEx(*f, label,
               (Vector2){tt_x + tt_pad_x, tt_y + tt_pad_y},
               tt_fs, 0, (Color){230, 235, 245, 255});
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

/* Scan ~/.ssh for SSH key pairs. Each `*.pub` file with a
   matching private file (same stem, no extension) is one entry.
   We sniff the algorithm from the first whitespace-delimited
   token of the .pub line ("ssh-ed25519 …", "ssh-rsa …", etc.)
   and call out to `ssh-keygen -lf <path>` for a fingerprint
   string. Best-effort throughout — bad entries are skipped, not
   fatal. */
static void ssh_keys_rescan(void) {
    g_ssh_keys_count = 0;
#ifndef _WIN32
    char dir[PATH_MAX];
    expand_home_path("~/.ssh", dir, sizeof(dir));
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (g_ssh_keys_count >= SSH_KEYS_MAX) break;
        const char *nm = de->d_name;
        size_t l = strlen(nm);
        if (l < 5 || strcmp(nm + l - 4, ".pub") != 0) continue;
        /* Skip anything starting with `.` — leftover backups. */
        if (nm[0] == '.') continue;

        SshKeyEntry *e = &g_ssh_keys[g_ssh_keys_count];
        memset(e, 0, sizeof(*e));
        size_t stem = l - 4;
        if (stem + 1 > sizeof(e->name)) continue;
        memcpy(e->name, nm, stem);
        e->name[stem] = 0;
        snprintf(e->pubpath,  sizeof(e->pubpath),  "%s/%s",     dir, nm);
        snprintf(e->privpath, sizeof(e->privpath), "%s/%s",     dir, e->name);
        struct stat st;
        e->has_private = (stat(e->privpath, &st) == 0);
        struct stat pst;
        e->mtime = (stat(e->pubpath, &pst) == 0) ? pst.st_mtime : 0;

        /* Read first line of the .pub file to extract the algo
           token ("ssh-ed25519", "ssh-rsa", …). */
        FILE *fp = fopen(e->pubpath, "r");
        if (fp) {
            char line[1024];
            if (fgets(line, sizeof(line), fp)) {
                char *sp = strchr(line, ' ');
                if (sp) *sp = 0;
                if (strncmp(line, "ssh-", 4) == 0) {
                    snprintf(e->algo, sizeof(e->algo), "%s", line + 4);
                } else {
                    snprintf(e->algo, sizeof(e->algo), "%s", line);
                }
            }
            fclose(fp);
        }

        /* Optional fingerprint via ssh-keygen -lf. We pipe through
           popen so a missing ssh-keygen doesn't crash. Output
           format: "256 SHA256:abc… user@host (ED25519)". */
        char cmd[PATH_MAX + 32];
        snprintf(cmd, sizeof(cmd),
                 "ssh-keygen -lf '%s' 2>/dev/null", e->pubpath);
        FILE *pp = popen(cmd, "r");
        if (pp) {
            char out[256];
            if (fgets(out, sizeof(out), pp)) {
                /* Trim trailing newline + the parenthetical algo
                   suffix that ssh-keygen always appends — saves
                   horizontal space in the row. */
                size_t ol = strlen(out);
                while (ol > 0 && (out[ol - 1] == '\n' || out[ol - 1] == '\r'))
                    out[--ol] = 0;
                snprintf(e->fingerprint, sizeof(e->fingerprint), "%s", out);
            }
            pclose(pp);
        }
        g_ssh_keys_count++;
    }
    closedir(d);

    /* Sort newest first by .pub mtime — keys generated in this
       session land at the top. Ties (or stat failures, both = 0) fall
       back to case-insensitive name order so the result is stable.
       Tiny n (≤ 32) — insertion sort. */
    for (int i = 1; i < g_ssh_keys_count; i++) {
        SshKeyEntry tmp = g_ssh_keys[i];
        int j = i - 1;
        while (j >= 0) {
            int cmp;
            if (g_ssh_keys[j].mtime != tmp.mtime) {
                cmp = (g_ssh_keys[j].mtime < tmp.mtime) ? 1 : -1;
            } else {
                cmp = strcasecmp(g_ssh_keys[j].name, tmp.name);
            }
            if (cmp <= 0) break;
            g_ssh_keys[j + 1] = g_ssh_keys[j];
            j--;
        }
        g_ssh_keys[j + 1] = tmp;
    }
#endif
}

/* Generate a fresh SSH key pair via libssh's PKI API and write
   the private + public files into ~/.ssh. No subprocess, no
   ssh-keygen dependency. Sets 0600 perms on the private file,
   0644 on the public file. Returns true on success; on failure
   writes a human-readable reason into `err`. */
#ifdef RBTERM_SSH
#include <libssh/libssh.h>
static bool ssh_keys_generate_native(const char *type_name,
                                     const char *file_stem,
                                     const char *passphrase,
                                     char *err, size_t errsz) {
    if (err && errsz) err[0] = 0;
    if (!file_stem || !*file_stem) {
        if (err && errsz) snprintf(err, errsz, "filename required");
        return false;
    }
    enum ssh_keytypes_e ktype = SSH_KEYTYPE_ED25519;
    int kparam = 0;
    if (type_name && strcmp(type_name, "rsa") == 0) {
        ktype = SSH_KEYTYPE_RSA;
        kparam = 4096;
    }
    /* libssh's pki_generate is the right entry point — for ED25519
       the parameter is ignored. */
    ssh_key key = NULL;
    if (ssh_pki_generate(ktype, kparam, &key) != SSH_OK || !key) {
        if (err && errsz) snprintf(err, errsz, "ssh_pki_generate failed");
        return false;
    }
    /* Resolve target paths under ~/.ssh, ensure the dir exists
       with the right perms (0700) since libssh just opens the
       file directly. */
    char ssh_dir[PATH_MAX], priv_path[PATH_MAX], pub_path[PATH_MAX];
    expand_home_path("~/.ssh", ssh_dir, sizeof(ssh_dir));
    mkdir_p(ssh_dir);
#ifndef _WIN32
    chmod(ssh_dir, 0700);
#endif
    snprintf(priv_path, sizeof(priv_path), "%s/%s",     ssh_dir, file_stem);
    snprintf(pub_path,  sizeof(pub_path),  "%s/%s.pub", ssh_dir, file_stem);

    /* Don't clobber an existing key. */
    {
        struct stat st;
        if (stat(priv_path, &st) == 0) {
            if (err && errsz)
                snprintf(err, errsz, "%s already exists", priv_path);
            ssh_key_free(key);
            return false;
        }
    }

    const char *pp = (passphrase && *passphrase) ? passphrase : NULL;
    if (ssh_pki_export_privkey_file(key, pp, NULL, NULL, priv_path) != SSH_OK) {
        if (err && errsz) snprintf(err, errsz, "writing private key failed");
        ssh_key_free(key);
        return false;
    }
    if (ssh_pki_export_pubkey_file(key, pub_path) != SSH_OK) {
        if (err && errsz) snprintf(err, errsz, "writing public key failed");
        unlink(priv_path);
        ssh_key_free(key);
        return false;
    }
#ifndef _WIN32
    chmod(priv_path, 0600);
    chmod(pub_path,  0644);
#endif
    ssh_key_free(key);
    return true;
}

/* Append a public key to ~/.ssh/authorized_keys on the remote
   host the saved profile points at. Uses libssh's normal
   connect / publickey_auto path (i.e. the user must already be
   able to log in via agent or another saved key). Once the exec
   channel is up we feed the pubkey in and invoke a small shell
   snippet that writes it with the right perms. */
static bool ssh_keys_install_native(const SshProfile *prof,
                                    const char *pubkey_text,
                                    char *err, size_t errsz) {
    if (err && errsz) err[0] = 0;
    if (!prof || !pubkey_text || !*pubkey_text) {
        if (err && errsz) snprintf(err, errsz, "missing host or key");
        return false;
    }
    ssh_session s = ssh_new();
    if (!s) {
        if (err && errsz) snprintf(err, errsz, "ssh_new failed");
        return false;
    }
    /* Same options stack the existing connect path uses so we
       pick up alias / HostName / User / Port from the user's
       ~/.ssh/config. Form-level overrides win. */
    ssh_options_set(s, SSH_OPTIONS_HOST, prof->name[0] ? prof->name : prof->hostname);
    ssh_options_parse_config(s, NULL);
    if (prof->hostname[0])
        ssh_options_set(s, SSH_OPTIONS_HOST, prof->hostname);
    if (prof->user[0])
        ssh_options_set(s, SSH_OPTIONS_USER, prof->user);
    if (prof->port > 0)
        ssh_options_set(s, SSH_OPTIONS_PORT, &prof->port);
    long timeout_s = 10;
    ssh_options_set(s, SSH_OPTIONS_TIMEOUT, &timeout_s);

    if (ssh_connect(s) != SSH_OK) {
        if (err && errsz) snprintf(err, errsz,
                                   "connect: %s", ssh_get_error(s));
        ssh_free(s);
        return false;
    }
    /* Trust-on-first-use: same policy as pty_ssh's connect path. */
    enum ssh_known_hosts_e hk = ssh_session_is_known_server(s);
    if (hk == SSH_KNOWN_HOSTS_OK) {
        /* fine */
    } else if (hk == SSH_KNOWN_HOSTS_UNKNOWN ||
               hk == SSH_KNOWN_HOSTS_NOT_FOUND) {
        ssh_session_update_known_hosts(s);
    } else {
        if (err && errsz) snprintf(err, errsz,
                                   "host key check failed (changed?)");
        ssh_disconnect(s);
        ssh_free(s);
        return false;
    }
    /* Empty-string passphrase prevents libssh / OpenSSL from falling
       back to a tty prompt when an encrypted ~/.ssh/id_* is loaded —
       on an unencrypted key it's a no-op, on an encrypted one we just
       skip it instead of leaving the user staring at an unresponsive
       modal while a hidden tty prompt waits in the launching shell. */
    if (ssh_userauth_publickey_auto(s, NULL, "") != SSH_AUTH_SUCCESS) {
        if (err && errsz) snprintf(err, errsz,
                                   "auth: %s", ssh_get_error(s));
        ssh_disconnect(s);
        ssh_free(s);
        return false;
    }

    ssh_channel ch = ssh_channel_new(s);
    if (!ch || ssh_channel_open_session(ch) != SSH_OK) {
        if (err && errsz) snprintf(err, errsz,
                                   "open channel: %s", ssh_get_error(s));
        if (ch) ssh_channel_free(ch);
        ssh_disconnect(s);
        ssh_free(s);
        return false;
    }
    /* Append-only writer with proper permissions. The pubkey is
       fed via stdin so we don't have to worry about shell-quoting
       the key bytes (which can contain special chars in the
       comment field). The grep guards against duplicate inserts
       on repeat installs. */
    const char *cmd =
        "umask 077 && mkdir -p ~/.ssh && "
        "kf=~/.ssh/authorized_keys && touch \"$kf\" && "
        "k=\"$(cat)\" && grep -qxF -- \"$k\" \"$kf\" || echo \"$k\" >> \"$kf\" ; "
        "chmod 600 \"$kf\"";
    if (ssh_channel_request_exec(ch, cmd) != SSH_OK) {
        if (err && errsz) snprintf(err, errsz,
                                   "exec: %s", ssh_get_error(s));
        ssh_channel_close(ch); ssh_channel_free(ch);
        ssh_disconnect(s); ssh_free(s);
        return false;
    }
    /* Feed the public key bytes as the command's stdin. */
    ssh_channel_write(ch, pubkey_text, (uint32_t)strlen(pubkey_text));
    if (pubkey_text[strlen(pubkey_text) - 1] != '\n') {
        ssh_channel_write(ch, "\n", 1);
    }
    ssh_channel_send_eof(ch);
    /* Drain stderr if anything came back. */
    char drain[256];
    while (ssh_channel_read_timeout(ch, drain, sizeof(drain), 1, 2000) > 0) { }
    int rc = ssh_channel_get_exit_status(ch);
    ssh_channel_close(ch);
    ssh_channel_free(ch);
    ssh_disconnect(s);
    ssh_free(s);
    if (rc != 0) {
        if (err && errsz) snprintf(err, errsz,
                                   "remote command exited %d", rc);
        return false;
    }
    return true;
}

/* Read a public-key file's text into out (NUL-terminated).
   Returns false on read error. */
static bool ssh_keys_read_pubfile(const char *path, char *out, size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = 0;
    FILE *fp = fopen(path, "r");
    if (!fp) return false;
    size_t n = fread(out, 1, cap - 1, fp);
    fclose(fp);
    out[n] = 0;
    return n > 0;
}
#else  /* RBTERM_SSH not defined — no-op stubs so callers don't need ifdefs. */
static bool ssh_keys_generate_native(const char *type_name,
                                     const char *file_stem,
                                     const char *passphrase,
                                     char *err, size_t errsz) {
    (void)type_name; (void)file_stem; (void)passphrase;
    if (err && errsz) snprintf(err, errsz, "SSH disabled in this build");
    return false;
}
static bool ssh_keys_install_native(const SshProfile *prof,
                                    const char *pubkey_text,
                                    char *err, size_t errsz) {
    (void)prof; (void)pubkey_text;
    if (err && errsz) snprintf(err, errsz, "SSH disabled in this build");
    return false;
}
static bool ssh_keys_read_pubfile(const char *path, char *out, size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = 0;
    FILE *fp = fopen(path, "r");
    if (!fp) return false;
    size_t n = fread(out, 1, cap - 1, fp);
    fclose(fp);
    out[n] = 0;
    return n > 0;
}
#endif

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
                const char *ldir_pfx     = "rbterm-log-dir:";
                const char *log_pfx      = "rbterm-log:";
                const char *color_pfx    = "rbterm-color:";
                const char *icwd_pfx     = "rbterm-init-cwd:";
                const char *icmd_pfx     = "rbterm-init-cmd:";
                const char *ccol_pfx     = "rbterm-cursor-color:";
                const char *hud_pfx      = "rbterm-hud:";
                const char *hud_pos_pfx  = "rbterm-hud-pos:";
                const char *hud_cpu_pfx  = "rbterm-hud-cpu:";
                /* Per-field "<name>=<show>,<color_idx>,<size>" comments,
                   one per field. Names mirror HudField order. */
                static const struct { const char *pfx; int idx; } hud_field_pfx[] = {
                    { "rbterm-hud-host:", 0 },
                    { "rbterm-hud-ip:",   1 },
                    { "rbterm-hud-load:", 2 },
                    { "rbterm-hud-mem:",  3 },
                    { "rbterm-hud-disk:", 4 },
                };
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
                } else if (strncmp(q, color_pfx, strlen(color_pfx)) == 0) {
                    q += strlen(color_pfx);
                    while (*q == ' ' || *q == '\t') q++;
                    trim_end(q);
                    strncpy(cur->color, q, sizeof(cur->color) - 1);
                    cur->color[sizeof(cur->color) - 1] = 0;
                } else if (strncmp(q, icwd_pfx, strlen(icwd_pfx)) == 0) {
                    q += strlen(icwd_pfx);
                    while (*q == ' ' || *q == '\t') q++;
                    trim_end(q);
                    strncpy(cur->init_cwd, q, sizeof(cur->init_cwd) - 1);
                    cur->init_cwd[sizeof(cur->init_cwd) - 1] = 0;
                } else if (strncmp(q, icmd_pfx, strlen(icmd_pfx)) == 0) {
                    q += strlen(icmd_pfx);
                    while (*q == ' ' || *q == '\t') q++;
                    trim_end(q);
                    strncpy(cur->init_cmd, q, sizeof(cur->init_cmd) - 1);
                    cur->init_cmd[sizeof(cur->init_cmd) - 1] = 0;
                } else if (strncmp(q, "rbterm-name:", 12) == 0) {
                    q += 12;
                    while (*q == ' ' || *q == '\t') q++;
                    trim_end(q);
                    strncpy(cur->display_name, q,
                            sizeof(cur->display_name) - 1);
                    cur->display_name[sizeof(cur->display_name) - 1] = 0;
                } else if (strncmp(q, "rbterm-layout:", 14) == 0) {
                    q += 14;
                    while (*q == ' ' || *q == '\t') q++;
                    trim_end(q);
                    strncpy(cur->layout, q, sizeof(cur->layout) - 1);
                    cur->layout[sizeof(cur->layout) - 1] = 0;
                } else if (strncmp(q, "rbterm-pane-", 12) == 0) {
                    /* `rbterm-pane-<N>-{cwd,cmd}: <value>` — leaf-
                       indexed init lines. Index must be 0..7. */
                    const char *r = q + 12;
                    if (*r >= '0' && *r <= '0' + (SSH_LAYOUT_MAX_PANES - 1) &&
                        r[1] == '-') {
                        int idx = *r - '0';
                        const char *kind = r + 2;
                        if (strncmp(kind, "cwd:", 4) == 0) {
                            const char *vv = kind + 4;
                            while (*vv == ' ' || *vv == '\t') vv++;
                            char tmp[256];
                            snprintf(tmp, sizeof(tmp), "%s", vv);
                            trim_end(tmp);
                            strncpy(cur->pane_cwds[idx], tmp,
                                    sizeof(cur->pane_cwds[idx]) - 1);
                            cur->pane_cwds[idx][sizeof(cur->pane_cwds[idx]) - 1] = 0;
                        } else if (strncmp(kind, "cmd:", 4) == 0) {
                            const char *vv = kind + 4;
                            while (*vv == ' ' || *vv == '\t') vv++;
                            char tmp[256];
                            snprintf(tmp, sizeof(tmp), "%s", vv);
                            trim_end(tmp);
                            strncpy(cur->pane_cmds[idx], tmp,
                                    sizeof(cur->pane_cmds[idx]) - 1);
                            cur->pane_cmds[idx][sizeof(cur->pane_cmds[idx]) - 1] = 0;
                        }
                    }
                } else if (strncmp(q, ccol_pfx, strlen(ccol_pfx)) == 0) {
                    q += strlen(ccol_pfx);
                    while (*q == ' ' || *q == '\t') q++;
                    trim_end(q);
                    strncpy(cur->cursor_color, q, sizeof(cur->cursor_color) - 1);
                    cur->cursor_color[sizeof(cur->cursor_color) - 1] = 0;
                /* Any rbterm-hud-* presence implies an override is in
                   effect. We seed the rest of cur->hud once on first
                   sight so unspecified fields take sensible defaults
                   instead of zero. */
                } else if (strncmp(q, hud_pos_pfx, strlen(hud_pos_pfx)) == 0) {
                    if (!cur->hud.override) {
                        cur->hud = hud_config_from_app_settings();
                        cur->hud.override = true;
                    }
                    q += strlen(hud_pos_pfx);
                    while (*q == ' ' || *q == '\t') q++;
                    trim_end(q);
                    if      (!strcmp(q, "tl")) cur->hud.pos = HUD_POS_TOP_LEFT;
                    else if (!strcmp(q, "tr")) cur->hud.pos = HUD_POS_TOP_RIGHT;
                    else if (!strcmp(q, "bl")) cur->hud.pos = HUD_POS_BOTTOM_LEFT;
                    else if (!strcmp(q, "br")) cur->hud.pos = HUD_POS_BOTTOM_RIGHT;
                } else if (strncmp(q, hud_cpu_pfx, strlen(hud_cpu_pfx)) == 0) {
                    if (!cur->hud.override) {
                        cur->hud = hud_config_from_app_settings();
                        cur->hud.override = true;
                    }
                    q += strlen(hud_cpu_pfx);
                    while (*q == ' ' || *q == '\t') q++;
                    trim_end(q);
                    cur->hud.show_cpu = (!strcmp(q, "on") || !strcmp(q, "true") || !strcmp(q, "1"));
                } else if (strncmp(q, hud_pfx, strlen(hud_pfx)) == 0) {
                    if (!cur->hud.override) {
                        cur->hud = hud_config_from_app_settings();
                        cur->hud.override = true;
                    }
                    q += strlen(hud_pfx);
                    while (*q == ' ' || *q == '\t') q++;
                    trim_end(q);
                    cur->hud.show = (!strcmp(q, "on") || !strcmp(q, "true") || !strcmp(q, "1"));
                } else if (strncmp(q, "rbterm-effects-", strlen("rbterm-effects-")) == 0) {
                    /* `# rbterm-effects-<key>: <value>` — visual-effect
                       override. First sighting flips effects_override
                       and seeds with a fresh defaults struct so any
                       fields the host doesn't override stay neutral
                       (rather than inheriting the global default,
                       which would be surprising for "phosphor: green"
                       set on a single host). */
                    if (!cur->effects_override) {
                        rec_effects_defaults(&cur->effects);
                        cur->effects_override = true;
                    }
                    const char *kk = q + strlen("rbterm-effects-");
                    const char *colon = strchr(kk, ':');
                    if (colon) {
                        char keybuf[24];
                        size_t klen = (size_t)(colon - kk);
                        if (klen >= sizeof(keybuf)) klen = sizeof(keybuf) - 1;
                        memcpy(keybuf, kk, klen);
                        keybuf[klen] = 0;
                        const char *vv = colon + 1;
                        while (*vv == ' ' || *vv == '\t') vv++;
                        char vbuf[64];
                        snprintf(vbuf, sizeof(vbuf), "%s", vv);
                        trim_end(vbuf);
                        rec_effects_set(&cur->effects, keybuf, vbuf);
                    }
                } else {
                    for (size_t hi = 0; hi < sizeof(hud_field_pfx) / sizeof(hud_field_pfx[0]); hi++) {
                        size_t pl = strlen(hud_field_pfx[hi].pfx);
                        if (strncmp(q, hud_field_pfx[hi].pfx, pl) != 0) continue;
                        if (!cur->hud.override) {
                            cur->hud = hud_config_from_app_settings();
                            cur->hud.override = true;
                        }
                        const char *r = q + pl;
                        while (*r == ' ' || *r == '\t') r++;
                        /* Format: <show>,<color_idx>,<size> — show is on/off,
                           color_idx is 0..HUD_PALETTE_COUNT-1, size is 10..18. */
                        char tok[64];
                        snprintf(tok, sizeof(tok), "%s", r);
                        trim_end(tok);
                        char *p1 = strchr(tok, ',');
                        if (p1) {
                            *p1++ = 0;
                            char *p2 = strchr(p1, ',');
                            if (p2) *p2++ = 0;
                            int idx = hud_field_pfx[hi].idx;
                            cur->hud.field_show[idx] =
                                (!strcmp(tok, "on") || !strcmp(tok, "true") || !strcmp(tok, "1"));
                            int ci = atoi(p1);
                            if (ci >= 0 && ci < HUD_PALETTE_COUNT)
                                cur->hud.field_color[idx] = ci;
                            if (p2) {
                                int sz = atoi(p2);
                                if (sz >= 10 && sz <= 18)
                                    cur->hud.field_size[idx] = sz;
                            }
                        }
                        break;
                    }
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
    strncpy(g_form.color, prof->color, sizeof(g_form.color) - 1);
    g_form.color[sizeof(g_form.color) - 1] = 0;
    strncpy(g_form.cursor_color, prof->cursor_color, sizeof(g_form.cursor_color) - 1);
    g_form.cursor_color[sizeof(g_form.cursor_color) - 1] = 0;
    strncpy(g_form.init_cwd, prof->init_cwd, sizeof(g_form.init_cwd) - 1);
    g_form.init_cwd[sizeof(g_form.init_cwd) - 1] = 0;
    strncpy(g_form.init_cmd, prof->init_cmd, sizeof(g_form.init_cmd) - 1);
    g_form.init_cmd[sizeof(g_form.init_cmd) - 1] = 0;
    strncpy(g_form.display_name, prof->display_name,
            sizeof(g_form.display_name) - 1);
    g_form.display_name[sizeof(g_form.display_name) - 1] = 0;
    strncpy(g_form.layout, prof->layout, sizeof(g_form.layout) - 1);
    g_form.layout[sizeof(g_form.layout) - 1] = 0;
    for (int i = 0; i < SSH_LAYOUT_MAX_PANES; i++) {
        strncpy(g_form.pane_cwds[i], prof->pane_cwds[i],
                sizeof(g_form.pane_cwds[i]) - 1);
        g_form.pane_cwds[i][sizeof(g_form.pane_cwds[i]) - 1] = 0;
        strncpy(g_form.pane_cmds[i], prof->pane_cmds[i],
                sizeof(g_form.pane_cmds[i]) - 1);
        g_form.pane_cmds[i][sizeof(g_form.pane_cmds[i]) - 1] = 0;
    }
    g_form.layout_status[0] = 0;
    /* If this profile has any HUD override, copy it; otherwise seed
       the form's per-host HUD from the global app settings so the
       user starts editing from a sensible baseline if they enable
       the override. */
    if (prof->hud.override) {
        g_form.hud = prof->hud;
    } else {
        g_form.hud = hud_config_from_app_settings();
        g_form.hud.override = false;
    }
    /* Visual effects: if the profile has any per-host override, lift
       it into the form (so the panel shows what's currently saved);
       otherwise seed with the global default + flag override=false
       so saving-without-changes doesn't accidentally promote the
       host to override mode. */
    if (prof->effects_override) {
        g_form.effects          = prof->effects;
        g_form.effects_override = true;
    } else {
        g_form.effects          = g_app_settings.effects;
        g_form.effects_override = false;
    }
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
    /* Seed the HUD config from the current app settings — if the user
       enables override on a fresh form they start from defaults that
       look like what they're already running with. */
    g_form.hud = hud_config_from_app_settings();
    g_form.hud.override = false;
    /* Visual effects: same idea — seed from the current global default
       so the panel looks "what I'd get without overriding" until the
       user actually moves a slider. */
    g_form.effects          = g_app_settings.effects;
    g_form.effects_override = false;
    form_undo_clear_all();
}

static void fonts_load(const char *current_path);

/* Open the SSH connect modal. Reloads ~/.ssh/config + the font
   list (so the per-host font picker is populated) and clears any
   leftover form state. */
static void ssh_form_open(void) {
    g_ui_mode = UI_SSH_FORM;
    g_ssh_form_tab = SSH_FORM_TAB_CONNECTION;
    ssh_form_clear();
    /* Refresh the saved-hosts list every time the modal opens so edits
       to ~/.ssh/config show up without restarting rbterm. */
    ssh_profiles_load();
    /* Ensure g_fonts is populated so the per-host font picker has
       entries even if Settings hasn't been opened yet. */
    if (g_font_count == 0)
        fonts_load(g_renderer ? g_renderer->font_path : NULL);
    /* If the active tab is an SSH session, auto-select the matching
       saved profile so the user's likely next action ("save layout"
       / "tweak this host") is one click away. Prefer the stored
       alias (handles two aliases pointing at the same backend) and
       fall back to (HostName, User, Port) for ad-hoc tabs that
       didn't come through a saved profile. */
    Tab *t = (g_active >= 0 && g_active < g_num_tabs) ? g_tabs[g_active] : NULL;
    if (t && t->is_ssh) {
        int matched = -1;
        if (t->ssh_alias[0]) {
            for (int i = 0; i < g_ssh_profile_count; i++) {
                if (strcmp(g_ssh_profiles[i].name, t->ssh_alias) == 0) {
                    matched = i;
                    break;
                }
            }
        }
        if (matched < 0 && t->ssh_host[0]) {
            int port = t->ssh_port > 0 ? t->ssh_port : 22;
            for (int i = 0; i < g_ssh_profile_count; i++) {
                const SshProfile *p = &g_ssh_profiles[i];
                const char *p_host = p->hostname[0] ? p->hostname : p->name;
                int p_port = p->port > 0 ? p->port : 22;
                if (strcmp(p_host, t->ssh_host) != 0) continue;
                if (port != p_port) continue;
                if (t->ssh_user[0] && p->user[0] &&
                    strcmp(t->ssh_user, p->user) != 0) continue;
                matched = i;
                break;
            }
        }
        if (matched >= 0) {
            g_ssh_list_selected = matched;
            ssh_form_apply_profile(&g_ssh_profiles[matched]);
        }
    }
}

/* Open the SFTP upload modal pre-populated with the active pane's
   tracked cwd as the remote path (falls back to "~/"). The system
   file picker is fired immediately so the common case — "open
   modal, pick file, hit upload" — collapses to one extra
   dialog. The modal stays open afterwards so the user can edit
   the remote path before committing. */
static void upload_form_open(void) {
    memset(&g_upload_form, 0, sizeof(g_upload_form));
    /* Default remote path: active pane's last-known cwd (set by
       OSC 7) if present, else home. */
    Tab *t = (g_active >= 0 && g_active < g_num_tabs) ? g_tabs[g_active] : NULL;
    Pane *ap = active_pane_of(t);
    if (ap && ap->cwd[0]) {
        size_t cl = strlen(ap->cwd);
        snprintf(g_upload_form.remote_path,
                 sizeof(g_upload_form.remote_path),
                 "%s%s", ap->cwd, (cl > 0 && ap->cwd[cl - 1] == '/') ? "" : "/");
    } else {
        snprintf(g_upload_form.remote_path,
                 sizeof(g_upload_form.remote_path), "~/");
    }
    /* Auto-fire the system file picker so the user lands on the
       modal with a file already chosen. They can still hit
       "Choose…" again to swap. */
#ifdef __APPLE__
    char picked[sizeof(g_upload_form.local_path)];
    if (mac_pick_open_file(picked, sizeof(picked))) {
        snprintf(g_upload_form.local_path,
                 sizeof(g_upload_form.local_path), "%s", picked);
    } else {
        /* User cancelled the picker — don't open the modal at all. */
        return;
    }
#endif
    g_upload_form.remote_focus = true;
    g_ui_mode = UI_SFTP_UPLOAD;
}

/* Forward decl — `rect_hit` is defined after the SSH-form layout
   helpers but the upload modal handlers need it earlier. */
static bool rect_hit(Rect r, int x, int y);

typedef struct {
    Rect modal;
    Rect choose_btn;
    Rect remote_field;
    Rect upload_btn;
    Rect cancel_btn;
} UploadFormLayout;

static UploadFormLayout upload_form_layout(int win_w, int win_h) {
    UploadFormLayout L = {0};
    int w = 640, h = 240;
    if (w > win_w - 40) w = win_w - 40;
    if (h > win_h - 40) h = win_h - 40;
    L.modal.x = (win_w - w) / 2;
    L.modal.y = (win_h - h) / 2;
    L.modal.w = w;
    L.modal.h = h;
    int pad = 22;
    int title_h = 38;
    int label_w = 100;
    int field_x = L.modal.x + pad + label_w;
    int field_w = w - 2 * pad - label_w;

    int row_y = L.modal.y + title_h + 14;
    /* Local-file row: caption is drawn left of a "Choose…" button
       that re-fires the picker. The picked path is rendered above
       in a non-interactive line. */
    L.choose_btn = (Rect){ field_x, row_y, 100, 28 };
    row_y += 28 + 14;
    L.remote_field = (Rect){ field_x, row_y, field_w, 28 };

    int btn_h = 32;
    int btn_y = L.modal.y + h - 22 - btn_h;
    L.upload_btn = (Rect){ L.modal.x + w - 22 - 110, btn_y, 110, btn_h };
    L.cancel_btn = (Rect){ L.upload_btn.x - 8 - 90,  btn_y,  90, btn_h };
    return L;
}

static void upload_form_submit(void) {
    g_upload_form.status[0] = 0;
    if (!g_upload_form.local_path[0]) {
        snprintf(g_upload_form.status, sizeof(g_upload_form.status),
                 "Choose a local file first");
        return;
    }
    Tab *t = (g_active >= 0 && g_active < g_num_tabs) ? g_tabs[g_active] : NULL;
    Pane *ap = active_pane_of(t);
    if (!ap) {
        snprintf(g_upload_form.status, sizeof(g_upload_form.status),
                 "no active pane");
        return;
    }
    /* Replace any existing upload — joining the previous worker is
       cheap because pty_upload_release sets cancel + waits. */
    if (ap->upload) {
        pty_upload_release(ap->upload);
        ap->upload = NULL;
    }
    char err[256] = {0};
    ap->upload = pty_upload_start(ap->pty,
                                  g_upload_form.local_path,
                                  g_upload_form.remote_path[0]
                                      ? g_upload_form.remote_path : "~/",
                                  err, sizeof(err));
    if (!ap->upload) {
        snprintf(g_upload_form.status, sizeof(g_upload_form.status),
                 "%s", err[0] ? err : "upload failed to start");
        return;
    }
    ap->upload_done_at = 0.0;
    g_ui_mode = UI_NORMAL;
}

static void upload_form_handle_mouse(UploadFormLayout L) {
    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) return;
    Vector2 mp = GetMousePosition();
    int mx = (int)mp.x, my = (int)mp.y;
    if (rect_hit(L.choose_btn, mx, my)) {
#ifdef __APPLE__
        char picked[sizeof(g_upload_form.local_path)];
        if (mac_pick_open_file(picked, sizeof(picked))) {
            snprintf(g_upload_form.local_path,
                     sizeof(g_upload_form.local_path), "%s", picked);
            g_upload_form.status[0] = 0;
        }
#endif
        g_upload_form.remote_focus = false;
        return;
    }
    if (rect_hit(L.remote_field, mx, my)) {
        g_upload_form.remote_focus = true;
        return;
    }
    if (rect_hit(L.upload_btn, mx, my)) {
        upload_form_submit();
        return;
    }
    if (rect_hit(L.cancel_btn, mx, my)) {
        g_ui_mode = UI_NORMAL;
        return;
    }
    /* Click anywhere outside controls drops keyboard focus from the
       remote-path field. */
    if (!rect_hit(L.modal, mx, my)) {
        g_ui_mode = UI_NORMAL;
        return;
    }
    g_upload_form.remote_focus = false;
}

static void upload_form_handle_keys(void) {
    if (IsKeyPressed(KEY_ESCAPE)) { g_ui_mode = UI_NORMAL; return; }
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
        upload_form_submit();
        return;
    }
    if (g_upload_form.remote_focus) {
        size_t len = strlen(g_upload_form.remote_path);
        if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) {
            if (len > 0) g_upload_form.remote_path[len - 1] = 0;
        }
        int cp;
        while ((cp = GetCharPressed()) != 0) {
            if (cp < 32 || cp >= 127) continue;
            if (len + 1 >= sizeof(g_upload_form.remote_path)) continue;
            g_upload_form.remote_path[len++] = (char)cp;
            g_upload_form.remote_path[len] = 0;
        }
    }
}

static void draw_upload_form(Renderer *r, int win_w, int win_h, UploadFormLayout L) {
    (void)win_w; (void)win_h;
    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(),
                  (Color){0, 0, 0, 150});
    DrawRectangle(L.modal.x, L.modal.y, L.modal.w, L.modal.h,
                  (Color){30, 34, 46, 255});
    DrawRectangleLines(L.modal.x, L.modal.y, L.modal.w, L.modal.h,
                       (Color){125, 207, 255, 220});
    Font *f = (Font *)r->font_data;
    DrawRectangle(L.modal.x + 1, L.modal.y + 1, L.modal.w - 2, 38,
                  (Color){38, 42, 58, 255});
    DrawTextEx(*f, "SFTP — Upload",
               (Vector2){L.modal.x + 20, L.modal.y + 11},
               16, 0, (Color){230, 232, 240, 255});

    /* Local-file caption + path display + Choose button. */
    DrawTextEx(*f, "Local file",
               (Vector2){L.modal.x + 22, L.choose_btn.y + 7},
               13, 0, (Color){180, 185, 200, 255});
    /* Pick button. */
    DrawRectangle(L.choose_btn.x, L.choose_btn.y,
                  L.choose_btn.w, L.choose_btn.h,
                  (Color){46, 52, 70, 255});
    DrawRectangleLines(L.choose_btn.x, L.choose_btn.y,
                       L.choose_btn.w, L.choose_btn.h,
                       (Color){125, 207, 255, 200});
    Vector2 csz = MeasureTextEx(*f, "Choose…", 13, 0);
    DrawTextEx(*f, "Choose…",
               (Vector2){L.choose_btn.x + (L.choose_btn.w - csz.x) / 2,
                         L.choose_btn.y + (L.choose_btn.h - csz.y) / 2},
               13, 0, (Color){230, 232, 240, 255});
    /* Path display next to the button. */
    const char *shown = g_upload_form.local_path[0]
                        ? g_upload_form.local_path : "(no file selected)";
    BeginScissorMode(L.choose_btn.x + L.choose_btn.w + 10,
                     L.choose_btn.y,
                     L.modal.x + L.modal.w - 22 - (L.choose_btn.x + L.choose_btn.w + 10),
                     L.choose_btn.h);
    DrawTextEx(*f, shown,
               (Vector2){L.choose_btn.x + L.choose_btn.w + 10,
                         L.choose_btn.y + 7},
               13, 0,
               g_upload_form.local_path[0] ? (Color){230, 232, 240, 255}
                                           : (Color){110, 115, 130, 255});
    EndScissorMode();

    /* Remote path field. */
    DrawTextEx(*f, "Remote path",
               (Vector2){L.modal.x + 22, L.remote_field.y + 7},
               13, 0, (Color){180, 185, 200, 255});
    DrawRectangle(L.remote_field.x, L.remote_field.y,
                  L.remote_field.w, L.remote_field.h,
                  (Color){22, 25, 34, 255});
    DrawRectangleLines(L.remote_field.x, L.remote_field.y,
                       L.remote_field.w, L.remote_field.h,
                       g_upload_form.remote_focus
                           ? (Color){125, 207, 255, 255}
                           : (Color){70, 74, 90, 255});
    BeginScissorMode(L.remote_field.x + 6, L.remote_field.y,
                     L.remote_field.w - 12, L.remote_field.h);
    DrawTextEx(*f, g_upload_form.remote_path,
               (Vector2){L.remote_field.x + 8, L.remote_field.y + 7},
               14, 0, (Color){230, 232, 240, 255});
    if (g_upload_form.remote_focus &&
        ((long long)(GetTime() * 2.0) & 1) == 0) {
        Vector2 vsz = MeasureTextEx(*f, g_upload_form.remote_path, 14, 0);
        DrawRectangle(L.remote_field.x + 8 + (int)vsz.x + 1,
                      L.remote_field.y + 6, 8, 16,
                      (Color){125, 207, 255, 255});
    }
    EndScissorMode();

    /* Status line (errors only). */
    if (g_upload_form.status[0]) {
        DrawTextEx(*f, g_upload_form.status,
                   (Vector2){L.modal.x + 22, L.upload_btn.y - 22},
                   13, 0, (Color){240, 120, 120, 255});
    }

    /* Buttons. */
    DrawRectangle(L.cancel_btn.x, L.cancel_btn.y, L.cancel_btn.w, L.cancel_btn.h,
                  (Color){48, 52, 66, 255});
    DrawRectangleLines(L.cancel_btn.x, L.cancel_btn.y, L.cancel_btn.w, L.cancel_btn.h,
                       (Color){150, 155, 170, 200});
    Vector2 xsz = MeasureTextEx(*f, "Cancel", 14, 0);
    DrawTextEx(*f, "Cancel",
               (Vector2){L.cancel_btn.x + (L.cancel_btn.w - xsz.x) / 2,
                         L.cancel_btn.y + (L.cancel_btn.h - xsz.y) / 2},
               14, 0, (Color){210, 215, 230, 255});

    DrawRectangle(L.upload_btn.x, L.upload_btn.y, L.upload_btn.w, L.upload_btn.h,
                  (Color){46, 92, 150, 255});
    DrawRectangleLines(L.upload_btn.x, L.upload_btn.y, L.upload_btn.w, L.upload_btn.h,
                       (Color){125, 207, 255, 220});
    Vector2 usz = MeasureTextEx(*f, "Upload", 14, 0);
    DrawTextEx(*f, "Upload",
               (Vector2){L.upload_btn.x + (L.upload_btn.w - usz.x) / 2,
                         L.upload_btn.y + (L.upload_btn.h - usz.y) / 2},
               14, 0, (Color){230, 240, 255, 255});
}

/* ---------------- SFTP download modal ---------------- */

typedef struct {
    Rect modal;
    Rect dir_field;
    Rect refresh_btn;
    Rect filter_field;
    Rect list;
    Rect download_btn;
    Rect cancel_btn;
} DownloadFormLayout;

/* Recompute the rects for the download modal. The modal is a bit
   larger than the upload one — needs to fit the directory listing.
   Layout: title bar | dir field + refresh | filter | list | btns. */
static DownloadFormLayout download_form_layout(int win_w, int win_h) {
    DownloadFormLayout L = {0};
    int w = 720, h = 520;
    if (w > win_w - 40) w = win_w - 40;
    if (h > win_h - 40) h = win_h - 40;
    L.modal.x = (win_w - w) / 2;
    L.modal.y = (win_h - h) / 2;
    L.modal.w = w;
    L.modal.h = h;
    int pad = 22;
    int title_h = 38;
    int field_x = L.modal.x + pad;
    int row_y = L.modal.y + title_h + 14;

    /* Directory bar: editable path + Refresh button. */
    int btn_w = 90, btn_h = 28;
    L.dir_field   = (Rect){ field_x, row_y, w - 2 * pad - btn_w - 8, btn_h };
    L.refresh_btn = (Rect){ L.dir_field.x + L.dir_field.w + 8,
                            row_y, btn_w, btn_h };
    row_y += btn_h + 10;
    L.filter_field = (Rect){ field_x, row_y, w - 2 * pad, btn_h };
    row_y += btn_h + 10;

    int footer_h = 32;
    int footer_y = L.modal.y + h - 22 - footer_h;
    L.list = (Rect){ field_x, row_y,
                     w - 2 * pad,
                     footer_y - row_y - 14 };

    L.download_btn = (Rect){ L.modal.x + w - 22 - 110, footer_y, 110, footer_h };
    L.cancel_btn   = (Rect){ L.download_btn.x - 8 - 90, footer_y, 90, footer_h };
    return L;
}

/* Refresh the directory listing for g_download_form.remote_dir. */
static void download_form_refresh(void) {
    if (g_download_form.entries) {
        pty_listdir_free(g_download_form.entries);
        g_download_form.entries = NULL;
        g_download_form.entry_count = 0;
    }
    g_download_form.selected = -1;
    g_download_form.scroll = 0;
    g_download_form.status[0] = 0;
    Tab *t = (g_active >= 0 && g_active < g_num_tabs) ? g_tabs[g_active] : NULL;
    Pane *ap = active_pane_of(t);
    if (!ap || !ap->pty) {
        snprintf(g_download_form.status, sizeof(g_download_form.status),
                 "no active pane");
        return;
    }
    char err[256] = {0};
    int n = 0;
    g_download_form.entries = pty_listdir(ap->pty, g_download_form.remote_dir,
                                          &n, err, sizeof(err));
    if (!g_download_form.entries) {
        snprintf(g_download_form.status, sizeof(g_download_form.status),
                 "%s", err[0] ? err : "listdir failed");
        g_download_form.entry_count = 0;
        return;
    }
    g_download_form.entry_count = n;
}

/* Filter predicate: returns true iff entry matches the live filter. */
static bool download_filter_match(const PtyDirEntry *e) {
    if (!g_download_form.filter[0]) return true;
    /* Case-insensitive substring match. */
    const char *needle = g_download_form.filter;
    const char *hay = e->name;
    size_t nl = strlen(needle);
    size_t hl = strlen(hay);
    if (nl > hl) return false;
    for (size_t i = 0; i + nl <= hl; i++) {
        bool ok = true;
        for (size_t j = 0; j < nl; j++) {
            char a = hay[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) { ok = false; break; }
        }
        if (ok) return true;
    }
    return false;
}

/* Compose the filtered view: out is filled with indices into
   g_download_form.entries[]; returns count. */
static int download_filtered_indices(int *out, int cap) {
    int n = 0;
    for (int i = 0; i < g_download_form.entry_count && n < cap; i++) {
        if (download_filter_match(&g_download_form.entries[i])) {
            out[n++] = i;
        }
    }
    return n;
}

/* True for the first frame after the download modal opens — used
   to swallow the same click that triggered the open so the
   "click-outside-closes" branch in download_form_handle_mouse
   doesn't immediately dismiss it. Cleared on the next frame's
   first call into the handler. */
static bool g_download_form_just_opened = false;

/* Open the download modal anchored on the active pane's tracked
   cwd (falls back to "~/"). */
static void download_form_open(void) {
    /* Free previous listing if any. */
    if (g_download_form.entries) {
        pty_listdir_free(g_download_form.entries);
    }
    memset(&g_download_form, 0, sizeof(g_download_form));
    g_download_form.selected = -1;
    Tab *t = (g_active >= 0 && g_active < g_num_tabs) ? g_tabs[g_active] : NULL;
    Pane *ap = active_pane_of(t);
    if (ap && ap->cwd[0]) {
        snprintf(g_download_form.remote_dir,
                 sizeof(g_download_form.remote_dir), "%s", ap->cwd);
    } else {
        snprintf(g_download_form.remote_dir,
                 sizeof(g_download_form.remote_dir), "~/");
    }
    g_download_form.filter_focus = false;
    g_download_form.dir_focus = false;
    g_ui_mode = UI_SFTP_DOWNLOAD;
    g_download_form_just_opened = true;
    download_form_refresh();
}

/* Build the dir-up path: "/a/b/c" → "/a/b", "/a" → "/", "~/foo" →
   "~/", "~" stays "~/". Result written into `out`. */
static void download_form_parent_dir(const char *in, char *out, size_t cap) {
    snprintf(out, cap, "%s", in);
    /* Strip any trailing slashes (except root). */
    size_t l = strlen(out);
    while (l > 1 && out[l - 1] == '/') { out[l - 1] = 0; l--; }
    /* Find last separator. */
    char *slash = strrchr(out, '/');
    if (!slash) {
        /* Bare alias like "~"; reset to "~/". */
        snprintf(out, cap, "~/");
        return;
    }
    if (slash == out) {
        /* "/foo" → "/". */
        out[1] = 0;
    } else {
        *slash = 0;
    }
}

static void download_form_submit_selected(void);

/* "Activate" the selected entry — fired by double-click and Enter.
   Folders navigate; files trigger a save dialog + download. The
   Download button uses download_form_submit_selected below, which
   downloads both. Two separate paths so that double-clicking a
   folder feels like a file manager rather than starting a big
   recursive transfer. */
static void download_form_activate_selected(void) {
    g_download_form.status[0] = 0;
    int idxs[1024];
    int fcount = download_filtered_indices(idxs, (int)(sizeof(idxs) / sizeof(idxs[0])));
    if (g_download_form.selected < 0 || g_download_form.selected >= fcount) {
        return;
    }
    PtyDirEntry *e = &g_download_form.entries[idxs[g_download_form.selected]];
    if (e->is_dir) {
        char next[sizeof(g_download_form.remote_dir)];
        if (strcmp(e->name, "..") == 0) {
            download_form_parent_dir(g_download_form.remote_dir,
                                     next, sizeof(next));
        } else {
            size_t cl = strlen(g_download_form.remote_dir);
            const char *sep = (cl > 0 && g_download_form.remote_dir[cl - 1] == '/') ? "" : "/";
            snprintf(next, sizeof(next), "%s%s%s",
                     g_download_form.remote_dir, sep, e->name);
        }
        snprintf(g_download_form.remote_dir,
                 sizeof(g_download_form.remote_dir), "%s", next);
        download_form_refresh();
        return;
    }
    /* File — fall through to the same code path the Download
       button takes, which prompts for a local destination and
       starts the SFTP read. */
    download_form_submit_selected();
}

/* Submit: fire NSSavePanel for local dest, kick off the download.
   Closes the modal on success; leaves it open with status set on
   error. */
static void download_form_submit_selected(void) {
    g_download_form.status[0] = 0;
    int idxs[1024];
    int fcount = download_filtered_indices(idxs, (int)(sizeof(idxs) / sizeof(idxs[0])));
    if (g_download_form.selected < 0 || g_download_form.selected >= fcount) {
        snprintf(g_download_form.status, sizeof(g_download_form.status),
                 "select a file first");
        return;
    }
    PtyDirEntry *e = &g_download_form.entries[idxs[g_download_form.selected]];
    /* Compose the absolute remote path once — same for the file
       and the directory branches. */
    char remote[4096];
    {
        size_t cl = strlen(g_download_form.remote_dir);
        const char *sep = (cl > 0 && g_download_form.remote_dir[cl - 1] == '/') ? "" : "/";
        snprintf(remote, sizeof(remote), "%s%s%s",
                 g_download_form.remote_dir, sep, e->name);
    }

    char local[4096] = {0};
    if (e->is_dir) {
        /* Special-case ".." — that's just "navigate up" in the
           listing UI; never download a parent dir. */
        if (strcmp(e->name, "..") == 0) {
            char next[sizeof(g_download_form.remote_dir)];
            download_form_parent_dir(g_download_form.remote_dir,
                                     next, sizeof(next));
            snprintf(g_download_form.remote_dir,
                     sizeof(g_download_form.remote_dir), "%s", next);
            download_form_refresh();
            return;
        }
        /* Pick a local *parent* directory; the remote folder name
           is appended so the user ends up with parent/<name>/<...>
           as a faithful mirror of the remote tree. */
        char parent[4096] = {0};
#ifdef __APPLE__
        char prompt[256];
        snprintf(prompt, sizeof(prompt),
                 "Choose where to save '%s'", e->name);
        if (!mac_pick_open_directory(prompt, parent, sizeof(parent))) {
            return;   /* user cancelled */
        }
#else
        const char *home = getenv("HOME");
        if (!home || !*home) home = ".";
        snprintf(parent, sizeof(parent), "%s/Downloads", home);
#endif
        snprintf(local, sizeof(local), "%s/%s", parent, e->name);
    } else {
        /* Plain file: NSSavePanel pre-fills with the remote name. */
#ifdef __APPLE__
        if (!mac_pick_save_file(e->name, local, sizeof(local))) {
            return;
        }
#else
        const char *home = getenv("HOME");
        if (!home || !*home) home = ".";
        snprintf(local, sizeof(local), "%s/Downloads/%s", home, e->name);
#endif
    }

    Tab *t = (g_active >= 0 && g_active < g_num_tabs) ? g_tabs[g_active] : NULL;
    Pane *ap = active_pane_of(t);
    if (!ap) {
        snprintf(g_download_form.status, sizeof(g_download_form.status),
                 "no active pane");
        return;
    }
    if (ap->download) {
        pty_download_release(ap->download);
        ap->download = NULL;
    }
    char err[256] = {0};
    ap->download = pty_download_start(ap->pty, remote, local, err, sizeof(err));
    if (!ap->download) {
        snprintf(g_download_form.status, sizeof(g_download_form.status),
                 "%s", err[0] ? err : "download failed to start");
        return;
    }
    ap->download_done_at = 0.0;
    g_ui_mode = UI_NORMAL;
}

static void download_form_close(void) {
    if (g_download_form.entries) {
        pty_listdir_free(g_download_form.entries);
        g_download_form.entries = NULL;
        g_download_form.entry_count = 0;
    }
    g_ui_mode = UI_NORMAL;
}

#define DL_ROW_H 22

static void download_form_handle_mouse(DownloadFormLayout L) {
    Vector2 mp_h = GetMousePosition();
    int hx = (int)mp_h.x, hy = (int)mp_h.y;
    if (rect_hit(L.list, hx, hy)) {
        float wheel = GetMouseWheelMove();
        if (wheel != 0.0f) {
            g_download_form.scroll -= (int)(wheel * 3.0f);
            if (g_download_form.scroll < 0) g_download_form.scroll = 0;
        }
    }
    /* Eat the opening click — the same MOUSE_BUTTON_LEFT press
       that fired download_form_open in the tab-bar handler is
       still seen by IsMouseButtonPressed in this frame, and the
       click coords (somewhere up in the tab bar) lie outside the
       modal rect, which would immediately close it. */
    if (g_download_form_just_opened) {
        g_download_form_just_opened = false;
        return;
    }
    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) return;
    Vector2 mp = GetMousePosition();
    int mx = (int)mp.x, my = (int)mp.y;

    if (rect_hit(L.dir_field, mx, my)) {
        g_download_form.dir_focus = true;
        g_download_form.filter_focus = false;
        return;
    }
    if (rect_hit(L.refresh_btn, mx, my)) {
        download_form_refresh();
        return;
    }
    if (rect_hit(L.filter_field, mx, my)) {
        g_download_form.filter_focus = true;
        g_download_form.dir_focus = false;
        return;
    }
    if (rect_hit(L.list, mx, my)) {
        g_download_form.dir_focus = false;
        g_download_form.filter_focus = false;
        int rel = (my - L.list.y) / DL_ROW_H + g_download_form.scroll;
        int idxs[1024];
        int fcount = download_filtered_indices(idxs, (int)(sizeof(idxs) / sizeof(idxs[0])));
        if (rel < 0 || rel >= fcount) return;
        /* Single-click selects; double-click activates (descends
           into a directory or fires the save dialog for a file). */
        static double last_click_t = -1;
        static int    last_click_i = -1;
        if (g_download_form.selected == rel &&
            GetTime() - last_click_t < 0.45 &&
            last_click_i == rel) {
            download_form_activate_selected();
            last_click_i = -1;
            return;
        }
        g_download_form.selected = rel;
        last_click_t = GetTime();
        last_click_i = rel;
        return;
    }
    if (rect_hit(L.download_btn, mx, my)) {
        download_form_submit_selected();
        return;
    }
    if (rect_hit(L.cancel_btn, mx, my)) {
        download_form_close();
        return;
    }
    if (!rect_hit(L.modal, mx, my)) {
        download_form_close();
        return;
    }
    g_download_form.dir_focus = false;
    g_download_form.filter_focus = false;
}

static void download_form_handle_keys(void) {
    if (IsKeyPressed(KEY_ESCAPE)) { download_form_close(); return; }
    int idxs[1024];
    int fcount = download_filtered_indices(idxs, (int)(sizeof(idxs) / sizeof(idxs[0])));
    if (IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN)) {
        if (fcount > 0) {
            g_download_form.selected =
                (g_download_form.selected < 0) ? 0
                : (g_download_form.selected + 1) % fcount;
        }
        return;
    }
    if (IsKeyPressed(KEY_UP) || IsKeyPressedRepeat(KEY_UP)) {
        if (fcount > 0) {
            g_download_form.selected =
                (g_download_form.selected <= 0) ? fcount - 1
                : g_download_form.selected - 1;
        }
        return;
    }
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
        if (g_download_form.dir_focus) {
            download_form_refresh();
            g_download_form.dir_focus = false;
        } else {
            download_form_activate_selected();
        }
        return;
    }

    /* Text editing for whichever field has focus. */
    char *buf = NULL;
    size_t cap = 0;
    if (g_download_form.dir_focus) {
        buf = g_download_form.remote_dir;
        cap = sizeof(g_download_form.remote_dir);
    } else if (g_download_form.filter_focus) {
        buf = g_download_form.filter;
        cap = sizeof(g_download_form.filter);
    }
    if (!buf) return;
    size_t len = strlen(buf);
    if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) {
        if (len > 0) buf[len - 1] = 0;
        return;
    }
    int cp;
    while ((cp = GetCharPressed()) != 0) {
        if (cp < 32 || cp >= 127) continue;
        if (len + 1 >= cap) continue;
        buf[len++] = (char)cp;
        buf[len] = 0;
    }
}

static void format_size(uint64_t bytes, char *out, size_t cap) {
    if (bytes < 1024)            snprintf(out, cap, "%llu B",  (unsigned long long)bytes);
    else if (bytes < 1024 * 1024) snprintf(out, cap, "%.1f KB", (double)bytes / 1024.0);
    else if (bytes < (uint64_t)1024 * 1024 * 1024)
                                  snprintf(out, cap, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    else                          snprintf(out, cap, "%.1f GB",
                                          (double)bytes / (1024.0 * 1024.0 * 1024.0));
}

static void draw_download_form(Renderer *r, int win_w, int win_h, DownloadFormLayout L) {
    (void)win_w; (void)win_h;
    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(),
                  (Color){0, 0, 0, 150});
    DrawRectangle(L.modal.x, L.modal.y, L.modal.w, L.modal.h,
                  (Color){30, 34, 46, 255});
    DrawRectangleLines(L.modal.x, L.modal.y, L.modal.w, L.modal.h,
                       (Color){125, 207, 255, 220});
    Font *f = (Font *)r->font_data;
    DrawRectangle(L.modal.x + 1, L.modal.y + 1, L.modal.w - 2, 38,
                  (Color){38, 42, 58, 255});
    DrawTextEx(*f, "SFTP — Download",
               (Vector2){L.modal.x + 20, L.modal.y + 11},
               16, 0, (Color){230, 232, 240, 255});

    /* Directory field. */
    DrawRectangle(L.dir_field.x, L.dir_field.y, L.dir_field.w, L.dir_field.h,
                  (Color){22, 25, 34, 255});
    DrawRectangleLines(L.dir_field.x, L.dir_field.y, L.dir_field.w, L.dir_field.h,
                       g_download_form.dir_focus ? (Color){125, 207, 255, 255}
                                                  : (Color){70, 74, 90, 255});
    BeginScissorMode(L.dir_field.x + 6, L.dir_field.y,
                     L.dir_field.w - 12, L.dir_field.h);
    DrawTextEx(*f, g_download_form.remote_dir,
               (Vector2){L.dir_field.x + 8, L.dir_field.y + 7},
               14, 0, (Color){230, 232, 240, 255});
    if (g_download_form.dir_focus &&
        ((long long)(GetTime() * 2.0) & 1) == 0) {
        Vector2 vsz = MeasureTextEx(*f, g_download_form.remote_dir, 14, 0);
        DrawRectangle(L.dir_field.x + 8 + (int)vsz.x + 1,
                      L.dir_field.y + 6, 8, 16,
                      (Color){125, 207, 255, 255});
    }
    EndScissorMode();
    /* Refresh button. */
    DrawRectangle(L.refresh_btn.x, L.refresh_btn.y,
                  L.refresh_btn.w, L.refresh_btn.h,
                  (Color){46, 52, 70, 255});
    DrawRectangleLines(L.refresh_btn.x, L.refresh_btn.y,
                       L.refresh_btn.w, L.refresh_btn.h,
                       (Color){125, 207, 255, 200});
    Vector2 rsz = MeasureTextEx(*f, "Refresh", 13, 0);
    DrawTextEx(*f, "Refresh",
               (Vector2){L.refresh_btn.x + (L.refresh_btn.w - rsz.x) / 2,
                         L.refresh_btn.y + (L.refresh_btn.h - rsz.y) / 2},
               13, 0, (Color){230, 232, 240, 255});

    /* Filter field. */
    DrawRectangle(L.filter_field.x, L.filter_field.y,
                  L.filter_field.w, L.filter_field.h,
                  (Color){22, 25, 34, 255});
    DrawRectangleLines(L.filter_field.x, L.filter_field.y,
                       L.filter_field.w, L.filter_field.h,
                       g_download_form.filter_focus ? (Color){125, 207, 255, 255}
                                                     : (Color){70, 74, 90, 255});
    BeginScissorMode(L.filter_field.x + 6, L.filter_field.y,
                     L.filter_field.w - 12, L.filter_field.h);
    if (g_download_form.filter[0]) {
        DrawTextEx(*f, g_download_form.filter,
                   (Vector2){L.filter_field.x + 8, L.filter_field.y + 7},
                   14, 0, (Color){230, 232, 240, 255});
    } else {
        DrawTextEx(*f, "(filter — type to narrow the list)",
                   (Vector2){L.filter_field.x + 8, L.filter_field.y + 7},
                   14, 0, (Color){110, 115, 130, 255});
    }
    EndScissorMode();

    /* Listing. */
    DrawRectangle(L.list.x, L.list.y, L.list.w, L.list.h,
                  (Color){22, 25, 34, 255});
    DrawRectangleLines(L.list.x, L.list.y, L.list.w, L.list.h,
                       (Color){70, 74, 90, 255});
    int idxs[1024];
    int fcount = download_filtered_indices(idxs, (int)(sizeof(idxs) / sizeof(idxs[0])));
    int visible = L.list.h / DL_ROW_H;
    int max_scroll = fcount - visible;
    if (max_scroll < 0) max_scroll = 0;
    if (g_download_form.scroll > max_scroll) g_download_form.scroll = max_scroll;
    /* Auto-scroll so the selected row stays in view. */
    if (g_download_form.selected >= 0) {
        if (g_download_form.selected < g_download_form.scroll)
            g_download_form.scroll = g_download_form.selected;
        if (g_download_form.selected >= g_download_form.scroll + visible)
            g_download_form.scroll = g_download_form.selected - visible + 1;
    }

    BeginScissorMode(L.list.x + 2, L.list.y + 2,
                     L.list.w - 4, L.list.h - 4);
    int size_col_x = L.list.x + L.list.w - 110;
    for (int i = 0; i < fcount; i++) {
        int row_y = L.list.y + (i - g_download_form.scroll) * DL_ROW_H;
        if (row_y + DL_ROW_H < L.list.y || row_y > L.list.y + L.list.h) continue;
        const PtyDirEntry *e = &g_download_form.entries[idxs[i]];
        bool sel = (i == g_download_form.selected);
        if (sel) {
            DrawRectangle(L.list.x + 2, row_y, L.list.w - 4, DL_ROW_H,
                          (Color){46, 62, 90, 220});
        }
        char display[300];
        if (e->is_dir) snprintf(display, sizeof(display), "%s/", e->name);
        else           snprintf(display, sizeof(display), "%s",  e->name);
        Color name_col = e->is_dir ? (Color){180, 220, 255, 255}
                                   : (Color){230, 232, 240, 255};
        DrawTextEx(*f, display,
                   (Vector2){L.list.x + 10, row_y + 3},
                   13, 0, name_col);
        if (!e->is_dir) {
            char sz[24];
            format_size(e->size, sz, sizeof(sz));
            DrawTextEx(*f, sz,
                       (Vector2){size_col_x, row_y + 3},
                       12, 0, (Color){170, 175, 195, 255});
        }
    }
    EndScissorMode();

    /* Status / error line. */
    if (g_download_form.status[0]) {
        DrawTextEx(*f, g_download_form.status,
                   (Vector2){L.modal.x + 22, L.cancel_btn.y - 22},
                   13, 0, (Color){240, 120, 120, 255});
    } else if (fcount == 0 && g_download_form.entry_count == 0) {
        DrawTextEx(*f, "(empty or still loading)",
                   (Vector2){L.modal.x + 22, L.cancel_btn.y - 22},
                   13, 0, (Color){140, 145, 160, 255});
    }

    /* Buttons. */
    DrawRectangle(L.cancel_btn.x, L.cancel_btn.y, L.cancel_btn.w, L.cancel_btn.h,
                  (Color){48, 52, 66, 255});
    DrawRectangleLines(L.cancel_btn.x, L.cancel_btn.y, L.cancel_btn.w, L.cancel_btn.h,
                       (Color){150, 155, 170, 200});
    Vector2 xsz = MeasureTextEx(*f, "Close", 14, 0);
    DrawTextEx(*f, "Close",
               (Vector2){L.cancel_btn.x + (L.cancel_btn.w - xsz.x) / 2,
                         L.cancel_btn.y + (L.cancel_btn.h - xsz.y) / 2},
               14, 0, (Color){210, 215, 230, 255});
    DrawRectangle(L.download_btn.x, L.download_btn.y,
                  L.download_btn.w, L.download_btn.h,
                  (Color){46, 92, 150, 255});
    DrawRectangleLines(L.download_btn.x, L.download_btn.y,
                       L.download_btn.w, L.download_btn.h,
                       (Color){125, 207, 255, 220});
    Vector2 dsz = MeasureTextEx(*f, "Download", 14, 0);
    DrawTextEx(*f, "Download",
               (Vector2){L.download_btn.x + (L.download_btn.w - dsz.x) / 2,
                         L.download_btn.y + (L.download_btn.h - dsz.y) / 2},
               14, 0, (Color){230, 240, 255, 255});
}

static char *form_buf(int field, size_t *cap) {
    switch (field) {
    case F_NAME:  *cap = sizeof(g_form.name);  return g_form.name;
    case F_HOST:  *cap = sizeof(g_form.host);  return g_form.host;
    case F_PORT:  *cap = sizeof(g_form.port);  return g_form.port;
    case F_USER:  *cap = sizeof(g_form.user);  return g_form.user;
    case F_PASS:  *cap = sizeof(g_form.pass);  return g_form.pass;
    case F_KEY:      *cap = sizeof(g_form.key);      return g_form.key;
    case F_INIT_CWD: *cap = sizeof(g_form.init_cwd); return g_form.init_cwd;
    case F_INIT_CMD: *cap = sizeof(g_form.init_cmd); return g_form.init_cmd;
    case F_DNAME:    *cap = sizeof(g_form.display_name); return g_form.display_name;
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
    int w = 860, h = 720;
    if (w > win_w - 40) w = win_w - 40;
    if (h > win_h - 40) h = win_h - 40;
    L.modal.x = (win_w - w) / 2;
    L.modal.y = (win_h - h) / 2;
    L.modal.w = w;
    L.modal.h = h;

    int pad = 22;
    int title_h = 46;

    /* Form-tab bar across the top of the modal, mirroring the
       Settings modal layout pattern. The host list sidebar starts
       below this so all four tabs share the same content area. */
    int tab_bar_y = L.modal.y + title_h + 6;
    int tab_bar_h = 28;
    {
        int gap = 4;
        int total_w = w - 2 * 14;
        int bw = (total_w - (SSH_FORM_TAB_COUNT - 1) * gap) / SSH_FORM_TAB_COUNT;
        int bx = L.modal.x + 14;
        for (int i = 0; i < SSH_FORM_TAB_COUNT; i++)
            L.form_tab[i] = (Rect){ bx + i * (bw + gap), tab_bar_y, bw, tab_bar_h };
    }

    int list_w = (g_ssh_profile_count > 0) ? 210 : 0;
    /* 28 px below the tab bar leaves room for the "Saved hosts (~/.ssh/config)"
       caption that sits 18 px above the list — at the previous 14 px gap
       the caption overlapped the Connection tab button. */
    int content_top = tab_bar_y + tab_bar_h + 28;
    if (list_w > 0) {
        L.list.x = L.modal.x + pad;
        L.list.y = content_top;
        L.list.w = list_w;
        L.list.h = h - (content_top - L.modal.y) - pad - 40;   /* leaves room for buttons */
    }

    int form_x = L.modal.x + pad + list_w + (list_w > 0 ? 14 : 0);
    int label_w = 100;
    int field_x = form_x + label_w;
    int field_w = L.modal.x + w - pad - field_x;
    int field_h = 28;

    /* CONNECTION tab — six text fields stacked. */
    if (g_ssh_form_tab == SSH_FORM_TAB_CONNECTION) {
        int y = content_top;
        int pick_w = 28;
        for (int i = 0; i < F_TEXT_FIELDS; i++) {
            int fw = field_w;
            if (i == F_KEY) fw -= pick_w + 4;  /* leave room for ▼ */
            L.field[i].x = field_x;
            L.field[i].y = y;
            L.field[i].w = fw;
            L.field[i].h = field_h;
            if (i == F_KEY) {
                L.key_pick_btn = (Rect){ field_x + fw + 4, y, pick_w, field_h };
            }
            y += field_h + 8;
        }
        /* Dropdown opens below the F_KEY field, full width. Height
           caps at 6 rows; click handler scrolls when more. */
        int kr_w = field_w;
        int kr_h = 6 * 22 + 4;
        L.key_pick_list = (Rect){
            field_x,
            L.field[F_KEY].y + field_h + 2,
            kr_w, kr_h
        };
        /* Save-layout button below the field stack. Captures the
           active SSH tab's tree into the host's stanza. */
        L.save_layout = (Rect){ field_x, y + 6, 220, field_h };
    }

    /* APPEARANCE tab — theme/font pickers, font-size row, cursor
       style row, and the tab-accent colour swatches. */
    if (g_ssh_form_tab == SSH_FORM_TAB_APPEARANCE) {
        int picker_h = 130;
        int picker_gap = 12;
        int picker_w = (field_w - picker_gap) / 2;
        int picker_y = content_top;
        L.theme_list = (Rect){ field_x,                          picker_y, picker_w, picker_h };
        L.font_list  = (Rect){ field_x + picker_w + picker_gap,   picker_y, picker_w, picker_h };

        int fs_row_y = picker_y + picker_h + 10;
        int fs_btn = 28;
        L.fs_val = (Rect){ field_x,                fs_row_y, 66, fs_btn };
        L.fs_dec = (Rect){ field_x + 74,           fs_row_y, fs_btn, fs_btn };
        L.fs_inc = (Rect){ field_x + 74 + fs_btn + 6, fs_row_y, fs_btn, fs_btn };

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
        int swatch_row_y = cur_row_y + 30 + 16;
        int n = SSH_COLOR_PRESET_COUNT + 1;
        int gap = 4;
        int sw = (field_w - (n - 1) * gap) / n;
        int sh = 28;
        for (int i = 0; i < n; i++) {
            L.color_swatch[i] = (Rect){
                field_x + i * (sw + gap), swatch_row_y, sw, sh };
        }
        int cur_swatch_y = swatch_row_y + sh + 14;
        for (int i = 0; i < n; i++) {
            L.cur_color_swatch[i] = (Rect){
                field_x + i * (sw + gap), cur_swatch_y, sw, sh };
        }
    }

    /* LOGGING tab — tri-state mode pill + per-host log directory. */
    if (g_ssh_form_tab == SSH_FORM_TAB_LOGGING) {
        int log_row_y = content_top;
        int log_btn_h = 28;
        int gap_log = 6;
        int bw = (field_w - 2 * gap_log) / 3;
        L.log_inherit = (Rect){ field_x,                       log_row_y, bw, log_btn_h };
        L.log_on      = (Rect){ field_x + (bw + gap_log),       log_row_y, bw, log_btn_h };
        L.log_off     = (Rect){ field_x + 2 * (bw + gap_log),   log_row_y, bw, log_btn_h };
        int logdir_row_y = log_row_y + 28 + 14;
        L.log_dir = (Rect){ field_x, logdir_row_y, field_w, 28 };
    }

    /* HUD tab — per-host override toggle, then the same controls
       Settings → HUD has but writing into g_form.hud. Layout mirrors
       Settings → HUD so muscle memory carries over. */
    if (g_ssh_form_tab == SSH_FORM_TAB_HUD) {
        int row_y = content_top;
        int btn = 28;
        L.hud_override_btn = (Rect){ field_x, row_y, 200, btn };
        row_y += btn + 14;
        L.hud_toggle = (Rect){ field_x, row_y, 110, btn };
        row_y += btn + 12;
        int corner_w = 90, corner_h = btn, corner_gap = 6;
        L.hud_pos_tl = (Rect){ field_x,                              row_y, corner_w, corner_h };
        L.hud_pos_tr = (Rect){ field_x + (corner_w + corner_gap),    row_y, corner_w, corner_h };
        row_y += corner_h + corner_gap;
        L.hud_pos_bl = (Rect){ field_x,                              row_y, corner_w, corner_h };
        L.hud_pos_br = (Rect){ field_x + (corner_w + corner_gap),    row_y, corner_w, corner_h };
        row_y += btn + 14;
        int show_w = 78, swatch_w = 26, sz_w = 22, val_w = 28;
        int gap_x = 4;
        int row_h = btn;
        for (int fi = 0; fi < HUD_FIELD_COUNT; fi++) {
            int x = field_x;
            L.hud_show_btn[fi]  = (Rect){ x, row_y, show_w,   row_h }; x += show_w   + gap_x;
            L.hud_color_btn[fi] = (Rect){ x, row_y, swatch_w, row_h }; x += swatch_w + gap_x;
            L.hud_size_dec[fi]  = (Rect){ x, row_y, sz_w,     row_h }; x += sz_w     + gap_x;
            L.hud_size_val[fi]  = (Rect){ x, row_y, val_w,    row_h }; x += val_w    + gap_x;
            L.hud_size_inc[fi]  = (Rect){ x, row_y, sz_w,     row_h };
            row_y += row_h + 4;
        }
        row_y += 8;
        L.hud_cpu_toggle = (Rect){ field_x, row_y, 200, btn };
    }

    if (g_ssh_form_tab == SSH_FORM_TAB_EFFECTS) {
        /* Override toggle, six sliders in two columns, Decay slider,
           Phosphor pill row, Preset pill row. Same column geometry
           as the Settings → Effects tab so the layouts feel familiar. */
        int row_y = content_top;
        int btn = 28;
        L.efx_override_btn = (Rect){ field_x, row_y, 200, btn };
        row_y += btn + 14;
        int row_h = 28;
        int row_gap = 10;
        int col_gap = 28;
        int half_w  = (field_w - col_gap) / 2;
        int label_off = 96;          /* draw code uses rr.x - 96 for label */
        int value_w   = 56;
        int slider_w = half_w - label_off - value_w;
        if (slider_w < 80) slider_w = 80;
        int left_slider_x  = field_x + label_off;
        int right_slider_x = field_x + half_w + col_gap + label_off;
        for (int i = 0; i < 3; i++) {
            int yy = row_y + i * (row_h + row_gap);
            L.efx_slider[k_efx_left_col[i]]  = (Rect){ left_slider_x,  yy, slider_w, row_h };
            L.efx_slider[k_efx_right_col[i]] = (Rect){ right_slider_x, yy, slider_w, row_h };
        }
        int decay_y = row_y + 3 * (row_h + row_gap);
        L.efx_decay = (Rect){ left_slider_x, decay_y, slider_w, row_h };
        int phos_y = decay_y + row_h + row_gap + 4;
        int phos_track_w = field_w - label_off;
        int phos_btn_w = (phos_track_w - 3 * 6) / PHOSPHOR_COUNT;
        for (int i = 0; i < PHOSPHOR_COUNT; i++)
            L.efx_phos[i] = (Rect){ left_slider_x + i * (phos_btn_w + 6),
                                    phos_y, phos_btn_w, row_h };
        /* Preset grid — same wrap as Settings → Effects so the layout
           feels familiar. Cols pinned at 5; rows derive from the
           preset count. */
        const int PRESETS_COLS = 5;
        int preset_y = phos_y + row_h + 12;
        int preset_btn_w = (field_w - (PRESETS_COLS - 1) * 6) / PRESETS_COLS;
        for (int i = 0; i < EFX_PRESET_COUNT; i++) {
            int rrow = i / PRESETS_COLS;
            int rcol = i % PRESETS_COLS;
            L.efx_preset[i] = (Rect){ field_x + rcol * (preset_btn_w + 6),
                                       preset_y + rrow * (row_h + 4),
                                       preset_btn_w, row_h };
        }
    }

    /* Buttons: [New] ... [Connect] [Delete] [Save] [Cancel].
       Delete has zero size when no saved host is selected so it can't
       receive clicks; everything else keeps its position. */
    int btn_h = 32;
    /* Auth errors with the "Tried: agent / id_ed25519 / …" detail can
       wrap to several lines. Reserve up to 4 lines (≈64 px) when an
       error is set so the button bar moves out of the way. Status
       (success) line is always single. */
    int err_reserve = g_form.error[0] ? 64 : (g_form_status[0] ? 24 : 0);
    int btn_y = L.modal.y + L.modal.h - btn_h - pad - err_reserve;
    int new_w = 70, connect_w = 110, del_w = 80, clone_w = 80, save_w = 90, cancel_w = 96;
    int test_w = 80;
    int gap = 8;
    /* Both Delete and Clone require a saved host to be selected (i.e.
       the form is editing an existing stanza); they vanish to zero
       width otherwise. */
    bool has_saved = (g_ssh_list_selected >= 0 && g_form.name[0]);
    int right_edge = L.modal.x + L.modal.w - pad;
    L.cancel.w   = cancel_w;  L.cancel.h   = btn_h;
    L.save.w     = save_w;    L.save.h     = btn_h;
    L.connect.w  = connect_w; L.connect.h  = btn_h;
    L.testbtn.w  = test_w;    L.testbtn.h  = btn_h;
    L.newbtn.w   = new_w;     L.newbtn.h   = btn_h;
    L.delbtn.w   = has_saved ? del_w   : 0;  L.delbtn.h   = btn_h;
    L.clonebtn.w = has_saved ? clone_w : 0;  L.clonebtn.h = btn_h;
    L.cancel.x   = right_edge - cancel_w;
    L.save.x     = L.cancel.x - gap - save_w;
    L.delbtn.x   = has_saved ? (L.save.x - gap - del_w) : 0;
    L.clonebtn.x = has_saved
                   ? (L.delbtn.x - gap - clone_w)
                   : 0;
    int connect_anchor = has_saved
                          ? L.clonebtn.x
                          : L.save.x;
    L.connect.x = connect_anchor - gap - connect_w;
    L.testbtn.x = L.connect.x - gap - test_w;
    L.newbtn.x  = L.modal.x + pad + list_w + (list_w > 0 ? 14 : 0);
    /* "New" floats one row above the main button bar so it reads as a
       form-level reset, not a commit-style action. */
    L.newbtn.y  = btn_y - btn_h - 10;
    L.cancel.y = L.save.y = L.connect.y = L.delbtn.y = L.clonebtn.y = L.testbtn.y = btn_y;
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
#ifdef RBTERM_SSH
    /* Async path: synthesize a transient SshProfile from the form's
       current values and kick a worker. The form modal closes
       immediately, the user sees a "Connecting to ..." placeholder
       tab while libssh handshakes off the main thread; the failure
       banner (if any) lands in the placeholder, not the form's
       error line. The synthetic profile is not persisted. */
    SshProfile sp;
    memset(&sp, 0, sizeof(sp));
    if (g_form.name[0]) strncpy(sp.name, g_form.name, sizeof(sp.name) - 1);
    strncpy(sp.hostname, g_form.host, sizeof(sp.hostname) - 1);
    if (g_form.user[0]) strncpy(sp.user, g_form.user, sizeof(sp.user) - 1);
    if (g_form.key[0])  strncpy(sp.identity, g_form.key, sizeof(sp.identity) - 1);
    sp.port = port;
    if (g_form.theme[0]) strncpy(sp.theme, g_form.theme, sizeof(sp.theme) - 1);
    sp.cursor_style = g_form.cursor_style;
    if (g_form.font[0]) strncpy(sp.font, g_form.font, sizeof(sp.font) - 1);
    sp.font_size = g_form.font_size;
    if (g_form.log_dir[0]) strncpy(sp.log_dir, g_form.log_dir, sizeof(sp.log_dir) - 1);
    sp.log_mode = g_form.log_mode;
    if (g_form.color[0]) strncpy(sp.color, g_form.color, sizeof(sp.color) - 1);
    if (g_form.cursor_color[0])
        strncpy(sp.cursor_color, g_form.cursor_color, sizeof(sp.cursor_color) - 1);
    if (g_form.hud.override) sp.hud = g_form.hud;
    if (g_form.effects_override) {
        sp.effects_override = true;
        sp.effects = g_form.effects;
    }
    if (g_form.init_cwd[0]) strncpy(sp.init_cwd, g_form.init_cwd, sizeof(sp.init_cwd) - 1);
    if (g_form.init_cmd[0]) strncpy(sp.init_cmd, g_form.init_cmd, sizeof(sp.init_cmd) - 1);
    if (g_form.display_name[0])
        strncpy(sp.display_name, g_form.display_name, sizeof(sp.display_name) - 1);
    if (g_form.layout[0]) {
        strncpy(sp.layout, g_form.layout, sizeof(sp.layout) - 1);
        for (int i = 0; i < 8; i++) {
            strncpy(sp.pane_cwds[i], g_form.pane_cwds[i],
                    sizeof(sp.pane_cwds[i]) - 1);
            strncpy(sp.pane_cmds[i], g_form.pane_cmds[i],
                    sizeof(sp.pane_cmds[i]) - 1);
        }
    }
    /* Use the saved-profile name (round-trippable to the SSH form
       by alias) when present, otherwise the host as a label so the
       placeholder banner reads sensibly. */
    const char *alias = g_form.name[0] ? g_form.name : g_form.host;
    if (!ssh_launch_kick(&sp, alias,
                         g_form.pass[0] ? g_form.pass : NULL,
                         /* is_active */ true,
                         cols, rows)) {
        strncpy(g_form.error, "too many in-flight SSH connects",
                sizeof(g_form.error) - 1);
        g_form.error[sizeof(g_form.error) - 1] = 0;
        return;
    }
    /* Scrub password from form memory now that the worker copied it. */
    memset(g_form.pass, 0, sizeof(g_form.pass));
    g_ui_mode = UI_NORMAL;
#else
    /* Stub builds (no libssh): synchronous path returns an error
       immediately. No real connect, no beachball risk. */
    char err[256] = {0};
    Tab *t = tab_open_ssh(
        g_form.user[0] ? g_form.user : NULL,
        g_form.host, port,
        g_form.pass[0] ? g_form.pass : NULL,
        g_form.key[0]  ? g_form.key  : NULL,
        g_form.theme[0] ? g_form.theme : NULL,
        g_form.cursor_style,
        g_form.font[0]    ? g_form.font    : NULL,
        g_form.font_size,
        g_form.log_dir[0] ? g_form.log_dir : NULL,
        g_form.log_mode,
        g_form.color[0]   ? g_form.color   : NULL,
        g_form.cursor_color[0] ? g_form.cursor_color : NULL,
        g_form.hud.override ? &g_form.hud  : NULL,
        g_form.effects_override ? &g_form.effects : NULL,
        g_form.init_cwd[0] ? g_form.init_cwd : NULL,
        g_form.init_cmd[0] ? g_form.init_cmd : NULL,
        g_form.layout[0] ? g_form.layout : NULL,
        g_form.layout[0] ? g_form.pane_cwds : NULL,
        g_form.layout[0] ? g_form.pane_cmds : NULL,
        cols, rows, err, sizeof(err));
    if (t) {
        memset(g_form.pass, 0, sizeof(g_form.pass));
        g_ui_mode = UI_NORMAL;
    } else {
        strncpy(g_form.error, err[0] ? err : "connection failed",
                sizeof(g_form.error) - 1);
        g_form.error[sizeof(g_form.error) - 1] = 0;
    }
#endif
#endif
}

#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)
/* Fast TCP preflight: open a non-blocking socket, connect with a
   tight timeout, return true on reachable. Used by the SSH form's
   Test button so a bogus port reports "connection refused" /
   "timed out" in ~3 s instead of stalling the main thread for the
   full libssh handshake timeout (long enough that macOS shows the
   beachball). errbuf gets a libc-style explanation on failure.
   Posix-only — Windows skips it; libssh's own timeout is enough
   there (no beachball-equivalent). */
#include <netdb.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <fcntl.h>
static bool ssh_form_tcp_check(const char *host, int port,
                               int timeout_ms,
                               char *errbuf, size_t errsz) {
    if (errbuf && errsz) errbuf[0] = 0;
    char ports[16];
    snprintf(ports, sizeof(ports), "%d", port);
    struct addrinfo hints = {0};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    int gai = getaddrinfo(host, ports, &hints, &res);
    if (gai != 0 || !res) {
        if (errbuf && errsz) snprintf(errbuf, errsz,
                                       "resolve %s: %s", host,
                                       gai_strerror(gai));
        return false;
    }
    bool ok = false;
    for (struct addrinfo *ai = res; ai && !ok; ai = ai->ai_next) {
        int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        int fl = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc == 0) { ok = true; close(fd); break; }
        if (errno != EINPROGRESS) {
            if (errbuf && errsz) snprintf(errbuf, errsz,
                                           "%s:%d: %s",
                                           host, port, strerror(errno));
            close(fd);
            continue;
        }
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        int sel = select(fd + 1, NULL, &wfds, NULL, &tv);
        if (sel == 0) {
            if (errbuf && errsz) snprintf(errbuf, errsz,
                                           "%s:%d: connection timed out",
                                           host, port);
        } else if (sel > 0) {
            int soerr = 0; socklen_t slen = sizeof(soerr);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen);
            if (soerr == 0) ok = true;
            else if (errbuf && errsz) snprintf(errbuf, errsz,
                                                "%s:%d: %s",
                                                host, port, strerror(soerr));
        } else {
            if (errbuf && errsz) snprintf(errbuf, errsz,
                                           "%s:%d: %s",
                                           host, port, strerror(errno));
        }
        close(fd);
    }
    freeaddrinfo(res);
    return ok;
}
#endif

/* Dry-run authentication with the form's current values. Opens
   a session, runs the same connect + auth + channel sequence
   Connect would, then tears it down without spawning a tab.
   Result lands in g_form.error (red) or g_form_status (green).
   Same UI freeze profile as Connect since libssh is blocking. */
static void ssh_form_test_auth(int cols, int rows) {
#ifdef __EMSCRIPTEN__
    (void)cols; (void)rows;
    strncpy(g_form.error,
            "SSH disabled in the web demo.",
            sizeof(g_form.error) - 1);
    g_form.error[sizeof(g_form.error) - 1] = 0;
    return;
#else
    if (!g_form.host[0]) {
        strncpy(g_form.error, "Host is required", sizeof(g_form.error) - 1);
        g_form.focus = F_HOST;
        return;
    }
#ifdef RBTERM_SSH
    /* Kick the test on a worker so the form doesn't beachball the
       UI for up to ~13 s on an unreachable host (3 s TCP probe + 10 s
       libssh timeout). bg_ssh_integrate() lands the result in
       g_form_status / g_form.error on a future frame. */
    if (bg_test_auth_busy()) {
        return;  /* Already in flight — Test re-clicks are no-ops. */
    }
    if (!bg_test_auth_kick(cols, rows)) {
        strncpy(g_form.error, "couldn't start test thread",
                sizeof(g_form.error) - 1);
        g_form.error[sizeof(g_form.error) - 1] = 0;
        return;
    }
    int port = atoi(g_form.port); if (port <= 0) port = 22;
    g_form.error[0] = 0;
    snprintf(g_form_status, sizeof(g_form_status),
             "Testing %s@%s:%d ...",
             g_form.user[0] ? g_form.user : "(default)",
             g_form.host, port);
#else
    /* Stub build — synchronous path returns instantly (no real SSH). */
    char err[256] = {0};
    int port = atoi(g_form.port); if (port <= 0) port = 22;
    Pty *p = pty_open_ssh(
        g_form.user[0] ? g_form.user : NULL,
        g_form.host, port,
        g_form.pass[0] ? g_form.pass : NULL,
        g_form.key[0]  ? g_form.key  : NULL,
        cols, rows, err, sizeof(err));
    if (p) {
        pty_close(p);
        g_form.error[0] = 0;
        snprintf(g_form_status, sizeof(g_form_status),
                 "Auth ok — %s@%s:%d",
                 g_form.user[0] ? g_form.user : "(default)",
                 g_form.host, port);
    } else {
        g_form_status[0] = 0;
        strncpy(g_form.error, err[0] ? err : "auth failed",
                sizeof(g_form.error) - 1);
        g_form.error[sizeof(g_form.error) - 1] = 0;
    }
#endif
#endif
}

/* Move keyboard focus through the SSH form's tab order by `delta`
   (+1 forward / -1 backward). Skips zero-sized rects so e.g. the
   Delete button is hidden until a saved host is selected. */
static void ssh_form_advance_focus(int delta) {
    /* Skip F_DELETE / F_CLONE while their buttons are hidden (no
       saved host selected). */
    bool can_del   = (g_ssh_list_selected >= 0 && g_form.name[0]);
    bool can_clone = can_del;
    for (int i = 0; i < F_COUNT; i++) {
        g_form.focus = (g_form.focus + delta + F_COUNT) % F_COUNT;
        if (g_form.focus == F_DELETE && !can_del)   continue;
        if (g_form.focus == F_CLONE  && !can_clone) continue;
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
    if (g_form.color[0])
        fprintf(fp, "    # rbterm-color: %s\n", g_form.color);
    if (g_form.cursor_color[0])
        fprintf(fp, "    # rbterm-cursor-color: %s\n", g_form.cursor_color);
    if (g_form.init_cwd[0])
        fprintf(fp, "    # rbterm-init-cwd: %s\n", g_form.init_cwd);
    if (g_form.init_cmd[0])
        fprintf(fp, "    # rbterm-init-cmd: %s\n", g_form.init_cmd);
    if (g_form.display_name[0])
        fprintf(fp, "    # rbterm-name: %s\n", g_form.display_name);
    /* Predefined split layout — string descriptor + per-pane
       cwd/cmd. Skipped entirely when no layout is set so the
       stanza stays clean for hosts that haven't been "Save Layout"-d. */
    if (g_form.layout[0]) {
        fprintf(fp, "    # rbterm-layout: %s\n", g_form.layout);
        for (int i = 0; i < SSH_LAYOUT_MAX_PANES; i++) {
            if (g_form.pane_cwds[i][0])
                fprintf(fp, "    # rbterm-pane-%d-cwd: %s\n",
                        i, g_form.pane_cwds[i]);
            if (g_form.pane_cmds[i][0])
                fprintf(fp, "    # rbterm-pane-%d-cmd: %s\n",
                        i, g_form.pane_cmds[i]);
        }
    }
    /* Per-host HUD override. Writes a few lines so plain ssh still
       parses it cleanly. We write the master toggle + position +
       per-field rows + the cpu-graph toggle so the round-trip is
       lossless. */
    if (g_form.hud.override) {
        fprintf(fp, "    # rbterm-hud: %s\n", g_form.hud.show ? "on" : "off");
        const char *pos_s = "tr";
        switch (g_form.hud.pos) {
        case HUD_POS_TOP_LEFT:     pos_s = "tl"; break;
        case HUD_POS_TOP_RIGHT:    pos_s = "tr"; break;
        case HUD_POS_BOTTOM_LEFT:  pos_s = "bl"; break;
        case HUD_POS_BOTTOM_RIGHT: pos_s = "br"; break;
        }
        fprintf(fp, "    # rbterm-hud-pos: %s\n", pos_s);
        const char *fname[HUD_FIELD_COUNT] = { "host", "ip", "load", "mem", "disk" };
        for (int fi = 0; fi < HUD_FIELD_COUNT; fi++) {
            fprintf(fp, "    # rbterm-hud-%s: %s,%d,%d\n",
                    fname[fi],
                    g_form.hud.field_show[fi] ? "on" : "off",
                    g_form.hud.field_color[fi],
                    g_form.hud.field_size[fi]);
        }
        fprintf(fp, "    # rbterm-hud-cpu: %s\n", g_form.hud.show_cpu ? "on" : "off");
    }
    /* Per-host visual effects. Skipped entirely when the form has no
       override, so a stanza that doesn't customise effects stays
       free of `# rbterm-effects-*` lines and inherits the global
       Settings → Effects defaults at connect time. */
    if (g_form.effects_override) {
        for (int i = 0; rec_effects_keys[i]; i++) {
            char buf[32];
            if (rec_effects_format(&g_form.effects, rec_effects_keys[i],
                                   buf, sizeof(buf))) {
                fprintf(fp, "    # rbterm-effects-%s: %s\n",
                        rec_effects_keys[i], buf);
            }
        }
    }
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
    /* Re-anchor the selection by name so the host the user just
       saved stays highlighted in the sidebar. */
    g_ssh_list_selected = -1;
    for (int i = 0; i < g_ssh_profile_count; i++) {
        if (strcmp(g_ssh_profiles[i].name, name) == 0) {
            g_ssh_list_selected = i;
            break;
        }
    }
    return true;
}

/* Append the form's current values as a new `Host` stanza, or, if the
   alias already exists, delegate to ssh_form_update_in_config which
   rewrites that stanza in place. */
/* Clone the currently-loaded host into a new alias. All the form's
   fields (host / user / port / key / theme / cursor / font / colour /
   HUD / effects / init_cwd / init_cmd / log) stay as-is; only the
   `name` is rotated to a unique value. We pick `<name>-copy` first,
   then `<name>-copy-2`, `<name>-copy-3`, … until we find one that
   doesn't collide with any existing saved host. The user can rename
   in the Name field before clicking Save; the new stanza is only
   written to ~/.ssh/config when Save fires. */
static void ssh_form_clone_active(void) {
    if (!g_form.name[0]) {
        strncpy(g_form.error, "Pick a saved host first to clone",
                sizeof(g_form.error) - 1);
        return;
    }
    const char *src = g_form.name;
    char candidate[sizeof(g_form.name)];
    bool taken = true;
    for (int n = 1; n < 1000 && taken; n++) {
        if (n == 1) snprintf(candidate, sizeof(candidate), "%s-copy", src);
        else        snprintf(candidate, sizeof(candidate), "%s-copy-%d", src, n);
        taken = false;
        for (int i = 0; i < g_ssh_profile_count; i++) {
            if (strcmp(g_ssh_profiles[i].name, candidate) == 0) {
                taken = true;
                break;
            }
        }
    }
    strncpy(g_form.name, candidate, sizeof(g_form.name) - 1);
    g_form.name[sizeof(g_form.name) - 1] = 0;
    /* The new alias doesn't exist on disk yet, so the saved-hosts
       sidebar shouldn't highlight anything (its highlight is
       index-based and would now point at the source row). The user
       has to click Save to commit. */
    g_ssh_list_selected = -1;
    g_form.focus  = F_NAME;
    g_form.sel_all = true;       /* whole new name pre-selected for easy rename */
    snprintf(g_form_status, sizeof(g_form_status),
             "Cloned '%s' → '%s'. Edit and Save to commit.",
             src, g_form.name);
    g_form.error[0] = 0;
}

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
    /* Refresh the sidebar so the new entry shows immediately, then
       re-anchor the selection by name — without this the list shows
       the new entry but the highlight collapses to "nothing
       selected", which looks like the save was reverted. */
    ssh_profiles_load();
    g_ssh_list_selected = -1;
    for (int i = 0; i < g_ssh_profile_count; i++) {
        if (strcmp(g_ssh_profiles[i].name, g_form.name) == 0) {
            g_ssh_list_selected = i;
            break;
        }
    }
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

    /* Effects-tab slider drag continuation. Mirrors the Settings tab,
       and uses an EFX_SLIDER_COUNT sentinel for the dedicated Decay
       slider since it lives outside the main slider array. */
    if (g_form_efx_drag) {
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            Rect rr;
            float *t = NULL;
            if (g_form_efx_drag_idx == EFX_SLIDER_COUNT) {
                rr = L.efx_decay;
                t  = &g_form.effects.decay;
            } else {
                rr = L.efx_slider[g_form_efx_drag_idx];
                t  = efx_slider_value(&g_form.effects,
                                      (EfxSlider)g_form_efx_drag_idx);
            }
            float v = (float)(mhx - rr.x) / (float)(rr.w > 1 ? rr.w : 1);
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            if (t) *t = v;
            g_form.effects_override = true;
            return;
        }
        g_form_efx_drag = false;
    }

    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) return;
    Vector2 mp = GetMousePosition();
    int mx = (int)mp.x, my = (int)mp.y;

    /* Form tab bar — switch which content section is shown. Clicking
       a tab also drops keyboard focus on the previous tab's controls
       so input doesn't bleed across. */
    for (int i = 0; i < SSH_FORM_TAB_COUNT; i++) {
        if (rect_hit(L.form_tab[i], mx, my)) {
            if (g_ssh_form_tab != i) {
                g_ssh_form_tab = i;
                g_form_logdir_focus = false;
                g_form_focused_list = FORM_FOCUS_NONE;
            }
            return;
        }
    }

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

    /* Key-file dropdown: handle the open list FIRST so a click inside
       it doesn't fall through to the field below. */
    if (g_form_key_dropdown) {
        if (rect_hit(L.key_pick_list, mx, my)) {
            ssh_keys_rescan();
            int row_h = 22;
            int idx = (my - L.key_pick_list.y) / row_h + g_form_key_scroll;
            if (idx >= 0 && idx < g_ssh_keys_count) {
                snprintf(g_form.key, sizeof(g_form.key),
                         "%s", g_ssh_keys[idx].privpath);
                g_form_key_dropdown = false;
                g_form.focus = F_KEY;
                g_form.sel_all = false;
            }
            return;
        }
        /* Click outside the open list — collapse and let the rest of
           the handler process the click as normal. */
        if (!rect_hit(L.key_pick_btn, mx, my)) {
            g_form_key_dropdown = false;
            /* fall through */
        }
    }
    if (rect_hit(L.key_pick_btn, mx, my)) {
        g_form_key_dropdown = !g_form_key_dropdown;
        if (g_form_key_dropdown) {
            ssh_keys_rescan();
            g_form_key_scroll = 0;
        }
        return;
    }
    for (int i = 0; i < F_TEXT_FIELDS; i++) {
        if (rect_hit(L.field[i], mx, my)) { g_form.focus = i; g_form.sel_all = false; return; }
    }
    /* Save Layout — capture the active SSH tab's split tree + per-pane
       cwd into the form, then write the host's stanza to ~/.ssh/config.
       Errors land in g_form.layout_status so the user sees feedback
       inline. The active tab must be SSH-flavoured AND have a non-
       empty target; otherwise we refuse with a helpful message. */
    if (g_ssh_form_tab == SSH_FORM_TAB_CONNECTION &&
        L.save_layout.w > 0 && rect_hit(L.save_layout, mx, my)) {
        if (!g_form.name[0]) {
            snprintf(g_form.layout_status, sizeof(g_form.layout_status),
                     "Set a Name first, then connect, split panes, and click here.");
            return;
        }
        Tab *src = (g_active >= 0 && g_active < g_num_tabs)
                       ? g_tabs[g_active] : NULL;
        if (!src || !src->is_ssh || !src->root) {
            snprintf(g_form.layout_status, sizeof(g_form.layout_status),
                     "No active SSH tab — connect first, split, then save.");
            return;
        }
        /* Reset cwd snapshot — we recapture every leaf's cwd from
           scratch. cmd is preserved across saves: we don't auto-
           detect what's running, so wiping it would silently drop
           anything the user hand-edited in ~/.ssh/config. */
        memset(g_form.pane_cwds, 0, sizeof(g_form.pane_cwds));
        int n = layout_serialize(src, g_form.layout, sizeof(g_form.layout),
                                 g_form.pane_cwds);
        if (n <= 0) {
            snprintf(g_form.layout_status, sizeof(g_form.layout_status),
                     "Layout too deep or too many panes (max %d).",
                     SSH_LAYOUT_MAX_PANES);
            g_form.layout[0] = 0;
            return;
        }
        if (n == 1) {
            /* Single pane = nothing to layout. Clear the layout
               string so the legacy init_cwd / init_cmd path drives
               the next connect, and tell the user. */
            g_form.layout[0] = 0;
            memset(g_form.pane_cwds, 0, sizeof(g_form.pane_cwds));
            memset(g_form.pane_cmds, 0, sizeof(g_form.pane_cmds));
            if (ssh_form_save_to_config()) {
                snprintf(g_form.layout_status, sizeof(g_form.layout_status),
                         "No splits to save. Cleared any previous layout.");
                ssh_profiles_load();
            }
            return;
        }
        if (ssh_form_save_to_config()) {
            snprintf(g_form.layout_status, sizeof(g_form.layout_status),
                     "Saved %d-pane layout for '%s'.", n, g_form.name);
            ssh_profiles_load();
        } else {
            snprintf(g_form.layout_status, sizeof(g_form.layout_status),
                     "Save failed: %s",
                     g_form.error[0] ? g_form.error : "unknown error");
        }
        return;
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

    /* Tab accent colour swatches. The last entry is the "none"
       sentinel — clears any colour override. */
    for (int i = 0; i < SSH_COLOR_PRESET_COUNT; i++) {
        if (rect_hit(L.color_swatch[i], mx, my)) {
            strncpy(g_form.color, SSH_COLOR_PRESETS[i], sizeof(g_form.color) - 1);
            g_form.color[sizeof(g_form.color) - 1] = 0;
            return;
        }
    }
    if (rect_hit(L.color_swatch[SSH_COLOR_PRESET_COUNT], mx, my)) {
        g_form.color[0] = 0;
        return;
    }
    /* Cursor-colour swatches — same shape, last entry resets
       to "inherit Settings → Cursor" by clearing the override. */
    for (int i = 0; i < SSH_COLOR_PRESET_COUNT; i++) {
        if (L.cur_color_swatch[i].w > 0 &&
            rect_hit(L.cur_color_swatch[i], mx, my)) {
            snprintf(g_form.cursor_color, sizeof(g_form.cursor_color),
                     "%s", SSH_COLOR_PRESETS[i]);
            return;
        }
    }
    if (L.cur_color_swatch[SSH_COLOR_PRESET_COUNT].w > 0 &&
        rect_hit(L.cur_color_swatch[SSH_COLOR_PRESET_COUNT], mx, my)) {
        g_form.cursor_color[0] = 0;
        return;
    }

    /* Per-host HUD tab — only meaningful when the user has flipped
       the override on. Clicks on the inner controls auto-enable the
       override so the user doesn't need two clicks to start
       customising. The L.hud_*.w == 0 check makes these no-ops on
       other tabs since gated rects stay zero. */
    if (L.hud_override_btn.w > 0 && rect_hit(L.hud_override_btn, mx, my)) {
        g_form.hud.override = !g_form.hud.override;
        return;
    }
    if (L.hud_toggle.w > 0 && rect_hit(L.hud_toggle, mx, my)) {
        g_form.hud.override = true;
        g_form.hud.show = !g_form.hud.show;
        return;
    }
    if (L.hud_pos_tl.w > 0) {
        struct { Rect r; int p; } pos_opts[] = {
            { L.hud_pos_tl, HUD_POS_TOP_LEFT },
            { L.hud_pos_tr, HUD_POS_TOP_RIGHT },
            { L.hud_pos_bl, HUD_POS_BOTTOM_LEFT },
            { L.hud_pos_br, HUD_POS_BOTTOM_RIGHT },
        };
        for (size_t pi = 0; pi < sizeof(pos_opts) / sizeof(pos_opts[0]); pi++) {
            if (rect_hit(pos_opts[pi].r, mx, my)) {
                g_form.hud.override = true;
                g_form.hud.pos = pos_opts[pi].p;
                return;
            }
        }
    }
    for (int fi = 0; fi < HUD_FIELD_COUNT; fi++) {
        if (L.hud_show_btn[fi].w > 0 && rect_hit(L.hud_show_btn[fi], mx, my)) {
            g_form.hud.override = true;
            g_form.hud.field_show[fi] = !g_form.hud.field_show[fi];
            return;
        }
        if (L.hud_color_btn[fi].w > 0 && rect_hit(L.hud_color_btn[fi], mx, my)) {
            g_form.hud.override = true;
            g_form.hud.field_color[fi] =
                (g_form.hud.field_color[fi] + 1) % HUD_PALETTE_COUNT;
            return;
        }
        if (L.hud_size_dec[fi].w > 0 && rect_hit(L.hud_size_dec[fi], mx, my)) {
            g_form.hud.override = true;
            int s = g_form.hud.field_size[fi] - 1;
            if (s < 10) s = 10;
            g_form.hud.field_size[fi] = s;
            return;
        }
        if (L.hud_size_inc[fi].w > 0 && rect_hit(L.hud_size_inc[fi], mx, my)) {
            g_form.hud.override = true;
            int s = g_form.hud.field_size[fi] + 1;
            if (s > 18) s = 18;
            g_form.hud.field_size[fi] = s;
            return;
        }
    }
    if (L.hud_cpu_toggle.w > 0 && rect_hit(L.hud_cpu_toggle, mx, my)) {
        g_form.hud.override = true;
        g_form.hud.show_cpu = !g_form.hud.show_cpu;
        return;
    }
    /* Per-host effects tab. Override toggle, sliders, Phosphor pills.
       Inner controls auto-flip the override bit so the user doesn't
       need two clicks to start customising — same UX as the HUD tab. */
    if (L.efx_override_btn.w > 0 && rect_hit(L.efx_override_btn, mx, my)) {
        g_form.effects_override = !g_form.effects_override;
        return;
    }
    for (int i = 0; i < EFX_SLIDER_COUNT; i++) {
        if (L.efx_slider[i].w > 0 && rect_hit(L.efx_slider[i], mx, my)) {
            g_form_efx_drag = true;
            g_form_efx_drag_idx = i;
            Rect rr = L.efx_slider[i];
            float v = (float)(mx - rr.x) / (float)(rr.w > 1 ? rr.w : 1);
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            float *t = efx_slider_value(&g_form.effects, (EfxSlider)i);
            if (t) *t = v;
            g_form.effects_override = true;
            return;
        }
    }
    if (L.efx_decay.w > 0 && rect_hit(L.efx_decay, mx, my)) {
        g_form_efx_drag = true;
        g_form_efx_drag_idx = EFX_SLIDER_COUNT;     /* "decay" sentinel */
        Rect rr = L.efx_decay;
        float v = (float)(mx - rr.x) / (float)(rr.w > 1 ? rr.w : 1);
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        g_form.effects.decay = v;
        g_form.effects_override = true;
        return;
    }
    for (int i = 0; i < PHOSPHOR_COUNT; i++) {
        if (L.efx_phos[i].w > 0 && rect_hit(L.efx_phos[i], mx, my)) {
            g_form.effects.phosphor = (PhosphorMode)i;
            g_form.effects_override = true;
            return;
        }
    }
    for (int i = 0; i < EFX_PRESET_COUNT; i++) {
        if (L.efx_preset[i].w > 0 && rect_hit(L.efx_preset[i], mx, my)) {
            rec_effects_apply_preset(&g_form.effects, (EfxPreset)i);
            g_form.effects_override = true;
            snprintf(g_form_status, sizeof(g_form_status),
                     "Applied '%s' preset to host effects.",
                     rec_effects_preset_label((EfxPreset)i));
            return;
        }
    }
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
        int row = (my - L.font_list.y) / row_h + g_form_font_scroll;
        if (row == 0) {
            g_form.font[0] = 0;
            g_form_font_idx = -1;
        } else {
            int idx = font_display_to_idx(row - 1);    /* -1 = header → no-op */
            if (idx >= 0 && idx < g_font_count) {
                strncpy(g_form.font, g_fonts[idx].path, sizeof(g_form.font) - 1);
                g_form.font[sizeof(g_form.font) - 1] = 0;
                g_form_font_idx = idx;
            }
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
    if (rect_hit(L.testbtn, mx, my)) {
        ssh_form_test_auth(cols, rows);
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
    if (L.clonebtn.w > 0 && rect_hit(L.clonebtn, mx, my)) {
        g_form.focus = F_CLONE;
        ssh_form_clone_active();
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
    bool is_text_field = (g_form.focus >= F_NAME && g_form.focus <= F_DNAME);

    if (IsKeyPressed(KEY_ESCAPE)) {
        if (g_form_key_dropdown) { g_form_key_dropdown = false; return; }
        g_ui_mode = UI_NORMAL; return;
    }

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
        if (g_form.focus == F_CLONE)  { ssh_form_clone_active();                  return; }
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

    /* Form-tab bar — matches the Settings modal pattern. */
    {
        const char *labels[SSH_FORM_TAB_COUNT] = {
            "Connection", "Appearance", "Logging", "HUD", "Effects"
        };
        for (int i = 0; i < SSH_FORM_TAB_COUNT; i++) {
            Rect tb = L.form_tab[i];
            bool on = (g_ssh_form_tab == i);
            DrawRectangle(tb.x, tb.y, tb.w, tb.h,
                          on ? (Color){46, 92, 150, 255} : (Color){34, 38, 52, 255});
            DrawRectangleLines(tb.x, tb.y, tb.w, tb.h,
                               (Color){125, 207, 255, on ? 255 : 150});
            Vector2 lsz = MeasureTextEx(*f, labels[i], 13, 0);
            DrawTextEx(*f, labels[i],
                       (Vector2){tb.x + (tb.w - lsz.x) / 2,
                                 tb.y + (tb.h - lsz.y) / 2},
                       13, 0, (Color){230, 232, 240, 255});
        }
    }

    /* Fields. */
    const char *labels[F_TEXT_FIELDS] = {
        "Name", "Host", "Port", "Username", "Password", "Key file",
        "Init dir", "Init command", "Tab name"
    };
    const char *hints[F_TEXT_FIELDS]  = {
        "(ssh_config alias, e.g. mia)", "example.com", "22", getenv("USER"),
        "(leave blank to use key)",
        "(default: ssh-agent + ~/.ssh/id_*)",
        "(optional: cd here on connect, e.g. ~/projects)",
        "(optional: command to run, e.g. tmux attach)",
        "(optional: shows in the tab bar; auto label suffixed)"
    };
    const char *values[F_TEXT_FIELDS] = {
        g_form.name, g_form.host, g_form.port, g_form.user, g_form.pass, g_form.key,
        g_form.init_cwd, g_form.init_cmd, g_form.display_name
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
    if (g_ssh_form_tab == SSH_FORM_TAB_CONNECTION)
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

    /* Key-file dropdown trigger (▼) sits flush to the right of the
       Key file field. The list itself draws after the rest of the
       form so it overlaps any field below. */
    if (g_ssh_form_tab == SSH_FORM_TAB_CONNECTION && L.key_pick_btn.w > 0) {
        Rect kb = L.key_pick_btn;
        bool open = g_form_key_dropdown;
        DrawRectangle(kb.x, kb.y, kb.w, kb.h,
                      open ? (Color){46, 92, 150, 255}
                           : (Color){34, 38, 52, 255});
        DrawRectangleLines(kb.x, kb.y, kb.w, kb.h,
                           (Color){125, 207, 255, open ? 255 : 150});
        const char *arrow = "v";
        Vector2 asz = MeasureTextEx(*f, arrow, 14, 0);
        DrawTextEx(*f, arrow,
                   (Vector2){kb.x + (kb.w - asz.x) / 2,
                             kb.y + (kb.h - asz.y) / 2},
                   14, 0, (Color){200, 215, 240, 255});
    }
    /* Save-layout button (Connection tab). Greyed out when no
       active SSH tab — the button still draws so the user knows
       it exists. The status line shows the most recent feedback,
       which sticks until the user re-clicks or picks another host. */
    if (g_ssh_form_tab == SSH_FORM_TAB_CONNECTION && L.save_layout.w > 0) {
        Tab *src = (g_active >= 0 && g_active < g_num_tabs)
                       ? g_tabs[g_active] : NULL;
        bool armed = src && src->is_ssh && src->root && g_form.name[0];
        Rect sl = L.save_layout;
        DrawRectangle(sl.x, sl.y, sl.w, sl.h,
                      armed ? (Color){46, 92, 150, 255}
                            : (Color){34, 38, 52, 255});
        DrawRectangleLines(sl.x, sl.y, sl.w, sl.h,
                           armed ? (Color){125, 207, 255, 255}
                                 : (Color){70, 74, 90, 255});
        const char *txt = "Save Layout from Active Tab";
        Vector2 tsz = MeasureTextEx(*f, txt, 13, 0);
        DrawTextEx(*f, txt,
                   (Vector2){sl.x + (sl.w - tsz.x) / 2,
                             sl.y + (sl.h - tsz.y) / 2},
                   13, 0, armed ? (Color){230, 232, 240, 255}
                                : (Color){140, 145, 160, 255});
        /* Status line right of the button. */
        const char *msg = g_form.layout_status[0]
                            ? g_form.layout_status
                            : (g_form.layout[0]
                                  ? g_form.layout
                                  : "(no saved layout for this host)");
        DrawTextEx(*f, msg,
                   (Vector2){sl.x + sl.w + 12, sl.y + 7},
                   13, 0, (Color){170, 180, 200, 255});
    }

    if (g_ssh_form_tab == SSH_FORM_TAB_APPEARANCE) {
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
        /* Row 0 is the synthetic "(inherit default)" row; rows 1..N
           are the grouped picker (headers + fonts), so the total is
           one more than font_display_row_count(). */
        int total = 1 + font_display_row_count();
        int visible = L.font_list.h / row_h;
        int max_scroll = total - visible;
        if (max_scroll < 0) max_scroll = 0;
        if (g_form_font_scroll > max_scroll) g_form_font_scroll = max_scroll;
        if (g_form_font_scroll < 0) g_form_font_scroll = 0;
        BeginScissorMode(L.font_list.x + 2, L.font_list.y + 2,
                         L.font_list.w - 4, L.font_list.h - 4);
        for (int row = 0; row < total; row++) {
            int ry = L.font_list.y + (row - g_form_font_scroll) * row_h;
            if (ry + row_h < L.font_list.y ||
                ry > L.font_list.y + L.font_list.h) continue;
            if (row == 0) {
                bool sel_inherit = !g_form.font[0];
                if (sel_inherit) {
                    DrawRectangle(L.font_list.x + 2, ry,
                                  L.font_list.w - 4, row_h,
                                  (Color){46, 62, 90, 220});
                }
                DrawTextEx(*f, "(inherit default)",
                           (Vector2){L.font_list.x + 10, ry + 4},
                           13, 0, (Color){150, 155, 170, 255});
                continue;
            }
            const char *header = font_header_at_row(row - 1);
            if (header) {
                DrawTextEx(*f, header,
                           (Vector2){L.font_list.x + 8, ry + 5},
                           12, 0, (Color){145, 165, 195, 255});
                DrawRectangle(L.font_list.x + 8, ry + row_h - 2,
                              L.font_list.w - 16, 1,
                              (Color){70, 80, 100, 200});
                continue;
            }
            int i = font_display_to_idx(row - 1);
            if (i < 0) continue;
            FontEntry *fe = &g_fonts[i];
            bool sel = (strcmp(fe->path, g_form.font) == 0);
            if (sel) {
                DrawRectangle(L.font_list.x + 2, ry,
                              L.font_list.w - 4, row_h,
                              (Color){46, 62, 90, 220});
            }
            /* Lazy-load preview as before; failures are sticky. */
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
    }   /* end SSH_FORM_TAB_APPEARANCE first half (theme/font/cursor) */

    if (g_ssh_form_tab == SSH_FORM_TAB_LOGGING) {
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
    }   /* end SSH_FORM_TAB_LOGGING */

    if (g_ssh_form_tab == SSH_FORM_TAB_APPEARANCE) {
    /* Tab accent colour swatches. Click cycles through 8 presets +
       a "none" sentinel that clears the override. The currently
       selected swatch wears a brighter outline. */
    {
        DrawTextEx(*f, "Tab color",
                   (Vector2){L.color_swatch[0].x - 104,
                             L.color_swatch[0].y + 7},
                   13, 0, (Color){180, 185, 200, 255});
        for (int i = 0; i < SSH_COLOR_PRESET_COUNT; i++) {
            Rect rr = L.color_swatch[i];
            Color c = (Color){90, 95, 110, 255};
            parse_hex_color(SSH_COLOR_PRESETS[i], &c);
            bool on = (strcmp(g_form.color, SSH_COLOR_PRESETS[i]) == 0);
            DrawRectangle(rr.x, rr.y, rr.w, rr.h, c);
            DrawRectangleLines(rr.x, rr.y, rr.w, rr.h,
                               on ? (Color){240, 245, 255, 255}
                                  : (Color){0, 0, 0, 120});
            if (on) {
                /* Inner highlight ring so the choice stands out
                   against any tint. */
                DrawRectangleLines(rr.x + 1, rr.y + 1, rr.w - 2, rr.h - 2,
                                   (Color){240, 245, 255, 180});
            }
        }
        Rect nr = L.color_swatch[SSH_COLOR_PRESET_COUNT];
        bool none_on = (g_form.color[0] == 0);
        DrawRectangle(nr.x, nr.y, nr.w, nr.h, (Color){34, 38, 52, 255});
        DrawRectangleLines(nr.x, nr.y, nr.w, nr.h,
                           none_on ? (Color){240, 245, 255, 255}
                                   : (Color){125, 207, 255, 150});
        Vector2 nsz2 = MeasureTextEx(*f, "none", 12, 0);
        DrawTextEx(*f, "none",
                   (Vector2){nr.x + (nr.w - nsz2.x) / 2,
                             nr.y + (nr.h - nsz2.y) / 2},
                   12, 0, (Color){200, 205, 220, 255});
    }
    /* Per-host cursor colour row. Same shape as tab-accent;
       last tile = "default" (inherit Settings → Cursor). */
    {
        DrawTextEx(*f, "Cursor color",
                   (Vector2){L.cur_color_swatch[0].x - 104,
                             L.cur_color_swatch[0].y + 7},
                   13, 0, (Color){180, 185, 200, 255});
        for (int i = 0; i < SSH_COLOR_PRESET_COUNT; i++) {
            Rect rr = L.cur_color_swatch[i];
            Color c = (Color){90, 95, 110, 255};
            parse_hex_color(SSH_COLOR_PRESETS[i], &c);
            bool on = (strcmp(g_form.cursor_color,
                              SSH_COLOR_PRESETS[i]) == 0);
            DrawRectangle(rr.x, rr.y, rr.w, rr.h, c);
            DrawRectangleLines(rr.x, rr.y, rr.w, rr.h,
                               on ? (Color){240, 245, 255, 255}
                                  : (Color){0, 0, 0, 120});
            if (on) DrawRectangleLines(rr.x + 1, rr.y + 1,
                                        rr.w - 2, rr.h - 2,
                                        (Color){240, 245, 255, 180});
        }
        Rect dr = L.cur_color_swatch[SSH_COLOR_PRESET_COUNT];
        bool def_on = (g_form.cursor_color[0] == 0);
        DrawRectangle(dr.x, dr.y, dr.w, dr.h, (Color){34, 38, 52, 255});
        DrawRectangleLines(dr.x, dr.y, dr.w, dr.h,
                           def_on ? (Color){240, 245, 255, 255}
                                  : (Color){125, 207, 255, 150});
        Vector2 dsz2 = MeasureTextEx(*f, "default", 12, 0);
        DrawTextEx(*f, "default",
                   (Vector2){dr.x + (dr.w - dsz2.x) / 2,
                             dr.y + (dr.h - dsz2.y) / 2},
                   12, 0, (Color){200, 205, 220, 255});
    }
    }   /* end SSH_FORM_TAB_APPEARANCE second half (tab color) */

    if (g_ssh_form_tab == SSH_FORM_TAB_HUD) {
        /* Per-host HUD config — mirror of Settings → HUD, writing
           into g_form.hud. Any control toggling auto-enables the
           override flag so the user doesn't need a separate click. */
        Color hl_on  = (Color){46, 92, 150, 255};
        Color hl_off = (Color){34, 38, 52, 255};
        Color outline= (Color){125, 207, 255, 150};
        Color outline_active = (Color){125, 207, 255, 255};
        Color text_main = (Color){230, 232, 240, 255};
        Color text_dim  = (Color){180, 185, 200, 255};

        /* Override toggle. Without this on, all HUD knobs are
           inert at runtime and the host falls back to the global
           settings. */
        DrawTextEx(*f, "Override",
                   (Vector2){L.hud_override_btn.x - 104,
                             L.hud_override_btn.y + 7},
                   13, 0, text_dim);
        const char *ov_label = g_form.hud.override
            ? "Use host overrides"
            : "Inherit from app";
        Rect ob = L.hud_override_btn;
        DrawRectangle(ob.x, ob.y, ob.w, ob.h, g_form.hud.override ? hl_on : hl_off);
        DrawRectangleLines(ob.x, ob.y, ob.w, ob.h,
                           g_form.hud.override ? outline_active : outline);
        Vector2 osz = MeasureTextEx(*f, ov_label, 13, 0);
        DrawTextEx(*f, ov_label,
                   (Vector2){ob.x + (ob.w - osz.x) / 2,
                             ob.y + (ob.h - osz.y) / 2},
                   13, 0, text_main);

        /* Show / hide. */
        DrawTextEx(*f, "Show HUD",
                   (Vector2){L.hud_toggle.x - 104,
                             L.hud_toggle.y + 7},
                   13, 0, text_dim);
        Rect tb = L.hud_toggle;
        const char *t_label = g_form.hud.show ? "Enabled" : "Disabled";
        DrawRectangle(tb.x, tb.y, tb.w, tb.h, g_form.hud.show ? hl_on : hl_off);
        DrawRectangleLines(tb.x, tb.y, tb.w, tb.h,
                           g_form.hud.show ? outline_active : outline);
        Vector2 tsz = MeasureTextEx(*f, t_label, 13, 0);
        DrawTextEx(*f, t_label,
                   (Vector2){tb.x + (tb.w - tsz.x) / 2,
                             tb.y + (tb.h - tsz.y) / 2},
                   13, 0, text_main);

        /* 2×2 corner picker. */
        DrawTextEx(*f, "Position",
                   (Vector2){L.hud_pos_tl.x - 104,
                             L.hud_pos_tl.y + 7},
                   13, 0, text_dim);
        struct { Rect r; const char *label; int p; } pos_opts[] = {
            { L.hud_pos_tl, "Top-left",     HUD_POS_TOP_LEFT },
            { L.hud_pos_tr, "Top-right",    HUD_POS_TOP_RIGHT },
            { L.hud_pos_bl, "Bottom-left",  HUD_POS_BOTTOM_LEFT },
            { L.hud_pos_br, "Bottom-right", HUD_POS_BOTTOM_RIGHT },
        };
        for (int pi = 0; pi < 4; pi++) {
            bool on = (g_form.hud.pos == pos_opts[pi].p);
            Rect rr = pos_opts[pi].r;
            DrawRectangle(rr.x, rr.y, rr.w, rr.h, on ? hl_on : hl_off);
            DrawRectangleLines(rr.x, rr.y, rr.w, rr.h, on ? outline_active : outline);
            Vector2 ssz = MeasureTextEx(*f, pos_opts[pi].label, 12, 0);
            DrawTextEx(*f, pos_opts[pi].label,
                       (Vector2){rr.x + (rr.w - ssz.x) / 2,
                                 rr.y + (rr.h - ssz.y) / 2},
                       12, 0, text_main);
        }

        /* Per-field grid: visibility, colour swatch, size − value +. */
        const char *fnames[HUD_FIELD_COUNT] = { "Host", "IP", "Load", "Mem", "Disk" };
        for (int fi = 0; fi < HUD_FIELD_COUNT; fi++) {
            DrawTextEx(*f, fnames[fi],
                       (Vector2){L.hud_show_btn[fi].x - 60,
                                 L.hud_show_btn[fi].y + 7},
                       13, 0, text_dim);
            Rect sb = L.hud_show_btn[fi];
            bool sh = g_form.hud.field_show[fi];
            DrawRectangle(sb.x, sb.y, sb.w, sb.h, sh ? hl_on : hl_off);
            DrawRectangleLines(sb.x, sb.y, sb.w, sb.h, sh ? outline_active : outline);
            const char *sl = sh ? "show" : "hide";
            Vector2 zz = MeasureTextEx(*f, sl, 12, 0);
            DrawTextEx(*f, sl,
                       (Vector2){sb.x + (sb.w - zz.x) / 2,
                                 sb.y + (sb.h - zz.y) / 2},
                       12, 0, text_main);

            /* Colour swatch (click cycles palette index). */
            Rect cb = L.hud_color_btn[fi];
            int ci = g_form.hud.field_color[fi];
            if (ci < 0 || ci >= HUD_PALETTE_COUNT) ci = 0;
            DrawRectangle(cb.x, cb.y, cb.w, cb.h, HUD_PALETTE[ci]);
            DrawRectangleLines(cb.x, cb.y, cb.w, cb.h, outline);

            /* Size − value +. */
            Rect dec = L.hud_size_dec[fi];
            DrawRectangle(dec.x, dec.y, dec.w, dec.h, hl_off);
            DrawRectangleLines(dec.x, dec.y, dec.w, dec.h, outline);
            Vector2 mss = MeasureTextEx(*f, "−", 16, 0);
            DrawTextEx(*f, "−",
                       (Vector2){dec.x + (dec.w - mss.x) / 2,
                                 dec.y + (dec.h - mss.y) / 2},
                       16, 0, text_main);
            Rect val = L.hud_size_val[fi];
            char vbuf[8]; snprintf(vbuf, sizeof(vbuf), "%d", g_form.hud.field_size[fi]);
            Vector2 vsz = MeasureTextEx(*f, vbuf, 13, 0);
            DrawTextEx(*f, vbuf,
                       (Vector2){val.x + (val.w - vsz.x) / 2,
                                 val.y + (val.h - vsz.y) / 2},
                       13, 0, text_main);
            Rect inc = L.hud_size_inc[fi];
            DrawRectangle(inc.x, inc.y, inc.w, inc.h, hl_off);
            DrawRectangleLines(inc.x, inc.y, inc.w, inc.h, outline);
            Vector2 pss = MeasureTextEx(*f, "+", 16, 0);
            DrawTextEx(*f, "+",
                       (Vector2){inc.x + (inc.w - pss.x) / 2,
                                 inc.y + (inc.h - pss.y) / 2},
                       16, 0, text_main);
        }

        /* CPU graph toggle. */
        Rect cb2 = L.hud_cpu_toggle;
        DrawRectangle(cb2.x, cb2.y, cb2.w, cb2.h, g_form.hud.show_cpu ? hl_on : hl_off);
        DrawRectangleLines(cb2.x, cb2.y, cb2.w, cb2.h,
                           g_form.hud.show_cpu ? outline_active : outline);
        const char *cl2 = g_form.hud.show_cpu ? "CPU graph: on" : "CPU graph: off";
        Vector2 cz = MeasureTextEx(*f, cl2, 13, 0);
        DrawTextEx(*f, cl2,
                   (Vector2){cb2.x + (cb2.w - cz.x) / 2,
                             cb2.y + (cb2.h - cz.y) / 2},
                   13, 0, text_main);
    }

    if (g_ssh_form_tab == SSH_FORM_TAB_EFFECTS) {
        /* Per-host visual-effect override. Mirrors the layout of the
           Settings → Effects tab; the Override toggle gates whether
           the form's slider/phosphor values get serialised to the
           ~/.ssh/config stanza on save. */
        Color hl_on  = (Color){46, 92, 150, 255};
        Color hl_off = (Color){34, 38, 52, 255};
        Color outline= (Color){125, 207, 255, 150};
        Color outline_active = (Color){125, 207, 255, 255};
        Color text_main = (Color){230, 232, 240, 255};
        Color text_dim  = (Color){180, 185, 200, 255};
        Color slider_bg     = (Color){22, 25, 34, 255};
        Color slider_fill   = (Color){46, 92, 150, 220};
        Color slider_border = (Color){70, 74, 90, 255};
        Color slider_thumb  = (Color){125, 207, 255, 255};
        Color val_col       = (Color){170, 200, 235, 255};

        DrawTextEx(*f, "Override",
                   (Vector2){L.efx_override_btn.x - 104,
                             L.efx_override_btn.y + 7},
                   13, 0, text_dim);
        const char *ov_label = g_form.effects_override
            ? "Use host overrides"
            : "Inherit from app";
        Rect ob = L.efx_override_btn;
        DrawRectangle(ob.x, ob.y, ob.w, ob.h, g_form.effects_override ? hl_on : hl_off);
        DrawRectangleLines(ob.x, ob.y, ob.w, ob.h,
                           g_form.effects_override ? outline_active : outline);
        Vector2 osz = MeasureTextEx(*f, ov_label, 13, 0);
        DrawTextEx(*f, ov_label,
                   (Vector2){ob.x + (ob.w - osz.x) / 2,
                             ob.y + (ob.h - osz.y) / 2},
                   13, 0, text_main);

        /* Six sliders + value readouts. */
        for (int i = 0; i < EFX_SLIDER_COUNT; i++) {
            Rect rr = L.efx_slider[i];
            float v = *efx_slider_value(&g_form.effects, (EfxSlider)i);
            int   fill_w = (int)(v * (float)rr.w);
            DrawTextEx(*f, efx_slider_label((EfxSlider)i),
                       (Vector2){rr.x - 96, rr.y + (rr.h - 13) / 2 + 1},
                       13, 0, text_dim);
            int track_h = 8;
            int track_y = rr.y + (rr.h - track_h) / 2;
            DrawRectangle(rr.x, track_y, rr.w, track_h, slider_bg);
            DrawRectangle(rr.x, track_y, fill_w, track_h, slider_fill);
            DrawRectangleLines(rr.x, track_y, rr.w, track_h, slider_border);
            int thumb_x = rr.x + fill_w;
            DrawCircle(thumb_x, track_y + track_h / 2, 7, slider_thumb);
            DrawCircleLines(thumb_x, track_y + track_h / 2, 7, (Color){30, 34, 46, 255});
            char vbuf[8];
            snprintf(vbuf, sizeof(vbuf), "%.0f%%", v * 100.0f);
            Vector2 vsz = MeasureTextEx(*f, vbuf, 12, 0);
            DrawTextEx(*f, vbuf,
                       (Vector2){rr.x + rr.w + 6,
                                 rr.y + (rr.h - vsz.y) / 2},
                       12, 0, val_col);
        }
        /* Decay slider — phosphor trail / ghosting. */
        {
            Rect rr = L.efx_decay;
            float v = g_form.effects.decay;
            int   fill_w = (int)(v * (float)rr.w);
            DrawTextEx(*f, "Decay",
                       (Vector2){rr.x - 96, rr.y + (rr.h - 13) / 2 + 1},
                       13, 0, text_dim);
            int track_h = 8;
            int track_y = rr.y + (rr.h - track_h) / 2;
            DrawRectangle(rr.x, track_y, rr.w, track_h, slider_bg);
            DrawRectangle(rr.x, track_y, fill_w, track_h, slider_fill);
            DrawRectangleLines(rr.x, track_y, rr.w, track_h, slider_border);
            int thumb_x = rr.x + fill_w;
            DrawCircle(thumb_x, track_y + track_h / 2, 7, slider_thumb);
            DrawCircleLines(thumb_x, track_y + track_h / 2, 7, (Color){30, 34, 46, 255});
            char vbuf[8];
            snprintf(vbuf, sizeof(vbuf), "%.0f%%", v * 100.0f);
            Vector2 vsz = MeasureTextEx(*f, vbuf, 12, 0);
            DrawTextEx(*f, vbuf,
                       (Vector2){rr.x + rr.w + 6,
                                 rr.y + (rr.h - vsz.y) / 2},
                       12, 0, val_col);
        }
        /* Phosphor pills. */
        DrawTextEx(*f, "Phosphor",
                   (Vector2){L.efx_phos[0].x - 96,
                             L.efx_phos[0].y + (L.efx_phos[0].h - 13) / 2 + 1},
                   13, 0, text_dim);
        for (int i = 0; i < PHOSPHOR_COUNT; i++) {
            Rect rr = L.efx_phos[i];
            bool sel = (g_form.effects.phosphor == (PhosphorMode)i);
            DrawRectangle(rr.x, rr.y, rr.w, rr.h, sel ? hl_on : hl_off);
            DrawRectangleLines(rr.x, rr.y, rr.w, rr.h, sel ? outline_active : outline);
            const char *plbl = phosphor_label((PhosphorMode)i);
            Vector2 ts = MeasureTextEx(*f, plbl, 12, 0);
            DrawTextEx(*f, plbl,
                       (Vector2){rr.x + (rr.w - ts.x) / 2,
                                 rr.y + (rr.h - ts.y) / 2},
                       12, 0, text_main);
        }
        /* Preset pills. */
        DrawTextEx(*f, "Preset",
                   (Vector2){L.efx_preset[0].x - 60,
                             L.efx_preset[0].y - 16},
                   13, 0, text_dim);
        for (int i = 0; i < EFX_PRESET_COUNT; i++) {
            Rect rr = L.efx_preset[i];
            DrawRectangle(rr.x, rr.y, rr.w, rr.h, hl_off);
            DrawRectangleLines(rr.x, rr.y, rr.w, rr.h, outline);
            const char *lbl = rec_effects_preset_label((EfxPreset)i);
            Vector2 ts = MeasureTextEx(*f, lbl, 12, 0);
            DrawTextEx(*f, lbl,
                       (Vector2){rr.x + (rr.w - ts.x) / 2,
                                 rr.y + (rr.h - ts.y) / 2},
                       12, 0, text_main);
        }
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

    /* Test — dry-run auth. Same row as Connect, muted accent so the
       eye lands on Connect. */
    DrawRectangle(L.testbtn.x, L.testbtn.y, L.testbtn.w, L.testbtn.h,
                  (Color){48, 52, 66, 255});
    DrawRectangleLines(L.testbtn.x, L.testbtn.y, L.testbtn.w, L.testbtn.h,
                       (Color){150, 200, 230, 180});
    Vector2 tsz = MeasureTextEx(*f, "Test", 14, 0);
    DrawTextEx(*f, "Test",
               (Vector2){L.testbtn.x + (L.testbtn.w - tsz.x) / 2,
                         L.testbtn.y + (L.testbtn.h - tsz.y) / 2},
               14, 0, (Color){210, 220, 235, 255});

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
    if (L.clonebtn.w > 0) {
        bool clf = g_form.focus == F_CLONE;
        DrawRectangle(L.clonebtn.x, L.clonebtn.y, L.clonebtn.w, L.clonebtn.h,
                      clf ? (Color){72, 76, 96, 255} : (Color){48, 52, 66, 255});
        DrawRectangleLines(L.clonebtn.x, L.clonebtn.y, L.clonebtn.w, L.clonebtn.h,
                           (Color){180, 200, 230, clf ? 255 : 180});
        Vector2 clsz = MeasureTextEx(*f, "Clone", 14, 0);
        DrawTextEx(*f, "Clone",
                   (Vector2){L.clonebtn.x + (L.clonebtn.w - clsz.x) / 2,
                             L.clonebtn.y + (L.clonebtn.h - clsz.y) / 2},
                   14, 0, (Color){220, 230, 245, 255});
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
    Vector2 xsz = MeasureTextEx(*f, "Close", 14, 0);
    DrawTextEx(*f, "Close",
               (Vector2){L.cancel.x + (L.cancel.w - xsz.x) / 2,
                         L.cancel.y + (L.cancel.h - xsz.y) / 2},
               14, 0, (Color){210, 215, 230, 255});

    /* Status (success) / error line under the buttons. Word-wrap
       the error inside the modal width — the auth-failed message
       includes the full "tried: agent / id_ed25519 / …" detail
       which used to overflow off the right edge. Up to 4 lines;
       beyond that the tail is truncated with an ellipsis. */
    int msg_x = L.modal.x + 22;
    int msg_y = L.connect.y + L.connect.h + 10;
    int msg_w_max = L.modal.w - 44;
    int line_h   = 14;
    if (g_form.error[0]) {
        const char *src = g_form.error;
        size_t srclen = strlen(src);
        size_t pos = 0;
        Color ec = (Color){240, 100, 100, 255};
        for (int line_i = 0; line_i < 4 && pos < srclen; line_i++) {
            char buf[256];
            size_t take = srclen - pos;
            if (take >= sizeof(buf)) take = sizeof(buf) - 1;
            memcpy(buf, src + pos, take);
            buf[take] = 0;
            /* Shrink to fit. Walk back to the last whitespace so we
               don't break a word. If no whitespace, hard break. */
            while (take > 0) {
                Vector2 sz = MeasureTextEx(*f, buf, 12, 0);
                if (sz.x <= msg_w_max) break;
                take--;
                buf[take] = 0;
            }
            if (pos + take < srclen) {
                size_t back = take;
                while (back > 0 && buf[back - 1] != ' ' &&
                       buf[back - 1] != ',' && buf[back - 1] != '/')
                    back--;
                if (back > take / 2) { take = back; buf[take] = 0; }
            }
            /* Last line + still text remaining → ellipsis. */
            if (line_i == 3 && pos + take < srclen) {
                while (take > 1) {
                    char tmp[260];
                    snprintf(tmp, sizeof(tmp), "%s…", buf);
                    Vector2 sz = MeasureTextEx(*f, tmp, 12, 0);
                    if (sz.x <= msg_w_max) {
                        memcpy(buf, tmp, strlen(tmp) + 1);
                        break;
                    }
                    take--;
                    buf[take] = 0;
                }
            }
            DrawTextEx(*f, buf,
                       (Vector2){msg_x, msg_y + line_i * line_h},
                       12, 0, ec);
            pos += take;
            while (pos < srclen && src[pos] == ' ') pos++;
        }
    } else if (g_form_status[0]) {
        DrawTextEx(*f, g_form_status,
                   (Vector2){msg_x, msg_y},
                   12, 0, (Color){120, 220, 140, 255});
    }

    /* Footer hint. */
    DrawTextEx(*f, "Tab navigates · Enter connects · Save appends to ~/.ssh/config · Esc cancels",
               (Vector2){L.modal.x + 22, L.modal.y + L.modal.h - 22},
               11, 0, (Color){110, 115, 130, 255});

    /* Key-file dropdown — drawn last so it stacks over neighbouring
       fields. The same list that powers Settings → Keys; clicking a
       row sets g_form.key to that key's absolute path. */
    if (g_ssh_form_tab == SSH_FORM_TAB_CONNECTION && g_form_key_dropdown) {
        Rect kr = L.key_pick_list;
        DrawRectangle(kr.x + 2, kr.y + 2, kr.w, kr.h, (Color){0, 0, 0, 120});
        DrawRectangle(kr.x, kr.y, kr.w, kr.h, (Color){22, 25, 34, 255});
        DrawRectangleLines(kr.x, kr.y, kr.w, kr.h, (Color){125, 207, 255, 220});
        int row_h = 22;
        int visible = kr.h / row_h;
        if (g_form_key_scroll < 0) g_form_key_scroll = 0;
        int max_scroll = g_ssh_keys_count - visible;
        if (max_scroll < 0) max_scroll = 0;
        if (g_form_key_scroll > max_scroll) g_form_key_scroll = max_scroll;
        BeginScissorMode(kr.x + 2, kr.y + 2, kr.w - 4, kr.h - 4);
        if (g_ssh_keys_count == 0) {
            DrawTextEx(*f, "(no keys in ~/.ssh — generate one in Settings → Keys)",
                       (Vector2){kr.x + 10, kr.y + 6},
                       12, 0, (Color){140, 145, 160, 255});
        }
        for (int i = 0; i < g_ssh_keys_count; i++) {
            int ry = kr.y + (i - g_form_key_scroll) * row_h;
            if (ry + row_h < kr.y || ry > kr.y + kr.h) continue;
            char line[320];
            const SshKeyEntry *e = &g_ssh_keys[i];
            snprintf(line, sizeof(line), "%s   %s",
                     e->name, e->algo[0] ? e->algo : "?");
            DrawTextEx(*f, line, (Vector2){kr.x + 10, ry + 4},
                       13, 0, (Color){200, 205, 220, 255});
        }
        EndScissorMode();
    }
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
    SETTINGS_TAB_EFFECTS    = 3,
    SETTINGS_TAB_SESSION    = 4,    /* legacy "Session" — labelled "Logging" */
    SETTINGS_TAB_WINDOW     = 5,
    SETTINGS_TAB_RECORDING  = 6,
    SETTINGS_TAB_HUD        = 7,
    SETTINGS_TAB_LAUNCH     = 8,
    SETTINGS_TAB_KEYS       = 9,
    SETTINGS_TAB_SESSIONS   = 10,   /* multi-host split sessions (designer + list) */
    SETTINGS_TAB_COUNT      = 11,
} SettingsTab;
static int g_settings_tab = SETTINGS_TAB_FONT;

static void fonts_load(const char *current_path);
/* Open the Settings modal. Reloads the on-disk font list so the
   font picker is populated with whatever's installed right now. */
static void settings_open(Renderer *r) {
    g_ui_mode = UI_SETTINGS;
    g_settings_status[0] = 0;
    fonts_load(r ? r->font_path : NULL);
    /* Pre-warm the keys list so the Keys tab is responsive on
       first open. Cheap — disk scan of ~/.ssh and a popen per
       .pub for the fingerprint. */
    ssh_keys_rescan();
    /* Sessions list — load lazily on each Settings open so hand-
       edits to ~/.config/rbterm/sessions.ini show up without
       restarting rbterm. */
    sessions_load();
    /* Reset Keys-tab modal state so a previously-open delete
       confirm doesn't reappear on the next open. */
    g_keys_delete_idx = -1;
    g_keys_install_dropdown = -1;
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
    /* Cursor-colour swatches: 8 presets + a "default" sentinel
       at index SSH_COLOR_PRESET_COUNT meaning "use natural fg
       colour". Also lives on the Cursor tab. */
    Rect cur_color_swatch[9];
    Rect pad_val;
    Rect pad_dec, pad_inc;
    Rect spc_val;
    Rect spc_dec, spc_inc;
    Rect log_toggle; /* on/off button */
    Rect log_dir;    /* editable text box with log directory */
    Rect log_browse; /* "Browse logs" — opens the logs modal */
    Rect ligatures_toggle; /* Settings → Font: enable HarfBuzz shaping */
    Rect rec_dir;    /* editable text box with recording-save directory */
    Rect repeat_initial; /* slider track — initial repeat delay */
    Rect repeat_rate;    /* slider track — per-repeat period */
    /* Startup-window picker. The original three rects (default /
       fullscreen / maximized) stay for back-compat in the click
       handler. New rects below cover the size presets and the
       borderless-fullscreen / fill-monitor extras. */
    Rect startup_default;
    Rect startup_fullscreen;
    Rect startup_maximized;
    Rect startup_small;
    Rect startup_medium;
    Rect startup_large;
    Rect startup_fill;
    Rect startup_borderless;
    /* HUD tab — system-info overlay configuration. */
    Rect hud_toggle;          /* enable / disable button */
    Rect hud_pos_tl, hud_pos_tr, hud_pos_bl, hud_pos_br; /* corner picker */
    /* Per-field controls: one row per field with [show toggle]
       [color swatch] [size -] [size value] [size +]. The HUD field
       enum (HUD_FIELD_HOST..HUD_FIELD_DISK) indexes these. */
    Rect hud_show_btn[5];
    Rect hud_color_btn[5];
    Rect hud_size_dec[5];
    Rect hud_size_val[5];
    Rect hud_size_inc[5];
    Rect hud_cpu_toggle;     /* CPU sparkline enable/disable */
    /* Launch tab — list of "open these on startup" entries plus
       Add buttons. One row per slot:
         [kind pill] [host picker] [active radio] [▲] [▼] [×]
       Active radio: clicking it makes that row the foreground
       tab once the launch sweep finishes. Up/Down swap the
       entry with its neighbour; both go dim (zero-w) at the
       ends of the list so clicks fall through. */
    Rect launch_kind  [LAUNCH_ENTRY_MAX];
    Rect launch_host  [LAUNCH_ENTRY_MAX];
    Rect launch_active[LAUNCH_ENTRY_MAX];
    Rect launch_up    [LAUNCH_ENTRY_MAX];
    Rect launch_down  [LAUNCH_ENTRY_MAX];
    Rect launch_del   [LAUNCH_ENTRY_MAX];
    Rect launch_add_local;
    Rect launch_add_ssh;
    Rect launch_add_session;
    /* Keys tab. Per-row [name+algo+fingerprint label] [Install]
       [Delete?] — for now we only show Install. Plus a "+
       Generate" button at the bottom. */
    Rect keys_install[SSH_KEYS_MAX];
    Rect keys_delete[SSH_KEYS_MAX];
    Rect keys_generate_btn;
    /* Generate modal — appears when keys_generate_btn is clicked.
       Internally a sub-modal centred on the Settings modal. */
    Rect keygen_modal;
    Rect keygen_type_ed;
    Rect keygen_type_rsa;
    Rect keygen_name_field;
    Rect keygen_pass_field;
    Rect keygen_cancel;
    Rect keygen_ok;
    /* Delete-confirmation sub-modal — shown when g_keys_delete_idx >= 0.
       Mirrors the keygen modal's layout. */
    Rect keysdel_modal;
    Rect keysdel_cancel;
    Rect keysdel_ok;
    /* Effects tab — six slider tracks (mirroring the rec save modal's
       set: CRT / Bloom / Grain / VHS / Glitch / Halftone) plus a
       four-button Phosphor picker. The slider's hit rect IS the
       track; thumb position derives from the underlying float. */
    Rect efx_set_slider[EFX_SLIDER_COUNT];
    Rect efx_set_decay;                  /* extra slider for phosphor decay */
    Rect efx_set_phos[PHOSPHOR_COUNT];
    Rect efx_set_preset[EFX_PRESET_COUNT];
    Rect efx_set_reset;
    /* Sessions tab — list with [Open][Edit][×] per row + a "+ New
       session" button at top. Up to SESSIONS_MAX rows. */
    Rect sess_new_btn;
    Rect sess_row[SESSIONS_MAX];
    Rect sess_open[SESSIONS_MAX];
    Rect sess_edit[SESSIONS_MAX];
    Rect sess_del[SESSIONS_MAX];
    Rect save_default; /* write current state to ~/.config/rbterm/config.ini */
    Rect close;
} SettingsLayout;

/* Drag state for the Effects-tab sliders. Same shape as the rec save
   modal's slider drag: the user presses inside a track to capture,
   movement updates the value while held, release ends the drag. */
static bool g_settings_efx_drag = false;
static int  g_settings_efx_drag_idx = 0;

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

/* Classify a font filename / display name as ligature-capable. The list
   is curated — every entry is a font whose primary distinguishing
   feature is a programming-ligature set (=> != >= -> etc.) shipped in
   the .ttf. Matched as a substring against the display name OR the
   filename so e.g. JetBrains-Mono-Vazir-Regular and JetBrainsMono
   both classify the same. False positives here are visual only — the
   picker just groups the row at the top. */
static bool font_has_ligatures(const char *display_name, const char *path) {
    static const char *pat[] = {
        "FiraCode",   "Fira Code",   "FiraMono",   "Fira Mono",
        "JetBrains",  "JetBrainsMono",
        "Cascadia",   "Caskaydia",   "CaskaydiaCove",
        "Iosevka",
        "Monaspace",
        "Hasklig",
        "Victor Mono", "VictorMono",
        "Maple",      "MapleMono",
        "Monoid",
        "0xProto",    "Pragmata",
        "Recursive",
        "Comic Mono", "ComicMono",
        NULL
    };
    if (!display_name) display_name = "";
    if (!path)         path         = "";
    for (int i = 0; pat[i]; i++) {
        if (strstr(display_name, pat[i]) ||
            (path[0] && strstr(path, pat[i])))
            return true;
    }
    return false;
}

/* qsort comparator — group ligature-capable fonts at the top of the
   list, then sort each group alphabetically (case-insensitive). The
   draw routine inserts a section header at the boundary so the user
   sees "Programming / ligatures" vs "Classic monospace" labels. */
static int cmp_font_grouped(const void *a, const void *b) {
    const FontEntry *fa = (const FontEntry *)a;
    const FontEntry *fb = (const FontEntry *)b;
    if (fa->ligatures != fb->ligatures)
        return fb->ligatures ? +1 : -1;        /* ligatures group first */
    return strcasecmp(fa->name, fb->name);
}

/* ---------- Picker row ↔ font-index mapping ---------------------------
   Both font pickers (Settings → Font and the SSH form's font tab) draw
   one row per font *plus* a section header before each group. The row
   ↔ font-index mapping has to skip those header rows for click
   hit-tests and scroll math. These helpers centralise that bookkeeping
   so each picker reads identical. */

static int font_classic_first_idx(void) {
    for (int i = 0; i < g_font_count; i++)
        if (!g_fonts[i].ligatures) return i;
    return g_font_count;
}

/* Total number of *displayed* rows (fonts + section headers). */
static int font_display_row_count(void) {
    int classic = font_classic_first_idx();
    int headers = (classic > 0)             /* "Programming / ligatures" */
                + (classic < g_font_count); /* "Classic monospace"        */
    return g_font_count + headers;
}

/* Convert a displayed row index to a font index, or -1 if the row is a
   section header. Inverse of font_idx_to_display. */
static int font_display_to_idx(int row) {
    if (row < 0) return -1;
    int classic = font_classic_first_idx();
    bool has_lig = (classic > 0);
    bool has_cla = (classic < g_font_count);
    int r = row;
    if (has_lig) {
        if (r == 0) return -1;          /* ligature group header */
        r -= 1;
    }
    if (has_lig && has_cla && r == classic) return -1;   /* classic header */
    if (has_lig && has_cla && r > classic) r -= 1;
    if (!has_lig && has_cla && r == 0) return -1;        /* classic header (no lig group) */
    if (!has_lig && has_cla && r > 0)  r -= 1;
    if (r < 0 || r >= g_font_count) return -1;
    return r;
}

/* Convert a font index to its displayed row. Used for "scroll-to-selected". */
static int font_idx_to_display(int idx) {
    if (idx < 0 || idx >= g_font_count) return -1;
    int classic = font_classic_first_idx();
    bool has_lig = (classic > 0);
    bool has_cla = (classic < g_font_count);
    int row = idx;
    if (has_lig) row += 1;                          /* skip ligature header */
    if (has_lig && has_cla && idx >= classic) row += 1;  /* skip classic header */
    if (!has_lig && has_cla) row += 1;              /* skip classic header (no lig group) */
    return row;
}

/* If `row` is a header row, return its caption string; else NULL. */
static const char *font_header_at_row(int row) {
    int classic = font_classic_first_idx();
    bool has_lig = (classic > 0);
    bool has_cla = (classic < g_font_count);
    if (has_lig && row == 0)
        return "Programming / ligatures";
    int r = row - (has_lig ? 1 : 0);
    if (has_lig && has_cla && r == classic) return "Classic monospace";
    if (!has_lig && has_cla && row == 0)    return "Classic monospace";
    return NULL;
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
        memset(f, 0, sizeof(*f));
        snprintf(f->path, sizeof(f->path), "%s/%s", dir, name);
        strncpy(f->name, trimmed_w, sizeof(f->name) - 1);
        f->name[sizeof(f->name) - 1] = 0;
        f->ligatures = font_has_ligatures(f->name, f->path);
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
        memset(f, 0, sizeof(*f));
        snprintf(f->path, sizeof(f->path), "%s/%s", dir, name);
        strncpy(f->name, trimmed, sizeof(f->name) - 1);
        f->name[sizeof(f->name) - 1] = 0;
        f->ligatures = font_has_ligatures(f->name, f->path);
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
        f->ligatures = font_has_ligatures(f->name, NULL);
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
    qsort(g_fonts, g_font_count, sizeof(FontEntry), cmp_font_grouped);
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
/* Launch tab host picker: -1 = no dropdown open, otherwise the
   row whose dropdown panel is currently visible. */
static int  g_settings_launch_dropdown = -1;
static int  g_settings_launch_scroll   = 0;
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
    int w = 760, h = 660;
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

        /* Ligatures toggle — sits one row below the cell-spacing
           controls. Only relevant when shape_available() (HarfBuzz
           linked at build time); the click handler hides the row when
           it isn't. */
        if (shape_available()) {
            int lig_row_y = spc_row_y + btn + 14;
            L.ligatures_toggle = (Rect){ L.modal.x + 140, lig_row_y, 200, btn };
        } else {
            L.ligatures_toggle = (Rect){ 0, 0, 0, 0 };
        }
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
        /* Cursor-colour swatch row, sitting under the key-repeat
           sliders. 8 presets + a "default" sentinel at index 8. */
        int sw_y = kr2_y + 30;
        int n = SSH_COLOR_PRESET_COUNT + 1;
        int sw_gap = 4;
        int sw = ((w - 140 - 22) - (n - 1) * sw_gap) / n;
        int sh = 28;
        for (int i = 0; i < n; i++) {
            L.cur_color_swatch[i] = (Rect){ L.modal.x + 140 + i * (sw + sw_gap),
                                            sw_y, sw, sh };
        }
    } else if (g_settings_tab == SETTINGS_TAB_EFFECTS) {
        /* Effects tab.

           Three slider rows in two columns:
             row 0: CRT  | VHS
             row 1: Bloom | Glitch
             row 2: Grain | Halftone
           Then a Decay slider on its own row (left column),
           a Phosphor pill row, a Preset pill row, and a Reset
           button. Per column we reserve:
              [label_w][slider_w][value_w]
           and put `col_gap` of breathing room between the two
           columns so the right column's label doesn't land on top
           of the left column's value readout. The draw code
           positions labels at `slider.x - 96`, so label_w /
           col_gap are tuned to keep that offset clear. */
        int row_y = content_y;
        int row_h = 28;
        int row_gap = 10;
        int inner_x = L.modal.x + 22;
        int inner_w = w - 44;
        int col_gap = 28;
        int half_w  = (inner_w - col_gap) / 2;
        int label_w = 96;
        int value_w = 56;
        int slider_w = half_w - label_w - value_w;
        if (slider_w < 80) slider_w = 80;        /* tiny-window safety */
        int left_slider_x  = inner_x + label_w;
        int right_slider_x = inner_x + half_w + col_gap + label_w;
        for (int i = 0; i < 3; i++) {
            int yy = row_y + i * (row_h + row_gap);
            L.efx_set_slider[k_efx_left_col[i]]  = (Rect){ left_slider_x,  yy, slider_w, row_h };
            L.efx_set_slider[k_efx_right_col[i]] = (Rect){ right_slider_x, yy, slider_w, row_h };
        }
        int decay_y = row_y + 3 * (row_h + row_gap);
        L.efx_set_decay = (Rect){ left_slider_x, decay_y, slider_w, row_h };
        int phos_y = decay_y + row_h + row_gap + 4;
        int phos_track_w = inner_w - label_w;
        int phos_btn_w = (phos_track_w - 3 * 6) / PHOSPHOR_COUNT;
        for (int i = 0; i < PHOSPHOR_COUNT; i++)
            L.efx_set_phos[i] = (Rect){ left_slider_x + i * (phos_btn_w + 6),
                                        phos_y, phos_btn_w, row_h };
        /* Preset grid — too many presets for a single row, so wrap
           into PRESETS_COLS columns. The button width is computed
           against the inner content rect; rows stack with `row_h + 4`
           pitch, matching the slider rhythm. */
        const int PRESETS_COLS = 5;
        int preset_y = phos_y + row_h + 12;
        int preset_btn_w = (inner_w - (PRESETS_COLS - 1) * 6) / PRESETS_COLS;
        int preset_rows  = (EFX_PRESET_COUNT + PRESETS_COLS - 1) / PRESETS_COLS;
        for (int i = 0; i < EFX_PRESET_COUNT; i++) {
            int rr = i / PRESETS_COLS;
            int cc = i % PRESETS_COLS;
            L.efx_set_preset[i] = (Rect){ inner_x + cc * (preset_btn_w + 6),
                                          preset_y + rr * (row_h + 4),
                                          preset_btn_w, row_h };
        }
        int reset_y = preset_y + preset_rows * (row_h + 4) + 10;
        L.efx_set_reset = (Rect){ left_slider_x, reset_y, 200, btn };
    } else if (g_settings_tab == SETTINGS_TAB_SESSION) {
        int log_row1_y = content_y;
        L.log_toggle = (Rect){ L.modal.x + w - 140, log_row1_y, 110, btn };

        int log_row2_y = log_row1_y + btn + 10;
        L.log_dir = (Rect){ L.modal.x + 140, log_row2_y, w - 140 - 22, btn };

        /* Browse-logs button below the directory field. Opens the
           logs modal (same as Cmd+Shift+O) so the user can pick a
           past session and re-open it. */
        int log_row3_y = log_row2_y + btn + 12;
        L.log_browse = (Rect){ L.modal.x + 140, log_row3_y, 200, btn };
    } else if (g_settings_tab == SETTINGS_TAB_WINDOW) {
        /* Two rows of size presets:
             row 1: Default | Small | Medium | Large
             row 2: Fill | Fullscreen | Own Space
           "Own Space" is the existing macOS native-fullscreen path;
           "Fullscreen" is a borderless windowed-fullscreen that
           stays in the current Space. "Fill" maximises within the
           current monitor without going fullscreen at all. */
        int bwidth = w - 140 - 22;
        int gap_sty = 6;
        int bh = 30;
        int bx = L.modal.x + 140;
        int row_y = content_y;
        int bw_r1 = (bwidth - 3 * gap_sty) / 4;
        L.startup_default = (Rect){ bx,                              row_y, bw_r1, bh };
        L.startup_small   = (Rect){ bx + 1 * (bw_r1 + gap_sty),       row_y, bw_r1, bh };
        L.startup_medium  = (Rect){ bx + 2 * (bw_r1 + gap_sty),       row_y, bw_r1, bh };
        L.startup_large   = (Rect){ bx + 3 * (bw_r1 + gap_sty),       row_y, bw_r1, bh };
        row_y += bh + gap_sty;
        int bw_r2 = (bwidth - 2 * gap_sty) / 3;
        L.startup_fill       = (Rect){ bx,                              row_y, bw_r2, bh };
        L.startup_borderless = (Rect){ bx + 1 * (bw_r2 + gap_sty),       row_y, bw_r2, bh };
        L.startup_maximized  = (Rect){ bx + 2 * (bw_r2 + gap_sty),       row_y, bw_r2, bh };
        /* Legacy rect — back-compat config still parses
           "fullscreen" but the option no longer has its own pill;
           we keep the rect zero-sized so the click handler skips
           it and falls through to the others. */
        L.startup_fullscreen = (Rect){ 0, 0, 0, 0 };
    } else if (g_settings_tab == SETTINGS_TAB_RECORDING) {
        int row_y = content_y;
        L.rec_dir = (Rect){ L.modal.x + 140, row_y, w - 140 - 22, btn };
    } else if (g_settings_tab == SETTINGS_TAB_HUD) {
        /* Top row: master toggle. Then 2x2 corner picker. Then a
           per-field grid: each row has [show] [colour] [- size +]. */
        int row_y = content_y;
        L.hud_toggle = (Rect){ L.modal.x + 140, row_y, 110, btn };
        row_y += btn + 12;
        int corner_w = 90, corner_h = btn, corner_gap = 6;
        L.hud_pos_tl = (Rect){ L.modal.x + 140,                              row_y, corner_w, corner_h };
        L.hud_pos_tr = (Rect){ L.modal.x + 140 + (corner_w + corner_gap),    row_y, corner_w, corner_h };
        row_y += corner_h + corner_gap;
        L.hud_pos_bl = (Rect){ L.modal.x + 140,                              row_y, corner_w, corner_h };
        L.hud_pos_br = (Rect){ L.modal.x + 140 + (corner_w + corner_gap),    row_y, corner_w, corner_h };
        row_y += btn + 14;

        /* Per-field grid. Field-name labels are drawn by the draw
           routine so we only lay out the click targets here. */
        int label_w = 60;
        int show_w = 78, swatch_w = 26, sz_w = 22, val_w = 28;
        int gap_x = 4;
        int row_h = btn;
        for (int fi = 0; fi < 5; fi++) {
            int x = L.modal.x + 140 + label_w;
            L.hud_show_btn[fi]  = (Rect){ x, row_y, show_w,   row_h }; x += show_w   + gap_x;
            L.hud_color_btn[fi] = (Rect){ x, row_y, swatch_w, row_h }; x += swatch_w + gap_x;
            L.hud_size_dec[fi]  = (Rect){ x, row_y, sz_w,     row_h }; x += sz_w     + gap_x;
            L.hud_size_val[fi]  = (Rect){ x, row_y, val_w,    row_h }; x += val_w    + gap_x;
            L.hud_size_inc[fi]  = (Rect){ x, row_y, sz_w,     row_h };
            row_y += row_h + 4;
        }
        row_y += 8;
        L.hud_cpu_toggle = (Rect){ L.modal.x + 140, row_y, 200, btn };
    } else if (g_settings_tab == SETTINGS_TAB_KEYS) {
        /* List of detected keys + a "+ Generate" button. Each
           row: [Install on host…] button on the right; the
           filename + algo + fingerprint render to the left of
           it in the draw routine. Reserve 26px at the top so the
           "SSH keys (~/.ssh)" header doesn't overlap the rows. */
        int row_y = content_y + 26;
        int row_h = btn;
        int field_x = L.modal.x + 22;
        int field_w_total = w - 22 - 22;
        int install_w = 150;
        int del_w = 32;          /* small × button after Install */
        int gap = 6;
        for (int i = 0; i < SSH_KEYS_MAX; i++) {
            if (i < g_ssh_keys_count) {
                int del_x = field_x + field_w_total - del_w;
                int inst_x = del_x - gap - install_w;
                L.keys_install[i] = (Rect){ inst_x, row_y, install_w, row_h };
                L.keys_delete[i]  = (Rect){ del_x,  row_y, del_w,     row_h };
                row_y += row_h + 6;
            } else {
                L.keys_install[i] = (Rect){0,0,0,0};
                L.keys_delete[i]  = (Rect){0,0,0,0};
            }
        }
        /* Reserve a row of space when there are zero keys so the
           "(no keys yet — generate one below)" hint has somewhere
           to sit between the header and the Generate button. */
        row_y += (g_ssh_keys_count == 0) ? 32 : 8;
        L.keys_generate_btn = (Rect){ field_x, row_y, 200, row_h };
        /* Delete-confirmation modal — same size + position as keygen. */
        {
            int dm_w = 520, dm_h = 200;
            L.keysdel_modal = (Rect){ L.modal.x + (w - dm_w) / 2,
                                       L.modal.y + 100, dm_w, dm_h };
            int dm_pad = 18;
            int dm_btn_y = L.keysdel_modal.y + dm_h - btn - dm_pad;
            L.keysdel_ok     = (Rect){ L.keysdel_modal.x + dm_w - dm_pad - 110,
                                        dm_btn_y, 110, btn };
            L.keysdel_cancel = (Rect){ L.keysdel_ok.x - 8 - 90,
                                        dm_btn_y, 90, btn };
        }
        /* Generate sub-modal — sized for two text inputs + a
           type-pill row + buttons. Drawn only when
           g_keygen_form.open. */
        int km_w = 480, km_h = 240;
        L.keygen_modal = (Rect){ L.modal.x + (w - km_w) / 2,
                                 L.modal.y + 80,
                                 km_w, km_h };
        int km_pad = 18;
        int km_y = L.keygen_modal.y + 50;
        int half = (km_w - 2 * km_pad - 6) / 2;
        L.keygen_type_ed  = (Rect){ L.keygen_modal.x + km_pad,                 km_y, half, btn };
        L.keygen_type_rsa = (Rect){ L.keygen_modal.x + km_pad + half + 6,      km_y, half, btn };
        km_y += btn + 12;
        L.keygen_name_field = (Rect){ L.keygen_modal.x + km_pad, km_y,
                                       km_w - 2 * km_pad, btn };
        km_y += btn + 8;
        L.keygen_pass_field = (Rect){ L.keygen_modal.x + km_pad, km_y,
                                       km_w - 2 * km_pad, btn };
        int km_btn_y = L.keygen_modal.y + km_h - btn - km_pad;
        L.keygen_ok      = (Rect){ L.keygen_modal.x + km_w - km_pad - 110,
                                    km_btn_y, 110, btn };
        L.keygen_cancel  = (Rect){ L.keygen_ok.x - 8 - 90,
                                    km_btn_y, 90, btn };
    } else if (g_settings_tab == SETTINGS_TAB_LAUNCH) {
        /* One row per launch entry:
             [kind pill] [host picker] [▲] [▼] [×]
           Then "+ Add local shell" / "+ Add SSH host" buttons at
           the bottom. We render at most LAUNCH_ENTRY_MAX rows.

           Reserve 22 px above the first row for the "Open these on
           launch" caption (drawn at cap_y = launch_kind[0].y - 22).
           Without this offset the caption lands behind the settings
           tab bar pills above. */
        int row_y = content_y + 22;
        int row_h = btn;
        int kind_w = 72;
        int reorder_w = 26;          /* up / down buttons */
        int active_w = 26;
        int del_w  = 32;
        int gap = 6;
        int field_x = L.modal.x + 22;
        int field_w_total = w - 22 - 22;
        int host_w = field_w_total
                     - kind_w - active_w - 2 * reorder_w - del_w - 5 * gap;
        for (int i = 0; i < LAUNCH_ENTRY_MAX; i++) {
            if (i < g_app_settings.launch_count) {
                int x = field_x;
                L.launch_kind  [i] = (Rect){ x, row_y, kind_w,   row_h }; x += kind_w   + gap;
                L.launch_host  [i] = (Rect){ x, row_y, host_w,   row_h }; x += host_w   + gap;
                L.launch_active[i] = (Rect){ x, row_y, active_w, row_h }; x += active_w + gap;
                /* ▲ / ▼ collapse to zero width at the ends so the
                   click handler can't flip them off the edges. */
                bool can_up   = (i > 0);
                bool can_down = (i < g_app_settings.launch_count - 1);
                L.launch_up  [i] = (Rect){ x, row_y, can_up   ? reorder_w : 0, row_h };
                x += reorder_w + gap;
                L.launch_down[i] = (Rect){ x, row_y, can_down ? reorder_w : 0, row_h };
                x += reorder_w + gap;
                L.launch_del [i] = (Rect){ x, row_y, del_w,    row_h };
                row_y += row_h + 6;
            } else {
                L.launch_kind  [i] = (Rect){0,0,0,0};
                L.launch_host  [i] = (Rect){0,0,0,0};
                L.launch_active[i] = (Rect){0,0,0,0};
                L.launch_up    [i] = (Rect){0,0,0,0};
                L.launch_down  [i] = (Rect){0,0,0,0};
                L.launch_del   [i] = (Rect){0,0,0,0};
            }
        }
        row_y += 6;
        int add_w = (field_w_total - 2 * gap) / 3;
        L.launch_add_local   = (Rect){ field_x,                            row_y, add_w, row_h };
        L.launch_add_ssh     = (Rect){ field_x +     add_w +     gap,      row_y, add_w, row_h };
        L.launch_add_session = (Rect){ field_x + 2 * add_w + 2 * gap,      row_y, add_w, row_h };
    } else if (g_settings_tab == SETTINGS_TAB_SESSIONS) {
        /* Row per saved session: name + [Open] [Edit] [×]; "+ New
           session" sits above the list. The designer modal handles
           create/edit, this tab is just a dispatch surface. */
        int row_y = content_y;
        int row_h = btn;
        int gap = 6;
        int field_x = L.modal.x + 22;
        int field_w_total = w - 22 - 22;
        int btn_open = 70, btn_edit = 60, btn_del = 32;
        int name_w = field_w_total - btn_open - btn_edit - btn_del - 3 * gap;
        L.sess_new_btn = (Rect){ field_x, row_y, 200, row_h };
        row_y += row_h + 12;
        for (int i = 0; i < SESSIONS_MAX; i++) {
            if (i < g_sessions_count) {
                int x = field_x;
                L.sess_row [i] = (Rect){ x, row_y, name_w,  row_h }; x += name_w  + gap;
                L.sess_open[i] = (Rect){ x, row_y, btn_open, row_h }; x += btn_open + gap;
                L.sess_edit[i] = (Rect){ x, row_y, btn_edit, row_h }; x += btn_edit + gap;
                L.sess_del [i] = (Rect){ x, row_y, btn_del,  row_h };
                row_y += row_h + 4;
            } else {
                L.sess_row [i] = (Rect){0,0,0,0};
                L.sess_open[i] = (Rect){0,0,0,0};
                L.sess_edit[i] = (Rect){0,0,0,0};
                L.sess_del [i] = (Rect){0,0,0,0};
            }
        }
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
    /* Effects-tab slider drag continuation. The drag idx is either a
       valid EfxSlider (0..EFX_SLIDER_COUNT-1) → that slider's value,
       or EFX_SLIDER_COUNT (sentinel) → the dedicated decay slider. */
    if (g_settings_efx_drag) {
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            Rect rr;
            float *t = NULL;
            if (g_settings_efx_drag_idx == EFX_SLIDER_COUNT) {
                rr = L.efx_set_decay;
                t  = &g_app_settings.effects.decay;
            } else {
                rr = L.efx_set_slider[g_settings_efx_drag_idx];
                t  = efx_slider_value(&g_app_settings.effects,
                                      (EfxSlider)g_settings_efx_drag_idx);
            }
            float v = (float)(mx - rr.x) / (float)(rr.w > 1 ? rr.w : 1);
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            if (t) *t = v;
            /* Live preview: copy the global default into every open
               pane so the user sees the change as they drag. Panes
               that have a per-host override applied still inherit
               the global; we don't try to be cleverer than that. */
            for (int ti = 0; ti < g_num_tabs; ti++) {
                Tab *tt = g_tabs[ti];
                if (!tt) continue;
                for (PaneNode *_l = pane_tree_first_leaf(tt->root); _l;
                     _l = pane_tree_next_leaf(_l)) {
                    if (_l->pane) _l->pane->effects = g_app_settings.effects;
                }
            }
            return;
        }
        g_settings_efx_drag = false;
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
                g_settings_launch_dropdown = -1;
                g_settings_focused_list = SETTINGS_FOCUS_NONE;
                g_slider_drag_track.w = 0;
                g_slider_drag_target = NULL;
                /* Refresh the keys list every time the user
                   re-enters the tab — `ssh-keygen` from another
                   terminal or rbterm's own generate path may
                   have added entries since last open. */
                if (i == SETTINGS_TAB_KEYS) ssh_keys_rescan();
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
    if (L.ligatures_toggle.w > 0 && rect_hit(L.ligatures_toggle, mx, my)) {
        g_app_settings.ligatures = !g_app_settings.ligatures;
        renderer_set_ligatures(r, g_app_settings.ligatures);
        snprintf(g_settings_status, sizeof(g_settings_status),
                 "Ligatures %s. Use a font like Fira Code / JetBrains Mono "
                 "and type \"=> != >=\" to see them.",
                 g_app_settings.ligatures ? "on" : "off");
        return;
    }
    if (rect_hit(L.font_list, mx, my)) {
        int row_h = 22;
        int row = (my - L.font_list.y) / row_h + g_font_list_scroll;
        int idx = font_display_to_idx(row);
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
        if (!hit && g_settings_tab == SETTINGS_TAB_CURSOR) {
            for (int i = 0; i < SSH_COLOR_PRESET_COUNT; i++) {
                if (rect_hit(L.cur_color_swatch[i], mx, my)) {
                    snprintf(g_app_settings.cursor_color,
                             sizeof(g_app_settings.cursor_color),
                             "%s", SSH_COLOR_PRESETS[i]);
                    /* Apply live to every open pane. */
                    Color cc;
                    if (parse_hex_color(g_app_settings.cursor_color, &cc)) {
                        uint32_t rgb = ((uint32_t)cc.r << 16) |
                                       ((uint32_t)cc.g << 8) | cc.b;
                        for (int ti = 0; ti < g_num_tabs; ti++) {
                            for (PaneNode *_l = pane_tree_first_leaf(g_tabs[ti]->root);
                                 _l; _l = pane_tree_next_leaf(_l)) {
                                if (_l->pane && _l->pane->scr) {
                                    screen_set_cursor_color(_l->pane->scr, rgb);
                                }
                            }
                        }
                    }
                    return;
                }
            }
            if (rect_hit(L.cur_color_swatch[SSH_COLOR_PRESET_COUNT], mx, my)) {
                g_app_settings.cursor_color[0] = 0;
                /* "Default" — re-apply each Screen's seed colour. */
                for (int ti = 0; ti < g_num_tabs; ti++) {
                    for (PaneNode *_l = pane_tree_first_leaf(g_tabs[ti]->root);
                         _l; _l = pane_tree_next_leaf(_l)) {
                        if (_l->pane && _l->pane->scr) {
                            screen_set_cursor_color(
                                _l->pane->scr,
                                screen_default_fg(_l->pane->scr));
                        }
                    }
                }
                return;
            }
        }
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
    if (L.log_browse.w > 0 && rect_hit(L.log_browse, mx, my)) {
        logs_open();
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
    /* Effects tab — sliders (capture for drag), Decay slider, Phosphor
       pills, Preset pills, Reset. Use a small helper so each slot
       hits the same "update + push live" pipeline. */
    if (g_settings_tab == SETTINGS_TAB_EFFECTS) {
        #define EFX_PUSH_LIVE() do {                                    \
            for (int ti = 0; ti < g_num_tabs; ti++) {                   \
                Tab *tt = g_tabs[ti];                                   \
                if (!tt) continue;                                      \
                for (PaneNode *_l = pane_tree_first_leaf(tt->root); _l; \
                     _l = pane_tree_next_leaf(_l)) {                    \
                    if (_l->pane) _l->pane->effects = g_app_settings.effects; \
                }                                                       \
            }                                                           \
        } while (0)
        for (int i = 0; i < EFX_SLIDER_COUNT; i++) {
            if (rect_hit(L.efx_set_slider[i], mx, my)) {
                g_settings_efx_drag = true;
                g_settings_efx_drag_idx = i;
                Rect rr = L.efx_set_slider[i];
                float v = (float)(mx - rr.x) / (float)(rr.w > 1 ? rr.w : 1);
                if (v < 0.0f) v = 0.0f;
                if (v > 1.0f) v = 1.0f;
                float *t = efx_slider_value(&g_app_settings.effects, (EfxSlider)i);
                if (t) *t = v;
                EFX_PUSH_LIVE();
                return;
            }
        }
        if (rect_hit(L.efx_set_decay, mx, my)) {
            /* Reuse the slider-drag machinery for Decay too — store it
               in a sentinel "drag idx" of EFX_SLIDER_COUNT so the
               continuation block below knows to target ->decay. */
            g_settings_efx_drag = true;
            g_settings_efx_drag_idx = EFX_SLIDER_COUNT;   /* "decay" sentinel */
            Rect rr = L.efx_set_decay;
            float v = (float)(mx - rr.x) / (float)(rr.w > 1 ? rr.w : 1);
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            g_app_settings.effects.decay = v;
            EFX_PUSH_LIVE();
            return;
        }
        for (int i = 0; i < PHOSPHOR_COUNT; i++) {
            if (rect_hit(L.efx_set_phos[i], mx, my)) {
                g_app_settings.effects.phosphor = (PhosphorMode)i;
                EFX_PUSH_LIVE();
                return;
            }
        }
        for (int i = 0; i < EFX_PRESET_COUNT; i++) {
            if (rect_hit(L.efx_set_preset[i], mx, my)) {
                rec_effects_apply_preset(&g_app_settings.effects, (EfxPreset)i);
                EFX_PUSH_LIVE();
                snprintf(g_settings_status, sizeof(g_settings_status),
                         "Applied '%s' preset. Save as Default to persist.",
                         rec_effects_preset_label((EfxPreset)i));
                return;
            }
        }
        if (rect_hit(L.efx_set_reset, mx, my)) {
            rec_effects_defaults(&g_app_settings.effects);
            EFX_PUSH_LIVE();
            snprintf(g_settings_status, sizeof(g_settings_status),
                     "Effects reset. Save as Default to persist.");
            return;
        }
        #undef EFX_PUSH_LIVE
    }
    /* Startup window mode pickers — applied on next launch. */
    {
        int pick = -1;
        if      (rect_hit(L.startup_default,    mx, my)) pick = STARTUP_WINDOW_DEFAULT;
        else if (rect_hit(L.startup_small,      mx, my)) pick = STARTUP_WINDOW_SMALL;
        else if (rect_hit(L.startup_medium,     mx, my)) pick = STARTUP_WINDOW_MEDIUM;
        else if (rect_hit(L.startup_large,      mx, my)) pick = STARTUP_WINDOW_LARGE;
        else if (rect_hit(L.startup_fill,       mx, my)) pick = STARTUP_WINDOW_FILL;
        else if (rect_hit(L.startup_borderless, mx, my)) pick = STARTUP_WINDOW_BORDERLESS;
        else if (rect_hit(L.startup_maximized,  mx, my)) pick = STARTUP_WINDOW_MAXIMIZED;
        if (pick >= 0) {
            g_app_settings.startup_window = pick;
            snprintf(g_settings_status, sizeof(g_settings_status),
                     "Startup window mode set. Save as Default to persist.");
            return;
        }
    }
    /* HUD tab — master toggle, corner picker, per-field grid.
       Show toggle flips visibility; colour swatch cycles through
       the preset palette; size +/- bumps within 10..18. */
    if (g_settings_tab == SETTINGS_TAB_HUD) {
        if (rect_hit(L.hud_toggle, mx, my)) {
            g_app_settings.show_hud = !g_app_settings.show_hud;
            return;
        }
        int pos = -1;
        if      (rect_hit(L.hud_pos_tl, mx, my)) pos = HUD_POS_TOP_LEFT;
        else if (rect_hit(L.hud_pos_tr, mx, my)) pos = HUD_POS_TOP_RIGHT;
        else if (rect_hit(L.hud_pos_bl, mx, my)) pos = HUD_POS_BOTTOM_LEFT;
        else if (rect_hit(L.hud_pos_br, mx, my)) pos = HUD_POS_BOTTOM_RIGHT;
        if (pos >= 0) {
            g_app_settings.hud_pos = pos;
            return;
        }
        for (int fi = 0; fi < HUD_FIELD_COUNT; fi++) {
            if (rect_hit(L.hud_show_btn[fi], mx, my)) {
                g_app_settings.hud_show[fi] = !g_app_settings.hud_show[fi];
                return;
            }
            if (rect_hit(L.hud_color_btn[fi], mx, my)) {
                g_app_settings.hud_color[fi] =
                    (g_app_settings.hud_color[fi] + 1) % HUD_PALETTE_COUNT;
                return;
            }
            if (rect_hit(L.hud_size_dec[fi], mx, my)) {
                int s = g_app_settings.hud_size[fi] - 1;
                if (s < 10) s = 10;
                g_app_settings.hud_size[fi] = s;
                return;
            }
            if (rect_hit(L.hud_size_inc[fi], mx, my)) {
                int s = g_app_settings.hud_size[fi] + 1;
                if (s > 18) s = 18;
                g_app_settings.hud_size[fi] = s;
                return;
            }
        }
        if (rect_hit(L.hud_cpu_toggle, mx, my)) {
            g_app_settings.hud_show_cpu = !g_app_settings.hud_show_cpu;
            return;
        }
    }
    /* Keys tab — generate / install dropdowns. The keygen sub-modal
       intercepts clicks while open. */
    if (g_settings_tab == SETTINGS_TAB_KEYS) {
        /* Delete-confirmation sub-modal — eats every click while up. */
        if (g_keys_delete_idx >= 0 && g_keys_delete_idx < g_ssh_keys_count) {
            if (rect_hit(L.keysdel_cancel, mx, my)) {
                g_keys_delete_idx = -1;
                return;
            }
            if (rect_hit(L.keysdel_ok, mx, my)) {
                int ki = g_keys_delete_idx;
                const char *priv = g_ssh_keys[ki].privpath;
                const char *pub  = g_ssh_keys[ki].pubpath;
                int rc1 = (priv[0] && g_ssh_keys[ki].has_private)
                          ? unlink(priv) : 0;
                int rc2 = (pub[0])  ? unlink(pub)  : 0;
                if (rc1 == 0 && rc2 == 0) {
                    snprintf(g_keys_status, sizeof(g_keys_status),
                             "Deleted %s.", g_ssh_keys[ki].name);
                } else {
                    snprintf(g_keys_status, sizeof(g_keys_status),
                             "Delete %s: %s",
                             g_ssh_keys[ki].name, strerror(errno));
                }
                g_keys_delete_idx = -1;
                ssh_keys_rescan();
                return;
            }
            /* Click outside the sub-modal cancels. */
            if (!rect_hit(L.keysdel_modal, mx, my)) {
                g_keys_delete_idx = -1;
                return;
            }
            return;
        }
        /* Sub-modal first (eats every click while up). */
        if (g_keygen_form.open) {
            if (rect_hit(L.keygen_type_ed, mx, my))
                { g_keygen_form.type_idx = 0; return; }
            if (rect_hit(L.keygen_type_rsa, mx, my))
                { g_keygen_form.type_idx = 1; return; }
            if (rect_hit(L.keygen_name_field, mx, my))
                { g_keygen_form.focus_field = 0; return; }
            if (rect_hit(L.keygen_pass_field, mx, my))
                { g_keygen_form.focus_field = 1; return; }
            if (rect_hit(L.keygen_cancel, mx, my)) {
                memset(&g_keygen_form, 0, sizeof(g_keygen_form));
                return;
            }
            if (rect_hit(L.keygen_ok, mx, my)) {
                const char *type_name =
                    (g_keygen_form.type_idx == 1) ? "rsa" : "ed25519";
#ifdef RBTERM_SSH
                /* RSA-4096 generation is CPU-bound for a few seconds
                   on slow hardware — kick a worker so the modal can
                   keep redrawing with a "Generating..." status. */
                if (bg_key_generate_busy()) {
                    snprintf(g_keygen_form.status,
                             sizeof(g_keygen_form.status),
                             "Generation already in progress");
                } else if (!bg_key_generate_kick(type_name,
                                                  g_keygen_form.name,
                                                  g_keygen_form.pass)) {
                    snprintf(g_keygen_form.status,
                             sizeof(g_keygen_form.status),
                             "Couldn't start generate thread");
                } else {
                    snprintf(g_keygen_form.status,
                             sizeof(g_keygen_form.status),
                             "Generating %s key ...", type_name);
                }
#else
                char err[256] = {0};
                bool ok = ssh_keys_generate_native(
                    type_name, g_keygen_form.name,
                    g_keygen_form.pass, err, sizeof(err));
                if (ok) {
                    snprintf(g_keys_status, sizeof(g_keys_status),
                             "Generated %s.", g_keygen_form.name);
                    ssh_keys_rescan();
                    memset(&g_keygen_form, 0, sizeof(g_keygen_form));
                } else {
                    snprintf(g_keygen_form.status,
                             sizeof(g_keygen_form.status),
                             "%s", err[0] ? err : "generate failed");
                }
#endif
                return;
            }
            /* Click outside the sub-modal closes it. */
            if (!rect_hit(L.keygen_modal, mx, my)) {
                memset(&g_keygen_form, 0, sizeof(g_keygen_form));
                return;
            }
            return;
        }
        /* Open install dropdown? Route click to it. */
        if (g_keys_install_dropdown >= 0 &&
            g_keys_install_dropdown < g_ssh_keys_count) {
            int ki = g_keys_install_dropdown;
            Rect ib = L.keys_install[ki];
            int row_h = 22;
            int n = g_ssh_profile_count;
            int dh = n * row_h;
            int max_dh = 220;
            if (dh > max_dh) dh = max_dh;
            Rect dd = (Rect){ ib.x, ib.y + ib.h, ib.w, dh };
            if (rect_hit(dd, mx, my)) {
                int rel = (my - dd.y) / row_h + g_keys_install_scroll;
                if (rel >= 0 && rel < n) {
                    char pubtext[8192];
                    if (ssh_keys_read_pubfile(g_ssh_keys[ki].pubpath,
                                               pubtext, sizeof(pubtext))) {
#ifdef RBTERM_SSH
                        /* Async — install runs full libssh connect +
                           auth + exec on a worker thread. UI shows
                           "Installing..." while in flight; the result
                           lands in g_keys_status via bg_ssh_integrate. */
                        if (bg_key_install_busy()) {
                            snprintf(g_keys_status, sizeof(g_keys_status),
                                     "Install already in progress");
                        } else if (!bg_key_install_kick(
                                       &g_ssh_profiles[rel], pubtext,
                                       g_ssh_keys[ki].name,
                                       g_ssh_profiles[rel].name)) {
                            snprintf(g_keys_status, sizeof(g_keys_status),
                                     "Couldn't start install thread");
                        } else {
                            snprintf(g_keys_status, sizeof(g_keys_status),
                                     "Installing %s on %s ...",
                                     g_ssh_keys[ki].name,
                                     g_ssh_profiles[rel].name);
                        }
#else
                        char err[256] = {0};
                        bool ok = ssh_keys_install_native(
                            &g_ssh_profiles[rel], pubtext,
                            err, sizeof(err));
                        if (ok) {
                            snprintf(g_keys_status, sizeof(g_keys_status),
                                     "Installed %s on %s.",
                                     g_ssh_keys[ki].name,
                                     g_ssh_profiles[rel].name);
                        } else {
                            snprintf(g_keys_status, sizeof(g_keys_status),
                                     "%s → %s: %s",
                                     g_ssh_keys[ki].name,
                                     g_ssh_profiles[rel].name,
                                     err[0] ? err : "install failed");
                        }
#endif
                    } else {
                        snprintf(g_keys_status, sizeof(g_keys_status),
                                 "Couldn't read %s", g_ssh_keys[ki].pubpath);
                    }
                    g_keys_install_dropdown = -1;
                }
                return;
            }
            /* Click outside closes the dropdown. */
            g_keys_install_dropdown = -1;
        }
        /* Per-row Install / Delete buttons. */
        for (int i = 0; i < g_ssh_keys_count; i++) {
            if (L.keys_install[i].w > 0 &&
                rect_hit(L.keys_install[i], mx, my)) {
                ssh_profiles_load();
                g_keys_install_dropdown = i;
                g_keys_install_scroll = 0;
                return;
            }
            if (L.keys_delete[i].w > 0 &&
                rect_hit(L.keys_delete[i], mx, my)) {
                g_keys_delete_idx = i;
                g_keys_install_dropdown = -1;
                return;
            }
        }
        if (rect_hit(L.keys_generate_btn, mx, my)) {
            memset(&g_keygen_form, 0, sizeof(g_keygen_form));
            g_keygen_form.open = true;
            g_keygen_form.type_idx = 0;
            snprintf(g_keygen_form.name, sizeof(g_keygen_form.name),
                     "%s", "id_rbterm");
            g_keygen_form.focus_field = 0;
            return;
        }
    }
    /* Sessions tab — list with [Open][Edit][×] per row + a "+ New
       session" button. The designer modal handles create/edit; this
       tab is just the dispatch surface. */
    if (g_settings_tab == SETTINGS_TAB_SESSIONS) {
        if (rect_hit(L.sess_new_btn, mx, my)) {
            g_ui_mode = UI_NORMAL;             /* close Settings */
            session_designer_open_for(-1);
            return;
        }
        for (int i = 0; i < g_sessions_count; i++) {
            if (rect_hit(L.sess_open[i], mx, my)) {
                int win_w = GetScreenWidth();
                int win_h = GetScreenHeight();
                int derive_cols = (win_w - 2 * r->pad_x) / r->cell_w;
                int derive_rows = (win_h - TAB_BAR_H - 2 * r->pad_y) / r->cell_h;
                if (derive_cols < 20) derive_cols = 20;
                if (derive_rows < 5)  derive_rows = 5;
                g_ui_mode = UI_NORMAL;
                tab_open_from_session(&g_sessions[i], derive_cols, derive_rows);
                return;
            }
            if (rect_hit(L.sess_edit[i], mx, my)) {
                g_ui_mode = UI_NORMAL;
                session_designer_open_for(i);
                return;
            }
            if (rect_hit(L.sess_del[i], mx, my)) {
                sessions_remove(i);
                sessions_save();
                return;
            }
        }
        return;
    }

    /* Launch tab — add / delete entries, toggle kind, open the
       saved-host dropdown for SSH rows, pick from it. */
    if (g_settings_tab == SETTINGS_TAB_LAUNCH) {
        /* If a dropdown is open, route this click to it first.
           Click inside → pick a host; click outside → close. */
        if (g_settings_launch_dropdown >= 0 &&
            g_settings_launch_dropdown < g_app_settings.launch_count) {
            int i = g_settings_launch_dropdown;
            Rect hb = L.launch_host[i];
            int row_h = 22;
            /* Pick the source list based on this row's kind so SSH
               rows show saved hosts and Session rows show saved
               sessions. */
            bool is_session = (g_app_settings.launch[i].kind == LAUNCH_KIND_SESSION);
            int n = is_session ? g_sessions_count : g_ssh_profile_count;
            int dh = n * row_h;
            int max_dh = 220;
            if (dh > max_dh) dh = max_dh;
            Rect dd = (Rect){ hb.x, hb.y + hb.h, hb.w, dh };
            if (rect_hit(dd, mx, my)) {
                int rel = (my - dd.y) / row_h + g_settings_launch_scroll;
                if (rel >= 0 && rel < n) {
                    const char *pick = is_session
                                          ? g_sessions[rel].name
                                          : g_ssh_profiles[rel].name;
                    snprintf(g_app_settings.launch[i].host,
                             sizeof(g_app_settings.launch[i].host),
                             "%s", pick);
                    g_settings_launch_dropdown = -1;
                }
                return;
            }
            /* Wheel scroll inside the dropdown. */
            {
                Vector2 hover = GetMousePosition();
                if (rect_hit(dd, (int)hover.x, (int)hover.y)) {
                    float wheel = GetMouseWheelMove();
                    if (wheel != 0.0f) {
                        g_settings_launch_scroll -= (int)(wheel * 3.0f);
                        if (g_settings_launch_scroll < 0)
                            g_settings_launch_scroll = 0;
                    }
                }
            }
            /* Click on the same host field again → close. Anything
               outside both the field and the panel also closes. */
            if (!rect_hit(hb, mx, my)) {
                g_settings_launch_dropdown = -1;
                /* Fall through so the click can still hit other
                   controls (e.g. delete button on a different
                   row). */
            } else {
                g_settings_launch_dropdown = -1;
                return;
            }
        }

        if (rect_hit(L.launch_add_local, mx, my)) {
            if (g_app_settings.launch_count < LAUNCH_ENTRY_MAX) {
                int i = g_app_settings.launch_count++;
                g_app_settings.launch[i].kind = 0;
                g_app_settings.launch[i].host[0] = 0;
            }
            return;
        }
        if (rect_hit(L.launch_add_ssh, mx, my)) {
            if (g_app_settings.launch_count < LAUNCH_ENTRY_MAX) {
                int i = g_app_settings.launch_count++;
                g_app_settings.launch[i].kind = 1;
                /* Pre-fill with first saved profile if any. */
                if (g_ssh_profile_count > 0) {
                    snprintf(g_app_settings.launch[i].host,
                             sizeof(g_app_settings.launch[i].host),
                             "%s", g_ssh_profiles[0].name);
                } else {
                    g_app_settings.launch[i].host[0] = 0;
                }
            }
            return;
        }
        if (rect_hit(L.launch_add_session, mx, my)) {
            if (g_app_settings.launch_count < LAUNCH_ENTRY_MAX) {
                /* Refresh the saved-sessions list so the picker
                   sees anything the user just authored. */
                sessions_load();
                int i = g_app_settings.launch_count++;
                g_app_settings.launch[i].kind = LAUNCH_KIND_SESSION;
                if (g_sessions_count > 0) {
                    snprintf(g_app_settings.launch[i].host,
                             sizeof(g_app_settings.launch[i].host),
                             "%s", g_sessions[0].name);
                } else {
                    g_app_settings.launch[i].host[0] = 0;
                }
            }
            return;
        }
        for (int i = 0; i < g_app_settings.launch_count; i++) {
            if (rect_hit(L.launch_del[i], mx, my)) {
                for (int j = i; j + 1 < g_app_settings.launch_count; j++) {
                    g_app_settings.launch[j] = g_app_settings.launch[j + 1];
                }
                g_app_settings.launch_count--;
                if (g_settings_launch_dropdown == i)
                    g_settings_launch_dropdown = -1;
                else if (g_settings_launch_dropdown > i)
                    g_settings_launch_dropdown--;
                /* Keep launch_active in range. If the deleted row
                   was the active one, fall back to the new first
                   row; otherwise shift down to track the same
                   entry. */
                if (g_app_settings.launch_active == i) {
                    g_app_settings.launch_active = 0;
                } else if (g_app_settings.launch_active > i) {
                    g_app_settings.launch_active--;
                }
                return;
            }
            if (L.launch_active[i].w > 0 && rect_hit(L.launch_active[i], mx, my)) {
                g_app_settings.launch_active = i;
                return;
            }
            if (L.launch_up[i].w > 0 && rect_hit(L.launch_up[i], mx, my)) {
                /* Swap with the row above; follow the dropdown
                   focus so it stays anchored to the same entry.
                   memcpy through a byte buffer because the entry
                   type is an anonymous struct inside AppSettings —
                   no name to give a temp variable. */
                if (i > 0) {
                    char tmp[sizeof(g_app_settings.launch[0])];
                    memcpy(tmp, &g_app_settings.launch[i - 1], sizeof(tmp));
                    memcpy(&g_app_settings.launch[i - 1],
                           &g_app_settings.launch[i], sizeof(tmp));
                    memcpy(&g_app_settings.launch[i], tmp, sizeof(tmp));
                    if (g_settings_launch_dropdown == i)        g_settings_launch_dropdown = i - 1;
                    else if (g_settings_launch_dropdown == i-1) g_settings_launch_dropdown = i;
                    if (g_app_settings.launch_active == i)        g_app_settings.launch_active = i - 1;
                    else if (g_app_settings.launch_active == i-1) g_app_settings.launch_active = i;
                }
                return;
            }
            if (L.launch_down[i].w > 0 && rect_hit(L.launch_down[i], mx, my)) {
                if (i + 1 < g_app_settings.launch_count) {
                    char tmp[sizeof(g_app_settings.launch[0])];
                    memcpy(tmp, &g_app_settings.launch[i + 1], sizeof(tmp));
                    memcpy(&g_app_settings.launch[i + 1],
                           &g_app_settings.launch[i], sizeof(tmp));
                    memcpy(&g_app_settings.launch[i], tmp, sizeof(tmp));
                    if (g_settings_launch_dropdown == i)        g_settings_launch_dropdown = i + 1;
                    else if (g_settings_launch_dropdown == i+1) g_settings_launch_dropdown = i;
                    if (g_app_settings.launch_active == i)        g_app_settings.launch_active = i + 1;
                    else if (g_app_settings.launch_active == i+1) g_app_settings.launch_active = i;
                }
                return;
            }
            if (rect_hit(L.launch_kind[i], mx, my)) {
                /* Cycle through the three kinds: local → ssh →
                   session → local. Resets the host field whenever
                   we leave a non-local kind so the row doesn't
                   carry stale ssh aliases into a session entry. */
                int next = (g_app_settings.launch[i].kind + 1) % 3;
                g_app_settings.launch[i].kind = next;
                if (next == LAUNCH_KIND_LOCAL)
                    g_app_settings.launch[i].host[0] = 0;
                g_settings_launch_dropdown = -1;
                return;
            }
            if (rect_hit(L.launch_host[i], mx, my)) {
                int kind = g_app_settings.launch[i].kind;
                if (kind == LAUNCH_KIND_SSH) {
                    /* Open the saved-hosts list for this row. */
                    ssh_profiles_load();
                    g_settings_launch_dropdown = i;
                    g_settings_launch_scroll = 0;
                } else if (kind == LAUNCH_KIND_SESSION) {
                    /* Open the saved-sessions list for this row.
                       The dropdown is rendered by the same path —
                       drawn by index, picked from g_sessions. */
                    sessions_load();
                    g_settings_launch_dropdown = i;
                    g_settings_launch_scroll = 0;
                }
                return;
            }
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

    /* Keygen sub-modal text input. Whichever field has focus
       receives keystrokes; Backspace deletes; Enter / Tab swaps
       focus or fires Generate. */
    if (g_keygen_form.open && !IsKeyPressed(KEY_ESCAPE)) {
        bool kg_shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
        bool kg_ctrl  = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
#if defined(__APPLE__)
        bool kg_cmd   = IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER);
#else
        bool kg_cmd   = false;
#endif
        bool kg_mod   = kg_ctrl || kg_cmd;
        (void)kg_shift;
        char *buf = (g_keygen_form.focus_field == 1)
                    ? g_keygen_form.pass : g_keygen_form.name;
        size_t cap = (g_keygen_form.focus_field == 1)
                    ? sizeof(g_keygen_form.pass)
                    : sizeof(g_keygen_form.name);

        /* Cmd+A — mark the field as fully selected. Next typed
           char or paste replaces it; Backspace clears it. */
        if (kg_mod && IsKeyPressed(KEY_A)) {
            g_keygen_form.sel_all = true;
            return;
        }
        /* Cmd+C — copy field. Passphrase is masked in the UI; not
           round-tripping it through the system clipboard matches
           the SSH form's F_PASS policy. */
        if (kg_mod && IsKeyPressed(KEY_C)) {
            if (g_keygen_form.focus_field == 0 && *buf) {
                SetClipboardText(buf);
            }
            return;
        }
        /* Cmd+V — paste clipboard. Replace if sel_all, else append.
           Filters newlines / tabs / non-ASCII so a clipboard that
           ends in `\n` doesn't terminate the form. */
        if (kg_mod && IsKeyPressed(KEY_V)) {
            const char *clip = GetClipboardText();
            if (clip && *clip) {
                if (g_keygen_form.sel_all) {
                    buf[0] = 0;
                    g_keygen_form.sel_all = false;
                }
                size_t bl = strlen(buf);
                for (const char *q = clip; *q; q++) {
                    unsigned char c = (unsigned char)*q;
                    if (c < 32 || c >= 127) continue;
                    if (bl + 1 >= cap) break;
                    buf[bl++] = (char)c;
                }
                buf[bl] = 0;
            }
            return;
        }

        size_t len = strlen(buf);
        if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) {
            if (g_keygen_form.sel_all) {
                buf[0] = 0;
                g_keygen_form.sel_all = false;
            } else if (len > 0) {
                buf[len - 1] = 0;
            }
        }
        if (IsKeyPressed(KEY_TAB)) {
            g_keygen_form.focus_field ^= 1;
            g_keygen_form.sel_all = false;
        }
        if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
            const char *type_name =
                (g_keygen_form.type_idx == 1) ? "rsa" : "ed25519";
#ifdef RBTERM_SSH
            if (bg_key_generate_busy()) {
                snprintf(g_keygen_form.status,
                         sizeof(g_keygen_form.status),
                         "Generation already in progress");
            } else if (!bg_key_generate_kick(type_name,
                                              g_keygen_form.name,
                                              g_keygen_form.pass)) {
                snprintf(g_keygen_form.status,
                         sizeof(g_keygen_form.status),
                         "Couldn't start generate thread");
            } else {
                snprintf(g_keygen_form.status,
                         sizeof(g_keygen_form.status),
                         "Generating %s key ...", type_name);
            }
#else
            char err[256] = {0};
            bool ok = ssh_keys_generate_native(
                type_name, g_keygen_form.name,
                g_keygen_form.pass, err, sizeof(err));
            if (ok) {
                snprintf(g_keys_status, sizeof(g_keys_status),
                         "Generated %s.", g_keygen_form.name);
                ssh_keys_rescan();
                memset(&g_keygen_form, 0, sizeof(g_keygen_form));
            } else {
                snprintf(g_keygen_form.status,
                         sizeof(g_keygen_form.status),
                         "%s", err[0] ? err : "generate failed");
            }
#endif
            return;
        }
        int cp;
        while ((cp = GetCharPressed()) != 0) {
            if (cp < 32 || cp >= 127) continue;
            /* First keystroke after Cmd+A replaces the field. */
            if (g_keygen_form.sel_all) {
                buf[0] = 0;
                len = 0;
                g_keygen_form.sel_all = false;
            }
            if (len + 1 >= cap) continue;
            buf[len++] = (char)cp;
            buf[len] = 0;
        }
        return;
    }

    if (IsKeyPressed(KEY_ESCAPE)) {
        if (g_settings_dir_focus) { g_settings_dir_focus = false; return; }
        if (g_settings_recdir_focus) { g_settings_recdir_focus = false; return; }
        if (g_keys_delete_idx >= 0) {
            g_keys_delete_idx = -1; return;
        }
        if (g_keys_install_dropdown >= 0) {
            g_keys_install_dropdown = -1; return;
        }
        if (g_settings_launch_dropdown >= 0) {
            g_settings_launch_dropdown = -1; return;
        }
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

/* ---------- Save-recording modal ----------------------------------------

   Layout grows top-to-bottom in fixed-height sections:

     ┌─ Save recording ───────────────────────────────┐
     │ Path  [editable text                       ]   │  path row
     │ Format [cast][txt][gif][mp4][webm][apng][webp] │  fmt row
     │ ────── Effects ─────                          │  separator
     │  CRT      [slider]    VHS      [slider]        │  effects rows
     │  Bloom    [slider]    Glitch   [slider]        │  (2 columns)
     │  Grain    [slider]    Halftone [slider]        │
     │  Phosphor [N|G|A|B]   Pixelate [1|2|…|8]       │
     │  Speed   [0.5×|1×|2×|4×|8×]                    │  speed row
     │  status / hint line                            │
     │                       [Preview][Close][Save]   │  footer
     └────────────────────────────────────────────────┘

   Each slider's hit rect IS the slider track; click-to-set + thumb-drag
   both use (mx - rect.x) / rect.w. Phosphor / Pixelate / Speed pickers
   are pill-button rows (matches the format row's visual style). */

typedef struct {
    Rect modal;
    Rect path;
    Rect fmt[REC_FMT_COUNT];
    Rect efx_slider[EFX_SLIDER_COUNT];      /* the draggable slider track */
    Rect phos[PHOSPHOR_COUNT];              /* 4 phosphor mode buttons */
    Rect speed[EFX_SPEED_COUNT];            /* 5 playback-speed buttons */
    Rect preset[EFX_PRESET_COUNT];          /* preset pills (Nostromo, VHS, ...) */
    Rect captions_btn;                      /* "Show captions" toggle (overlay user input) */
    Rect save_btn, close_btn, preview_btn;
} RecSaveLayout;

static void draw_rec_save_modal(Renderer *r, int win_w, int win_h, RecSaveLayout L);

static RecSaveLayout rec_save_layout(int win_w, int win_h) {
    RecSaveLayout L = {0};
    /* Width: keeps the format pills + status path readable at 16pt mono.
       Height grew from 280→460→640 as the effects panel + preset
       grid landed; we still clamp to window-minus-margin so small
       displays stay usable. */
    int w = 1080, h = 640;
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

    /* Preset pill grid — same 5-column wrap as Settings → Effects so
       muscle memory carries over. Sits above the sliders so picking
       a look is the prominent action; the sliders below are the
       advanced tuning. */
    const int PRESETS_COLS = 5;
    int preset_y = fmt_y + fmt_h + 22 + 18;          /* leaves room for "Preset" caption */
    int preset_btn_h = 26;
    int preset_btn_w = (field_w - (PRESETS_COLS - 1) * 6) / PRESETS_COLS;
    int preset_rows = (EFX_PRESET_COUNT + PRESETS_COLS - 1) / PRESETS_COLS;
    for (int i = 0; i < EFX_PRESET_COUNT; i++) {
        int rrow = i / PRESETS_COLS;
        int rcol = i % PRESETS_COLS;
        L.preset[i] = (Rect){
            field_x + rcol * (preset_btn_w + 6),
            preset_y + rrow * (preset_btn_h + 4),
            preset_btn_w, preset_btn_h
        };
    }
    int preset_bottom = preset_y + preset_rows * (preset_btn_h + 4);

    /* Effects panel: header + three slider rows in two columns + one
       picker row (phosphor / pixelate) + the speed row. */
    int efx_top   = preset_bottom + 18;              /* gap below preset grid */
    int efx_row_h = 28;
    int efx_gap_y = 8;
    int half_w    = (field_w - 14) / 2;             /* gap between columns */
    int slider_label_w = 80;                        /* fixed-width label, slider takes the rest */
    int col_lx    = field_x;
    int col_rx    = field_x + half_w + 14;
    for (int i = 0; i < 3; i++) {
        int yy = efx_top + i * (efx_row_h + efx_gap_y);
        L.efx_slider[k_efx_left_col[i]]  =
            (Rect){ col_lx + slider_label_w, yy,
                    half_w - slider_label_w, efx_row_h };
        L.efx_slider[k_efx_right_col[i]] =
            (Rect){ col_rx + slider_label_w, yy,
                    half_w - slider_label_w, efx_row_h };
    }

    /* Phosphor row (left col) — pill-button picker, 4th row down. The
       right column at this y is reserved for the speed picker so we
       fit the full effects panel into the modal without scrolling. */
    int picker_y = efx_top + 3 * (efx_row_h + efx_gap_y);
    int phos_track_w = half_w - slider_label_w;
    int phos_btn_w   = (phos_track_w - 3 * 4) / PHOSPHOR_COUNT;
    for (int i = 0; i < PHOSPHOR_COUNT; i++)
        L.phos[i] = (Rect){ col_lx + slider_label_w + i * (phos_btn_w + 4),
                            picker_y, phos_btn_w, efx_row_h };

    /* Speed row — sits in the right column at the same y as Phosphor. */
    int speed_track_w = half_w - slider_label_w;
    int speed_btn_w   = (speed_track_w - (EFX_SPEED_COUNT - 1) * 4) / EFX_SPEED_COUNT;
    for (int i = 0; i < EFX_SPEED_COUNT; i++)
        L.speed[i] = (Rect){ col_rx + slider_label_w + i * (speed_btn_w + 4),
                             picker_y, speed_btn_w, efx_row_h };

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
    /* Captions toggle — left-anchored on the same row as the action
       buttons, away from them so an accidental click on Save can't
       hit it instead. */
    int cap_w = 150;
    L.captions_btn = (Rect){ L.modal.x + 22, row_y2, cap_w, btn_h };
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
    Screen *scr = screen_new(cf->cols, cf->rows, SCROLLBACK_LINES, sio);
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
    /* Effects pass — only allocated when at least one visual effect
       is enabled. The shader reads from `rt` (the freshly-rendered
       terminal frame) and writes here; readback then comes from rt_fx
       instead of rt. Speed is purely temporal so it doesn't need
       another texture. */
    bool use_effects = rec_effects_any_visual(&g_rec_save.effects);
    RenderTexture2D rt_fx = {0};
    if (use_effects) {
        rt_fx = LoadRenderTexture(width, height);
        if (rt_fx.id == 0) {
            /* Soft-fail: if we can't allocate the second target, just
               disable visual effects and continue with the speed
               modifier alone — better than failing the whole save. */
            use_effects = false;
            TraceLog(LOG_WARNING,
                     "rec_effects: failed to allocate %dx%d FX target — "
                     "rendering without visual effects", width, height);
        }
    }
    /* Speed multiplier: 1.0 = real-time. >1 compresses, <1 stretches.
       Captured once and clamped to a sane range so a stale 0 doesn't
       divide-by-zero downstream. */
    float speed = g_rec_save.effects.speed;
    if (speed < 0.05f) speed = 1.0f;

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
            UnloadRenderTexture(rt); if (rt_fx.id) UnloadRenderTexture(rt_fx); screen_free(scr); cast_free(cf);
            return false;
        }
    } else if (fmt == REC_FMT_WEBP) {
        webp = webp_begin(dst, width, height, delay_ms);
        if (!webp) {
            snprintf(err, errsz, "couldn't open %s for webp encoding", dst);
            UnloadRenderTexture(rt); if (rt_fx.id) UnloadRenderTexture(rt_fx); screen_free(scr); cast_free(cf);
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
            UnloadRenderTexture(rt); if (rt_fx.id) UnloadRenderTexture(rt_fx); screen_free(scr); cast_free(cf);
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
        UnloadRenderTexture(rt); if (rt_fx.id) UnloadRenderTexture(rt_fx);
        screen_free(scr); cast_free(cf);
        return false;
    }
    /* Skip blank lead-in. The synthetic snapshot at t=0 fires
       immediately, but real PTY output often takes a second or two
       to follow — and even then the FIRST real bytes are commonly
       title escapes / color setup / private-mode toggles that don't
       visibly change the screen. A naive "trim until events[1]"
       skipped only the wall-clock gap, leaving the modal frame
       held while invisible escapes processed.

       Real fix: detection-replay. Spin up a hidden Screen, feed
       events one by one, and hash cells + cursor after each. The
       first event whose hash diverges from the snapshot's anchors
       t0 (with a 200 ms lead-in for context). Detection is fast —
       a hash walk is microseconds for a typical 100x30 grid, and
       it stops on the first divergence, so the search bounded by
       the actual interesting events.

       Speed multiplier divides total output duration: speed=2
       produces half the frames over half the wall-clock time,
       while the per-frame cast lookup multiplies by speed so the
       same events still play out. */
    double t0 = cf->events[0].t;
    {
        ScreenIO det_sio = {0};
        Screen *det = screen_new(cf->cols, cf->rows, 0, det_sio);
        if (det) {
            screen_set_cell_h_px(det, ch_);
            screen_feed(det, cf->events[0].data, cf->events[0].n);
            #define HASH_INIT 1469598103934665603ull
            #define HASH_MUL  1099511628211ull
            uint64_t snap_hash = HASH_INIT;
            for (int yy = 0; yy < cf->rows; yy++) {
                for (int xx = 0; xx < cf->cols; xx++) {
                    Cell cc = screen_view_cell(det, xx, yy);
                    snap_hash ^= cc.cp;
                    snap_hash *= HASH_MUL;
                }
            }
            snap_hash ^= (uint64_t)screen_cursor_x(det);
            snap_hash *= HASH_MUL;
            snap_hash ^= (uint64_t)screen_cursor_y(det);
            snap_hash *= HASH_MUL;

            double anchor_t = -1.0;
            for (size_t k = 1; k < cf->count; k++) {
                /* Skip user-input events — only output drives the screen. */
                if (cf->events[k].kind != 'o') continue;
                screen_feed(det, cf->events[k].data, cf->events[k].n);
                uint64_t h = HASH_INIT;
                for (int yy = 0; yy < cf->rows; yy++) {
                    for (int xx = 0; xx < cf->cols; xx++) {
                        Cell cc = screen_view_cell(det, xx, yy);
                        h ^= cc.cp;
                        h *= HASH_MUL;
                    }
                }
                h ^= (uint64_t)screen_cursor_x(det);
                h *= HASH_MUL;
                h ^= (uint64_t)screen_cursor_y(det);
                h *= HASH_MUL;
                if (h != snap_hash) { anchor_t = cf->events[k].t; break; }
            }
            #undef HASH_INIT
            #undef HASH_MUL
            screen_free(det);
            if (anchor_t > t0 + 0.3) {
                t0 = anchor_t - 0.2;
                if (t0 < cf->events[0].t) t0 = cf->events[0].t;
            }
        }
    }
    if (t0 < 0.0) t0 = 0.0;
    double total = cf->duration_s - t0 + 0.3;
    if (total < 0.3) total = 0.3;
    int total_frames = (int)((total / (double)speed) * fps) + 1;
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
            /* Map output frame index → source-cast timestamp via the
               speed multiplier: each step of `1/fps` of output time
               consumes `speed/fps` seconds of cast time. */
            double frame_t = t0 + ((double)f * (double)speed) / (double)fps;
            while (ev_idx < cf->count && cf->events[ev_idx].t <= frame_t) {
                /* Only "o" events drive the screen — "i" events are
                   user input, used by the caption overlay below. */
                if (cf->events[ev_idx].kind == 'o') {
                    screen_feed(scr, cf->events[ev_idx].data, cf->events[ev_idx].n);
                }
                ev_idx++;
            }
            BeginTextureMode(rt);
                ClearBackground(BLACK);
                renderer_draw(g_renderer, scr, 0.0, false, NULL, 0, 0, -1, -1);
            EndTextureMode();
            /* Visual effects pass: shader copies rt → rt_fx with all
               enabled effects applied. `time_s` advances per frame so
               grain / VHS / glitch animate across the recording. */
            RenderTexture2D *src_rt = &rt;
            if (use_effects) {
                rec_effects_apply(&rt.texture, &rt_fx,
                                  width, height,
                                  &g_rec_save.effects,
                                  (double)f / (double)fps);
                src_rt = &rt_fx;
            }
            /* Caption overlay (closed-caption style). Walks "i"
               events that fall inside a 2.5 s sliding window
               anchored at frame_t, transforms each byte into a
               printable glyph (raw ASCII for letters/digits/punc;
               ⏎ for Enter, ⌫ for Backspace, ↑↓←→ for arrow CSI),
               and draws the result as a translucent strip near
               the bottom of the frame. Drawn AFTER the effects
               pass so VHS / glitch warps the screen but leaves
               the captions readable. */
            if (g_rec_save.show_captions) {
                char cap_buf[768];
                size_t cn = 0;
                #define CAP_APPEND_UTF8(s, n) do { \
                    for (size_t _i = 0; _i < (size_t)(n) && cn + 1 < sizeof(cap_buf); _i++) \
                        cap_buf[cn++] = (s)[_i]; \
                } while (0)
                double cap_window_start = frame_t - 2.5;
                for (size_t k = 0; k < cf->count; k++) {
                    if (cf->events[k].kind != 'i') continue;
                    if (cf->events[k].t > frame_t) break;
                    if (cf->events[k].t < cap_window_start) continue;
                    const uint8_t *d = cf->events[k].data;
                    size_t dn = cf->events[k].n;
                    for (size_t b = 0; b < dn; b++) {
                        uint8_t c = d[b];
                        /* Detect the common arrow-key CSI form
                           ESC '[' ('A'..'D'). Skip the 3 bytes
                           after we render the label. ASCII labels
                           only: raylib's GetFontDefault() is
                           an ASCII-only pixel font, so any non-
                           ASCII codepoint (↑↓←→⏎⌫⇥) renders as a
                           `?` glyph. */
                        if (c == 0x1B && b + 2 < dn && d[b + 1] == '[') {
                            const char *arrow = NULL;
                            switch (d[b + 2]) {
                                case 'A': arrow = "[Up]"; break;
                                case 'B': arrow = "[Dn]"; break;
                                case 'C': arrow = "[Rt]"; break;
                                case 'D': arrow = "[Lt]"; break;
                            }
                            if (arrow) {
                                CAP_APPEND_UTF8(arrow, strlen(arrow));
                                b += 2;
                                continue;
                            }
                            /* Other escape sequences: skip until a
                               final byte (letter or '~') so we
                               don't dump raw control bytes into
                               the caption. */
                            size_t e = b + 1;
                            while (e < dn) {
                                uint8_t ec = d[e];
                                if ((ec >= 0x40 && ec <= 0x7E) || ec == '~') break;
                                e++;
                            }
                            b = e;
                            continue;
                        }
                        if (c == '\r' || c == '\n')      { CAP_APPEND_UTF8("[Ret]", 5); continue; }
                        if (c == '\t')                   { CAP_APPEND_UTF8("[Tab]", 5); continue; }
                        if (c == 0x7F || c == 0x08)      { CAP_APPEND_UTF8("[Bks]", 5); continue; }
                        if (c >= 0x20 && c < 0x7F)       { cap_buf[cn++] = (char)c; continue; }
                        /* Ctrl-letter (0x01..0x1A): show as ^A..^Z. */
                        if (c >= 0x01 && c <= 0x1A) {
                            char buf2[3] = { '^', (char)('A' + c - 1), 0 };
                            CAP_APPEND_UTF8(buf2, 2);
                            continue;
                        }
                    }
                }
                #undef CAP_APPEND_UTF8
                cap_buf[cn] = 0;
                if (cn > 0) {
                    /* Trim from the front so the most recent
                       keystrokes are always visible (right-aligned
                       feel without actually right-aligning). */
                    const int max_chars = 80;
                    if ((int)cn > max_chars) {
                        memmove(cap_buf, cap_buf + (cn - max_chars),
                                max_chars + 1);
                        cn = max_chars;
                    }
                    BeginTextureMode(*src_rt);
                        Font fnt = GetFontDefault();
                        int fontsz = (height < 300) ? 14 : 18;
                        Vector2 ts = MeasureTextEx(fnt, cap_buf, (float)fontsz, 1);
                        int box_pad = 8;
                        int box_w = (int)ts.x + 2 * box_pad;
                        int box_h = (int)ts.y + 2 * box_pad;
                        if (box_w > width - 16) box_w = width - 16;
                        int box_x = (width - box_w) / 2;
                        int box_y = height - box_h - 12;
                        DrawRectangle(box_x, box_y, box_w, box_h,
                                      (Color){0, 0, 0, 200});
                        DrawRectangleLines(box_x, box_y, box_w, box_h,
                                           (Color){240, 240, 240, 120});
                        DrawTextEx(fnt, cap_buf,
                                   (Vector2){box_x + box_pad, box_y + box_pad},
                                   (float)fontsz, 1,
                                   (Color){255, 255, 240, 255});
                    EndTextureMode();
                }
            }
            Image img = LoadImageFromTexture(src_rt->texture);
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
            UnloadRenderTexture(rt); if (rt_fx.id) UnloadRenderTexture(rt_fx); screen_free(scr); cast_free(cf);
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
            UnloadRenderTexture(rt); if (rt_fx.id) UnloadRenderTexture(rt_fx); screen_free(scr); cast_free(cf);
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
            UnloadRenderTexture(rt); if (rt_fx.id) UnloadRenderTexture(rt_fx); screen_free(scr); cast_free(cf);
            return false;
        }
        unlink(ff_log);
        unlink(gif_intermediate);
    }
    UnloadRenderTexture(rt);
    if (rt_fx.id) UnloadRenderTexture(rt_fx);
    screen_free(scr);
    cast_free(cf);
    return true;
}

/* Public entry — preserves the old name so call sites keep working. */
static bool rec_convert(RecFmt fmt, const char *src_cast, const char *dst,
                        char *err, size_t errsz) {
    return rec_render_native(fmt, src_cast, dst, err, errsz);
}

/* Compose `<dst-without-ext>-p<idx>.<ext>` into `out`. Used by the
   Save handler when the recording captured multiple panes — each
   slot writes to its own file alongside the user-typed dst. */
static void rec_dst_for_pane(const char *dst, int idx, char *out, size_t cap) {
    char base[PATH_MAX];
    snprintf(base, sizeof(base), "%s", dst);
    char *slash = strrchr(base, '/');
    char *dot   = strrchr(base, '.');
    char ext[16];
    ext[0] = 0;
    if (dot && (!slash || dot > slash)) {
        snprintf(ext, sizeof(ext), "%s", dot);
        *dot = 0;
    }
    snprintf(out, cap, "%s-p%d%s", base, idx, ext);
}

/* Update a slider's value from the current mouse X position, clamped
   to the track. Called both on initial click (snap-to-cursor) and
   while dragging. */
static void efx_slider_set_from_mouse(RecSaveLayout L, EfxSlider s, int mx) {
    Rect r = L.efx_slider[s];
    float v = (float)(mx - r.x) / (float)(r.w > 1 ? r.w : 1);
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    float *target = efx_slider_value(&g_rec_save.effects, s);
    if (target) *target = v;
}

/* Click-handling for the save modal. Tracks consecutive clicks on
   the path field for double-click-to-select-all (~0.45s gap). Also
   owns slider-thumb drag state on the effects panel: a button-press
   inside any slider rect captures, subsequent movement updates the
   value while held, release lets go. */
static void rec_save_handle_mouse(RecSaveLayout L) {
    static double last_click_t = -1.0;
    static int    click_count = 0;
    Vector2 mp = GetMousePosition();
    int mx = (int)mp.x, my = (int)mp.y;

    /* Slider drag continuation. Runs every frame the button is held
       down after a press inside one of the slider tracks. We service
       this *before* the early-return so dragging works without
       requiring a fresh press each frame. */
    if (g_rec_save.slider_drag) {
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            efx_slider_set_from_mouse(L, (EfxSlider)g_rec_save.slider_drag_idx, mx);
        } else {
            g_rec_save.slider_drag = false;
        }
    }

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
    /* Preset pills — picking one stamps the full effect set, so a
       click here overrides whatever sliders / phosphor / speed
       were set. The user can still tweak afterwards. */
    for (int i = 0; i < EFX_PRESET_COUNT; i++) {
        if (rect_hit(L.preset[i], mx, my)) {
            rec_effects_apply_preset(&g_rec_save.effects, (EfxPreset)i);
            g_rec_save.status[0] = 0;
            return;
        }
    }
    /* Effects panel: sliders, phosphor pickers, pixelate factors, speed. */
    for (int i = 0; i < EFX_SLIDER_COUNT; i++) {
        if (rect_hit(L.efx_slider[i], mx, my)) {
            g_rec_save.slider_drag = true;
            g_rec_save.slider_drag_idx = i;
            efx_slider_set_from_mouse(L, (EfxSlider)i, mx);
            g_rec_save.status[0] = 0;
            return;
        }
    }
    for (int i = 0; i < PHOSPHOR_COUNT; i++) {
        if (rect_hit(L.phos[i], mx, my)) {
            g_rec_save.effects.phosphor = (PhosphorMode)i;
            g_rec_save.status[0] = 0;
            return;
        }
    }
    for (int i = 0; i < EFX_SPEED_COUNT; i++) {
        if (rect_hit(L.speed[i], mx, my)) {
            g_rec_save.effects.speed = k_speed_values[i];
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
        int n = g_rec_save.src_count > 0 ? g_rec_save.src_count : 1;
        fprintf(stderr, "rbterm: save → fmt=%s dst=%s panes=%d\n",
                rec_fmt_ext(g_rec_save.fmt), g_rec_save.dst_path, n);
        snprintf(g_rec_save.status, sizeof(g_rec_save.status),
                 "rendering %d %s file%s — this can take a few seconds…",
                 n, rec_fmt_ext(g_rec_save.fmt), n == 1 ? "" : "s");
        long long total_sz = 0;
        char last_dst[PATH_MAX];
        last_dst[0] = 0;
        for (int i = 0; i < n; i++) {
            const char *src = g_rec_save.src_count > 0
                                  ? g_rec_save.src_paths[i]
                                  : g_rec_save.src_path;
            char dst[PATH_MAX];
            if (n > 1) {
                rec_dst_for_pane(g_rec_save.dst_path, i, dst, sizeof(dst));
            } else {
                snprintf(dst, sizeof(dst), "%s", g_rec_save.dst_path);
            }
            char err[256] = {0};
            if (!rec_convert(g_rec_save.fmt, src, dst, err, sizeof(err))) {
                snprintf(g_rec_save.status, sizeof(g_rec_save.status),
                         "pane %d: %s", i,
                         err[0] ? err : "(no error message captured)");
                fprintf(stderr, "rbterm: save FAILED (pane %d): %s\n",
                        i, g_rec_save.status);
                return;
            }
            struct stat st;
            if (stat(dst, &st) == 0) total_sz += (long long)st.st_size;
            snprintf(last_dst, sizeof(last_dst), "%s", dst);
            /* Cast saves move the source to the destination; pin the
               new location so a follow-up re-export from the modal
               still finds data. */
            if (g_rec_save.fmt == REC_FMT_CAST) {
                strncpy(g_rec_save.src_paths[i], dst,
                        sizeof(g_rec_save.src_paths[i]) - 1);
                g_rec_save.src_paths[i][sizeof(g_rec_save.src_paths[i]) - 1] = 0;
                if (i == 0) {
                    strncpy(g_rec_save.src_path, dst,
                            sizeof(g_rec_save.src_path) - 1);
                    g_rec_save.src_path[sizeof(g_rec_save.src_path) - 1] = 0;
                }
            }
        }
        const char *unit = "B"; double v = (double)total_sz;
        if (v >= 1024.0)        { v /= 1024.0;        unit = "KB"; }
        if (v >= 1024.0)        { v /= 1024.0;        unit = "MB"; }
        if (n > 1) {
            snprintf(g_rec_save.status, sizeof(g_rec_save.status),
                     "wrote %d %s files (%.0f %s total) — last: %s",
                     n, rec_fmt_ext(g_rec_save.fmt), v, unit, last_dst);
        } else {
            snprintf(g_rec_save.status, sizeof(g_rec_save.status),
                     "wrote %.0f %s %s to %s",
                     v, unit, rec_fmt_ext(g_rec_save.fmt), last_dst);
        }
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
    if (rect_hit(L.captions_btn, mx, my)) {
        g_rec_save.show_captions = !g_rec_save.show_captions;
        g_rec_save.status[0] = 0;
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

    /* ---- Preset pill grid — Nostromo, VHS, Tron, etc. ---- */
    {
        Color text_main = (Color){230, 232, 240, 255};
        Color outline   = (Color){125, 207, 255, 150};
        int hdr_y = L.preset[0].y - 18;
        DrawTextEx(*f, "Preset",
                   (Vector2){L.modal.x + 22, hdr_y},
                   14, 0, (Color){200, 205, 220, 255});
        DrawLine(L.modal.x + 22 + 60, hdr_y + 9,
                 L.modal.x + L.modal.w - 22, hdr_y + 9,
                 (Color){70, 74, 90, 255});
        for (int i = 0; i < EFX_PRESET_COUNT; i++) {
            Rect rr = L.preset[i];
            DrawRectangle(rr.x, rr.y, rr.w, rr.h, (Color){34, 38, 52, 255});
            DrawRectangleLines(rr.x, rr.y, rr.w, rr.h, outline);
            const char *lbl = rec_effects_preset_label((EfxPreset)i);
            Vector2 ts = MeasureTextEx(*f, lbl, 12, 0);
            DrawTextEx(*f, lbl,
                       (Vector2){rr.x + (rr.w - ts.x) / 2,
                                 rr.y + (rr.h - ts.y) / 2},
                       12, 0, text_main);
        }
    }

    /* ---- Effects panel: section header, sliders, picker rows. ---- */
    {
        /* Section header sits in the gap above the first slider row. */
        int hdr_y = L.efx_slider[k_efx_left_col[0]].y - 18;
        DrawTextEx(*f, "Effects",
                   (Vector2){L.modal.x + 22, hdr_y},
                   14, 0, (Color){200, 205, 220, 255});
        DrawLine(L.modal.x + 22 + 60, hdr_y + 9,
                 L.modal.x + L.modal.w - 22, hdr_y + 9,
                 (Color){70, 74, 90, 255});

        /* Six sliders. Left edge of each gets the effect name, right
           side is the track + filled bar + thumb. The slider rect IS
           the track; thumb position derives from value. */
        Color slider_bg     = (Color){22, 25, 34, 255};
        Color slider_fill   = (Color){46, 92, 150, 220};
        Color slider_border = (Color){70, 74, 90, 255};
        Color slider_thumb  = (Color){125, 207, 255, 255};
        Color label_col     = (Color){200, 205, 220, 255};
        Color val_col       = (Color){170, 200, 235, 255};
        for (int i = 0; i < EFX_SLIDER_COUNT; i++) {
            Rect rr = L.efx_slider[i];
            float v = *efx_slider_value(&g_rec_save.effects, (EfxSlider)i);
            int   fill_w = (int)(v * (float)rr.w);
            /* Label text just left of the track. */
            const char *lbl = efx_slider_label((EfxSlider)i);
            DrawTextEx(*f, lbl,
                       (Vector2){rr.x - 76, rr.y + (rr.h - 13) / 2 + 1},
                       13, 0, label_col);
            /* Track. */
            int track_h = 8;
            int track_y = rr.y + (rr.h - track_h) / 2;
            DrawRectangle(rr.x, track_y, rr.w, track_h, slider_bg);
            DrawRectangle(rr.x, track_y, fill_w, track_h, slider_fill);
            DrawRectangleLines(rr.x, track_y, rr.w, track_h, slider_border);
            /* Thumb (filled circle, raised against the track). */
            int thumb_x = rr.x + fill_w;
            DrawCircle(thumb_x, track_y + track_h / 2, 7, slider_thumb);
            DrawCircleLines(thumb_x, track_y + track_h / 2, 7, (Color){30, 34, 46, 255});
            /* Value readout, right-aligned past the track. */
            char vbuf[8];
            snprintf(vbuf, sizeof(vbuf), "%.0f%%", v * 100.0f);
            Vector2 vsz = MeasureTextEx(*f, vbuf, 12, 0);
            DrawTextEx(*f, vbuf,
                       (Vector2){rr.x + rr.w + 6,
                                 rr.y + (rr.h - vsz.y) / 2},
                       12, 0, val_col);
        }

        /* Phosphor row (left col) — pill buttons. */
        DrawTextEx(*f, "Phosphor",
                   (Vector2){L.phos[0].x - 76,
                             L.phos[0].y + (L.phos[0].h - 13) / 2 + 1},
                   13, 0, label_col);
        for (int i = 0; i < PHOSPHOR_COUNT; i++) {
            Rect rr = L.phos[i];
            bool sel = (g_rec_save.effects.phosphor == (PhosphorMode)i);
            Color bg = sel ? (Color){46, 92, 150, 255} : (Color){34, 38, 52, 255};
            DrawRectangle(rr.x, rr.y, rr.w, rr.h, bg);
            DrawRectangleLines(rr.x, rr.y, rr.w, rr.h,
                               (Color){125, 207, 255, sel ? 255 : 120});
            const char *plbl = phosphor_label((PhosphorMode)i);
            Vector2 ts = MeasureTextEx(*f, plbl, 12, 0);
            DrawTextEx(*f, plbl,
                       (Vector2){rr.x + (rr.w - ts.x) / 2,
                                 rr.y + (rr.h - ts.y) / 2},
                       12, 0, (Color){230, 232, 240, 255});
        }

        /* Speed row — right column, same y as Phosphor. */
        DrawTextEx(*f, "Speed",
                   (Vector2){L.speed[0].x - 76,
                             L.speed[0].y + (L.speed[0].h - 13) / 2 + 1},
                   13, 0, label_col);
        for (int i = 0; i < EFX_SPEED_COUNT; i++) {
            Rect rr = L.speed[i];
            bool sel = (g_rec_save.effects.speed == k_speed_values[i]);
            Color bg = sel ? (Color){46, 92, 150, 255} : (Color){34, 38, 52, 255};
            DrawRectangle(rr.x, rr.y, rr.w, rr.h, bg);
            DrawRectangleLines(rr.x, rr.y, rr.w, rr.h,
                               (Color){125, 207, 255, sel ? 255 : 120});
            Vector2 ts = MeasureTextEx(*f, k_speed_labels[i], 13, 0);
            DrawTextEx(*f, k_speed_labels[i],
                       (Vector2){rr.x + (rr.w - ts.x) / 2,
                                 rr.y + (rr.h - ts.y) / 2},
                       13, 0, (Color){230, 232, 240, 255});
        }
    }

    /* Captions toggle (left-anchored on the action row). Reads
       like a checkbox — pressed-in fill when on, neutral grey when
       off, the label switches between [✓ Captions] / [Captions]. */
    {
        Rect cb = L.captions_btn;
        bool on = g_rec_save.show_captions;
        Color bg = on ? (Color){46, 92, 150, 255} : (Color){34, 38, 52, 255};
        Color border = on ? (Color){125, 207, 255, 255} : (Color){90, 95, 110, 200};
        Color text = on ? (Color){230, 245, 255, 255} : (Color){200, 205, 220, 255};
        DrawRectangle(cb.x, cb.y, cb.w, cb.h, bg);
        DrawRectangleLines(cb.x, cb.y, cb.w, cb.h, border);
        const char *lbl = on ? "[✓] Captions" : "[ ] Captions";
        Vector2 ts = MeasureTextEx(*f, lbl, 13, 0);
        DrawTextEx(*f, lbl,
                   (Vector2){cb.x + (cb.w - ts.x) / 2,
                             cb.y + (cb.h - ts.y) / 2},
                   13, 0, text);
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
        /* Navigation chords are split into four sub-tabs so each
           panel fits without scrolling and the user finds chords
           grouped by what they affect. */
        if (g_help_nav_subtab == 0) {
            ROW("",                          "Tabs");
            ROW("Cmd+T",                     "New tab (local shell)");
            ROW("Cmd+Shift+T",               "New tab via SSH (open the connect form)");
            ROW("Cmd+W",                     "Close active tab (or pane if split)");
            ROW("Cmd+1..9",                  "Jump to tab N");
            ROW("Cmd+Left / Cmd+Right",      "Cycle to previous / next tab");
            ROW("Cmd+[ / Cmd+]",             "Cycle to previous / next tab (alt)");
            ROW("Cmd+Shift+Left/Right",      "Move active tab left / right");
            ROW("Cmd+R / dbl-click title",   "Rename active tab");
            ROW("Cmd+N",                     "New rbterm window (separate process)");
        } else if (g_help_nav_subtab == 1) {
            ROW("",                          "Panes (splits)");
            ROW("Cmd+D",                     "Split active tab vertically (side-by-side)");
            ROW("Cmd+Shift+D",               "Split horizontally (top / bottom)");
            ROW("Cmd+Shift+W",               "Close active pane (collapses to single)");
            ROW("Cmd+K",                     "Cycle focus to next leaf in DFS order");
            ROW("Cmd+Shift+K",               "Cycle focus to previous leaf");
            ROW("Cmd+Opt+Arrows",            "Directional pane focus (iTerm2 convention)");
            ROW("Click a pane",              "Focus that pane");
            ROW("Drag the splitter",         "Resize panes");
            ROW("Cmd+Shift+I",               "Toggle broadcast — type into every pane in the active tab");
        } else if (g_help_nav_subtab == 2) {
            ROW("",                          "Scroll + font");
            ROW("Mouse wheel",               "Scroll into history (when not in app cursor mode)");
            ROW("Ctrl+Shift+Up/Down",        "Scroll one row");
            ROW("Shift+PageUp/PageDown",     "Scroll one screen");
            ROW("Cmd+= / Cmd+-",             "Font size up / down");
            ROW("Cmd+0",                     "Reset font size to 20pt");
            ROW("Cmd+Up / Cmd+Down",         "Jump to previous / next prompt (needs OSC 133)");
        } else {
            ROW("",                          "Modals + windows");
            ROW("Cmd+,",                     "Settings");
            ROW("Cmd+Shift+T",               "SSH connect form");
            ROW("Cmd+Shift+P",               "Session Designer (multi-host split sessions)");
            ROW("Cmd+Shift+O",               "Browse session logs");
            ROW("Cmd+Shift+S",               "Screenshot active pane");
            ROW("Cmd+CapsLock",              "Quake-drop the window (when borderless mode is enabled)");
            ROW("Esc / click outside",       "Dismiss the active modal");
            ROW("",                          "");
            ROW("Note",                      "Cmd shows on macOS; Ctrl is the modifier on Linux + Windows.");
        }
    } else if (g_help_tab == 1) {
        if (g_help_edit_subtab == 0) {
            ROW("",                          "Selection + clipboard");
            ROW("Cmd+A",                     "Select all visible text in active pane");
            ROW("Cmd+C",                     "Copy selection");
            ROW("Cmd+V",                     "Paste");
            ROW("Click + drag",              "Select text");
            ROW("Double-click",              "Select word (smart trim of trailing punctuation)");
            ROW("Triple-click",              "Select line (joins wrapped rows)");
            ROW("",                          "Cross-ref (needs OSC 133 — see Shell tab)");
            ROW("Cmd+Shift+L",               "Select last command's output");
        } else {
            ROW("",                          "Search");
            ROW("Cmd+F",                     "Open search bar on the active pane");
            ROW("Enter / Down / F3",         "Next match");
            ROW("Shift+Enter / Up / Shift+F3","Previous match");
            ROW("Cmd+A (in search)",         "Select all in search query");
            ROW("Click / Shift+click / drag","Position caret / extend / range select in search");
            ROW("Esc",                       "Close search, restore scroll position");
            ROW("",                          "Recompute is debounced 350 ms after the last keystroke,");
            ROW("",                          "so a million-line scrollback stays responsive while typing.");
        }
    } else {
        /* Shell integration — narrative + per-shell setup line.
           Both panels share the same intro/caveats; only the
           one-line `source` snippet differs (zsh vs bash). */
        bool zsh = (g_help_shell_subtab == 0);
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
        if (zsh) {
            ROW("",     "Source the helper from your ~/.zshrc:");
            ROW("zsh:",  "echo 'source <rbterm-repo>/tools/rbterm-shell-integration.zsh' >> ~/.zshrc");
            ROW("",      "Then either restart zsh (`exec zsh`) or open a new");
            ROW("",      "pane so the precmd / preexec hooks load.");
        } else {
            ROW("",     "Source the helper from your ~/.bashrc:");
            ROW("bash:", "echo 'source <rbterm-repo>/tools/rbterm-shell-integration.bash' >> ~/.bashrc");
            ROW("",      "Then `source ~/.bashrc` (or open a new pane) so the");
            ROW("",      "DEBUG / PROMPT_COMMAND traps install.");
        }
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
    /* title bar (38) + tab bar (26 + 6 gap) + sub-tab bar
       (22 + 6 gap). All three top-level tabs now have sub-tabs
       (Tabs/Panes/Scroll/Modals on Navigation, Selection/Search
       on Edit, zsh/bash on Shell), so header_h is constant — the
       modal's overall size never shifts. */
    int header_h = 50 + 32 + 28;
    int side_pad = 28;
    int top_pad  = 18;
    int footer_h = 32;
    /* Fixed modal size — same dimensions regardless of which tab
       or sub-tab is active. Picked to comfortably fit the Shell
       integration tab (the largest content). Tabs with less
       content just have empty space below; this keeps the modal
       from "jumping" as the user clicks between tabs. */
    (void)n; (void)row_h;  /* dynamic-row sizing no longer used */
    int w = 760, h = 600;
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

    /* Tab bar between title and content. Active tab is visually
       distinct three ways: brighter fill, full-strength text vs
       dimmed inactive text, and a 3 px cyan accent strip along
       its bottom edge. */
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
                          on ? (Color){62, 120, 180, 255}
                             : (Color){26, 30, 42, 255});
            DrawRectangleLines(tr.x, tr.y, tr.w, tr.h,
                               on ? (Color){200, 230, 255, 255}
                                  : (Color){80,  90, 110, 130});
            if (on) {
                /* 3-px accent strip — strong active indicator. */
                DrawRectangle(tr.x, tr.y + tr.h - 3,
                              tr.w, 3,
                              (Color){125, 207, 255, 255});
            }
            Vector2 ts = MeasureTextEx(*f, labels[i], 13, 0);
            DrawTextEx(*f, labels[i],
                       (Vector2){tr.x + (tr.w - ts.x) / 2,
                                 tr.y + (tr.h - ts.y) / 2},
                       13, 0, on ? (Color){250, 252, 255, 255}
                                 : (Color){140, 150, 170, 200});
        }
    }
    /* Sub-tab strip — only on the Navigation top-level tab.
       Splits the chord list into Tabs / Panes / Scroll / Modals
       so each panel fits without scrolling. Same active-state
       cues as the top-level tabs (brighter fill + dimmed
       inactive text + accent strip) for visual consistency. */
    if (g_help_tab == 0) {
        const char *sublabels[HELP_NAV_SUBTAB_COUNT] = {
            "Tabs", "Panes", "Scroll", "Modals"
        };
        int sbar_y = my + 44 + 26 + 6;
        int sbar_h = 22;
        int sbw = (w - 2 * 14 - 3 * 6) / HELP_NAV_SUBTAB_COUNT;
        int sbx = mx + 14;
        for (int i = 0; i < HELP_NAV_SUBTAB_COUNT; i++) {
            Rect tr = { sbx + i * (sbw + 6), sbar_y, sbw, sbar_h };
            g_help_nav_subtab_rects[i] = tr;
            bool on = (g_help_nav_subtab == i);
            DrawRectangle(tr.x, tr.y, tr.w, tr.h,
                          on ? (Color){80, 145, 200, 255}
                             : (Color){22, 26, 36, 255});
            DrawRectangleLines(tr.x, tr.y, tr.w, tr.h,
                               on ? (Color){200, 230, 255, 220}
                                  : (Color){70, 80, 100, 110});
            if (on) {
                DrawRectangle(tr.x, tr.y + tr.h - 2,
                              tr.w, 2,
                              (Color){170, 220, 255, 255});
            }
            Vector2 ts = MeasureTextEx(*f, sublabels[i], 12, 0);
            DrawTextEx(*f, sublabels[i],
                       (Vector2){tr.x + (tr.w - ts.x) / 2,
                                 tr.y + (tr.h - ts.y) / 2},
                       12, 0, on ? (Color){250, 252, 255, 255}
                                 : (Color){135, 145, 165, 180});
        }
    } else {
        for (int i = 0; i < HELP_NAV_SUBTAB_COUNT; i++)
            g_help_nav_subtab_rects[i] = (Rect){0, 0, 0, 0};
    }
    /* Edit & Search sub-tab strip — Selection / Search. */
    if (g_help_tab == 1) {
        const char *sublabels[HELP_EDIT_SUBTAB_COUNT] = {
            "Selection", "Search"
        };
        int sbar_y = my + 44 + 26 + 6;
        int sbar_h = 22;
        int sbw = (w - 2 * 14 - (HELP_EDIT_SUBTAB_COUNT - 1) * 6)
                  / HELP_EDIT_SUBTAB_COUNT;
        int sbx = mx + 14;
        for (int i = 0; i < HELP_EDIT_SUBTAB_COUNT; i++) {
            Rect tr = { sbx + i * (sbw + 6), sbar_y, sbw, sbar_h };
            g_help_edit_subtab_rects[i] = tr;
            bool on = (g_help_edit_subtab == i);
            DrawRectangle(tr.x, tr.y, tr.w, tr.h,
                          on ? (Color){80, 145, 200, 255}
                             : (Color){22, 26, 36, 255});
            DrawRectangleLines(tr.x, tr.y, tr.w, tr.h,
                               on ? (Color){200, 230, 255, 220}
                                  : (Color){70, 80, 100, 110});
            if (on) {
                DrawRectangle(tr.x, tr.y + tr.h - 2, tr.w, 2,
                              (Color){170, 220, 255, 255});
            }
            Vector2 ts = MeasureTextEx(*f, sublabels[i], 12, 0);
            DrawTextEx(*f, sublabels[i],
                       (Vector2){tr.x + (tr.w - ts.x) / 2,
                                 tr.y + (tr.h - ts.y) / 2},
                       12, 0, on ? (Color){250, 252, 255, 255}
                                 : (Color){135, 145, 165, 180});
        }
    } else {
        for (int i = 0; i < HELP_EDIT_SUBTAB_COUNT; i++)
            g_help_edit_subtab_rects[i] = (Rect){0, 0, 0, 0};
    }
    /* Shell integration sub-tab strip — zsh / bash. */
    if (g_help_tab == 2) {
        const char *sublabels[HELP_SHELL_SUBTAB_COUNT] = {
            "zsh", "bash"
        };
        int sbar_y = my + 44 + 26 + 6;
        int sbar_h = 22;
        int sbw = (w - 2 * 14 - (HELP_SHELL_SUBTAB_COUNT - 1) * 6)
                  / HELP_SHELL_SUBTAB_COUNT;
        int sbx = mx + 14;
        for (int i = 0; i < HELP_SHELL_SUBTAB_COUNT; i++) {
            Rect tr = { sbx + i * (sbw + 6), sbar_y, sbw, sbar_h };
            g_help_shell_subtab_rects[i] = tr;
            bool on = (g_help_shell_subtab == i);
            DrawRectangle(tr.x, tr.y, tr.w, tr.h,
                          on ? (Color){80, 145, 200, 255}
                             : (Color){22, 26, 36, 255});
            DrawRectangleLines(tr.x, tr.y, tr.w, tr.h,
                               on ? (Color){200, 230, 255, 220}
                                  : (Color){70, 80, 100, 110});
            if (on) {
                DrawRectangle(tr.x, tr.y + tr.h - 2, tr.w, 2,
                              (Color){170, 220, 255, 255});
            }
            Vector2 ts = MeasureTextEx(*f, sublabels[i], 12, 0);
            DrawTextEx(*f, sublabels[i],
                       (Vector2){tr.x + (tr.w - ts.x) / 2,
                                 tr.y + (tr.h - ts.y) / 2},
                       12, 0, on ? (Color){250, 252, 255, 255}
                                 : (Color){135, 145, 165, 180});
        }
    } else {
        for (int i = 0; i < HELP_SHELL_SUBTAB_COUNT; i++)
            g_help_shell_subtab_rects[i] = (Rect){0, 0, 0, 0};
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
            "Font", "Theme", "Cursor", "Effects", "Logging", "Window",
            "Recording", "HUD", "Launch", "Keys", "Sessions"
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
        int total_rows = font_display_row_count();
        int visible = L.font_list.h / row_h;
        int max_scroll = total_rows - visible;
        if (max_scroll < 0) max_scroll = 0;
        if (g_font_list_scroll > max_scroll) g_font_list_scroll = max_scroll;
        if (g_font_list_scroll < 0) g_font_list_scroll = 0;
        BeginScissorMode(L.font_list.x + 2, L.font_list.y + 2,
                         L.font_list.w - 4, L.font_list.h - 4);
        for (int row = 0; row < total_rows; row++) {
            int ry = L.font_list.y + (row - g_font_list_scroll) * row_h;
            if (ry + row_h < L.font_list.y || ry > L.font_list.y + L.font_list.h)
                continue;
            const char *header = font_header_at_row(row);
            if (header) {
                /* Section header — non-clickable, dim accent + tiny
                   underline so it stands apart from font rows. */
                DrawTextEx(*f, header,
                           (Vector2){L.font_list.x + 8, ry + 5},
                           12, 0, (Color){145, 165, 195, 255});
                DrawRectangle(L.font_list.x + 8, ry + row_h - 2,
                              L.font_list.w - 16, 1,
                              (Color){70, 80, 100, 200});
                continue;
            }
            int i = font_display_to_idx(row);
            if (i < 0) continue;
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
        if (total_rows > visible) {
            int track_x = L.font_list.x + L.font_list.w - 5;
            int bar_h = L.font_list.h * visible / total_rows;
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

    /* Cursor-colour swatch row. 8 presets + a "Default" tile.
       Selected swatch wears a brighter outline. */
    {
        DrawTextEx(*f, "Color",
                   (Vector2){L.modal.x + 22,
                             L.cur_color_swatch[0].y + 8},
                   14, 0, (Color){200, 205, 220, 255});
        for (int i = 0; i < SSH_COLOR_PRESET_COUNT; i++) {
            Rect rr = L.cur_color_swatch[i];
            Color c = (Color){90, 95, 110, 255};
            parse_hex_color(SSH_COLOR_PRESETS[i], &c);
            bool on = (strcmp(g_app_settings.cursor_color,
                              SSH_COLOR_PRESETS[i]) == 0);
            DrawRectangle(rr.x, rr.y, rr.w, rr.h, c);
            DrawRectangleLines(rr.x, rr.y, rr.w, rr.h,
                               on ? (Color){240, 245, 255, 255}
                                  : (Color){0, 0, 0, 120});
            if (on) DrawRectangleLines(rr.x + 1, rr.y + 1,
                                        rr.w - 2, rr.h - 2,
                                        (Color){240, 245, 255, 180});
        }
        Rect dr = L.cur_color_swatch[SSH_COLOR_PRESET_COUNT];
        bool def_on = (g_app_settings.cursor_color[0] == 0);
        DrawRectangle(dr.x, dr.y, dr.w, dr.h, (Color){34, 38, 52, 255});
        DrawRectangleLines(dr.x, dr.y, dr.w, dr.h,
                           def_on ? (Color){240, 245, 255, 255}
                                  : (Color){125, 207, 255, 150});
        Vector2 dsz = MeasureTextEx(*f, "default", 12, 0);
        DrawTextEx(*f, "default",
                   (Vector2){dr.x + (dr.w - dsz.x) / 2,
                             dr.y + (dr.h - dsz.y) / 2},
                   12, 0, (Color){200, 205, 220, 255});
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

    /* Ligatures toggle. Hidden when shape_available() is false (no
       HarfBuzz at build time) — the layout sets a zero-width rect in
       that case so the click handler can't hit it either. */
    if (L.ligatures_toggle.w > 0) {
        DrawTextEx(*f, "Ligatures",
                   (Vector2){L.modal.x + 22, L.ligatures_toggle.y + 8},
                   14, 0, (Color){200, 205, 220, 255});
        bool on = g_app_settings.ligatures;
        Color bg = on ? (Color){46, 92, 150, 255} : (Color){34, 38, 52, 255};
        DrawRectangle(L.ligatures_toggle.x, L.ligatures_toggle.y,
                      L.ligatures_toggle.w, L.ligatures_toggle.h, bg);
        DrawRectangleLines(L.ligatures_toggle.x, L.ligatures_toggle.y,
                           L.ligatures_toggle.w, L.ligatures_toggle.h,
                           (Color){125, 207, 255, on ? 255 : 120});
        const char *lbl = on ? "On (HarfBuzz)" : "Off";
        Vector2 ts = MeasureTextEx(*f, lbl, 13, 0);
        DrawTextEx(*f, lbl,
                   (Vector2){L.ligatures_toggle.x + (L.ligatures_toggle.w - ts.x) / 2,
                             L.ligatures_toggle.y + (L.ligatures_toggle.h - ts.y) / 2},
                   13, 0, (Color){230, 232, 240, 255});
    }

    }  /* end Font tab (padding + spacing) */
    if (g_settings_tab == SETTINGS_TAB_EFFECTS) {
        /* Six slider rows + Phosphor pills + Reset button. The
           interaction model matches the rec-save modal: click track
           to set + capture for drag, drag updates while held. */
        Color slider_bg     = (Color){22, 25, 34, 255};
        Color slider_fill   = (Color){46, 92, 150, 220};
        Color slider_border = (Color){70, 74, 90, 255};
        Color slider_thumb  = (Color){125, 207, 255, 255};
        Color label_col     = (Color){200, 205, 220, 255};
        Color val_col       = (Color){170, 200, 235, 255};

        for (int i = 0; i < EFX_SLIDER_COUNT; i++) {
            Rect rr = L.efx_set_slider[i];
            float v = *efx_slider_value(&g_app_settings.effects, (EfxSlider)i);
            int   fill_w = (int)(v * (float)rr.w);
            const char *lbl = efx_slider_label((EfxSlider)i);
            DrawTextEx(*f, lbl,
                       (Vector2){rr.x - 96, rr.y + (rr.h - 13) / 2 + 1},
                       13, 0, label_col);
            int track_h = 8;
            int track_y = rr.y + (rr.h - track_h) / 2;
            DrawRectangle(rr.x, track_y, rr.w, track_h, slider_bg);
            DrawRectangle(rr.x, track_y, fill_w, track_h, slider_fill);
            DrawRectangleLines(rr.x, track_y, rr.w, track_h, slider_border);
            int thumb_x = rr.x + fill_w;
            DrawCircle(thumb_x, track_y + track_h / 2, 7, slider_thumb);
            DrawCircleLines(thumb_x, track_y + track_h / 2, 7, (Color){30, 34, 46, 255});
            char vbuf[8];
            snprintf(vbuf, sizeof(vbuf), "%.0f%%", v * 100.0f);
            Vector2 vsz = MeasureTextEx(*f, vbuf, 12, 0);
            DrawTextEx(*f, vbuf,
                       (Vector2){rr.x + rr.w + 6,
                                 rr.y + (rr.h - vsz.y) / 2},
                       12, 0, val_col);
        }
        /* Decay slider — own row below the six main sliders. */
        {
            Rect rr = L.efx_set_decay;
            float v = g_app_settings.effects.decay;
            int   fill_w = (int)(v * (float)rr.w);
            DrawTextEx(*f, "Decay",
                       (Vector2){rr.x - 96, rr.y + (rr.h - 13) / 2 + 1},
                       13, 0, label_col);
            int track_h = 8;
            int track_y = rr.y + (rr.h - track_h) / 2;
            DrawRectangle(rr.x, track_y, rr.w, track_h, slider_bg);
            DrawRectangle(rr.x, track_y, fill_w, track_h, slider_fill);
            DrawRectangleLines(rr.x, track_y, rr.w, track_h, slider_border);
            int thumb_x = rr.x + fill_w;
            DrawCircle(thumb_x, track_y + track_h / 2, 7, slider_thumb);
            DrawCircleLines(thumb_x, track_y + track_h / 2, 7, (Color){30, 34, 46, 255});
            char vbuf[8];
            snprintf(vbuf, sizeof(vbuf), "%.0f%%", v * 100.0f);
            Vector2 vsz = MeasureTextEx(*f, vbuf, 12, 0);
            DrawTextEx(*f, vbuf,
                       (Vector2){rr.x + rr.w + 6,
                                 rr.y + (rr.h - vsz.y) / 2},
                       12, 0, val_col);
        }
        DrawTextEx(*f, "Phosphor",
                   (Vector2){L.efx_set_phos[0].x - 96,
                             L.efx_set_phos[0].y + (L.efx_set_phos[0].h - 13) / 2 + 1},
                   13, 0, label_col);
        for (int i = 0; i < PHOSPHOR_COUNT; i++) {
            Rect rr = L.efx_set_phos[i];
            bool sel = (g_app_settings.effects.phosphor == (PhosphorMode)i);
            Color bg = sel ? (Color){46, 92, 150, 255} : (Color){34, 38, 52, 255};
            DrawRectangle(rr.x, rr.y, rr.w, rr.h, bg);
            DrawRectangleLines(rr.x, rr.y, rr.w, rr.h,
                               (Color){125, 207, 255, sel ? 255 : 120});
            const char *plbl = phosphor_label((PhosphorMode)i);
            Vector2 ts = MeasureTextEx(*f, plbl, 12, 0);
            DrawTextEx(*f, plbl,
                       (Vector2){rr.x + (rr.w - ts.x) / 2,
                                 rr.y + (rr.h - ts.y) / 2},
                       12, 0, (Color){230, 232, 240, 255});
        }
        /* Preset row — one-click cinema looks. */
        DrawTextEx(*f, "Preset",
                   (Vector2){L.efx_set_preset[0].x - 60,
                             L.efx_set_preset[0].y - 16},
                   13, 0, label_col);
        for (int i = 0; i < EFX_PRESET_COUNT; i++) {
            Rect rr = L.efx_set_preset[i];
            DrawRectangle(rr.x, rr.y, rr.w, rr.h, (Color){34, 38, 52, 255});
            DrawRectangleLines(rr.x, rr.y, rr.w, rr.h, (Color){125, 207, 255, 140});
            const char *lbl = rec_effects_preset_label((EfxPreset)i);
            Vector2 ts = MeasureTextEx(*f, lbl, 12, 0);
            DrawTextEx(*f, lbl,
                       (Vector2){rr.x + (rr.w - ts.x) / 2,
                                 rr.y + (rr.h - ts.y) / 2},
                       12, 0, (Color){230, 232, 240, 255});
        }
        DrawRectangle(L.efx_set_reset.x, L.efx_set_reset.y,
                      L.efx_set_reset.w, L.efx_set_reset.h,
                      (Color){48, 52, 66, 255});
        DrawRectangleLines(L.efx_set_reset.x, L.efx_set_reset.y,
                           L.efx_set_reset.w, L.efx_set_reset.h,
                           (Color){150, 160, 180, 200});
        Vector2 rs = MeasureTextEx(*f, "Reset all", 13, 0);
        DrawTextEx(*f, "Reset all",
                   (Vector2){L.efx_set_reset.x + (L.efx_set_reset.w - rs.x) / 2,
                             L.efx_set_reset.y + (L.efx_set_reset.h - rs.y) / 2},
                   13, 0, (Color){210, 220, 235, 255});
    }
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

    /* Browse-logs button. Opens the logs modal (Cmd+Shift+O). */
    if (L.log_browse.w > 0) {
        DrawRectangle(L.log_browse.x, L.log_browse.y,
                      L.log_browse.w, L.log_browse.h,
                      (Color){48, 60, 86, 255});
        DrawRectangleLines(L.log_browse.x, L.log_browse.y,
                           L.log_browse.w, L.log_browse.h,
                           (Color){125, 207, 255, 200});
        const char *bl = "Browse logs ...";
        Vector2 bsz = MeasureTextEx(*f, bl, 13, 0);
        DrawTextEx(*f, bl,
                   (Vector2){L.log_browse.x + (L.log_browse.w - bsz.x) / 2,
                             L.log_browse.y + (L.log_browse.h - bsz.y) / 2},
                   13, 0, (Color){210, 225, 245, 255});
    }

    }  /* end Session tab */
    if (g_settings_tab == SETTINGS_TAB_WINDOW) {
    /* Startup window mode — two rows.
       Row 1: Default | Small | Medium | Large
       Row 2: Fill | Fullscreen | Own Space
       Applied on next launch; Save as Default persists. */
    {
        DrawTextEx(*f, "On launch",
                   (Vector2){L.modal.x + 22, L.startup_default.y + 8},
                   14, 0, (Color){200, 205, 220, 255});
        struct { Rect r; const char *label; int mode; } opts[] = {
            { L.startup_default,    "Default",      STARTUP_WINDOW_DEFAULT },
            { L.startup_small,      "Small",        STARTUP_WINDOW_SMALL },
            { L.startup_medium,     "Medium",       STARTUP_WINDOW_MEDIUM },
            { L.startup_large,      "Large",        STARTUP_WINDOW_LARGE },
            { L.startup_fill,       "Fill screen",  STARTUP_WINDOW_FILL },
            { L.startup_borderless, "Fullscreen",   STARTUP_WINDOW_BORDERLESS },
#ifdef __APPLE__
            { L.startup_maximized,  "Own Space",    STARTUP_WINDOW_MAXIMIZED },
#else
            { L.startup_maximized,  "Maximized",    STARTUP_WINDOW_MAXIMIZED },
#endif
        };
        int n = (int)(sizeof(opts) / sizeof(opts[0]));
        for (int i = 0; i < n; i++) {
            Rect rr = opts[i].r;
            if (rr.w == 0) continue;
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

    if (g_settings_tab == SETTINGS_TAB_HUD) {
        const char *field_labels[HUD_FIELD_COUNT] = {
            "Host", "IP", "Load", "Memory", "Disk"
        };

        /* Master + position. ON = green, OFF / unselected = grey. */
        struct { Rect r; const char *lbl; bool on; } top_btns[] = {
            { L.hud_toggle, g_app_settings.show_hud ? "Enabled" : "Disabled", g_app_settings.show_hud },
            { L.hud_pos_tl, "Top-left",     g_app_settings.hud_pos == HUD_POS_TOP_LEFT     },
            { L.hud_pos_tr, "Top-right",    g_app_settings.hud_pos == HUD_POS_TOP_RIGHT    },
            { L.hud_pos_bl, "Bottom-left",  g_app_settings.hud_pos == HUD_POS_BOTTOM_LEFT  },
            { L.hud_pos_br, "Bottom-right", g_app_settings.hud_pos == HUD_POS_BOTTOM_RIGHT },
        };
        DrawTextEx(*f, "Show HUD",
                   (Vector2){L.modal.x + 22, L.hud_toggle.y + 8},
                   14, 0, (Color){200, 205, 220, 255});
        DrawTextEx(*f, "Position",
                   (Vector2){L.modal.x + 22, L.hud_pos_tl.y + 8},
                   14, 0, (Color){200, 205, 220, 255});
        for (size_t bi = 0; bi < sizeof(top_btns) / sizeof(*top_btns); bi++) {
            Color bg   = top_btns[bi].on ? (Color){48, 78, 58, 255}  : (Color){38, 42, 56, 255};
            Color line = top_btns[bi].on ? (Color){150, 220, 170, 200} : (Color){70, 74, 90, 255};
            Color tx   = top_btns[bi].on ? (Color){220, 240, 225, 255} : (Color){180, 185, 195, 255};
            DrawRectangle(top_btns[bi].r.x, top_btns[bi].r.y, top_btns[bi].r.w, top_btns[bi].r.h, bg);
            DrawRectangleLines(top_btns[bi].r.x, top_btns[bi].r.y, top_btns[bi].r.w, top_btns[bi].r.h, line);
            Vector2 bs = MeasureTextEx(*f, top_btns[bi].lbl, 13, 0);
            DrawTextEx(*f, top_btns[bi].lbl,
                       (Vector2){top_btns[bi].r.x + (top_btns[bi].r.w - bs.x) / 2,
                                 top_btns[bi].r.y + (top_btns[bi].r.h - bs.y) / 2},
                       13, 0, tx);
        }

        /* Per-field grid: label / show / colour swatch / size +-. */
        for (int fi = 0; fi < HUD_FIELD_COUNT; fi++) {
            DrawTextEx(*f, field_labels[fi],
                       (Vector2){L.modal.x + 22 + 60,
                                 L.hud_show_btn[fi].y + 8},
                       13, 0, (Color){200, 205, 220, 255});
            /* The label should sit to the LEFT of the show button — the
               row's x starts at L.modal.x + 140 + 60. Pull it back. */
            DrawTextEx(*f, field_labels[fi],
                       (Vector2){L.modal.x + 140,
                                 L.hud_show_btn[fi].y + 8},
                       13, 0, (Color){200, 205, 220, 255});

            /* Show toggle. */
            bool sh = g_app_settings.hud_show[fi];
            Color sbg   = sh ? (Color){48, 78, 58, 255}  : (Color){38, 42, 56, 255};
            Color sline = sh ? (Color){150, 220, 170, 200} : (Color){70, 74, 90, 255};
            Color stx   = sh ? (Color){220, 240, 225, 255} : (Color){180, 185, 195, 255};
            DrawRectangle(L.hud_show_btn[fi].x, L.hud_show_btn[fi].y,
                          L.hud_show_btn[fi].w, L.hud_show_btn[fi].h, sbg);
            DrawRectangleLines(L.hud_show_btn[fi].x, L.hud_show_btn[fi].y,
                               L.hud_show_btn[fi].w, L.hud_show_btn[fi].h, sline);
            const char *show_lbl = sh ? "Visible" : "Hidden";
            Vector2 ssz = MeasureTextEx(*f, show_lbl, 12, 0);
            DrawTextEx(*f, show_lbl,
                       (Vector2){L.hud_show_btn[fi].x + (L.hud_show_btn[fi].w - ssz.x) / 2,
                                 L.hud_show_btn[fi].y + (L.hud_show_btn[fi].h - ssz.y) / 2},
                       12, 0, stx);

            /* Colour swatch — fills with the current palette colour;
               click to cycle. */
            int ci = g_app_settings.hud_color[fi];
            if (ci < 0 || ci >= HUD_PALETTE_COUNT) ci = 0;
            DrawRectangle(L.hud_color_btn[fi].x, L.hud_color_btn[fi].y,
                          L.hud_color_btn[fi].w, L.hud_color_btn[fi].h,
                          HUD_PALETTE[ci]);
            DrawRectangleLines(L.hud_color_btn[fi].x, L.hud_color_btn[fi].y,
                               L.hud_color_btn[fi].w, L.hud_color_btn[fi].h,
                               (Color){90, 95, 110, 255});

            /* Size minus / value / plus. */
            DrawRectangle(L.hud_size_dec[fi].x, L.hud_size_dec[fi].y,
                          L.hud_size_dec[fi].w, L.hud_size_dec[fi].h,
                          (Color){38, 42, 56, 255});
            DrawRectangleLines(L.hud_size_dec[fi].x, L.hud_size_dec[fi].y,
                               L.hud_size_dec[fi].w, L.hud_size_dec[fi].h,
                               (Color){70, 74, 90, 255});
            Vector2 mns = MeasureTextEx(*f, "-", 14, 0);
            DrawTextEx(*f, "-",
                       (Vector2){L.hud_size_dec[fi].x + (L.hud_size_dec[fi].w - mns.x) / 2,
                                 L.hud_size_dec[fi].y + (L.hud_size_dec[fi].h - mns.y) / 2},
                       14, 0, (Color){200, 205, 220, 255});

            char szbuf[8];
            snprintf(szbuf, sizeof(szbuf), "%d", g_app_settings.hud_size[fi]);
            Vector2 svs = MeasureTextEx(*f, szbuf, 13, 0);
            DrawTextEx(*f, szbuf,
                       (Vector2){L.hud_size_val[fi].x + (L.hud_size_val[fi].w - svs.x) / 2,
                                 L.hud_size_val[fi].y + (L.hud_size_val[fi].h - svs.y) / 2},
                       13, 0, (Color){220, 224, 232, 255});

            DrawRectangle(L.hud_size_inc[fi].x, L.hud_size_inc[fi].y,
                          L.hud_size_inc[fi].w, L.hud_size_inc[fi].h,
                          (Color){38, 42, 56, 255});
            DrawRectangleLines(L.hud_size_inc[fi].x, L.hud_size_inc[fi].y,
                               L.hud_size_inc[fi].w, L.hud_size_inc[fi].h,
                               (Color){70, 74, 90, 255});
            Vector2 pls = MeasureTextEx(*f, "+", 14, 0);
            DrawTextEx(*f, "+",
                       (Vector2){L.hud_size_inc[fi].x + (L.hud_size_inc[fi].w - pls.x) / 2,
                                 L.hud_size_inc[fi].y + (L.hud_size_inc[fi].h - pls.y) / 2},
                       14, 0, (Color){200, 205, 220, 255});
        }

        /* CPU graph toggle. */
        bool cpu_on = g_app_settings.hud_show_cpu;
        Color cbg   = cpu_on ? (Color){48, 78, 58, 255}  : (Color){38, 42, 56, 255};
        Color cline = cpu_on ? (Color){150, 220, 170, 200} : (Color){70, 74, 90, 255};
        Color ctx   = cpu_on ? (Color){220, 240, 225, 255} : (Color){180, 185, 195, 255};
        DrawRectangle(L.hud_cpu_toggle.x, L.hud_cpu_toggle.y,
                      L.hud_cpu_toggle.w, L.hud_cpu_toggle.h, cbg);
        DrawRectangleLines(L.hud_cpu_toggle.x, L.hud_cpu_toggle.y,
                           L.hud_cpu_toggle.w, L.hud_cpu_toggle.h, cline);
        const char *clbl = cpu_on ? "CPU graph: on" : "CPU graph: off";
        Vector2 cz = MeasureTextEx(*f, clbl, 13, 0);
        DrawTextEx(*f, clbl,
                   (Vector2){L.hud_cpu_toggle.x + (L.hud_cpu_toggle.w - cz.x) / 2,
                             L.hud_cpu_toggle.y + (L.hud_cpu_toggle.h - cz.y) / 2},
                   13, 0, ctx);

        DrawTextEx(*f, "System info overlay shown in the corner of every pane.",
                   (Vector2){L.modal.x + 22,
                             L.hud_cpu_toggle.y + L.hud_cpu_toggle.h + 10},
                   12, 0, (Color){140, 150, 170, 255});
    }  /* end HUD tab */

    if (g_settings_tab == SETTINGS_TAB_SESSIONS) {
        int cap_y = L.sess_new_btn.y - 22;
        DrawTextEx(*f, "Saved sessions",
                   (Vector2){L.modal.x + 22, cap_y},
                   14, 0, (Color){200, 205, 220, 255});
        /* New-session button. */
        Rect nb = L.sess_new_btn;
        DrawRectangle(nb.x, nb.y, nb.w, nb.h, (Color){46, 92, 150, 255});
        DrawRectangleLines(nb.x, nb.y, nb.w, nb.h, (Color){125, 207, 255, 255});
        const char *nbl = "+ New session  (Cmd+Shift+P)";
        Vector2 nsz = MeasureTextEx(*f, nbl, 13, 0);
        DrawTextEx(*f, nbl,
                   (Vector2){nb.x + (nb.w - nsz.x) / 2,
                             nb.y + (nb.h - nsz.y) / 2},
                   13, 0, (Color){230, 240, 255, 255});
        /* Per-row layout. */
        for (int i = 0; i < g_sessions_count; i++) {
            Rect rb = L.sess_row[i];
            Rect ob = L.sess_open[i];
            Rect eb = L.sess_edit[i];
            Rect xb = L.sess_del[i];
            DrawRectangle(rb.x, rb.y, rb.w, rb.h, (Color){22, 25, 34, 255});
            DrawRectangleLines(rb.x, rb.y, rb.w, rb.h, (Color){70, 74, 90, 255});
            DrawTextEx(*f, g_sessions[i].name,
                       (Vector2){rb.x + 10, rb.y + 7},
                       13, 0, (Color){230, 232, 240, 255});
            int n = session_count_leaves(g_sessions[i].root);
            char meta[32];
            snprintf(meta, sizeof(meta), "%d pane%s", n, n == 1 ? "" : "s");
            Vector2 msz = MeasureTextEx(*f, meta, 11, 0);
            DrawTextEx(*f, meta,
                       (Vector2){rb.x + rb.w - msz.x - 10, rb.y + 9},
                       11, 0, (Color){140, 145, 160, 255});
            /* Open / Edit / Delete buttons. */
            DrawRectangle(ob.x, ob.y, ob.w, ob.h, (Color){46, 92, 150, 255});
            DrawRectangleLines(ob.x, ob.y, ob.w, ob.h, (Color){125, 207, 255, 220});
            DrawTextEx(*f, "Open",
                       (Vector2){ob.x + (ob.w - MeasureText("Open", 13)) / 2.0f,
                                 ob.y + (ob.h - 13) / 2.0f},
                       13, 0, (Color){230, 240, 255, 255});
            DrawRectangle(eb.x, eb.y, eb.w, eb.h, (Color){48, 52, 66, 255});
            DrawRectangleLines(eb.x, eb.y, eb.w, eb.h, (Color){150, 155, 170, 200});
            DrawTextEx(*f, "Edit",
                       (Vector2){eb.x + (eb.w - MeasureText("Edit", 13)) / 2.0f,
                                 eb.y + (eb.h - 13) / 2.0f},
                       13, 0, (Color){210, 215, 230, 255});
            DrawRectangle(xb.x, xb.y, xb.w, xb.h, (Color){62, 30, 32, 255});
            DrawRectangleLines(xb.x, xb.y, xb.w, xb.h, (Color){200, 80, 80, 220});
            DrawTextEx(*f, "×",
                       (Vector2){xb.x + (xb.w - MeasureText("×", 16)) / 2.0f,
                                 xb.y + (xb.h - 16) / 2.0f},
                       16, 0, (Color){240, 200, 200, 255});
        }
        if (g_sessions_count == 0) {
            DrawTextEx(*f,
                       "No saved sessions yet. Click + New session to design one.",
                       (Vector2){L.modal.x + 22, L.sess_new_btn.y + 50},
                       12, 0, (Color){140, 145, 160, 255});
        }
    }

    if (g_settings_tab == SETTINGS_TAB_LAUNCH) {
        /* Caption anchored just above the first row so it doesn't
           collide with the row pills (which sit at content_top).
           When no rows are configured, fall back to a fixed y
           below the tab bar so it still renders. */
        int cap_y = (g_app_settings.launch_count > 0)
                    ? (L.launch_kind[0].y - 22)
                    : (L.modal.y + 76);
        DrawTextEx(*f, "Open these on launch",
                   (Vector2){L.modal.x + 22, cap_y},
                   14, 0, (Color){200, 205, 220, 255});
        /* "Active" column header — centred over the radio column.
           Only meaningful when there's at least one row to label. */
        if (g_app_settings.launch_count > 0) {
            const char *hdr = "Active";
            Vector2 hsz = MeasureTextEx(*f, hdr, 11, 0);
            Rect ab = L.launch_active[0];
            DrawTextEx(*f, hdr,
                       (Vector2){ab.x + (ab.w - hsz.x) / 2, cap_y + 2},
                       11, 0, (Color){170, 175, 195, 255});
        }
        for (int i = 0; i < g_app_settings.launch_count; i++) {
            Rect kb = L.launch_kind[i];
            Rect hb = L.launch_host[i];
            Rect xb = L.launch_del[i];
            int kind = g_app_settings.launch[i].kind;
            bool is_ssh     = (kind == LAUNCH_KIND_SSH);
            bool is_session = (kind == LAUNCH_KIND_SESSION);
            bool needs_pick = is_ssh || is_session;

            /* Kind pill — Local (grey) / SSH (cyan) / Session
               (purple) so the colour reads as a kind cue at a
               glance. */
            Color kbg, kline;
            if (is_session) {
                kbg   = (Color){82, 50, 122, 255};
                kline = (Color){180, 130, 230, 220};
            } else if (is_ssh) {
                kbg   = (Color){46, 92, 150, 255};
                kline = (Color){125, 207, 255, 220};
            } else {
                kbg   = (Color){48, 52, 66, 255};
                kline = (Color){150, 155, 170, 200};
            }
            DrawRectangle(kb.x, kb.y, kb.w, kb.h, kbg);
            DrawRectangleLines(kb.x, kb.y, kb.w, kb.h, kline);
            const char *klbl = is_session ? "Session" : (is_ssh ? "SSH" : "Local");
            Vector2 ksz = MeasureTextEx(*f, klbl, 13, 0);
            DrawTextEx(*f, klbl,
                       (Vector2){kb.x + (kb.w - ksz.x) / 2,
                                 kb.y + (kb.h - ksz.y) / 2},
                       13, 0, (Color){230, 232, 240, 255});

            /* Host picker — for SSH/Session rows it's a click-to-open
               dropdown; for Local it's a disabled label. */
            DrawRectangle(hb.x, hb.y, hb.w, hb.h, (Color){22, 25, 34, 255});
            bool dd_open = (g_settings_launch_dropdown == i && needs_pick);
            DrawRectangleLines(hb.x, hb.y, hb.w, hb.h,
                               dd_open ? (Color){125, 207, 255, 255}
                                       : (Color){70, 74, 90, 255});
            BeginScissorMode(hb.x + 6, hb.y, hb.w - 12, hb.h);
            const char *htext;
            if (is_session) {
                htext = g_app_settings.launch[i].host[0]
                    ? g_app_settings.launch[i].host
                    : "(click to choose a saved session)";
            } else if (is_ssh) {
                htext = g_app_settings.launch[i].host[0]
                    ? g_app_settings.launch[i].host
                    : "(click to choose a saved host)";
            } else {
                htext = "(default shell — no remote host)";
            }
            Color htcol = (needs_pick && g_app_settings.launch[i].host[0])
                           ? (Color){230, 232, 240, 255}
                           : (Color){110, 115, 130, 255};
            DrawTextEx(*f, htext, (Vector2){hb.x + 8, hb.y + 7},
                       14, 0, htcol);
            EndScissorMode();
            /* Chevron on the right edge for picker rows. */
            if (needs_pick) {
                float cx = hb.x + hb.w - 14, cy = hb.y + hb.h / 2;
                Color ch = (Color){180, 195, 220, 220};
                DrawLineEx((Vector2){cx - 4, cy - 2},
                           (Vector2){cx,     cy + 2}, 1.6f, ch);
                DrawLineEx((Vector2){cx + 4, cy - 2},
                           (Vector2){cx,     cy + 2}, 1.6f, ch);
            }

            /* "Active" radio — clicking marks this row as the
               focused tab once the launch sweep finishes. Drawn
               as a small circle outline; filled when selected. */
            {
                Rect ab = L.launch_active[i];
                if (ab.w > 0) {
                    bool is_active = (g_app_settings.launch_active == i);
                    DrawRectangle(ab.x, ab.y, ab.w, ab.h, (Color){38, 42, 56, 255});
                    DrawRectangleLines(ab.x, ab.y, ab.w, ab.h,
                                       is_active ? (Color){255, 220, 100, 220}
                                                 : (Color){90, 95, 110, 200});
                    float cx = ab.x + ab.w / 2.0f;
                    float cy = ab.y + ab.h / 2.0f;
                    DrawCircleLines((int)cx, (int)cy, 6.0f,
                                    is_active ? (Color){255, 220, 100, 240}
                                              : (Color){180, 185, 200, 200});
                    if (is_active) {
                        DrawCircle((int)cx, (int)cy, 3.5f, (Color){255, 220, 100, 255});
                    }
                }
            }

            /* Up / Down reorder buttons. Drawn from raylib line
               primitives so they don't depend on the active font
               having ▲/▼ codepoints. Edge rows have one of the
               two collapsed to width 0 (set in the layout); skip
               drawing for them. */
            for (int dir = 0; dir < 2; dir++) {
                Rect rb = (dir == 0) ? L.launch_up[i] : L.launch_down[i];
                if (rb.w == 0) continue;
                DrawRectangle(rb.x, rb.y, rb.w, rb.h, (Color){38, 42, 56, 255});
                DrawRectangleLines(rb.x, rb.y, rb.w, rb.h, (Color){125, 207, 255, 200});
                float cx = rb.x + rb.w / 2.0f;
                float cy = rb.y + rb.h / 2.0f;
                float s  = rb.h * 0.30f;
                Color glyph = (Color){220, 235, 255, 255};
                if (dir == 0) {
                    /* Up chevron. */
                    DrawLineEx((Vector2){cx - s, cy + s/2},
                               (Vector2){cx,     cy - s/2}, 1.8f, glyph);
                    DrawLineEx((Vector2){cx + s, cy + s/2},
                               (Vector2){cx,     cy - s/2}, 1.8f, glyph);
                } else {
                    /* Down chevron. */
                    DrawLineEx((Vector2){cx - s, cy - s/2},
                               (Vector2){cx,     cy + s/2}, 1.8f, glyph);
                    DrawLineEx((Vector2){cx + s, cy - s/2},
                               (Vector2){cx,     cy + s/2}, 1.8f, glyph);
                }
            }

            /* Delete (×) button. */
            DrawRectangle(xb.x, xb.y, xb.w, xb.h, (Color){48, 30, 30, 255});
            DrawRectangleLines(xb.x, xb.y, xb.w, xb.h, (Color){200, 110, 110, 200});
            Vector2 xsz = MeasureTextEx(*f, "x", 16, 0);
            DrawTextEx(*f, "x",
                       (Vector2){xb.x + (xb.w - xsz.x) / 2,
                                 xb.y + (xb.h - xsz.y) / 2},
                       16, 0, (Color){240, 200, 200, 255});
        }

        /* Add buttons. Disabled (dim) when the slot list is full. */
        bool full = (g_app_settings.launch_count >= LAUNCH_ENTRY_MAX);
        struct { Rect r; const char *label; } adds[] = {
            { L.launch_add_local,   "+ Add local shell" },
            { L.launch_add_ssh,     "+ Add SSH host"    },
            { L.launch_add_session, "+ Add session"     },
        };
        for (size_t ai = 0; ai < sizeof(adds) / sizeof(*adds); ai++) {
            Color ab   = full ? (Color){34, 38, 52, 255} : (Color){48, 52, 70, 255};
            Color al   = full ? (Color){70, 74, 90, 200} : (Color){125, 207, 255, 200};
            Color at   = full ? (Color){110, 115, 130, 255} : (Color){220, 235, 255, 255};
            DrawRectangle(adds[ai].r.x, adds[ai].r.y, adds[ai].r.w, adds[ai].r.h, ab);
            DrawRectangleLines(adds[ai].r.x, adds[ai].r.y, adds[ai].r.w, adds[ai].r.h, al);
            Vector2 az = MeasureTextEx(*f, adds[ai].label, 13, 0);
            DrawTextEx(*f, adds[ai].label,
                       (Vector2){adds[ai].r.x + (adds[ai].r.w - az.x) / 2,
                                 adds[ai].r.y + (adds[ai].r.h - az.y) / 2},
                       13, 0, at);
        }

        if (g_app_settings.launch_count == 0) {
            DrawTextEx(*f, "(no entries — rbterm opens one local shell on launch)",
                       (Vector2){L.modal.x + 22, L.launch_add_local.y - 26},
                       12, 0, (Color){140, 150, 170, 255});
        }

        /* Dropdown panel — drawn last so it overlays the rows
           below it. Only one is open at a time; sources from
           g_ssh_profiles[] for SSH rows, g_sessions[] for Session
           rows. */
        if (g_settings_launch_dropdown >= 0 &&
            g_settings_launch_dropdown < g_app_settings.launch_count) {
            int i = g_settings_launch_dropdown;
            int kind = g_app_settings.launch[i].kind;
            if (kind == LAUNCH_KIND_SSH || kind == LAUNCH_KIND_SESSION) {
                Rect hb = L.launch_host[i];
                int row_h = 22;
                bool is_session = (kind == LAUNCH_KIND_SESSION);
                int n = is_session ? g_sessions_count : g_ssh_profile_count;
                int dh = n * row_h;
                int max_dh = 220;
                if (dh > max_dh) dh = max_dh;
                if (n == 0) dh = row_h;   /* leave room for the empty hint */
                Rect dd = (Rect){ hb.x, hb.y + hb.h, hb.w, dh };
                DrawRectangle(dd.x, dd.y, dd.w, dd.h, (Color){22, 25, 34, 255});
                DrawRectangleLines(dd.x, dd.y, dd.w, dd.h, (Color){125, 207, 255, 220});
                BeginScissorMode(dd.x + 1, dd.y + 1, dd.w - 2, dd.h - 2);
                int visible = dd.h / row_h;
                int max_scroll = n - visible;
                if (max_scroll < 0) max_scroll = 0;
                if (g_settings_launch_scroll > max_scroll)
                    g_settings_launch_scroll = max_scroll;
                for (int k = 0; k < n; k++) {
                    int ry = dd.y + (k - g_settings_launch_scroll) * row_h;
                    if (ry + row_h < dd.y || ry > dd.y + dd.h) continue;
                    const char *name = is_session
                                          ? g_sessions[k].name
                                          : g_ssh_profiles[k].name;
                    bool current = (strcmp(name,
                                           g_app_settings.launch[i].host) == 0);
                    if (current) {
                        DrawRectangle(dd.x + 2, ry, dd.w - 4, row_h,
                                      (Color){46, 62, 90, 220});
                    }
                    DrawTextEx(*f, name,
                               (Vector2){dd.x + 10, ry + 4},
                               13, 0,
                               current ? (Color){230, 232, 240, 255}
                                       : (Color){200, 205, 220, 255});
                }
                if (n == 0) {
                    DrawTextEx(*f,
                               is_session
                                   ? "(no saved sessions yet)"
                                   : "(no saved hosts in ~/.ssh/config)",
                               (Vector2){dd.x + 10, dd.y + 6},
                               12, 0, (Color){140, 145, 160, 255});
                }
                EndScissorMode();
            }
        }
    }

    if (g_settings_tab == SETTINGS_TAB_KEYS) {
        /* Header sits in the 26px gap reserved above the first row
           (see settings_layout). Anchor to the row top minus the
           header height + a small gap so it never collides. */
        int header_y = (g_ssh_keys_count > 0
                        ? L.keys_install[0].y - 22
                        : L.keys_generate_btn.y - 22);
        DrawTextEx(*f, "SSH keys (~/.ssh)",
                   (Vector2){L.modal.x + 22, header_y},
                   14, 0, (Color){200, 205, 220, 255});

        for (int i = 0; i < g_ssh_keys_count; i++) {
            Rect ib = L.keys_install[i];
            /* Row backdrop. */
            DrawRectangle(L.modal.x + 22, ib.y,
                          ib.x - (L.modal.x + 22) - 10, ib.h,
                          (Color){22, 25, 34, 255});
            DrawRectangleLines(L.modal.x + 22, ib.y,
                               ib.x - (L.modal.x + 22) - 10, ib.h,
                               (Color){70, 74, 90, 255});

            /* Name + algo + truncated fingerprint, left-aligned. */
            char line[256];
            const SshKeyEntry *e = &g_ssh_keys[i];
            if (e->fingerprint[0]) {
                snprintf(line, sizeof(line), "%s   %s   %s",
                         e->name,
                         e->algo[0] ? e->algo : "?",
                         e->fingerprint);
            } else {
                snprintf(line, sizeof(line), "%s   %s",
                         e->name, e->algo[0] ? e->algo : "?");
            }
            BeginScissorMode(L.modal.x + 28, ib.y,
                             (ib.x - (L.modal.x + 22) - 18), ib.h);
            DrawTextEx(*f, line,
                       (Vector2){L.modal.x + 30, ib.y + 7},
                       13, 0, (Color){230, 232, 240, 255});
            EndScissorMode();

            /* Install button. */
            bool open = (g_keys_install_dropdown == i);
            DrawRectangle(ib.x, ib.y, ib.w, ib.h,
                          open ? (Color){46, 92, 150, 255}
                               : (Color){46, 52, 70, 255});
            DrawRectangleLines(ib.x, ib.y, ib.w, ib.h,
                               (Color){125, 207, 255, open ? 255 : 200});
            const char *ilbl = "Install on host…";
            Vector2 isz = MeasureTextEx(*f, ilbl, 13, 0);
            DrawTextEx(*f, ilbl,
                       (Vector2){ib.x + (ib.w - isz.x) / 2,
                                 ib.y + (ib.h - isz.y) / 2},
                       13, 0, (Color){230, 240, 255, 255});

            /* Delete (×) button — sits to the right of Install. */
            Rect db = L.keys_delete[i];
            DrawRectangle(db.x, db.y, db.w, db.h, (Color){58, 30, 34, 255});
            DrawRectangleLines(db.x, db.y, db.w, db.h, (Color){220, 110, 110, 200});
            const char *xlbl = "x";
            Vector2 xsz = MeasureTextEx(*f, xlbl, 14, 0);
            DrawTextEx(*f, xlbl,
                       (Vector2){db.x + (db.w - xsz.x) / 2,
                                 db.y + (db.h - xsz.y) / 2},
                       14, 0, (Color){240, 200, 200, 255});
        }
        if (g_ssh_keys_count == 0) {
            /* Place the empty-state hint a row below the header.
               Without the +18 it landed at the same y as the
               header (both anchor off keys_generate_btn.y - 22)
               and the two strings drew on top of each other. */
            DrawTextEx(*f, "(no keys yet — generate one below)",
                       (Vector2){L.modal.x + 22,
                                 header_y + 18},
                       12, 0, (Color){140, 150, 170, 255});
        }

        /* Generate button. */
        Rect gb = L.keys_generate_btn;
        DrawRectangle(gb.x, gb.y, gb.w, gb.h, (Color){48, 78, 58, 255});
        DrawRectangleLines(gb.x, gb.y, gb.w, gb.h, (Color){150, 220, 170, 200});
        Vector2 gsz = MeasureTextEx(*f, "+ Generate new key", 13, 0);
        DrawTextEx(*f, "+ Generate new key",
                   (Vector2){gb.x + (gb.w - gsz.x) / 2,
                             gb.y + (gb.h - gsz.y) / 2},
                   13, 0, (Color){220, 240, 225, 255});

        /* Status / last operation outcome. */
        if (g_keys_status[0]) {
            DrawTextEx(*f, g_keys_status,
                       (Vector2){L.modal.x + 22, gb.y + gb.h + 10},
                       12, 0, (Color){170, 220, 180, 255});
        }

        /* Install dropdown — overlays everything else when open. */
        if (g_keys_install_dropdown >= 0 &&
            g_keys_install_dropdown < g_ssh_keys_count) {
            int ki = g_keys_install_dropdown;
            Rect ib = L.keys_install[ki];
            int row_h = 22;
            int n = g_ssh_profile_count;
            int dh = n * row_h;
            int max_dh = 220;
            if (dh > max_dh) dh = max_dh;
            Rect dd = (Rect){ ib.x, ib.y + ib.h, ib.w, dh };
            DrawRectangle(dd.x, dd.y, dd.w, dd.h, (Color){22, 25, 34, 255});
            DrawRectangleLines(dd.x, dd.y, dd.w, dd.h, (Color){125, 207, 255, 220});
            BeginScissorMode(dd.x + 1, dd.y + 1, dd.w - 2, dd.h - 2);
            int visible = dd.h / row_h;
            int max_scroll = n - visible;
            if (max_scroll < 0) max_scroll = 0;
            if (g_keys_install_scroll > max_scroll)
                g_keys_install_scroll = max_scroll;
            for (int kk = 0; kk < n; kk++) {
                int ry = dd.y + (kk - g_keys_install_scroll) * row_h;
                if (ry + row_h < dd.y || ry > dd.y + dd.h) continue;
                DrawTextEx(*f, g_ssh_profiles[kk].name,
                           (Vector2){dd.x + 10, ry + 4},
                           13, 0, (Color){230, 232, 240, 255});
            }
            if (n == 0) {
                DrawTextEx(*f, "(no saved hosts in ~/.ssh/config)",
                           (Vector2){dd.x + 10, dd.y + 6},
                           12, 0, (Color){140, 145, 160, 255});
            }
            EndScissorMode();
        }

        /* Generate sub-modal. */
        if (g_keygen_form.open) {
            DrawRectangle(L.keygen_modal.x, L.keygen_modal.y,
                          L.keygen_modal.w, L.keygen_modal.h,
                          (Color){30, 34, 46, 245});
            DrawRectangleLines(L.keygen_modal.x, L.keygen_modal.y,
                               L.keygen_modal.w, L.keygen_modal.h,
                               (Color){125, 207, 255, 230});
            DrawTextEx(*f, "Generate SSH key",
                       (Vector2){L.keygen_modal.x + 18,
                                 L.keygen_modal.y + 14},
                       16, 0, (Color){230, 232, 240, 255});

            /* Type pills. */
            struct { Rect r; const char *lbl; int idx; } types[] = {
                { L.keygen_type_ed,  "ed25519", 0 },
                { L.keygen_type_rsa, "rsa 4096", 1 },
            };
            for (int ti = 0; ti < 2; ti++) {
                bool on = (g_keygen_form.type_idx == types[ti].idx);
                DrawRectangle(types[ti].r.x, types[ti].r.y,
                              types[ti].r.w, types[ti].r.h,
                              on ? (Color){46, 92, 150, 255}
                                 : (Color){34, 38, 52, 255});
                DrawRectangleLines(types[ti].r.x, types[ti].r.y,
                                   types[ti].r.w, types[ti].r.h,
                                   (Color){125, 207, 255, on ? 255 : 150});
                Vector2 tsz = MeasureTextEx(*f, types[ti].lbl, 13, 0);
                DrawTextEx(*f, types[ti].lbl,
                           (Vector2){types[ti].r.x + (types[ti].r.w - tsz.x) / 2,
                                     types[ti].r.y + (types[ti].r.h - tsz.y) / 2},
                           13, 0, (Color){230, 232, 240, 255});
            }

            /* Name + Pass text fields. */
            struct { Rect r; const char *label; const char *value;
                     bool focus; bool mask; const char *hint; } flds[] = {
                { L.keygen_name_field, "Name",
                  g_keygen_form.name, g_keygen_form.focus_field == 0,
                  false, "id_rbterm" },
                { L.keygen_pass_field, "Passphrase",
                  g_keygen_form.pass, g_keygen_form.focus_field == 1,
                  true,  "(optional)" },
            };
            for (int fi = 0; fi < 2; fi++) {
                Rect rr = flds[fi].r;
                DrawRectangle(rr.x, rr.y, rr.w, rr.h, (Color){22, 25, 34, 255});
                DrawRectangleLines(rr.x, rr.y, rr.w, rr.h,
                                   flds[fi].focus
                                       ? (Color){125, 207, 255, 255}
                                       : (Color){70, 74, 90, 255});
                DrawTextEx(*f, flds[fi].label,
                           (Vector2){rr.x - 100, rr.y + 7},
                           13, 0, (Color){180, 185, 200, 255});
                BeginScissorMode(rr.x + 6, rr.y, rr.w - 12, rr.h);
                char masked[256];
                const char *shown = flds[fi].value;
                if (flds[fi].mask && *shown) {
                    int n = (int)strlen(flds[fi].value);
                    if (n > (int)sizeof(masked) - 1) n = (int)sizeof(masked) - 1;
                    for (int k = 0; k < n; k++) masked[k] = '*';
                    masked[n] = 0;
                    shown = masked;
                }
                Color tc = (Color){230, 232, 240, 255};
                if (!*shown) {
                    shown = flds[fi].hint;
                    tc = (Color){110, 115, 130, 255};
                }
                /* Cmd+A select-all highlight on the focused field. */
                if (flds[fi].focus && g_keygen_form.sel_all && flds[fi].value[0]) {
                    Vector2 ssz = MeasureTextEx(*f, shown, 14, 0);
                    int sw = (int)ssz.x + 4;
                    if (sw > rr.w - 12) sw = rr.w - 12;
                    DrawRectangle(rr.x + 6, rr.y + 4,
                                  sw, rr.h - 8,
                                  (Color){64, 100, 150, 200});
                }
                DrawTextEx(*f, shown,
                           (Vector2){rr.x + 8, rr.y + 7},
                           14, 0, tc);
                if (flds[fi].focus && flds[fi].value[0] && !flds[fi].mask &&
                    ((long long)(GetTime() * 2.0) & 1) == 0) {
                    Vector2 vsz = MeasureTextEx(*f, flds[fi].value, 14, 0);
                    DrawRectangle(rr.x + 8 + (int)vsz.x + 1,
                                  rr.y + 6, 8, 16,
                                  (Color){125, 207, 255, 255});
                }
                EndScissorMode();
            }

            /* Status line / error from a failed generate. */
            if (g_keygen_form.status[0]) {
                DrawTextEx(*f, g_keygen_form.status,
                           (Vector2){L.keygen_modal.x + 18,
                                     L.keygen_ok.y - 22},
                           12, 0, (Color){240, 130, 130, 255});
            }

            /* Cancel + Generate buttons. */
            DrawRectangle(L.keygen_cancel.x, L.keygen_cancel.y,
                          L.keygen_cancel.w, L.keygen_cancel.h,
                          (Color){48, 52, 66, 255});
            DrawRectangleLines(L.keygen_cancel.x, L.keygen_cancel.y,
                               L.keygen_cancel.w, L.keygen_cancel.h,
                               (Color){150, 155, 170, 200});
            Vector2 csz2 = MeasureTextEx(*f, "Cancel", 14, 0);
            DrawTextEx(*f, "Cancel",
                       (Vector2){L.keygen_cancel.x + (L.keygen_cancel.w - csz2.x) / 2,
                                 L.keygen_cancel.y + (L.keygen_cancel.h - csz2.y) / 2},
                       14, 0, (Color){210, 215, 230, 255});

            DrawRectangle(L.keygen_ok.x, L.keygen_ok.y,
                          L.keygen_ok.w, L.keygen_ok.h,
                          (Color){48, 78, 58, 255});
            DrawRectangleLines(L.keygen_ok.x, L.keygen_ok.y,
                               L.keygen_ok.w, L.keygen_ok.h,
                               (Color){150, 220, 170, 200});
            Vector2 osz2 = MeasureTextEx(*f, "Generate", 14, 0);
            DrawTextEx(*f, "Generate",
                       (Vector2){L.keygen_ok.x + (L.keygen_ok.w - osz2.x) / 2,
                                 L.keygen_ok.y + (L.keygen_ok.h - osz2.y) / 2},
                       14, 0, (Color){220, 240, 225, 255});
        }

        /* Delete-confirmation sub-modal. Shows the full private +
           public paths so the user knows exactly what's about to be
           unlinked, and a Cancel/Delete pair. */
        if (g_keys_delete_idx >= 0 && g_keys_delete_idx < g_ssh_keys_count) {
            const SshKeyEntry *e = &g_ssh_keys[g_keys_delete_idx];
            DrawRectangle(L.keysdel_modal.x, L.keysdel_modal.y,
                          L.keysdel_modal.w, L.keysdel_modal.h,
                          (Color){30, 34, 46, 245});
            DrawRectangleLines(L.keysdel_modal.x, L.keysdel_modal.y,
                               L.keysdel_modal.w, L.keysdel_modal.h,
                               (Color){220, 110, 110, 230});
            DrawTextEx(*f, "Delete SSH key?",
                       (Vector2){L.keysdel_modal.x + 18,
                                 L.keysdel_modal.y + 14},
                       16, 0, (Color){240, 200, 200, 255});
            DrawTextEx(*f, "These files will be removed from disk:",
                       (Vector2){L.keysdel_modal.x + 18,
                                 L.keysdel_modal.y + 48},
                       12, 0, (Color){200, 205, 220, 255});
            char l1[PATH_MAX + 16];
            char l2[PATH_MAX + 16];
            snprintf(l1, sizeof(l1), "  %s", e->privpath);
            snprintf(l2, sizeof(l2), "  %s", e->pubpath);
            DrawTextEx(*f, l1,
                       (Vector2){L.keysdel_modal.x + 18,
                                 L.keysdel_modal.y + 70},
                       12, 0, (Color){230, 232, 240, 255});
            DrawTextEx(*f, l2,
                       (Vector2){L.keysdel_modal.x + 18,
                                 L.keysdel_modal.y + 90},
                       12, 0, (Color){230, 232, 240, 255});

            DrawRectangle(L.keysdel_cancel.x, L.keysdel_cancel.y,
                          L.keysdel_cancel.w, L.keysdel_cancel.h,
                          (Color){48, 52, 66, 255});
            DrawRectangleLines(L.keysdel_cancel.x, L.keysdel_cancel.y,
                               L.keysdel_cancel.w, L.keysdel_cancel.h,
                               (Color){150, 155, 170, 200});
            Vector2 dcsz = MeasureTextEx(*f, "Cancel", 14, 0);
            DrawTextEx(*f, "Cancel",
                       (Vector2){L.keysdel_cancel.x + (L.keysdel_cancel.w - dcsz.x) / 2,
                                 L.keysdel_cancel.y + (L.keysdel_cancel.h - dcsz.y) / 2},
                       14, 0, (Color){210, 215, 230, 255});

            DrawRectangle(L.keysdel_ok.x, L.keysdel_ok.y,
                          L.keysdel_ok.w, L.keysdel_ok.h,
                          (Color){90, 38, 42, 255});
            DrawRectangleLines(L.keysdel_ok.x, L.keysdel_ok.y,
                               L.keysdel_ok.w, L.keysdel_ok.h,
                               (Color){220, 110, 110, 230});
            Vector2 dosz = MeasureTextEx(*f, "Delete", 14, 0);
            DrawTextEx(*f, "Delete",
                       (Vector2){L.keysdel_ok.x + (L.keysdel_ok.w - dosz.x) / 2,
                                 L.keysdel_ok.y + (L.keysdel_ok.h - dosz.y) / 2},
                       14, 0, (Color){250, 220, 220, 255});
        }
    }

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

#ifdef RBTERM_SSH
/* SSH-launch worker entry point. Calls pty_open_ssh on a dedicated
   thread so multiple startup hosts can handshake in parallel; the
   main thread polls w->done and integrates the resulting Pty. */
static void *ssh_launch_worker_run(void *arg) {
    SshLaunchWorker *w = arg;
    char err[256] = {0};
    w->pty = pty_open_ssh(
        w->user[0] ? w->user : NULL,
        w->host,
        w->port,
        w->password[0] ? w->password : NULL,
        w->keyfile[0] ? w->keyfile : NULL,
        w->cols, w->rows,
        err, sizeof(err));
    if (!w->pty) snprintf(w->err, sizeof(w->err), "%s", err);
    /* Scrub the password copy as soon as the connect attempt finishes
       — auth has already happened (or failed) by this point and we
       don't want it sitting in worker memory until integration. */
    memset(w->password, 0, sizeof(w->password));
    __atomic_store_n(&w->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

/* Find or recycle a worker slot. Connects from the SSH form / [+]
   menu can stack across a long session; reusing fully-integrated
   slots keeps the bounded array from filling up. Returns NULL if
   every slot is still in flight (LAUNCH_WORKERS_MAX simultaneously
   pending — practically never). */
static SshLaunchWorker *ssh_launch_alloc_slot(void) {
    for (int i = 0; i < g_launch_workers_count; i++) {
        SshLaunchWorker *w = &g_launch_workers[i];
        if (!w->started || w->integrated) {
            memset(w, 0, sizeof(*w));
            return w;
        }
    }
    if (g_launch_workers_count >= LAUNCH_WORKERS_MAX) return NULL;
    SshLaunchWorker *w = &g_launch_workers[g_launch_workers_count++];
    memset(w, 0, sizeof(*w));
    return w;
}

/* Kick an SSH connect on a worker thread. Spawns a placeholder Tab
   immediately ("Connecting to ...") and pthread_creates the worker.
   The integration block in main() picks up the result on a future
   frame and either runs tab_attach_ssh_finalize (success) or paints
   the failure banner. Returns false if no slot was available or the
   thread failed to start; in that case no Tab is created and the
   caller should fall back / report. */
static bool ssh_launch_kick(const SshProfile *prof, const char *alias,
                            const char *password, bool is_active,
                            int cols, int rows) {
    SshLaunchWorker *w = ssh_launch_alloc_slot();
    if (!w) return false;
    if (alias) strncpy(w->alias, alias, sizeof(w->alias) - 1);
    w->is_active = is_active;
    w->cols = cols;
    w->rows = rows;
    if (prof) {
        w->prof = *prof;
        w->prof_valid = true;
        if (prof->user[0])
            strncpy(w->user, prof->user, sizeof(w->user) - 1);
        const char *host = prof->hostname[0] ? prof->hostname
                                             : (alias ? alias : "");
        strncpy(w->host, host, sizeof(w->host) - 1);
        w->port = prof->port > 0 ? prof->port : 22;
        if (prof->identity[0])
            strncpy(w->keyfile, prof->identity, sizeof(w->keyfile) - 1);
    } else if (alias) {
        strncpy(w->host, alias, sizeof(w->host) - 1);
        w->port = 22;
    }
    if (password && *password)
        strncpy(w->password, password, sizeof(w->password) - 1);
    w->placeholder = tab_open_ssh_placeholder(prof, alias, is_active,
                                              cols, rows);
    w->started = 1;
    if (pthread_create(&w->th, NULL, ssh_launch_worker_run, w) != 0) {
        /* Thread spawn failed — recycle the slot. The placeholder
           Tab is left in place with its banner; it'll just never
           transition (rare; treat as best-effort). */
        memset(w->password, 0, sizeof(w->password));
        w->started = 0;
        w->integrated = 1;
        return false;
    }
    return true;
}

/* ---------- Background SSH ops ---------- */

/* Test Auth (SSH form's Test button). Runs ssh_form_tcp_check then
   pty_open_ssh on a worker; integration writes the result into the
   form's status / error line. */
typedef struct {
    pthread_t    th;
    int          started;
    volatile int done;
    int          integrated;
    char user[96];
    char host[256];
    char keyfile[PATH_MAX];
    char password[256];
    int  port;
    int  cols, rows;
    bool ok;
    char msg[256];
} BgTestAuth;
static BgTestAuth g_bg_test_auth;

/* Key Install (Settings → Keys row → Install dropdown pick). Runs
   ssh_keys_install_native on a worker; integration writes a one-
   line status into g_keys_status. */
typedef struct {
    pthread_t    th;
    int          started;
    volatile int done;
    int          integrated;
    SshProfile   prof;
    char         pubkey[8192];
    char         key_label[128];
    char         host_label[128];
    bool         ok;
    char         msg[256];
} BgKeyInstall;
static BgKeyInstall g_bg_key_install;

/* Key Generate (keygen sub-modal). Runs ssh_keys_generate_native
   on a worker; integration rescans ~/.ssh and either closes the
   sub-modal (success) or fills its status line (failure). RSA-4096
   takes a few seconds CPU-bound, so the worker matters even though
   no network is involved. */
typedef struct {
    pthread_t    th;
    int          started;
    volatile int done;
    int          integrated;
    char         type_name[16];
    char         file_stem[128];
    char         passphrase[256];
    bool         ok;
    char         msg[256];
} BgKeyGenerate;
static BgKeyGenerate g_bg_key_generate;

#define BG_BUSY(g) ((g).started && !(g).integrated)

static bool bg_test_auth_busy(void)    { return BG_BUSY(g_bg_test_auth); }
static bool bg_key_install_busy(void)  { return BG_BUSY(g_bg_key_install); }
static bool bg_key_generate_busy(void) { return BG_BUSY(g_bg_key_generate); }

static void *bg_test_auth_run(void *arg) {
    BgTestAuth *t = arg;
    char err[256] = {0};
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
    /* TCP preflight: bogus port / unreachable host fails in ~3s
       instead of waiting through the full libssh timeout. */
    if (!ssh_form_tcp_check(t->host, t->port, 3000, err, sizeof(err))) {
        t->ok = false;
        snprintf(t->msg, sizeof(t->msg), "%s",
                 err[0] ? err : "host unreachable");
        memset(t->password, 0, sizeof(t->password));
        __atomic_store_n(&t->done, 1, __ATOMIC_RELEASE);
        return NULL;
    }
#endif
    Pty *p = pty_open_ssh(
        t->user[0] ? t->user : NULL,
        t->host, t->port,
        t->password[0] ? t->password : NULL,
        t->keyfile[0] ? t->keyfile : NULL,
        t->cols, t->rows, err, sizeof(err));
    if (p) {
        pty_close(p);
        t->ok = true;
        snprintf(t->msg, sizeof(t->msg),
                 "Auth ok — %s@%s:%d",
                 t->user[0] ? t->user : "(default)", t->host, t->port);
    } else {
        t->ok = false;
        snprintf(t->msg, sizeof(t->msg), "%s",
                 err[0] ? err : "auth failed");
    }
    memset(t->password, 0, sizeof(t->password));
    __atomic_store_n(&t->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void *bg_key_install_run(void *arg) {
    BgKeyInstall *t = arg;
    char err[256] = {0};
    bool ok = ssh_keys_install_native(&t->prof, t->pubkey, err, sizeof(err));
    t->ok = ok;
    if (ok) {
        snprintf(t->msg, sizeof(t->msg),
                 "Installed %s on %s.", t->key_label, t->host_label);
    } else {
        snprintf(t->msg, sizeof(t->msg), "%s → %s: %s",
                 t->key_label, t->host_label,
                 err[0] ? err : "install failed");
    }
    memset(t->pubkey, 0, sizeof(t->pubkey));
    __atomic_store_n(&t->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void *bg_key_generate_run(void *arg) {
    BgKeyGenerate *t = arg;
    char err[256] = {0};
    bool ok = ssh_keys_generate_native(t->type_name, t->file_stem,
                                       t->passphrase, err, sizeof(err));
    t->ok = ok;
    snprintf(t->msg, sizeof(t->msg), "%s",
             ok ? "" : (err[0] ? err : "generate failed"));
    memset(t->passphrase, 0, sizeof(t->passphrase));
    __atomic_store_n(&t->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static bool bg_test_auth_kick(int cols, int rows) {
    if (bg_test_auth_busy()) return false;
    memset(&g_bg_test_auth, 0, sizeof(g_bg_test_auth));
    if (g_form.user[0]) strncpy(g_bg_test_auth.user, g_form.user, sizeof(g_bg_test_auth.user) - 1);
    strncpy(g_bg_test_auth.host, g_form.host, sizeof(g_bg_test_auth.host) - 1);
    if (g_form.key[0])  strncpy(g_bg_test_auth.keyfile, g_form.key, sizeof(g_bg_test_auth.keyfile) - 1);
    if (g_form.pass[0]) strncpy(g_bg_test_auth.password, g_form.pass, sizeof(g_bg_test_auth.password) - 1);
    int port = atoi(g_form.port); if (port <= 0) port = 22;
    g_bg_test_auth.port = port;
    g_bg_test_auth.cols = cols;
    g_bg_test_auth.rows = rows;
    g_bg_test_auth.started = 1;
    if (pthread_create(&g_bg_test_auth.th, NULL,
                       bg_test_auth_run, &g_bg_test_auth) != 0) {
        memset(&g_bg_test_auth, 0, sizeof(g_bg_test_auth));
        return false;
    }
    return true;
}

static bool bg_key_install_kick(const SshProfile *prof,
                                const char *pubkey,
                                const char *key_label,
                                const char *host_label) {
    if (bg_key_install_busy()) return false;
    memset(&g_bg_key_install, 0, sizeof(g_bg_key_install));
    g_bg_key_install.prof = *prof;
    if (pubkey)     strncpy(g_bg_key_install.pubkey, pubkey, sizeof(g_bg_key_install.pubkey) - 1);
    if (key_label)  strncpy(g_bg_key_install.key_label, key_label, sizeof(g_bg_key_install.key_label) - 1);
    if (host_label) strncpy(g_bg_key_install.host_label, host_label, sizeof(g_bg_key_install.host_label) - 1);
    g_bg_key_install.started = 1;
    if (pthread_create(&g_bg_key_install.th, NULL,
                       bg_key_install_run, &g_bg_key_install) != 0) {
        memset(&g_bg_key_install, 0, sizeof(g_bg_key_install));
        return false;
    }
    return true;
}

static bool bg_key_generate_kick(const char *type_name,
                                 const char *file_stem,
                                 const char *passphrase) {
    if (bg_key_generate_busy()) return false;
    memset(&g_bg_key_generate, 0, sizeof(g_bg_key_generate));
    if (type_name)  strncpy(g_bg_key_generate.type_name, type_name, sizeof(g_bg_key_generate.type_name) - 1);
    if (file_stem)  strncpy(g_bg_key_generate.file_stem, file_stem, sizeof(g_bg_key_generate.file_stem) - 1);
    if (passphrase) strncpy(g_bg_key_generate.passphrase, passphrase, sizeof(g_bg_key_generate.passphrase) - 1);
    g_bg_key_generate.started = 1;
    if (pthread_create(&g_bg_key_generate.th, NULL,
                       bg_key_generate_run, &g_bg_key_generate) != 0) {
        memset(&g_bg_key_generate, 0, sizeof(g_bg_key_generate));
        return false;
    }
    return true;
}

static bool bg_ssh_integrate(void) {
    bool any = false;
    if (g_bg_test_auth.started && !g_bg_test_auth.integrated &&
        __atomic_load_n(&g_bg_test_auth.done, __ATOMIC_ACQUIRE)) {
        pthread_join(g_bg_test_auth.th, NULL);
        g_bg_test_auth.integrated = 1;
        if (g_bg_test_auth.ok) {
            g_form.error[0] = 0;
            strncpy(g_form_status, g_bg_test_auth.msg, sizeof(g_form_status) - 1);
            g_form_status[sizeof(g_form_status) - 1] = 0;
        } else {
            g_form_status[0] = 0;
            strncpy(g_form.error, g_bg_test_auth.msg, sizeof(g_form.error) - 1);
            g_form.error[sizeof(g_form.error) - 1] = 0;
        }
        any = true;
    }
    if (g_bg_key_install.started && !g_bg_key_install.integrated &&
        __atomic_load_n(&g_bg_key_install.done, __ATOMIC_ACQUIRE)) {
        pthread_join(g_bg_key_install.th, NULL);
        g_bg_key_install.integrated = 1;
        strncpy(g_keys_status, g_bg_key_install.msg, sizeof(g_keys_status) - 1);
        g_keys_status[sizeof(g_keys_status) - 1] = 0;
        any = true;
    }
    if (g_bg_key_generate.started && !g_bg_key_generate.integrated &&
        __atomic_load_n(&g_bg_key_generate.done, __ATOMIC_ACQUIRE)) {
        pthread_join(g_bg_key_generate.th, NULL);
        g_bg_key_generate.integrated = 1;
        if (g_bg_key_generate.ok) {
            snprintf(g_keys_status, sizeof(g_keys_status),
                     "Generated %s.", g_bg_key_generate.file_stem);
            ssh_keys_rescan();
            memset(&g_keygen_form, 0, sizeof(g_keygen_form));
        } else {
            strncpy(g_keygen_form.status, g_bg_key_generate.msg,
                    sizeof(g_keygen_form.status) - 1);
            g_keygen_form.status[sizeof(g_keygen_form.status) - 1] = 0;
        }
        any = true;
    }
    return any;
}
#endif /* RBTERM_SSH */

/* ---------- Logs browser ----------------------------------------

   Lists files in g_app_settings.log_dir (newest first), lets the
   user open one in a new local pane via `less -R`. The modal is
   intentionally minimal — no preview, no export, no delete in v1.
   Opened by Cmd+Shift+L or via the Browse logs button on
   Settings → Logging. */

typedef struct {
    char     name[256];
    char     path[PATH_MAX];
    long long size;
    time_t   mtime;
} LogEntry;

#define LOGS_MAX 256
static LogEntry g_logs[LOGS_MAX];
static int      g_logs_count    = 0;
static int      g_logs_selected = -1;
static int      g_logs_scroll   = 0;
static char     g_logs_status[256];

typedef struct {
    Rect modal;
    Rect list;
    Rect row[LOGS_MAX];
    Rect open_btn;
    Rect close_btn;
} LogsLayout;

static int log_compare_mtime_desc(const void *a, const void *b) {
    const LogEntry *la = a, *lb = b;
    if (la->mtime != lb->mtime) return lb->mtime > la->mtime ? 1 : -1;
    return strcmp(la->name, lb->name);
}

static void logs_scan(void) {
    g_logs_count = 0;
    g_logs_selected = -1;
    g_logs_scroll = 0;
    g_logs_status[0] = 0;
    char dir[PATH_MAX];
    if (!g_app_settings.log_dir[0]) return;
    expand_home_path(g_app_settings.log_dir, dir, sizeof(dir));
#ifdef _WIN32
    char pattern[PATH_MAX + 4];
    snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        snprintf(g_logs_status, sizeof(g_logs_status),
                 "Couldn't open %s", dir);
        return;
    }
    do {
        if (g_logs_count >= LOGS_MAX) break;
        if (fd.cFileName[0] == '.') continue;
        size_t nlen = strlen(fd.cFileName);
        if (nlen < 5 ||
            (strcmp(fd.cFileName + nlen - 4, ".log") != 0 &&
             strcmp(fd.cFileName + nlen - 4, ".txt") != 0)) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        LogEntry *e = &g_logs[g_logs_count++];
        strncpy(e->name, fd.cFileName, sizeof(e->name) - 1);
        e->name[sizeof(e->name) - 1] = 0;
        snprintf(e->path, sizeof(e->path), "%s\\%s", dir, fd.cFileName);
        e->size = ((long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        /* FILETIME → time_t: 100 ns ticks since 1601-01-01,
           subtract the 1601→1970 offset, divide by 10^7. */
        ULARGE_INTEGER ft;
        ft.LowPart  = fd.ftLastWriteTime.dwLowDateTime;
        ft.HighPart = fd.ftLastWriteTime.dwHighDateTime;
        e->mtime = (time_t)((ft.QuadPart - 116444736000000000ULL) / 10000000ULL);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) {
        snprintf(g_logs_status, sizeof(g_logs_status),
                 "Couldn't open %s: %s", dir, strerror(errno));
        return;
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL && g_logs_count < LOGS_MAX) {
        if (de->d_name[0] == '.') continue;
        size_t nlen = strlen(de->d_name);
        /* Include both raw .log and clean .txt — the user can
           pick whichever flavour they want to read. */
        if (nlen < 5 ||
            (strcmp(de->d_name + nlen - 4, ".log") != 0 &&
             strcmp(de->d_name + nlen - 4, ".txt") != 0)) continue;
        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", dir, de->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (!S_ISREG(st.st_mode)) continue;
        LogEntry *e = &g_logs[g_logs_count++];
        strncpy(e->name, de->d_name, sizeof(e->name) - 1);
        e->name[sizeof(e->name) - 1] = 0;
        strncpy(e->path, full, sizeof(e->path) - 1);
        e->path[sizeof(e->path) - 1] = 0;
        e->size = (long long)st.st_size;
        e->mtime = st.st_mtime;
    }
    closedir(d);
#endif
    if (g_logs_count > 1) {
        qsort(g_logs, (size_t)g_logs_count, sizeof(LogEntry),
              log_compare_mtime_desc);
    }
    if (g_logs_count > 0) g_logs_selected = 0;
}

static void logs_open(void) {
    logs_scan();
    g_ui_mode = UI_LOGS;
}

static LogsLayout logs_layout_compute(int win_w, int win_h) {
    LogsLayout L = {0};
    int w = 720, h = 540;
    if (w > win_w - 40) w = win_w - 40;
    if (h > win_h - 40) h = win_h - 40;
    L.modal.x = (win_w - w) / 2;
    L.modal.y = (win_h - h) / 2;
    L.modal.w = w; L.modal.h = h;
    int title_h = 38;
    int pad = 22;
    int btn_h = 32;
    int row_h = 26;
    int footer_y = L.modal.y + h - 22 - btn_h;
    int top_y = L.modal.y + title_h + 18;
    int list_h = footer_y - top_y - 16;
    L.list = (Rect){ L.modal.x + pad, top_y, w - 2 * pad, list_h };
    int visible_rows = list_h / row_h;
    if (visible_rows < 1) visible_rows = 1;
    if (g_logs_scroll < 0) g_logs_scroll = 0;
    int max_scroll = g_logs_count - visible_rows;
    if (max_scroll < 0) max_scroll = 0;
    if (g_logs_scroll > max_scroll) g_logs_scroll = max_scroll;
    for (int i = 0; i < LOGS_MAX; i++) L.row[i] = (Rect){0,0,0,0};
    for (int i = 0; i < g_logs_count; i++) {
        int rel = i - g_logs_scroll;
        if (rel < 0 || rel >= visible_rows) continue;
        L.row[i] = (Rect){ L.list.x, L.list.y + rel * row_h,
                           L.list.w, row_h };
    }
    int open_w = 130, close_w = 90;
    L.close_btn = (Rect){ L.modal.x + w - pad - close_w, footer_y, close_w, btn_h };
    L.open_btn  = (Rect){ L.close_btn.x - 8 - open_w,    footer_y, open_w,  btn_h };
    return L;
}

/* Open the selected log in the OS's default text editor. macOS
   `open -t` honours the user's preferred .txt editor; Linux uses
   xdg-open which honours the desktop association; Windows shells
   out via `start ""` to associate the file with whatever the
   user has set. The launcher returns immediately — we don't
   block the rbterm UI on the editor's startup. */
static void logs_open_selected_in_editor(void) {
    if (g_logs_selected < 0 || g_logs_selected >= g_logs_count) return;
    const char *path = g_logs[g_logs_selected].path;
    char cmd[PATH_MAX + 32];
#if defined(__APPLE__)
    snprintf(cmd, sizeof(cmd), "open -t '%s'", path);
#elif defined(_WIN32)
    snprintf(cmd, sizeof(cmd), "start \"\" \"%s\"", path);
#else
    snprintf(cmd, sizeof(cmd), "xdg-open '%s' >/dev/null 2>&1 &", path);
#endif
    int rc = system(cmd);
    (void)rc;
    g_ui_mode = UI_NORMAL;
}

static void logs_handle_mouse(LogsLayout L, int cols, int rows) {
    Vector2 mp = GetMousePosition();
    int mx = (int)mp.x, my = (int)mp.y;
    /* Wheel scroll inside the list. */
    if (rect_hit(L.list, mx, my)) {
        float wheel = GetMouseWheelMove();
        if (wheel != 0.0f) {
            g_logs_scroll -= (int)(wheel * 3.0f);
            if (g_logs_scroll < 0) g_logs_scroll = 0;
        }
    }
    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) return;
    if (rect_hit(L.close_btn, mx, my)) { g_ui_mode = UI_NORMAL; return; }
    if (rect_hit(L.open_btn,  mx, my)) { logs_open_selected_in_editor(); return; }
    for (int i = 0; i < g_logs_count; i++) {
        if (L.row[i].w == 0) continue;
        if (rect_hit(L.row[i], mx, my)) {
            /* Single click selects, double-click opens. The
               static last-click tracker matches the path-field
               double-click pattern in the save modal. */
            static double last_t = -1.0;
            static int    last_idx = -1;
            double now = GetTime();
            if (last_idx == i && (now - last_t) < 0.45) {
                logs_open_selected_in_editor();
                last_idx = -1;
                last_t = -1.0;
            } else {
                g_logs_selected = i;
                last_idx = i;
                last_t = now;
            }
            return;
        }
    }
}

static void logs_handle_keys(int cols, int rows) {
    if (IsKeyPressed(KEY_ESCAPE)) { g_ui_mode = UI_NORMAL; return; }
    if (IsKeyPressed(KEY_UP) || IsKeyPressedRepeat(KEY_UP)) {
        if (g_logs_selected > 0) g_logs_selected--;
        return;
    }
    if (IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN)) {
        if (g_logs_selected < g_logs_count - 1) g_logs_selected++;
        return;
    }
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
        logs_open_selected_in_editor();
        return;
    }
}

static void draw_logs_modal(Renderer *r, int win_w, int win_h, LogsLayout L) {
    Font *f = (Font *)r->font_data;
    /* Backdrop dim. */
    DrawRectangle(0, 0, win_w, win_h, (Color){0, 0, 0, 160});
    DrawRectangle(L.modal.x, L.modal.y, L.modal.w, L.modal.h,
                  (Color){26, 30, 42, 250});
    DrawRectangleLines(L.modal.x, L.modal.y, L.modal.w, L.modal.h,
                       (Color){90, 100, 130, 220});
    /* Title bar. */
    DrawRectangle(L.modal.x, L.modal.y, L.modal.w, 38,
                  (Color){34, 38, 52, 255});
    DrawTextEx(*f, "Session logs",
               (Vector2){L.modal.x + 22, L.modal.y + 11},
               16, 0, (Color){230, 235, 248, 255});
    /* Subtitle: log_dir. */
    {
        char dir[PATH_MAX];
        expand_home_path(g_app_settings.log_dir, dir, sizeof(dir));
        char sub[PATH_MAX + 16];
        snprintf(sub, sizeof(sub), "from %s", dir);
        DrawTextEx(*f, sub,
                   (Vector2){L.modal.x + 22 + 130, L.modal.y + 14},
                   12, 0, (Color){140, 150, 170, 255});
    }
    /* List. */
    DrawRectangle(L.list.x, L.list.y, L.list.w, L.list.h,
                  (Color){18, 22, 32, 255});
    DrawRectangleLines(L.list.x, L.list.y, L.list.w, L.list.h,
                       (Color){60, 70, 90, 180});
    if (g_logs_count == 0) {
        const char *msg = g_logs_status[0]
            ? g_logs_status
            : "No .log files in this directory yet.";
        DrawTextEx(*f, msg,
                   (Vector2){L.list.x + 12, L.list.y + 10},
                   13, 0, (Color){170, 175, 195, 255});
    }
    BeginScissorMode(L.list.x, L.list.y, L.list.w, L.list.h);
    for (int i = 0; i < g_logs_count; i++) {
        if (L.row[i].w == 0) continue;
        Rect rr = L.row[i];
        bool sel = (g_logs_selected == i);
        if (sel) {
            DrawRectangle(rr.x, rr.y, rr.w, rr.h,
                          (Color){46, 92, 150, 255});
        }
        /* Format size: kB / MB. */
        char sz[24];
        if (g_logs[i].size < 1024)
            snprintf(sz, sizeof(sz), "%lld B", g_logs[i].size);
        else if (g_logs[i].size < 1024 * 1024)
            snprintf(sz, sizeof(sz), "%lld KB", g_logs[i].size / 1024);
        else
            snprintf(sz, sizeof(sz), "%.1f MB",
                     (double)g_logs[i].size / (1024.0 * 1024.0));
        /* Format mtime as YYYY-MM-DD HH:MM. */
        char ts[32];
        struct tm tm;
#ifdef _WIN32
        localtime_s(&tm, &g_logs[i].mtime);
#else
        localtime_r(&g_logs[i].mtime, &tm);
#endif
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M", &tm);
        Color text_main = sel ? (Color){240, 245, 255, 255}
                              : (Color){210, 215, 230, 255};
        Color text_dim  = sel ? (Color){200, 215, 235, 255}
                              : (Color){140, 150, 170, 255};
        /* Draw timestamp + size on the right, name on the left,
           with the name truncated if necessary. */
        char meta[64];
        snprintf(meta, sizeof(meta), "%s   %s", ts, sz);
        Vector2 msz = MeasureTextEx(*f, meta, 12, 0);
        int meta_x = rr.x + rr.w - (int)msz.x - 10;
        DrawTextEx(*f, meta,
                   (Vector2){meta_x, rr.y + (rr.h - msz.y) / 2 + 1},
                   12, 0, text_dim);
        char name_buf[256];
        snprintf(name_buf, sizeof(name_buf), "%s", g_logs[i].name);
        int name_max_w = meta_x - rr.x - 18;
        Vector2 nsz = MeasureTextEx(*f, name_buf, 13, 0);
        if ((int)nsz.x > name_max_w) {
            /* Mid-truncate. */
            int nlen = (int)strlen(name_buf);
            for (int k = nlen - 4; k > 8; k--) {
                name_buf[k] = '.'; name_buf[k - 1] = '.'; name_buf[k - 2] = '.';
                name_buf[k - 3] = 0;
                nsz = MeasureTextEx(*f, name_buf, 13, 0);
                if ((int)nsz.x <= name_max_w) break;
            }
        }
        DrawTextEx(*f, name_buf,
                   (Vector2){rr.x + 10, rr.y + (rr.h - 13) / 2 + 1},
                   13, 0, text_main);
    }
    EndScissorMode();
    /* Buttons. */
    bool has_sel = g_logs_selected >= 0 && g_logs_selected < g_logs_count;
    Color obg = has_sel ? (Color){48, 78, 58, 255} : (Color){36, 40, 54, 255};
    Color otext = has_sel ? (Color){220, 240, 225, 255} : (Color){130, 140, 160, 255};
    DrawRectangle(L.open_btn.x, L.open_btn.y, L.open_btn.w, L.open_btn.h, obg);
    DrawRectangleLines(L.open_btn.x, L.open_btn.y, L.open_btn.w, L.open_btn.h,
                       (Color){150, 160, 180, 200});
    {
        const char *lbl = "Open in editor";
        Vector2 ts = MeasureTextEx(*f, lbl, 14, 0);
        DrawTextEx(*f, lbl,
                   (Vector2){L.open_btn.x + (L.open_btn.w - ts.x) / 2,
                             L.open_btn.y + (L.open_btn.h - ts.y) / 2},
                   14, 0, otext);
    }
    DrawRectangle(L.close_btn.x, L.close_btn.y, L.close_btn.w, L.close_btn.h,
                  (Color){48, 52, 66, 255});
    DrawRectangleLines(L.close_btn.x, L.close_btn.y, L.close_btn.w, L.close_btn.h,
                       (Color){150, 160, 180, 200});
    {
        const char *lbl = "Close";
        Vector2 ts = MeasureTextEx(*f, lbl, 14, 0);
        DrawTextEx(*f, lbl,
                   (Vector2){L.close_btn.x + (L.close_btn.w - ts.x) / 2,
                             L.close_btn.y + (L.close_btn.h - ts.y) / 2},
                   14, 0, (Color){210, 215, 230, 255});
    }
}

int main(int argc, char **argv) {
    const char *font_path = NULL;
    int font_size = 20;
    bool font_size_explicit = false;     /* CLI override beats DPI scaling */
    int init_cols = 100, init_rows = 30;
    int init_padding = 6;
    float init_opacity = 1.0f;
    bool init_undecorated = false;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(); return 0; }
        else if (!strcmp(argv[i], "--font") && i + 1 < argc) font_path = argv[++i];
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) {
            font_size = atoi(argv[++i]);
            font_size_explicit = true;
        }
        else if (!strcmp(argv[i], "--cols") && i + 1 < argc) init_cols = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--rows") && i + 1 < argc) init_rows = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--padding") && i + 1 < argc) init_padding = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--opacity") && i + 1 < argc) init_opacity = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--undecorated")) init_undecorated = true;
        else { fprintf(stderr, "unknown arg: %s\n", argv[i]); usage(); return 2; }
    }
    /* font_size_explicit is only consumed in the Windows DPI
       block below; mark it used on the other platforms so the
       compiler doesn't warn. */
    (void)font_size_explicit;
#ifdef _WIN32
    /* Windows runs without FLAG_WINDOW_HIGHDPI (the layout bug
       in CLAUDE.md), so the framebuffer is in physical pixels.
       On a HiDPI display — Retina Macs running Parallels, 4K
       monitors at 150 % scale, etc. — a 20pt font renders at 20
       actual pixels and looks microscopic. Read the system DPI
       (96 = 100 %, 144 = 150 %, 192 = 200 %) and scale the
       default font size to match. CLI --size and the persisted
       config value still win — only the bare default scales. */
    if (!font_size_explicit) {
        UINT sys_dpi = GetDpiForSystem();
        if (sys_dpi >= 120) {
            font_size = (int)(((double)font_size * (double)sys_dpi) / 96.0 + 0.5);
            if (font_size > 96) font_size = 96;
        }
    }
#endif
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
#ifdef _WIN32
    /* Opt out of DPI awareness BEFORE GLFW init. raylib's bundled
       GLFW silently marks the process per-monitor DPI-aware on
       glfwInit; our UI is in raw pixel coords (24 px tab bar,
       30 px buttons, 20pt font), so on a HiDPI display every
       widget renders microscopic — see CLAUDE.md "HIGHDPI" note.
       Going UNAWARE lets Windows transparently upscale the
       framebuffer to physical pixels (slight bilinear softness,
       but every UI element is the right SIZE — which is the
       complaint that prompted this fix). The font_size DPI
       scaling above is now redundant on UNAWARE processes
       (GetDpiForSystem virtualises to 96) but harmless. */
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_UNAWARE);
#endif
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
    switch (g_app_settings.startup_window) {
    case STARTUP_WINDOW_MAXIMIZED:
        /* macOS: native fullscreen in its own Space. Three-finger
           swipe will jump between rbterm and other desktops. */
#ifdef __APPLE__
        mac_enter_native_fullscreen();
#else
        MaximizeWindow();
#endif
        break;
    case STARTUP_WINDOW_SMALL:
    case STARTUP_WINDOW_MEDIUM:
    case STARTUP_WINDOW_LARGE: {
        int sw_w =
            g_app_settings.startup_window == STARTUP_WINDOW_SMALL  ? 720  :
            g_app_settings.startup_window == STARTUP_WINDOW_MEDIUM ? 1024 :
                                                                    1280;
        int sw_h =
            g_app_settings.startup_window == STARTUP_WINDOW_SMALL  ? 480  :
            g_app_settings.startup_window == STARTUP_WINDOW_MEDIUM ? 720  :
                                                                    900;
        SetWindowSize(sw_w, sw_h);
        /* Centre on the current monitor so the user gets a clean
           fresh-window position, not whatever raylib decided. */
        int mi = GetCurrentMonitor();
        int mw = GetMonitorWidth(mi);
        int mh = GetMonitorHeight(mi);
        if (mw > sw_w && mh > sw_h) {
            SetWindowPosition((mw - sw_w) / 2, (mh - sw_h) / 2);
        }
        break;
    }
    case STARTUP_WINDOW_FILL:
        /* Native green-button "zoom" — on macOS GLFW dispatches to
           NSWindow's zoom: which fills the visible work area (below
           the menu bar, above the dock). On Linux/Windows
           glfwMaximizeWindow does the equivalent maximise. Manual
           SetWindowSize+SetWindowPosition is unreliable on macOS
           because raylib's monitor dimensions don't account for
           the menu bar / DPI scaling, leaving the bottom of the
           window clipped under the dock. */
        MaximizeWindow();
        break;
    case STARTUP_WINDOW_BORDERLESS:
        /* Borderless windowed-fullscreen on the current monitor.
           Stays in the user's current Space and (crucially) at
           the normal window level so Cmd+Tab still cycles to
           other apps. We don't use ToggleBorderlessWindowed —
           that path puts the NSWindow at NSMainMenuWindowLevel
           on macOS and blocks system window switching. Strip
           decorations + maximise instead; keeps normal level. */
        SetWindowState(FLAG_WINDOW_UNDECORATED);
        MaximizeWindow();
        break;
    default:
        break;
    }
    /* STARTUP_WINDOW_FULLSCREEN (the legacy enum value, mode=1) is
       accepted by config parsing for back-compat — config_load
       remaps it to BORDERLESS, so it never reaches this switch. */
#endif

#ifdef __APPLE__
    /* Strip Cmd+W from AppKit's File > Close Window menu item so our
       in-app handler can use the chord to close just the active tab. */
    mac_disable_close_menu_item();
    /* AppKit intercepts Ctrl+Tab before GLFW can see it. A local
       NSEvent monitor swallows the key and latches a flag we read
       each frame via mac_consume_ctrl_tab(). */
    mac_install_ctrl_tab_monitor();
    /* Quake-style global toggle is opt-in: we only install the
       NSEvent monitor when the user picked Fullscreen
       (STARTUP_WINDOW_BORDERLESS). In other modes Cmd+` would
       just be a confusing system-wide hotkey nobody asked for. */
    if (g_app_settings.startup_window == STARTUP_WINDOW_BORDERLESS) {
        mac_install_quake_hotkey();
    }
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
    /* Apply persisted ligature toggle. shape_available() is the
       compile-time HarfBuzz check — when shaping isn't built in,
       leaving the flag off keeps the existing 1:1 codepoint path. */
    if (g_app_settings.ligatures && shape_available()) {
        renderer_set_ligatures(&r, true);
    }

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

    /* Skip the auto-fit / centre block when the user explicitly
       set the window size via Settings → Window. MaximizeWindow /
       Own Space / Borderless want the OS-chosen geometry kept; the
       Small / Medium / Large presets want the pixel dimensions we
       set above kept. Without this gate the unconditional
       SetWindowSize(win_w, win_h) clobbers all of them — Medium /
       Large fell through to the renderer-derived default and
       looked identical at boot. */
    bool window_is_explicitly_sized =
        g_app_settings.startup_window == STARTUP_WINDOW_MAXIMIZED  ||
        g_app_settings.startup_window == STARTUP_WINDOW_FILL       ||
        g_app_settings.startup_window == STARTUP_WINDOW_BORDERLESS ||
        g_app_settings.startup_window == STARTUP_WINDOW_SMALL      ||
        g_app_settings.startup_window == STARTUP_WINDOW_MEDIUM     ||
        g_app_settings.startup_window == STARTUP_WINDOW_LARGE;
    if (!window_is_explicitly_sized) {
        SetWindowSize(win_w, win_h);
    }
    SetWindowMinSize(r.cell_w * 20 + 2 * r.pad_x, r.cell_h * 5 + TAB_BAR_H + 2 * r.pad_y);

    /* Windows + VMs can report wildly wrong monitor dimensions through
       GetMonitorWidth() (DPI scaling, virtual desktops, Parallels's
       extra chrome), so trying to centre the window ourselves
       reliably places it partly off-screen. Let the OS pick its own
       default position on Windows; on macOS / Linux the monitor
       metrics are trustworthy enough to use. */
#ifndef _WIN32
    if (!window_is_explicitly_sized && mw > 0 && mh > 0) {
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

    /* Open whatever the user has configured under Settings →
       Launch. If there are no launch entries, fall back to a single
       local shell. SSH entries that fail to connect are skipped
       with a stderr message; we don't want a typo in one entry to
       block the whole startup. */
    {
        bool any = false;
        if (g_app_settings.launch_count > 0) {
            ssh_profiles_load();
            sessions_load();
            /* Open locals up front (cheap — forkpty), queue SSH
               entries for the parallel-connect drain after the
               first frame, and instantiate session entries inline
               (they're a mix of local + ssh and the open-from-
               session path opens the locals fast and the ssh leaves
               sequentially — async per-leaf parallel connect inside
               a session is a known follow-up). */
            for (int i = 0; i < g_app_settings.launch_count; i++) {
                if (g_app_settings.launch[i].kind == LAUNCH_KIND_SSH) {
                    /* Kick the placeholder + async connect inline so
                       the tab lands at this iteration's position in
                       the bar. The earlier "queue + drain after
                       first frame" path put placeholders at the
                       end, which broke the user's chosen ordering
                       and made the launch_active radio appear to
                       focus the wrong tab. */
#ifdef RBTERM_SSH
                    const char *alias = g_app_settings.launch[i].host;
                    const SshProfile *prof = NULL;
                    for (int k = 0; k < g_ssh_profile_count; k++) {
                        if (strcmp(g_ssh_profiles[k].name, alias) == 0) {
                            prof = &g_ssh_profiles[k]; break;
                        }
                    }
                    bool is_active = (i == g_app_settings.launch_active);
                    if (ssh_launch_kick(prof, alias, NULL, is_active,
                                        init_cols, init_rows)) {
                        any = true;
                    } else {
                        fprintf(stderr,
                                "rbterm: launch ssh '%s' failed to kick\n",
                                alias);
                    }
#else
                    fprintf(stderr,
                            "rbterm: ssh disabled in this build, skipping '%s'\n",
                            g_app_settings.launch[i].host);
#endif
                } else if (g_app_settings.launch[i].kind == LAUNCH_KIND_SESSION) {
                    int sidx = sessions_find_by_name(g_app_settings.launch[i].host);
                    if (sidx >= 0) {
                        Tab *t = tab_open_from_session(&g_sessions[sidx],
                                                        init_cols, init_rows);
                        if (t) {
                            any = true;
                            if (i == g_app_settings.launch_active)
                                g_active = g_num_tabs - 1;
                        } else {
                            fprintf(stderr,
                                    "rbterm: launch session '%s' failed\n",
                                    g_app_settings.launch[i].host);
                        }
                    } else {
                        fprintf(stderr,
                                "rbterm: launch session '%s' not found\n",
                                g_app_settings.launch[i].host);
                    }
                } else {
                    bool was_active = (i == g_app_settings.launch_active);
                    if (tab_open(init_cols, init_rows)) {
                        any = true;
                        if (was_active) g_active = g_num_tabs - 1;
                    }
                }
            }
        }
        /* Always end up with at least one open tab so the main
           loop has something to render — if every launch entry
           is SSH (and they're all deferred to the post-frame
           drain), open a fallback default local now so the
           window isn't empty during the handshake wait. */
        if (g_num_tabs == 0) {
            if (!tab_open(init_cols, init_rows)) {
                renderer_shutdown(&r);
                CloseWindow();
                return 1;
            }
        }
        /* Each tab_open / tab_open_from_session / placeholder helper
           sets g_active to its own new tab, so by the time the loop
           finishes g_active points at whichever entry was last in
           the list — not the one the user marked as default. Now
           that placeholders open inline (tab indices match launch
           entry indices), the saved launch_active maps directly to
           the right tab. Snap it back into place. */
        if (g_app_settings.launch_active >= 0 &&
            g_app_settings.launch_active < g_num_tabs) {
            g_active = g_app_settings.launch_active;
        }
        (void)any;
    }

    SetTargetFPS(60);

    uint8_t readbuf[65536];
    uint8_t inputbuf[4096];
    bool was_focused = true;
    int  prev_mouse_btn = -1;
    int  prev_mouse_col = -1, prev_mouse_row = -1;

    /* Idle render-skipping. raylib has no damage model — every frame
       walks the full grid and re-batches glyphs, which costs a
       constant ~15% CPU on a 100x30 grid even when nothing changed.
       To get to alacritty/kitty-style idle (~0% CPU when nothing's
       moving), we track a "did anything happen this frame that
       requires a redraw?" flag and, when no, skip BeginDrawing
       entirely and just poll input + sleep. The cursor blink phase
       and per-second HUD samples are the only animations that mark
       dirty without an external event, so the loop wakes at most
       at the cursor-blink rate (~2Hz) while idle. */
    Vector2 prev_mp = {0, 0};
    bool prev_focused = true;
    /* Snapshot of cross-frame UI state so we mark dirty when a
       chord that changes UI without producing PTY output fires
       (Cmd+1..9 tab switch, Cmd+[/], Cmd+Shift+Arrow reorder, etc.).
       Without this, the chord runs, g_active updates, but
       BeginDrawing is skipped because nothing else dirtied — the
       screen looked frozen on the old tab until the next mouse
       move or shell write. */
    int prev_active = g_active;
    int prev_ui_mode = (int)g_ui_mode;
    /* Active-leaf snapshot — covers the per-tab focus changes
       (Cmd+K, Cmd+Opt+arrow, click-to-focus inside a split) that
       prev_active alone would miss. */
    PaneNode *prev_active_leaf = NULL;

    while (!WindowShouldClose() && g_num_tabs > 0) {
        bool dirty = false;
        bool focused = IsWindowFocused();
        if (focused != prev_focused) dirty = true;
        prev_focused = focused;
        int win_w_now = GetScreenWidth();
        int win_h_now = GetScreenHeight();

#ifdef RBTERM_SSH
        /* Parallel SSH launch. On the very first iteration we
           snapshot every pending entry into a worker struct and
           kick a thread per host that calls pty_open_ssh. Each
           subsequent iteration polls for completion and integrates
           any finished worker into a real Tab. The user sees the
           tabs pop in as their handshakes finish (in handshake-
           latency order, not queue order), and total wall time is
           max(per-host) instead of sum. */
        if (!g_launch_workers_kicked && g_launch_pending_count > 0) {
            g_launch_workers_kicked = true;
            int derive_cols, derive_rows;
            {
                int win_w_dr = GetScreenWidth();
                int win_h_dr = GetScreenHeight();
                derive_cols = (win_w_dr - 2 * r.pad_x) / r.cell_w;
                derive_rows = (win_h_dr - TAB_BAR_H - 2 * r.pad_y) / r.cell_h;
                if (derive_cols < 20) derive_cols = 20;
                if (derive_rows < 5)  derive_rows = 5;
            }
            for (int q = 0; q < g_launch_pending_count
                            && g_launch_workers_count < LAUNCH_WORKERS_MAX; q++) {
                const char *alias = g_launch_pending[q].host;
                if (!alias[0]) continue;
                const SshProfile *prof = NULL;
                for (int k = 0; k < g_ssh_profile_count; k++) {
                    if (strcmp(g_ssh_profiles[k].name, alias) == 0) {
                        prof = &g_ssh_profiles[k]; break;
                    }
                }
                SshLaunchWorker *w = &g_launch_workers[g_launch_workers_count++];
                memset(w, 0, sizeof(*w));
                strncpy(w->alias, alias, sizeof(w->alias) - 1);
                w->is_active = g_launch_pending[q].is_active;
                if (prof) {
                    w->prof = *prof;
                    w->prof_valid = true;
                    if (prof->user[0])     strncpy(w->user, prof->user, sizeof(w->user) - 1);
                    strncpy(w->host,
                            prof->hostname[0] ? prof->hostname : alias,
                            sizeof(w->host) - 1);
                    w->port = prof->port > 0 ? prof->port : 22;
                    if (prof->identity[0])
                        strncpy(w->keyfile, prof->identity, sizeof(w->keyfile) - 1);
                } else {
                    strncpy(w->host, alias, sizeof(w->host) - 1);
                    w->port = 22;
                }
                w->cols = derive_cols;
                w->rows = derive_rows;
                /* Create the placeholder tab right now so the user
                   sees a "Connecting to <alias>..." tab immediately
                   instead of staring at the local-only tab bar
                   while the handshake runs. */
                w->placeholder = tab_open_ssh_placeholder(prof, alias,
                                                          w->is_active,
                                                          derive_cols,
                                                          derive_rows);
                w->started = 1;
                pthread_create(&w->th, NULL, ssh_launch_worker_run, w);
            }
            g_launch_pending_pos = g_launch_pending_count;   /* legacy drain dormant */
        }

        /* Integrate completed workers. */
        for (int wi = 0; wi < g_launch_workers_count; wi++) {
            SshLaunchWorker *w = &g_launch_workers[wi];
            if (!w->started || w->integrated) continue;
            if (!__atomic_load_n(&w->done, __ATOMIC_ACQUIRE)) continue;
            pthread_join(w->th, NULL);
            w->integrated = 1;
            if (!w->pty) {
                fprintf(stderr,
                        "rbterm: launch ssh '%s' failed: %s\n",
                        w->alias, w->err[0] ? w->err : "(unknown)");
                /* Surface the failure on the placeholder so the user
                   sees what went wrong instead of an empty tab. */
                if (w->placeholder && w->placeholder->active &&
                    w->placeholder->active->pane) {
                    Pane *pp = w->placeholder->active->pane;
                    char msg[512];
                    int n = snprintf(msg, sizeof(msg),
                                     "\x1b[2J\x1b[H\x1b[1;31mFailed to connect to %s\x1b[0m\r\n"
                                     "\x1b[2;37m%s\x1b[0m\r\n",
                                     w->alias,
                                     w->err[0] ? w->err : "(no error message)");
                    if (n > 0 && n < (int)sizeof(msg)) {
                        screen_feed(pp->scr, (const uint8_t *)msg, (size_t)n);
                    }
                    snprintf(pp->title, sizeof(pp->title),
                             "Failed: %s", w->alias);
                    pp->title_dirty = true;
                }
                dirty = true;
                continue;
            }
            /* Placeholder may have been closed by the user before
               the worker finished — release the just-opened Pty so
               we don't leak the libssh session and the per-pty
               threads it owns. */
            if (!w->placeholder) {
                pty_close(w->pty);
                w->pty = NULL;
                continue;
            }
            const SshProfile *prof = w->prof_valid ? &w->prof : NULL;
            tab_attach_ssh_finalize(w->placeholder, w->pty, prof,
                                    w->cols, w->rows);
            dirty = true;
        }
        /* Background SSH ops (Test Auth / Key Install / Key Generate)
           — separate worker pool, separate visible status fields. */
        if (bg_ssh_integrate()) dirty = true;
#endif /* RBTERM_SSH */

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
            /* Collect dead leaves first; close after the iteration so
               we don't mutate the tree mid-walk. Tree depth is small
               in practice; 64 leaves is already an absurd split. */
            PaneNode *dead_leaves[64];
            int dead_count = 0;
            for (PaneNode *leaf = pane_tree_first_leaf(t->root); leaf;
                 leaf = pane_tree_next_leaf(leaf)) {
                Pane *p = leaf->pane;
                if (!p) continue;
                /* Skip placeholder panes (in-flight SSH connect). They
                   have a Screen but no Pty yet; pty_read on NULL would
                   return -1 and mark the leaf dead, which would close
                   the tab before the worker could attach the real Pty. */
                if (!p->pty) continue;
                size_t drained = 0;
                const size_t drain_cap = 16 * 1024 * 1024;
                bool dead = false;
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
                    if (n < 0) dead = true;
                    break;
                }
                if (drained > 0) {
                    pty_snap_cursor(p->pty,
                                    screen_cursor_y(p->scr),
                                    screen_cursor_x(p->scr));
                }
                if (!pty_alive(p->pty)) dead = true;
                if (drained > 0) dirty = true;
                if (drained > 0 && i != g_active) t->activity = true;
                if (dead && dead_count < 64) dead_leaves[dead_count++] = leaf;
            }
            for (int d = 0; d < dead_count; d++) {
                tab_close_leaf(t, dead_leaves[d]);
                if (t->dead) break;
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
            for (PaneNode *leaf = pane_tree_first_leaf(t->root); leaf;
                 leaf = pane_tree_next_leaf(leaf)) {
                Pane *p = leaf->pane;
                if (!p) continue;
                if (now - p->cwd_poll_at < 0.3) continue;
                p->cwd_poll_at = now;
                char buf[PATH_MAX];
                if (pty_cwd(p->pty, buf, sizeof(buf)) &&
                    strcmp(buf, p->cwd) != 0) {
                    strncpy(p->cwd, buf, sizeof(p->cwd) - 1);
                    p->cwd[sizeof(p->cwd) - 1] = 0;
                    if (i == g_active && leaf == t->active)
                        p->title_dirty = true;
                }
            }
        }

        /* HUD refresh — every 1 sec per pane. Local panes use cheap
           main-thread syscalls; SSH panes consume snapshots produced
           by their probe thread (pty_ssh.c). Both paths feed the
           same Pane->hud_* fields and the same delta-CPU + ring
           buffer logic. */
        for (int i = 0; i < g_num_tabs; i++) {
            Tab *t = g_tabs[i];
            for (PaneNode *leaf = pane_tree_first_leaf(t->root); leaf;
                 leaf = pane_tree_next_leaf(leaf)) {
                Pane *p = leaf->pane;
                if (!p) continue;
                if (now < p->hud_next_poll_at) continue;
                p->hud_next_poll_at = now + 1.0;

                unsigned long long busy = 0, total = 0;
                bool cpu_ok = false;
                if (pty_is_local(p->pty)) {
                    hud_local_poll(p->hud_hostname, sizeof(p->hud_hostname),
                                   p->hud_ip,       sizeof(p->hud_ip),
                                   &p->hud_load1,
                                   &p->hud_mem_free_mb,
                                   &p->hud_disk_free_pct);
                    cpu_ok = hud_read_cpu_ticks(&busy, &total);
                } else {
                    PtyHudSnapshot snap = {0};
                    if (pty_hud_snapshot(p->pty, &snap)) {
                        if (snap.hostname[0]) {
                            strncpy(p->hud_hostname, snap.hostname, sizeof(p->hud_hostname) - 1);
                            p->hud_hostname[sizeof(p->hud_hostname) - 1] = 0;
                        }
                        if (snap.ip[0]) {
                            strncpy(p->hud_ip, snap.ip, sizeof(p->hud_ip) - 1);
                            p->hud_ip[sizeof(p->hud_ip) - 1] = 0;
                        }
                        p->hud_load1         = snap.load1;
                        p->hud_mem_free_mb   = snap.mem_free_mb;
                        p->hud_disk_free_pct = snap.disk_free_pct;
                        if (snap.cpu_valid) {
                            busy = snap.cpu_busy;
                            total = snap.cpu_total;
                            cpu_ok = true;
                        }
                    }
                }

                /* CPU%: tick counters are cumulative; derive % busy
                   from the delta between consecutive samples. First
                   call just primes the previous-tick fields. Same
                   code path for local + SSH. */
                if (cpu_ok) {
                    if (!p->hud_cpu_inited) {
                        p->hud_cpu_inited = true;
                        for (int k = 0; k < HUD_CPU_HISTORY; k++)
                            p->hud_cpu_pct[k] = -1;
                    } else if (total > p->hud_cpu_prev_total) {
                        unsigned long long bd = busy  - p->hud_cpu_prev_busy;
                        unsigned long long td = total - p->hud_cpu_prev_total;
                        int pct = (td > 0) ? (int)((bd * 100ULL) / td) : 0;
                        if (pct < 0) pct = 0;
                        if (pct > 100) pct = 100;
                        p->hud_cpu_head = (p->hud_cpu_head + 1) % HUD_CPU_HISTORY;
                        p->hud_cpu_pct[p->hud_cpu_head] = pct;
                    }
                    p->hud_cpu_prev_busy  = busy;
                    p->hud_cpu_prev_total = total;
                }
                p->hud_updated_at = now;
            }
        }

        /* SFTP transfer sweep: covers both uploads and downloads.
           When the worker finishes (status != 0), record the time
           so the toast can fade; after 4 s release the handle. */
        for (int i = 0; i < g_num_tabs; i++) {
            Tab *t = g_tabs[i];
            for (PaneNode *leaf = pane_tree_first_leaf(t->root); leaf;
                 leaf = pane_tree_next_leaf(leaf)) {
                Pane *p = leaf->pane;
                if (!p) continue;
                if (p->upload) {
                    int st = pty_upload_status(p->upload, NULL, NULL, NULL, 0);
                    if (st != 0 && p->upload_done_at == 0.0) {
                        p->upload_done_at = now;
                    }
                    if (p->upload_done_at != 0.0 && now - p->upload_done_at > 4.0) {
                        pty_upload_release(p->upload);
                        p->upload = NULL;
                        p->upload_done_at = 0.0;
                    }
                }
                if (p->download) {
                    int st = pty_download_status(p->download, NULL, NULL, NULL, 0);
                    if (st != 0 && p->download_done_at == 0.0) {
                        p->download_done_at = now;
                    }
                    if (p->download_done_at != 0.0 && now - p->download_done_at > 4.0) {
                        pty_download_release(p->download);
                        p->download = NULL;
                        p->download_done_at = 0.0;
                    }
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
                if (getenv("RBTERM_DEBUG"))
                    fprintf(stderr, "[hit] mx=%d hit: tab=%d close=%d plus=%d ssh=%d gear=%d help=%d "
                                    "split_v=%d split_h=%d rec_start=%d rec_stop=%d upload=%d download=%d\n",
                            (int)mp.x, h.tab_idx, h.on_close, h.on_plus, h.on_ssh,
                            h.on_gear, h.on_help, h.on_split_v, h.on_split_h,
                            h.on_rec_start, h.on_rec_stop, h.on_upload, h.on_download);
                if (h.on_plus) {
                    /* Defer the actual action: short release fires
                       the legacy "open local tab" path; long hold
                       surfaces the kind-picker menu instead. The
                       press/release tracking happens further below
                       so the rest of this if-cascade still gets a
                       chance at other tab-bar buttons. Refresh the
                       sessions list now so the menu shows current
                       file contents when it pops. */
                    sessions_load();
                    g_plus_pressing = true;
                    g_plus_press_at = GetTime();
                    g_plus_menu_active = false;
                }
                else if (h.on_ssh) ssh_form_open();
                else if (h.on_gear) settings_open(&r);
                else if (h.on_help) {
                    g_ui_mode = UI_HELP;
                    g_help_just_opened = true;
                }
                else if (h.on_rec_start) {
                    if (!g_rec.active) {
                        Tab *_t = active_tab();
                        if (_t) rec_start(_t);
                    }
                }
                else if (h.on_rec_stop) {
                    rec_stop();
                }
                else if (h.on_snap) {
                    screenshot_active_pane(&r);
                }
                else if (h.on_upload) {
                    upload_form_open();
                }
                else if (h.on_download) {
                    if (getenv("RBTERM_DEBUG"))
                        fprintf(stderr, "[dl] tab-bar click: on_download fired, calling download_form_open()\n");
                    download_form_open();
                    if (getenv("RBTERM_DEBUG"))
                        fprintf(stderr, "[dl] after open: g_ui_mode=%d entry_count=%d status='%s' remote_dir='%s'\n",
                                (int)g_ui_mode,
                                g_download_form.entry_count,
                                g_download_form.status,
                                g_download_form.remote_dir);
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
                else if (h.on_bcast) {
                    g_broadcast_active = !g_broadcast_active;
                }
                else if (h.tab_idx >= 0) {
                    if (h.on_close) tab_close(h.tab_idx);
                    else {
                        /* Double-click on the tab body (not the close
                           'x') opens the rename overlay — same effect
                           as Cmd+R. ~0.45s gap mirrors the saved-host
                           list's double-click timing. */
                        static double last_tab_click_t   = -1.0;
                        static int    last_tab_click_idx = -1;
                        double now = GetTime();
                        bool is_dbl = (last_tab_click_idx == h.tab_idx) &&
                                      (last_tab_click_t > 0) &&
                                      (now - last_tab_click_t < 0.45);
                        last_tab_click_idx = h.tab_idx;
                        last_tab_click_t   = now;
                        g_active = h.tab_idx;
                        if (is_dbl) {
                            Tab *rt = g_tabs[h.tab_idx];
                            g_tab_rename_active = true;
                            g_tab_rename_idx    = h.tab_idx;
                            snprintf(g_tab_rename_buf,
                                     sizeof(g_tab_rename_buf),
                                     "%s", rt->tab_name);
                            g_tab_rename_caret =
                                (int)strlen(g_tab_rename_buf);
                            g_tab_press_idx = -1;
                            last_tab_click_idx = -1;   /* eat triple-click */
                        } else {
                            /* Arm a potential drag — the hold-handler
                               below promotes this to a real drag once
                               the cursor moves past the threshold. */
                            g_tab_press_idx = h.tab_idx;
                            g_tab_press_mx  = (int)mp.x;
                            g_tab_dragging  = false;
                        }
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
                    int tab_start = TAB_SSH_W + TAB_PLUS_W;
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
            /* Locate the dragging splitter, if any: either an in-flight
               drag (some node has splitter_drag set) or a fresh press
               on a splitter strip. */
            static PaneRect s_drag_outer;
            static PaneNode *s_drag_node;
            PaneNode *drag_node = NULL;
            PaneRect drag_outer = {0};
            /* If a previous frame set a node's splitter_drag, find it. */
            {
                PaneNode *stack[64]; int sp_top = 0;
                if (cur->root) stack[sp_top++] = cur->root;
                while (sp_top > 0) {
                    PaneNode *n = stack[--sp_top];
                    if (!n || n->split == SPLIT_NONE) continue;
                    if (n->splitter_drag) {
                        drag_node = n;
                        if (n == s_drag_node) drag_outer = s_drag_outer;
                        else {
                            /* Recompute outer if we don't have a stash. */
                            pane_tree_node_rect_walk(cur->root, n,
                                pane_tree_terminal_outer(win_w_now, win_h_now),
                                &drag_outer);
                        }
                        break;
                    }
                    if (sp_top + 1 < 64) stack[sp_top++] = n->child[0];
                    if (sp_top + 1 < 64) stack[sp_top++] = n->child[1];
                }
            }
            bool on_splitter = false;
            PaneNode *hit_node = NULL;
            PaneRect hit_outer = {0};
            if (!drag_node) {
                hit_node = pane_tree_splitter_at(cur->root,
                    pane_tree_terminal_outer(win_w_now, win_h_now),
                    (int)mp.x, (int)mp.y, SPLITTER_GRAB, &hit_outer);
                on_splitter = (hit_node != NULL);
            }
            if (on_splitter && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                hit_node->splitter_drag = true;
                drag_node = hit_node;
                drag_outer = hit_outer;
                s_drag_node = hit_node;
                s_drag_outer = hit_outer;
            }
            if (drag_node) {
                if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                    float nr;
                    if (drag_node->split == SPLIT_VERTICAL) {
                        int span = drag_outer.w - SPLITTER_PX;
                        nr = (span > 0)
                             ? (float)((int)mp.x - drag_outer.x) / (float)span
                             : 0.5f;
                    } else {
                        int span = drag_outer.h - SPLITTER_PX;
                        nr = (span > 0)
                             ? (float)((int)mp.y - drag_outer.y) / (float)span
                             : 0.5f;
                    }
                    if (nr < 0.15f) nr = 0.15f;
                    if (nr > 0.85f) nr = 0.85f;
                    drag_node->ratio = nr;
                } else {
                    drag_node->splitter_drag = false;
                    s_drag_node = NULL;
                }
            } else {
                /* Maximize / restore rollover button — checked
                   before click-to-focus so a click on the corner
                   button toggles maximize without ALSO mutating
                   focus or starting a selection drag. The
                   maximize_click_consumed flag suppresses every
                   downstream click handler in the per-pane block. */
                bool maximize_click_consumed = false;
                if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && cur && cur->root) {
                    int mxi = (int)mp.x, myi = (int)mp.y;
                    for (PaneNode *_l = pane_tree_first_leaf(cur->root); _l;
                         _l = pane_tree_next_leaf(_l)) {
                        Pane *_p = _l->pane;
                        if (!_p || _p->maximize_btn_w == 0) continue;
                        if (mxi >= _p->maximize_btn_x &&
                            mxi <  _p->maximize_btn_x + _p->maximize_btn_w &&
                            myi >= _p->maximize_btn_y &&
                            myi <  _p->maximize_btn_y + _p->maximize_btn_h) {
                            cur->active = _l;
                            tab_toggle_maximize(cur);
                            maximize_click_consumed = true;
                            break;
                        }
                    }
                }
                /* Click-to-focus: left-press inside a pane makes it the active one. */
                if (!maximize_click_consumed && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    PaneNode *hit = pane_at(cur, win_w_now, win_h_now,
                                            (int)mp.x, (int)mp.y);
                    if (hit) cur->active = hit;
                }
                Pane *p = active_pane_of(cur);
                if (p && p->scr) {
                    /* Translate window-pixel coords → active pane's cell coords. */
                    PaneRect pr;
                    if (!leaf_rect(cur, cur->active, win_w_now, win_h_now, &pr))
                        pr = pane_tree_terminal_outer(win_w_now, win_h_now);
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
                        if (!maximize_click_consumed &&
                            IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
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

        /* [+] long-press menu: click-to-open then click-to-pick.
           - Hold the [+] button past PLUS_HOLD_MS → menu pops up.
           - Release: if the menu has popped, it stays open. Quick
             release (before threshold) opens a local tab.
           - Hovering a primary row expands its submenu to the
             right; releasing on a primary or hovering+clicking
             keeps it open (so the user can pick).
           - Click on a row (after release) fires its action and
             dismisses. Click outside the menu dismisses without
             firing. */
        int menu_x = TAB_SSH_W;
        int menu_y = TAB_BAR_H + 2;
        int mxr = (int)mp.x, myr = (int)mp.y;

        /* Threshold-based menu activation while the initial press
           is still held. */
        if (g_plus_pressing &&
            !g_plus_menu_active &&
            (GetTime() - g_plus_press_at) * 1000.0 > PLUS_HOLD_MS) {
            g_plus_menu_active = true;
            ssh_profiles_load();
            sessions_load();
            dirty = true;
        }

        /* Hover-driven submenu expansion runs every frame while the
           menu is active (whether or not the button is held).

           Sticky behavior: once a submenu is open, it stays open
           until the user EITHER hovers a different primary that has
           its own submenu (we swap), OR explicitly hovers the
           Local-shell primary (no submenu — collapse is the right
           outcome there). Mouse drifting through the 4px gap
           between primary and submenu, or pausing on empty space
           after triggering a submenu, no longer collapses it.
           Outside-click still dismisses the entire menu. */
        if (g_plus_menu_active) {
            int hovered_primary = -1;
            for (int k = 0; k < 3; k++) {
                Rect ir = { menu_x, menu_y + k * PLUS_MENU_H,
                            PLUS_MENU_W, PLUS_MENU_H };
                if (rect_hit(ir, mxr, myr)) { hovered_primary = k; break; }
            }
            /* Track how long the cursor has been on the current
               primary so a brief crossing (e.g. diagonal path from
               SSH primary to an SSH submenu row past Session
               primary) doesn't flip the submenu mid-move. */
            double now_t = GetTime();
            if (hovered_primary != g_plus_hover_primary) {
                g_plus_hover_primary    = hovered_primary;
                g_plus_hover_started_at = now_t;
            }
            bool hover_settled =
                (hovered_primary >= 0) &&
                ((now_t - g_plus_hover_started_at) * 1000.0 >= PLUS_HOVER_DEBOUNCE_MS);
            /* Instant-switch on a freshly-opened menu (no submenu
               yet) so the first hover is responsive — debounce
               only kicks in once a submenu is already up. */
            if (g_plus_submenu < 0) hover_settled = (hovered_primary >= 0);
            if (hover_settled && (hovered_primary == PLUS_PRIMARY_SSH ||
                                  hovered_primary == PLUS_PRIMARY_SESSION)) {
                if (g_plus_submenu != hovered_primary) {
                    g_plus_submenu = hovered_primary;
                    dirty = true;
                }
            } else if (hover_settled && hovered_primary == PLUS_PRIMARY_LOCAL) {
                if (g_plus_submenu != -1) {
                    g_plus_submenu = -1;
                    dirty = true;
                }
            }
            /* No "else" branch — empty space and the gap leave the
               submenu state alone, so the user can drift between
               panels without it disappearing. */
        }

        /* Initial-press release. Quick releases (no menu yet) fire
           the legacy local-tab path; releases after the menu opened
           leave it on screen so the user can navigate without
           keeping the button held. */
        if (g_plus_pressing && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            if (g_plus_menu_active) {
                /* Menu stays up; nothing fires here. */
            } else {
                tab_open(content_cols, content_rows);
                g_plus_menu_active = false;
                g_plus_submenu = -1;
            }
            g_plus_pressing = false;
            dirty = true;
        }

        /* After the initial press is released, subsequent clicks
           inside the menu pick an item; clicks outside dismiss
           without firing. We guard on !g_plus_pressing so the
           initial press isn't double-handled (it's already
           consumed by the release branch above). */
        if (g_plus_menu_active && !g_plus_pressing &&
            IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            int hit_primary = -1;
            for (int k = 0; k < 3; k++) {
                Rect ir = { menu_x, menu_y + k * PLUS_MENU_H,
                            PLUS_MENU_W, PLUS_MENU_H };
                if (rect_hit(ir, mxr, myr)) { hit_primary = k; break; }
            }
            int hit_sub = -1;
            if (g_plus_submenu >= 0) {
                int sub_count = (g_plus_submenu == PLUS_PRIMARY_SSH)
                                   ? g_ssh_profile_count
                                   : g_sessions_count;
                int sub_y = menu_y + g_plus_submenu * PLUS_MENU_H;
                for (int k = 0; k < sub_count; k++) {
                    Rect ir = { menu_x + PLUS_MENU_W + 4,
                                sub_y + k * PLUS_MENU_H,
                                PLUS_SUBMENU_W, PLUS_MENU_H };
                    if (rect_hit(ir, mxr, myr)) { hit_sub = k; break; }
                }
            }
            bool fired = false;
            if (hit_sub >= 0 && g_plus_submenu == PLUS_PRIMARY_SSH) {
                const SshProfile *prof = &g_ssh_profiles[hit_sub];
#ifdef RBTERM_SSH
                /* Async kick — placeholder tab opens immediately,
                   libssh handshakes off the main thread. An
                   unreachable host will surface as a red banner
                   inside the placeholder when the worker times
                   out, instead of beachballing the UI. */
                if (!ssh_launch_kick(prof, prof->name, NULL,
                                     /* is_active */ true,
                                     content_cols, content_rows)) {
                    fprintf(stderr, "rbterm: ssh launch slots exhausted\n");
                }
#else
                char err[256] = {0};
                Tab *t = tab_open_ssh(
                    prof->user[0]     ? prof->user     : NULL,
                    prof->hostname[0] ? prof->hostname : prof->name,
                    prof->port,
                    NULL,
                    prof->identity[0] ? prof->identity : NULL,
                    prof->theme[0]    ? prof->theme    : NULL,
                    prof->cursor_style,
                    prof->font[0]     ? prof->font     : NULL,
                    prof->font_size,
                    prof->log_dir[0]  ? prof->log_dir  : NULL,
                    prof->log_mode,
                    prof->color[0]        ? prof->color        : NULL,
                    prof->cursor_color[0] ? prof->cursor_color : NULL,
                    prof->hud.override    ? &prof->hud         : NULL,
                    prof->effects_override ? &prof->effects    : NULL,
                    prof->init_cwd[0]     ? prof->init_cwd     : NULL,
                    prof->init_cmd[0]     ? prof->init_cmd     : NULL,
                    prof->layout[0]       ? prof->layout       : NULL,
                    prof->layout[0]       ? prof->pane_cwds    : NULL,
                    prof->layout[0]       ? prof->pane_cmds    : NULL,
                    content_cols, content_rows,
                    err, sizeof(err));
                if (t) {
                    strncpy(t->ssh_alias, prof->name,
                            sizeof(t->ssh_alias) - 1);
                    t->ssh_alias[sizeof(t->ssh_alias) - 1] = 0;
                    if (prof->display_name[0]) {
                        strncpy(t->tab_name, prof->display_name,
                                sizeof(t->tab_name) - 1);
                        t->tab_name[sizeof(t->tab_name) - 1] = 0;
                    }
                } else if (err[0]) {
                    fprintf(stderr,
                            "rbterm: ssh '%s' failed: %s\n",
                            prof->name, err);
                }
#endif
                fired = true;
            } else if (hit_sub >= 0 &&
                       g_plus_submenu == PLUS_PRIMARY_SESSION) {
                tab_open_from_session(&g_sessions[hit_sub],
                                      content_cols, content_rows);
                fired = true;
            } else if (hit_primary == PLUS_PRIMARY_LOCAL) {
                tab_open(content_cols, content_rows);
                fired = true;
            } else if (hit_primary == PLUS_PRIMARY_SSH) {
                ssh_form_open();
                fired = true;
            } else if (hit_primary == PLUS_PRIMARY_SESSION) {
                settings_open(&r);
                g_settings_tab = SETTINGS_TAB_SESSIONS;
                fired = true;
            }
            /* Always dismiss after a click — the user picked or
               aimed elsewhere; either way the menu's task is done. */
            g_plus_menu_active = false;
            g_plus_submenu = -1;
            dirty = true;
            (void)fired;
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

        /* SFTP download modal. */
        if (g_ui_mode == UI_SFTP_DOWNLOAD) {
            static int s_dl_modal_first = 1;
            if (s_dl_modal_first && getenv("RBTERM_DEBUG")) {
                fprintf(stderr, "[dl] entering UI_SFTP_DOWNLOAD render branch\n");
                s_dl_modal_first = 0;
            }
            if (g_ui_mode != UI_SFTP_DOWNLOAD) s_dl_modal_first = 1;
            DownloadFormLayout DL = download_form_layout(win_w_now, win_h_now);
            download_form_handle_mouse(DL);
            download_form_handle_keys();
            cur = active_tab();
            if (!cur) break;
            BeginDrawing();
            ClearBackground((Color){0, 0, 0, 255});
            draw_tab_bar(&r, win_w_now);
            draw_tab_contents(&r, cur, win_w_now, win_h_now, GetTime(), IsWindowFocused());
            if (g_ui_mode == UI_SFTP_DOWNLOAD) {
                draw_download_form(&r, win_w_now, win_h_now, DL);
            }
            EndDrawing();
            continue;
        }

        /* SFTP upload modal. */
        if (g_ui_mode == UI_SFTP_UPLOAD) {
            UploadFormLayout UL = upload_form_layout(win_w_now, win_h_now);
            upload_form_handle_mouse(UL);
            upload_form_handle_keys();
            cur = active_tab();
            if (!cur) break;
            BeginDrawing();
            ClearBackground((Color){0, 0, 0, 255});
            draw_tab_bar(&r, win_w_now);
            draw_tab_contents(&r, cur, win_w_now, win_h_now, GetTime(), IsWindowFocused());
            if (g_ui_mode == UI_SFTP_UPLOAD) {
                draw_upload_form(&r, win_w_now, win_h_now, UL);
            }
            EndDrawing();
            continue;
        }

        /* Session-designer modal — visual builder for multi-host
           split tabs. Saves to ~/.config/rbterm/sessions.ini and
           optionally opens the new session as a Tab. */
        if (g_ui_mode == UI_SESSION_DESIGNER) {
            SessionDesignerLayout SL = session_designer_layout(win_w_now, win_h_now);
            session_designer_handle_mouse(SL, content_cols, content_rows);
            session_designer_handle_keys(content_cols, content_rows);
            cur = active_tab();
            BeginDrawing();
            ClearBackground((Color){0, 0, 0, 255});
            draw_tab_bar(&r, win_w_now);
            if (cur) draw_tab_contents(&r, cur, win_w_now, win_h_now,
                                       GetTime(), IsWindowFocused());
            if (g_ui_mode == UI_SESSION_DESIGNER) {
                /* Re-layout in case the window was resized. */
                SL = session_designer_layout(win_w_now, win_h_now);
                draw_session_designer(&r, win_w_now, win_h_now, SL);
            }
            EndDrawing();
            continue;
        }

        /* Logs browser modal. */
        if (g_ui_mode == UI_LOGS) {
            LogsLayout LL = logs_layout_compute(win_w_now, win_h_now);
            int derive_cols = (win_w_now - 2 * r.pad_x) / r.cell_w;
            int derive_rows = (win_h_now - TAB_BAR_H - 2 * r.pad_y) / r.cell_h;
            if (derive_cols < 20) derive_cols = 20;
            if (derive_rows < 5)  derive_rows = 5;
            logs_handle_mouse(LL, derive_cols, derive_rows);
            logs_handle_keys(derive_cols, derive_rows);
            cur = active_tab();
            BeginDrawing();
            ClearBackground((Color){0, 0, 0, 255});
            draw_tab_bar(&r, win_w_now);
            if (cur) draw_tab_contents(&r, cur, win_w_now, win_h_now,
                                       GetTime(), IsWindowFocused());
            if (g_ui_mode == UI_LOGS) {
                LL = logs_layout_compute(win_w_now, win_h_now);
                draw_logs_modal(&r, win_w_now, win_h_now, LL);
            }
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
                /* Tab + sub-tab switch — uses rects populated by
                   draw_help_modal on the previous frame. Sub-tabs
                   only render on the Navigation tab so their
                   rects are zero outside that. */
                int mxp = (int)mp_help.x, myp = (int)mp_help.y;
                bool consumed = false;
                for (int _i = 0; _i < HELP_NAV_SUBTAB_COUNT; _i++) {
                    Rect tr = g_help_nav_subtab_rects[_i];
                    if (tr.w > 0 && mxp >= tr.x && mxp < tr.x + tr.w &&
                        myp >= tr.y && myp < tr.y + tr.h) {
                        g_help_nav_subtab = _i;
                        consumed = true;
                        break;
                    }
                }
                if (!consumed) {
                    for (int _i = 0; _i < HELP_EDIT_SUBTAB_COUNT; _i++) {
                        Rect tr = g_help_edit_subtab_rects[_i];
                        if (tr.w > 0 && mxp >= tr.x && mxp < tr.x + tr.w &&
                            myp >= tr.y && myp < tr.y + tr.h) {
                            g_help_edit_subtab = _i;
                            consumed = true;
                            break;
                        }
                    }
                }
                if (!consumed) {
                    for (int _i = 0; _i < HELP_SHELL_SUBTAB_COUNT; _i++) {
                        Rect tr = g_help_shell_subtab_rects[_i];
                        if (tr.w > 0 && mxp >= tr.x && mxp < tr.x + tr.w &&
                            myp >= tr.y && myp < tr.y + tr.h) {
                            g_help_shell_subtab = _i;
                            consumed = true;
                            break;
                        }
                    }
                }
                if (!consumed) {
                    for (int _i = 0; _i < HELP_TAB_COUNT; _i++) {
                        Rect tr = g_help_tab_rects[_i];
                        if (tr.w > 0 && mxp >= tr.x && mxp < tr.x + tr.w &&
                            myp >= tr.y && myp < tr.y + tr.h) {
                            g_help_tab = _i;
                            break;
                        }
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
            /* Cmd+Opt+arrows: directional pane focus. Picks the leaf
               whose centre lies closest in the chord direction (with
               vertical/horizontal overlap as a tiebreak). Match the
               iTerm2 convention so muscle memory carries over. Has
               to come before the OSC 133 Cmd+Up / Cmd+Down branch
               so the Alt-modified version doesn't fall through to
               prompt navigation. */
            else if ((IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT)) &&
                     (IsKeyPressed(KEY_LEFT)  || IsKeyPressed(KEY_RIGHT) ||
                      IsKeyPressed(KEY_UP)    || IsKeyPressed(KEY_DOWN))) {
                int dx = 0, dy = 0;
                if      (IsKeyPressed(KEY_LEFT))  dx = -1;
                else if (IsKeyPressed(KEY_RIGHT)) dx = +1;
                else if (IsKeyPressed(KEY_UP))    dy = -1;
                else if (IsKeyPressed(KEY_DOWN))  dy = +1;
                Tab *_ct = active_tab();
                if (_ct && _ct->root) {
                    PaneNode *target = pane_focus_directional(
                        _ct, win_w_now, win_h_now, dx, dy);
                    if (target && target != _ct->active) {
                        _ct->active = target;
                        /* Force a redraw — the dirty-flag snapshot
                           in the main loop watches g_active and
                           g_ui_mode, but not the per-tab active
                           leaf. Without this the chord lands but
                           the screen doesn't refresh until the
                           next mouse move or PTY byte, which feels
                           like the chord is "very slow". */
                        dirty = true;
                    }
                }
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
            else if (shift_held && IsKeyPressed(KEY_S)) {
                /* Cmd+Shift+S — capture the active pane to PNG and
                   open it in the system image viewer. Same code path
                   the camera button in the tab bar uses. */
                screenshot_active_pane(&r);
            }
            else if (shift_held && IsKeyPressed(KEY_I)) {
                /* Cmd+Shift+I — toggle broadcast input. Only useful
                   on a multi-pane tab; we still let the user toggle
                   when single-pane (it just won't fan out). */
                g_broadcast_active = !g_broadcast_active;
            }
            else if (shift_held && IsKeyPressed(KEY_P)) {
                /* Cmd+Shift+P — open the Session Designer. Lazily
                   loads any saved sessions on first use so the host
                   dropdown reflects what's on disk. */
                if (g_sessions_count == 0) sessions_load();
                session_designer_open_for(-1);
            }
            else if (shift_held && IsKeyPressed(KEY_O)) {
                /* Cmd+Shift+O — browse session logs. Lists log
                   files from the configured log_dir, opens any of
                   them in a new local pane via `less -R`. */
                logs_open();
            }
            else if (IsKeyPressed(KEY_R)) {
                /* Cmd+R — rename the active tab. Pre-fill the buffer
                   with whatever name is set so the user can edit
                   instead of starting from scratch. */
                if (cur && g_active >= 0 && g_active < g_num_tabs) {
                    g_tab_rename_active = true;
                    g_tab_rename_idx    = g_active;
                    snprintf(g_tab_rename_buf, sizeof(g_tab_rename_buf),
                             "%s", cur->tab_name);
                    g_tab_rename_caret = (int)strlen(g_tab_rename_buf);
                }
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
            /* Cmd+K cycles focus to the next leaf in the tab's pane
               tree (Cmd+Shift+K goes the other way). Wraps around. */
            else if (IsKeyPressed(KEY_K)) {
                if (cur && cur->root) {
                    cur->active = pane_tree_cycle_leaf(cur->root, cur->active,
                                                       shift_held ? -1 : +1);
                }
            }
            /* Cmd+Shift+M — toggle maximize on the active pane.
               Cmd+Z does the same on macOS (matches tmux's
               "<prefix> z" zoom). Linux / Windows don't get the
               Cmd+Z binding because their UI modifier is Ctrl
               and the input layer also emits 0x1A (SIGTSTP) for
               Ctrl+Z, which would suspend the foreground shell
               every time the user wanted to zoom a pane. */
            else if (shift_held && IsKeyPressed(KEY_M)) {
                tab_toggle_maximize(cur);
            }
#ifdef __APPLE__
            else if (!shift_held && IsKeyPressed(KEY_Z)) {
                tab_toggle_maximize(cur);
            }
#endif
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
        /* Quake-style hide/show fires inline from inside the
           NSEvent monitor (raylib pauses our loop while the
           window is hidden, so a polled flag wouldn't get
           drained). We still consume the flag here so any
           leftover state stays consistent — the toggle has
           already happened by the time we get here. */
        (void)mac_consume_quake_toggle();
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
        if (g_tab_rename_active) {
            /* Rename overlay owns the keyboard — drain chars +
               backspace + Enter/Esc here so they don't reach the
               shell or fire chords. */
            if (IsKeyPressed(KEY_ESCAPE)) {
                g_tab_rename_active = false;
                g_tab_rename_idx = -1;
                dirty = true;
            } else if (IsKeyPressed(KEY_ENTER) ||
                       IsKeyPressed(KEY_KP_ENTER)) {
                if (g_tab_rename_idx >= 0 && g_tab_rename_idx < g_num_tabs) {
                    Tab *rt = g_tabs[g_tab_rename_idx];
                    snprintf(rt->tab_name, sizeof(rt->tab_name), "%s",
                             g_tab_rename_buf);
                    /* Force a window-title repaint via the active
                       pane's title_dirty flag — easiest way to push
                       the new label into the macOS title bar. */
                    Pane *rp = rt->active ? rt->active->pane : NULL;
                    if (rp) rp->title_dirty = true;
                }
                g_tab_rename_active = false;
                g_tab_rename_idx = -1;
                dirty = true;
            } else if (IsKeyPressed(KEY_BACKSPACE) ||
                       IsKeyPressedRepeat(KEY_BACKSPACE)) {
                if (g_tab_rename_caret > 0) {
                    g_tab_rename_caret--;
                    g_tab_rename_buf[g_tab_rename_caret] = 0;
                    dirty = true;
                }
            } else {
                int ch;
                while ((ch = GetCharPressed()) != 0) {
                    if (ch < 32 || ch >= 127) continue;
                    if (g_tab_rename_caret + 1 >= (int)sizeof(g_tab_rename_buf)) break;
                    g_tab_rename_buf[g_tab_rename_caret++] = (char)ch;
                    g_tab_rename_buf[g_tab_rename_caret]   = 0;
                    dirty = true;
                }
            }
        } else if (ap && ap->search.active) {
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
                /* Broadcast: fan to every leaf of the active tab.
                   Each leaf decides on its own whether bracketed-
                   paste is in effect (a server-side mode flag), so
                   we ask each Screen rather than reusing ap->scr's
                   answer. */
                if (g_broadcast_active && cur && pane_tree_count(cur->root) >= 2) {
                    for (PaneNode *_l = pane_tree_first_leaf(cur->root); _l;
                         _l = pane_tree_next_leaf(_l)) {
                        Pane *_p = _l->pane;
                        if (!_p || !_p->pty) continue;
                        if (_p->scr && screen_bracketed_paste(_p->scr)) {
                            pty_write(_p->pty, (const uint8_t *)"\x1b[200~", 6);
                            pty_write(_p->pty, (const uint8_t *)t, strlen(t));
                            pty_write(_p->pty, (const uint8_t *)"\x1b[201~", 6);
                        } else {
                            pty_write(_p->pty, (const uint8_t *)t, strlen(t));
                        }
                        rec_input(_p, (const uint8_t *)t, strlen(t));
                    }
                } else if (screen_bracketed_paste(ap->scr)) {
                    pty_write(ap->pty, (const uint8_t *)"\x1b[200~", 6);
                    pty_write(ap->pty, (const uint8_t *)t, strlen(t));
                    pty_write(ap->pty, (const uint8_t *)"\x1b[201~", 6);
                    rec_input(ap, (const uint8_t *)t, strlen(t));
                } else {
                    pty_write(ap->pty, (const uint8_t *)t, strlen(t));
                    rec_input(ap, (const uint8_t *)t, strlen(t));
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
        if (in_n > 0) dirty = true;
        if (acts.font_delta != 100) dirty = true;
        if (ap && in_n > 0) {
            if (g_broadcast_active && cur && pane_tree_count(cur->root) >= 2) {
                /* Broadcast: write the same byte stream to every
                   leaf and reset their scrollback so each pane
                   jumps to the live edge. Selection clearing is
                   per-leaf so a stale highlight doesn't linger
                   anywhere in the group. */
                for (PaneNode *_l = pane_tree_first_leaf(cur->root); _l;
                     _l = pane_tree_next_leaf(_l)) {
                    Pane *_p = _l->pane;
                    if (!_p || !_p->pty) continue;
                    if (_p->scr) screen_scroll_reset(_p->scr);
                    pty_write(_p->pty, inputbuf, in_n);
                    rec_input(_p, inputbuf, in_n);
                    if (_p->sel.active && !_p->sel.dragging) _p->sel.active = false;
                }
            } else {
                screen_scroll_reset(ap->scr);
                pty_write(ap->pty, inputbuf, in_n);
                rec_input(ap, inputbuf, in_n);
                if (ap->sel.active && !ap->sel.dragging) ap->sel.active = false;
            }
        }

        if (ap && ap->title_dirty) {
            SetWindowTitle(ap->title[0] ? ap->title : tab_label(cur));
            ap->title_dirty = false;
            dirty = true;
        }

        /* Wake-on-event sentinel checks. Previous-frame state vs
           current — any change marks dirty so we redraw. We avoid
           GetKeyPressed/GetCharPressed here because those POP the
           event queue; the input section above already saw them
           via input_poll and set dirty when in_n > 0.
           Mouse + button checks are gated behind `focused` because
           macOS keeps reporting cursor positions to background
           windows, and IsMouseButtonReleased / IsKeyPressed all
           reflect events delivered to the focused app. Unfocused
           rbterm shouldn't burn CPU re-rendering on cursor twitches
           that aren't even visible to it. */
        if (focused) {
            Vector2 cur_mp = GetMousePosition();
            if (cur_mp.x != prev_mp.x || cur_mp.y != prev_mp.y) dirty = true;
            prev_mp = cur_mp;
            if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)
                || IsMouseButtonDown(MOUSE_BUTTON_RIGHT)
                || IsMouseButtonReleased(MOUSE_BUTTON_LEFT)
                || IsMouseButtonReleased(MOUSE_BUTTON_RIGHT)
                || GetMouseWheelMoveV().y != 0) {
                dirty = true;
            }
        }
        if (IsWindowResized()) dirty = true;
        /* UI-state changes that aren't tied to PTY output need to
           force a redraw on their own. Cmd+1..9 / Cmd+[/], etc.
           switch tabs without producing any shell output, so
           without this the new tab wouldn't paint until the next
           unrelated dirty trigger (mouse move, etc.) — felt like
           "extreme sluggishness" on tab switches. */
        if (g_active != prev_active) {
            dirty = true;
            /* Safety: stepping into a different tab disarms broadcast
               so a stray Cmd+T or [+] doesn't sneak typing into a
               local shell that wasn't part of the original group. */
            g_broadcast_active = false;
        }
        if ((int)g_ui_mode != prev_ui_mode) dirty = true;
        {
            Tab *_pt = active_tab();
            PaneNode *_pl = _pt ? _pt->active : NULL;
            if (_pl != prev_active_leaf) dirty = true;
            prev_active_leaf = _pl;
        }
        prev_active = g_active;
        prev_ui_mode = (int)g_ui_mode;
        /* Cursor-blink driven dirty is intentionally NOT a trigger:
           two redraws/sec just to flip cursor colour was the single
           biggest contributor to idle CPU. The cursor stops blinking
           while idle — input or shell output triggers a render and
           lands the cursor at the current blink phase. Most
           terminals do the same as a power-save gesture. */

        if (dirty) {
            BeginDrawing();
            ClearBackground((Color){0, 0, 0, 255});
            draw_tab_bar(&r, win_w_now);
            draw_tab_contents(&r, cur, win_w_now, win_h_now, GetTime(), IsWindowFocused());
            draw_tab_bar_tooltip(&r, win_w_now);
            /* [+] long-press menu — three primary items + an
               optional submenu when SSH or Session is hovered. */
            if (g_plus_menu_active) {
                Font *_f = (Font *)r.font_data;
                Vector2 _mp = GetMousePosition();
                int _mx = (int)_mp.x, _my = (int)_mp.y;
                int menu_x = TAB_SSH_W;
                int menu_y = TAB_BAR_H + 2;
                Color local_accent = (Color){90, 100, 120, 255};
                Color ssh_accent   = (Color){46, 92, 150, 255};
                Color sess_accent  = (Color){82, 50, 122, 255};
                /* Primary backdrop. */
                DrawRectangle(menu_x - 1, menu_y - 1,
                              PLUS_MENU_W + 2,
                              3 * PLUS_MENU_H + 2,
                              (Color){8, 10, 16, 240});
                struct { const char *label; Color accent; bool has_sub; }
                primaries[3] = {
                    { "Local shell", local_accent, false },
                    { "SSH host…",   ssh_accent,   true  },
                    { "Session…",    sess_accent,  true  },
                };
                for (int k = 0; k < 3; k++) {
                    Rect ir = { menu_x, menu_y + k * PLUS_MENU_H,
                                PLUS_MENU_W, PLUS_MENU_H };
                    bool hover = rect_hit(ir, _mx, _my);
                    bool sub_open = (g_plus_submenu == k);
                    Color bg = (hover || sub_open) ? primaries[k].accent
                                                   : (Color){32, 36, 50, 255};
                    DrawRectangle(ir.x, ir.y, ir.w, ir.h, bg);
                    DrawRectangleLines(ir.x, ir.y, ir.w, ir.h,
                                       (Color){125, 207, 255, 200});
                    DrawRectangle(ir.x + 6, ir.y + 8, 6, ir.h - 16,
                                  primaries[k].accent);
                    DrawTextEx(*_f, primaries[k].label,
                               (Vector2){ir.x + 22, ir.y + 7},
                               14, 0, (Color){230, 235, 248, 255});
                    if (primaries[k].has_sub) {
                        const char *chev = ">";
                        Vector2 csz = MeasureTextEx(*_f, chev, 14, 0);
                        DrawTextEx(*_f, chev,
                                   (Vector2){ir.x + ir.w - csz.x - 10,
                                             ir.y + 7},
                                   14, 0, (Color){200, 215, 240, 220});
                    }
                }
                /* Submenu — saved hosts / saved sessions. */
                if (g_plus_submenu == PLUS_PRIMARY_SSH ||
                    g_plus_submenu == PLUS_PRIMARY_SESSION) {
                    bool is_ssh = (g_plus_submenu == PLUS_PRIMARY_SSH);
                    int n = is_ssh ? g_ssh_profile_count : g_sessions_count;
                    Color accent = is_ssh ? ssh_accent : sess_accent;
                    int sub_x = menu_x + PLUS_MENU_W + 4;
                    int sub_y = menu_y + g_plus_submenu * PLUS_MENU_H;
                    int rows = (n > 0) ? n : 1;
                    int sub_h = rows * PLUS_MENU_H;
                    DrawRectangle(sub_x - 1, sub_y - 1,
                                  PLUS_SUBMENU_W + 2, sub_h + 2,
                                  (Color){8, 10, 16, 240});
                    if (n == 0) {
                        Rect ir = { sub_x, sub_y, PLUS_SUBMENU_W, PLUS_MENU_H };
                        DrawRectangle(ir.x, ir.y, ir.w, ir.h,
                                      (Color){32, 36, 50, 255});
                        DrawRectangleLines(ir.x, ir.y, ir.w, ir.h,
                                           (Color){125, 207, 255, 200});
                        const char *empty = is_ssh
                            ? "(no saved hosts in ~/.ssh/config)"
                            : "(no saved sessions yet)";
                        DrawTextEx(*_f, empty,
                                   (Vector2){ir.x + 12, ir.y + 7},
                                   13, 0, (Color){140, 145, 160, 255});
                    }
                    for (int k = 0; k < n; k++) {
                        Rect ir = { sub_x, sub_y + k * PLUS_MENU_H,
                                    PLUS_SUBMENU_W, PLUS_MENU_H };
                        bool hover = rect_hit(ir, _mx, _my);
                        Color bg = hover ? accent
                                         : (Color){32, 36, 50, 255};
                        DrawRectangle(ir.x, ir.y, ir.w, ir.h, bg);
                        DrawRectangleLines(ir.x, ir.y, ir.w, ir.h,
                                           (Color){125, 207, 255, 200});
                        DrawRectangle(ir.x + 6, ir.y + 8, 6, ir.h - 16,
                                      accent);
                        const char *name = is_ssh
                            ? g_ssh_profiles[k].name
                            : g_sessions[k].name;
                        DrawTextEx(*_f, name,
                                   (Vector2){ir.x + 22, ir.y + 7},
                                   14, 0, (Color){230, 235, 248, 255});
                    }
                }
            }
            EndDrawing();
        } else {
            /* Idle path: pump events + sleep. PollInputEvents on
               macOS is heavy (dispatches the NSApp queue) and we
               can't kernel-block on events without rebuilding
               raylib with USE_EXTERNAL_GLFW — see "Idle CPU floor"
               in CLAUDE.md.

               cap = worst-case input latency. Keep it tight:
               typing AND chord shortcuts (Cmd+T, Cmd+1) need to
               feel instant or the editor feels broken. 0.008 =
               ~125 Hz wake, ~8 ms worst-case typing latency.
               Unfocused windows still need fast wake too —
               otherwise Cmd+Tab takes the full unfocused-cap to
               surface the app, which feels like a stall. Use 0.05
               unfocused: ~50 ms to come back to front, which feels
               immediate. CPU when unfocused stays low because we
               immediately go focused → typing path. */
            PollInputEvents();
            double cap = focused ? 0.004 : 0.05;
            WaitTime(cap);
        }
    }

    for (int i = g_num_tabs - 1; i >= 0; i--) tab_close(i);
    rec_effects_shutdown();
    renderer_shutdown(&r);
    CloseWindow();
    return 0;
}
