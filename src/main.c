#include "raylib.h"
#include "screen.h"
#include "render.h"
#include "input.h"
#include "pty.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <signal.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

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
} Tab;

#define MAX_TABS 16
#define TAB_BAR_H 30
#define TAB_MIN_W 100
#define TAB_MAX_W 240
#define TAB_CLOSE_W 22
#define TAB_PLUS_W  30

static Tab *g_tabs[MAX_TABS];
static int g_num_tabs = 0;
static int g_active = 0;

/* Modal SSH connection form. When non-NORMAL, the terminal is locked:
   keystrokes edit form fields and mouse clicks focus them instead of
   going to the active tab. Layout is computed on the fly in
   ssh_form_layout() so draw and hit-test share one source of truth. */
typedef enum { UI_NORMAL = 0, UI_SSH_FORM } UiMode;
typedef enum {
    F_HOST = 0, F_PORT, F_USER, F_KEY,
    F_CONNECT, F_CANCEL,
    F_COUNT
} SshField;
typedef struct {
    char host[256];
    char port[16];
    char user[96];
    char key[512];
    int  focus;              /* SshField */
    char error[256];
} SshForm;
static UiMode  g_ui_mode = UI_NORMAL;
static SshForm g_form;

typedef struct {
    int x, y, w, h;
} Rect;

typedef struct {
    Rect modal;
    Rect field[4];  /* host, port, user, key */
    Rect connect;
    Rect cancel;
} SshFormLayout;

static Tab *active_tab(void) {
    if (g_num_tabs == 0) return NULL;
    if (g_active < 0) g_active = 0;
    if (g_active >= g_num_tabs) g_active = g_num_tabs - 1;
    return g_tabs[g_active];
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
    return t;
}

static Tab *tab_open_ssh(const char *user, const char *host, int port,
                         const char *keyfile, int cols, int rows,
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
    t->pty = pty_open_ssh(user, host, port, keyfile, cols, rows, err, errsz);
    if (!t->pty) { free(t); return NULL; }
    ScreenIO io = { .user = t, .write = io_write_cb,
                    .set_title = io_set_title_cb, .bell = io_bell_cb };
    t->scr = screen_new(cols, rows, 5000, io);
    g_tabs[g_num_tabs] = t;
    g_active = g_num_tabs;
    g_num_tabs++;
    return t;
}

static void tab_close(int idx) {
    if (idx < 0 || idx >= g_num_tabs) return;
    Tab *t = g_tabs[idx];
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
} TabBarHit;

static int tab_width_for(int win_w) {
    int avail = win_w - TAB_PLUS_W;
    if (g_num_tabs <= 0) return TAB_MIN_W;
    int w = avail / g_num_tabs;
    if (w > TAB_MAX_W) w = TAB_MAX_W;
    if (w < TAB_MIN_W) w = TAB_MIN_W;
    return w;
}

static TabBarHit tab_bar_hit_test(int win_w, int mx, int my) {
    TabBarHit h = { -1, false, false };
    if (my < 0 || my >= TAB_BAR_H) return h;
    int plus_x = win_w - TAB_PLUS_W;
    if (mx >= plus_x) { h.on_plus = true; return h; }
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
    DrawRectangle(plus_x, 0, TAB_PLUS_W, TAB_BAR_H, (Color){28, 32, 44, 255});
    const char *plus = "+";
    Vector2 psz = MeasureTextEx(*f, plus, 18, 0);
    DrawTextEx(*f, plus,
               (Vector2){ plus_x + (TAB_PLUS_W - psz.x) / 2.0f,
                          (TAB_BAR_H - psz.y) / 2.0f },
               18, 0, (Color){200, 200, 215, 255});
}

/* ---------- SSH connection form (PuTTY-style) ---------- */

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
}

static char *form_buf(int field, size_t *cap) {
    switch (field) {
    case F_HOST: *cap = sizeof(g_form.host); return g_form.host;
    case F_PORT: *cap = sizeof(g_form.port); return g_form.port;
    case F_USER: *cap = sizeof(g_form.user); return g_form.user;
    case F_KEY:  *cap = sizeof(g_form.key);  return g_form.key;
    default: *cap = 0; return NULL;
    }
}

static SshFormLayout ssh_form_layout(int win_w, int win_h) {
    SshFormLayout L = {0};
    int w = 600, h = 320;
    if (w > win_w - 40) w = win_w - 40;
    if (h > win_h - 40) h = win_h - 40;
    L.modal.x = (win_w - w) / 2;
    L.modal.y = (win_h - h) / 2;
    L.modal.w = w;
    L.modal.h = h;

    int pad = 22;
    int title_h = 46;
    int field_x = L.modal.x + 130;
    int field_w = w - 130 - pad;
    int field_h = 28;
    int y = L.modal.y + title_h + 10;
    for (int i = 0; i < 4; i++) {
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
        g_form.key[0] ? g_form.key : NULL,
        cols, rows, err, sizeof(err));
    if (t) {
        g_ui_mode = UI_NORMAL;
    } else {
        strncpy(g_form.error, err[0] ? err : "connection failed",
                sizeof(g_form.error) - 1);
        g_form.error[sizeof(g_form.error) - 1] = 0;
    }
}

static void ssh_form_advance_focus(int delta) {
    g_form.focus = (g_form.focus + delta + F_COUNT) % F_COUNT;
}

static void ssh_form_handle_mouse(SshFormLayout L) {
    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) return;
    Vector2 mp = GetMousePosition();
    int mx = (int)mp.x, my = (int)mp.y;
    for (int i = 0; i < 4; i++) {
        if (rect_hit(L.field[i], mx, my)) { g_form.focus = i; return; }
    }
    if (rect_hit(L.connect, mx, my)) g_form.focus = F_CONNECT;
    if (rect_hit(L.cancel,  mx, my)) g_form.focus = F_CANCEL;
}

static void ssh_form_handle_keys(int cols, int rows, SshFormLayout L) {
    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);

    if (IsKeyPressed(KEY_ESCAPE)) { g_ui_mode = UI_NORMAL; return; }

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

    /* Clicks on Connect/Cancel via space while they're focused. */
    if (g_form.focus == F_CONNECT && IsKeyPressed(KEY_SPACE)) {
        ssh_form_submit(cols, rows); return;
    }
    if (g_form.focus == F_CANCEL && IsKeyPressed(KEY_SPACE)) {
        g_ui_mode = UI_NORMAL; return;
    }

    /* Text edit the focused field (only real text fields). */
    if (g_form.focus >= F_HOST && g_form.focus <= F_KEY) {
        size_t cap;
        char *buf = form_buf(g_form.focus, &cap);
        int len = (int)strlen(buf);
        if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) {
            if (len > 0) { buf[len - 1] = 0; g_form.error[0] = 0; }
        }
        int cp;
        while ((cp = GetCharPressed()) != 0) {
            if (cp < 32 || cp >= 127) continue;
            if (g_form.focus == F_PORT && !(cp >= '0' && cp <= '9')) continue;
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
    const char *labels[4]   = {"Host",         "Port",  "Username", "Key file"};
    const char *hints[4]    = {"example.com",  "22",    getenv("USER"),
                               "(default: ssh-agent + ~/.ssh/id_*)"};
    const char *values[4]   = {g_form.host,    g_form.port, g_form.user, g_form.key};

    for (int i = 0; i < 4; i++) {
        /* Label. */
        DrawTextEx(*f, labels[i],
                   (Vector2){L.modal.x + 22, L.field[i].y + 7},
                   13, 0, (Color){180, 185, 200, 255});
        /* Field box. */
        bool focused = g_form.focus == i;
        DrawRectangle(L.field[i].x, L.field[i].y, L.field[i].w, L.field[i].h,
                      (Color){22, 25, 34, 255});
        DrawRectangleLines(L.field[i].x, L.field[i].y, L.field[i].w, L.field[i].h,
                           focused ? (Color){125, 207, 255, 255}
                                   : (Color){70, 74, 90, 255});
        /* Value or placeholder. */
        const char *shown = values[i][0] ? values[i]
                            : (hints[i] ? hints[i] : "");
        Color tc = values[i][0]
                   ? (Color){230, 232, 240, 255}
                   : (Color){110, 115, 130, 255};
        BeginScissorMode(L.field[i].x + 6, L.field[i].y,
                         L.field[i].w - 12, L.field[i].h);
        DrawTextEx(*f, shown,
                   (Vector2){L.field[i].x + 8, L.field[i].y + 7},
                   14, 0, tc);
        if (focused && values[i][0] &&
            ((long long)(GetTime() * 2.0) & 1) == 0) {
            Vector2 vsz = MeasureTextEx(*f, values[i], 14, 0);
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

        /* Drain each PTY so background tabs stay live. */
        for (int i = 0; i < g_num_tabs; i++) {
            Tab *t = g_tabs[i];
            for (int iter = 0; iter < 32; iter++) {
                int n = pty_read(t->pty, readbuf, sizeof(readbuf));
                if (n > 0) { screen_feed(t->scr, readbuf, (size_t)n); continue; }
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
            ssh_form_handle_mouse(L);
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

        /* Tab shortcuts (pre-input so Cmd/Ctrl+T/W/digit don't slip into the shell). */
        bool shift_held = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
        if (ui_key_down()) {
            if (shift_held && IsKeyPressed(KEY_T)) {
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
