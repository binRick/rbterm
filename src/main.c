#define _DARWIN_C_SOURCE
#define _GNU_SOURCE
#include "raylib.h"
#include "screen.h"
#include "render.h"
#include "input.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#if defined(__APPLE__)
  #include <util.h>
#elif defined(__linux__)
  #include <pty.h>
#else
  #error "Unsupported platform (needs PTY support)"
#endif

/* ---------- PTY ---------- */

typedef struct {
    pid_t pid;
    int   fd;
} Pty;

static bool pty_spawn_shell(Pty *out, int cols, int rows) {
    struct winsize ws = { .ws_row = (unsigned short)rows,
                          .ws_col = (unsigned short)cols };
    int master;
    pid_t pid = forkpty(&master, NULL, NULL, &ws);
    if (pid < 0) { perror("forkpty"); return false; }
    if (pid == 0) {
        signal(SIGCHLD, SIG_DFL);
        signal(SIGHUP,  SIG_DFL);
        signal(SIGINT,  SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGALRM, SIG_DFL);
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        unsetenv("TERM_PROGRAM");
        unsetenv("TERM_SESSION_ID");

        const char *home = getenv("HOME");
        if (!home || !*home) {
            struct passwd *pw = getpwuid(getuid());
            if (pw && pw->pw_dir) home = pw->pw_dir;
        }
        if (home && *home) {
            (void)chdir(home);
            setenv("PWD", home, 1);
        }

        const char *shell = getenv("SHELL");
        if (!shell || !*shell) shell = "/bin/bash";
        const char *argv0 = strrchr(shell, '/');
        argv0 = argv0 ? argv0 + 1 : shell;
        char dash[64];
        snprintf(dash, sizeof(dash), "-%s", argv0);
        execl(shell, dash, (char *)NULL);
        perror("execl");
        _exit(127);
    }
    int flags = fcntl(master, F_GETFL, 0);
    fcntl(master, F_SETFL, flags | O_NONBLOCK);
    out->pid = pid;
    out->fd  = master;
    return true;
}

static void pty_resize(Pty *p, int cols, int rows) {
    struct winsize ws = { .ws_row = (unsigned short)rows,
                          .ws_col = (unsigned short)cols };
    ioctl(p->fd, TIOCSWINSZ, &ws);
}

static void pty_write_all(Pty *p, const uint8_t *buf, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(p->fd, buf + off, n - off);
        if (w < 0) { if (errno == EINTR) continue; if (errno == EAGAIN) return; return; }
        off += (size_t)w;
    }
}

/* ---------- Tabs ---------- */

typedef struct {
    Pty pty;
    Screen *scr;
    Selection sel;
    char title[256];
    bool title_dirty;
    bool dead;
    int click_count;
    double last_click_time;
    int last_click_col, last_click_row;
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

static Tab *active_tab(void) {
    if (g_num_tabs == 0) return NULL;
    if (g_active < 0) g_active = 0;
    if (g_active >= g_num_tabs) g_active = g_num_tabs - 1;
    return g_tabs[g_active];
}

/* ---------- IO glue for screen -> PTY writes (per-tab) ---------- */

static void io_write_cb(void *u, const uint8_t *buf, size_t n) {
    Tab *t = (Tab *)u;
    pty_write_all(&t->pty, buf, n);
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
    if (!pty_spawn_shell(&t->pty, cols, rows)) { free(t); return NULL; }
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
    if (t->pty.pid > 0) {
        kill(t->pty.pid, SIGHUP);
        int status;
        waitpid(t->pty.pid, &status, 0);
    }
    close(t->pty.fd);
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

/* ---------- Tab bar UI ---------- */

typedef struct {
    int tab_idx;    /* -1 if not on a tab */
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
    /* Bar background. */
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
        if (active) {
            DrawRectangle(x, 0, tw, 2, (Color){125, 207, 255, 255});
        }
        /* Tab title (scissored so it doesn't bleed into the close X). */
        const char *title = g_tabs[i]->title;
        if (!title[0]) title = "shell";
        BeginScissorMode(x + 8, 0, tw - TAB_CLOSE_W - 12, TAB_BAR_H);
        Vector2 tsz = MeasureTextEx(*f, title, fs, 0);
        Vector2 tp  = { x + 10, (TAB_BAR_H - tsz.y) / 2.0f };
        DrawTextEx(*f, title, tp, fs, 0, fg);
        EndScissorMode();
        /* Close X */
        const char *cross = "x";
        Vector2 csz = MeasureTextEx(*f, cross, fs, 0);
        DrawTextEx(*f, cross,
                   (Vector2){ x + tw - TAB_CLOSE_W / 2 - csz.x / 2,
                              (TAB_BAR_H - csz.y) / 2.0f },
                   fs, 0, fg);
        /* Separator between tabs (skip before first). */
        if (i > 0) DrawLine(x, 4, x, TAB_BAR_H - 4, (Color){60, 60, 75, 255});
    }

    /* Plus button on the right. */
    int plus_x = win_w - TAB_PLUS_W;
    DrawRectangle(plus_x, 0, TAB_PLUS_W, TAB_BAR_H, (Color){28, 32, 44, 255});
    const char *plus = "+";
    Vector2 psz = MeasureTextEx(*f, plus, 18, 0);
    DrawTextEx(*f, plus,
               (Vector2){ plus_x + (TAB_PLUS_W - psz.x) / 2.0f,
                          (TAB_BAR_H - psz.y) / 2.0f },
               18, 0, (Color){200, 200, 215, 255});
}

/* ---------- Cmd / Ctrl helpers ---------- */

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
           "\nKeys:\n"
           "  Cmd + T           open a new tab\n"
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

    signal(SIGPIPE, SIG_IGN);

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

        /* Resize — apply to every tab. */
        int content_rows = (win_h_now - TAB_BAR_H) / r.cell_h;
        int content_cols = win_w_now / r.cell_w;
        if (content_cols < 1) content_cols = 1;
        if (content_rows < 1) content_rows = 1;
        for (int i = 0; i < g_num_tabs; i++) {
            if (screen_cols(g_tabs[i]->scr) != content_cols ||
                screen_rows(g_tabs[i]->scr) != content_rows) {
                screen_resize(g_tabs[i]->scr, content_cols, content_rows);
                pty_resize(&g_tabs[i]->pty, content_cols, content_rows);
            }
        }

        /* Read from every PTY, not just the active one, so background tabs
           stay up-to-date. */
        for (int i = 0; i < g_num_tabs; i++) {
            Tab *t = g_tabs[i];
            for (int iter = 0; iter < 32; iter++) {
                ssize_t n = read(t->pty.fd, readbuf, sizeof(readbuf));
                if (n > 0) { screen_feed(t->scr, readbuf, (size_t)n); continue; }
                if (n == 0) { t->dead = true; break; }
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                if (errno == EINTR) continue;
                t->dead = true; break;
            }
            int status;
            pid_t w = waitpid(t->pty.pid, &status, WNOHANG);
            if (w == t->pty.pid) t->dead = true;
        }
        /* Reap dead tabs (iterate backwards to keep indices stable). */
        for (int i = g_num_tabs - 1; i >= 0; i--) {
            if (g_tabs[i]->dead) tab_close(i);
        }
        if (g_num_tabs == 0) break;

        Tab *cur = active_tab();

        /* Tab-bar mouse and content-area selection both need the pointer. */
        Vector2 mp = GetMousePosition();
        bool in_tab_bar = (mp.y < TAB_BAR_H);

        if (in_tab_bar) {
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                TabBarHit h = tab_bar_hit_test(win_w_now, (int)mp.x, (int)mp.y);
                if (h.on_plus) {
                    tab_open(content_cols, content_rows);
                } else if (h.tab_idx >= 0) {
                    if (h.on_close) {
                        tab_close(h.tab_idx);
                    } else {
                        g_active = h.tab_idx;
                    }
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
                if (cur->click_count == 2) {
                    select_word(cur->scr, &cur->sel, mcol, mrow);
                } else if (cur->click_count >= 3) {
                    select_line(cur->scr, &cur->sel, mrow);
                    cur->click_count = 3;
                } else {
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

        /* Tab keyboard shortcuts — check before input_poll. Cmd+letter chords
           don't produce GetCharPressed events on macOS so input_poll won't
           also send these bytes to the shell. */
        if (ui_key_down()) {
            if (IsKeyPressed(KEY_T)) {
                tab_open(content_cols, content_rows);
                cur = active_tab();
            } else if (IsKeyPressed(KEY_W)) {
                tab_close(g_active);
                if (g_num_tabs == 0) break;
                cur = active_tab();
            } else if (IsKeyPressed(KEY_LEFT_BRACKET)) {
                g_active = (g_active - 1 + g_num_tabs) % g_num_tabs;
                cur = active_tab();
            } else if (IsKeyPressed(KEY_RIGHT_BRACKET)) {
                g_active = (g_active + 1) % g_num_tabs;
                cur = active_tab();
            } else {
                for (int k = 0; k < 9; k++) {
                    if (IsKeyPressed(KEY_ONE + k)) {
                        if (k < g_num_tabs) { g_active = k; cur = active_tab(); }
                        break;
                    }
                }
            }
        }

        if (!cur) break;

        /* Input + clipboard + font chords — all targeted at the active tab. */
        InputActions acts;
        size_t in_n = input_poll(cur->scr, inputbuf, sizeof(inputbuf), &acts);
        if (acts.scroll_rows != 0) screen_scroll_view(cur->scr, acts.scroll_rows);
        if (acts.copy) copy_selection(cur->scr, &cur->sel);
        if (acts.paste) {
            const char *t = GetClipboardText();
            if (t && *t) pty_write_all(&cur->pty, (const uint8_t *)t, strlen(t));
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
                    pty_resize(&g_tabs[i]->pty, nc, nr);
                }
                SetWindowMinSize(r.cell_w * 20, r.cell_h * 5 + TAB_BAR_H);
            }
        }
        if (in_n > 0) {
            screen_scroll_reset(cur->scr);
            pty_write_all(&cur->pty, inputbuf, in_n);
            if (cur->sel.active && !cur->sel.dragging) cur->sel.active = false;
        }

        if (cur->title_dirty) {
            SetWindowTitle(cur->title);
            cur->title_dirty = false;
        }

        BeginDrawing();
        ClearBackground((Color){0, 0, 0, 255});
        draw_tab_bar(&r, win_w_now);
        renderer_draw(&r, cur->scr, GetTime(), IsWindowFocused(),
                      &cur->sel, TAB_BAR_H);
        EndDrawing();
    }

    /* Shut down any remaining tabs. */
    for (int i = g_num_tabs - 1; i >= 0; i--) tab_close(i);
    renderer_shutdown(&r);
    CloseWindow();
    return 0;
}
