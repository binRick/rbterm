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
#include <sys/ioctl.h>
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
        // Child: reset signals, set env, exec shell.
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
        const char *shell = getenv("SHELL");
        if (!shell || !*shell) shell = "/bin/bash";
        const char *argv0 = strrchr(shell, '/');
        argv0 = argv0 ? argv0 + 1 : shell;
        char dash[64];
        snprintf(dash, sizeof(dash), "-%s", argv0);  // login shell
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
        if (w < 0) { if (errno == EINTR) continue; if (errno == EAGAIN) { /* drop */ return; } return; }
        off += (size_t)w;
    }
}

/* ---------- IO glue for screen -> PTY writes ---------- */

static void io_write(void *u, const uint8_t *buf, size_t n) {
    pty_write_all((Pty *)u, buf, n);
}
static char g_title[256] = "rbterm";
static bool g_title_dirty = false;
static void io_set_title(void *u, const char *t) {
    (void)u;
    strncpy(g_title, t, sizeof(g_title) - 1);
    g_title[sizeof(g_title) - 1] = 0;
    g_title_dirty = true;
}
static void io_bell(void *u) { (void)u; /* TODO: flash */ }

/* ---------- Clipboard + selection ---------- */

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

/* Expand selection to a word/line when double/triple-clicking. */
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

/* ---------- Main ---------- */

static void usage(void) {
    printf("rbterm — raylib terminal emulator\n"
           "Usage: rbterm [--font PATH] [--size N] [--cols N] [--rows N]\n"
           "  --font PATH     path to a .ttf / .otf / .ttc monospace font\n"
           "  --size N        font size in points (default 14)\n"
           "  --cols N        initial cols (default 100)\n"
           "  --rows N        initial rows (default 30)\n"
           "\nKeys:\n"
           "  Ctrl/Cmd + +/-/0   grow/shrink/reset font size\n"
           "  Shift + PgUp/PgDn  scroll through history\n"
           "  Mouse wheel        scroll through history\n"
           "  Cmd+C / Ctrl+Shift+C   copy visible screen to clipboard\n"
           "  Cmd+V / Ctrl+Shift+V   paste clipboard into terminal\n");
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

    // Ignore SIGPIPE so PTY write failures don't kill us.
    signal(SIGPIPE, SIG_IGN);

    // Raylib setup — use a placeholder size; we'll resize after font loads.
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT | FLAG_WINDOW_HIGHDPI);
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(800, 500, "rbterm");
    SetExitKey(KEY_NULL);  // don't let raylib intercept ESC

    Renderer r;
    if (!renderer_init(&r, font_path, font_size)) {
        CloseWindow();
        return 1;
    }

    int win_w = init_cols * r.cell_w;
    int win_h = init_rows * r.cell_h;
    SetWindowSize(win_w, win_h);
    SetWindowMinSize(r.cell_w * 20, r.cell_h * 5);

    Pty pty;
    if (!pty_spawn_shell(&pty, init_cols, init_rows)) {
        renderer_shutdown(&r);
        CloseWindow();
        return 1;
    }

    ScreenIO sio = { .user = &pty, .write = io_write,
                     .set_title = io_set_title, .bell = io_bell };
    Screen *scr = screen_new(init_cols, init_rows, 5000, sio);

    SetTargetFPS(60);

    uint8_t readbuf[65536];
    uint8_t inputbuf[4096];

    bool child_exited = false;

    Selection sel = {0};
    int click_count = 0;
    double last_click_time = -1.0;
    int last_click_col = -1, last_click_row = -1;

    while (!WindowShouldClose()) {
        // Handle resize of raylib window -> resize screen + pty
        if (IsWindowResized() || GetScreenWidth() == 0) {
            int w = GetScreenWidth();
            int h = GetScreenHeight();
            int cols = w / r.cell_w;
            int rows = h / r.cell_h;
            if (cols < 1) cols = 1;
            if (rows < 1) rows = 1;
            if (cols != screen_cols(scr) || rows != screen_rows(scr)) {
                screen_resize(scr, cols, rows);
                pty_resize(&pty, cols, rows);
            }
        }

        // Read from PTY (non-blocking) into screen
        for (int iter = 0; iter < 64; iter++) {
            ssize_t n = read(pty.fd, readbuf, sizeof(readbuf));
            if (n > 0) {
                screen_feed(scr, readbuf, (size_t)n);
                continue;
            }
            if (n == 0) { child_exited = true; break; }
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            child_exited = true;
            break;
        }
        if (child_exited) break;

        // Check child status non-blocking
        int status;
        pid_t w = waitpid(pty.pid, &status, WNOHANG);
        if (w == pty.pid) { child_exited = true; break; }

        // Mouse selection
        {
            Vector2 mp = GetMousePosition();
            int mcol = (int)(mp.x / r.cell_w);
            int mrow = (int)(mp.y / r.cell_h);
            if (mcol < 0) mcol = 0;
            if (mcol >= screen_cols(scr)) mcol = screen_cols(scr) - 1;
            if (mrow < 0) mrow = 0;
            if (mrow >= screen_rows(scr)) mrow = screen_rows(scr) - 1;

            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                double t = GetTime();
                bool fast = (t - last_click_time < 0.45);
                bool same = (mcol == last_click_col && mrow == last_click_row);
                click_count = (fast && same) ? (click_count + 1) : 1;
                last_click_time = t;
                last_click_col = mcol;
                last_click_row = mrow;

                if (click_count == 2) {
                    select_word(scr, &sel, mcol, mrow);
                } else if (click_count >= 3) {
                    select_line(scr, &sel, mrow);
                    click_count = 3;
                } else {
                    sel.active = true;
                    sel.dragging = true;
                    sel.a_col = sel.b_col = mcol;
                    sel.a_row = sel.b_row = mrow;
                }
            }
            if (sel.dragging && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                sel.b_col = mcol;
                sel.b_row = mrow;
            }
            if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                if (sel.dragging && click_count < 2) {
                    sel.dragging = false;
                    if (sel.a_col == sel.b_col && sel.a_row == sel.b_row) {
                        sel.active = false;   // simple click deselects
                    }
                }
            }
        }

        // Input
        InputActions acts;
        size_t in_n = input_poll(scr, inputbuf, sizeof(inputbuf), &acts);
        if (acts.scroll_rows != 0) {
            screen_scroll_view(scr, acts.scroll_rows);
        }
        if (acts.copy) copy_selection(scr, &sel);
        if (acts.paste) {
            const char *t = GetClipboardText();
            if (t && *t) pty_write_all(&pty, (const uint8_t *)t, strlen(t));
        }
        if (acts.font_delta != 100) {
            int old = r.font_size;
            int ns = (acts.font_delta == 0) ? 14
                    : old + (acts.font_delta > 0 ? 1 : -1);
            if (renderer_set_font_size(&r, ns)) {
                // Recompute cols/rows to fit current window
                int cols = GetScreenWidth()  / r.cell_w;
                int rows = GetScreenHeight() / r.cell_h;
                if (cols < 1) cols = 1;
                if (rows < 1) rows = 1;
                screen_resize(scr, cols, rows);
                pty_resize(&pty, cols, rows);
                SetWindowMinSize(r.cell_w * 20, r.cell_h * 5);
            }
        }
        if (in_n > 0) {
            screen_scroll_reset(scr);
            pty_write_all(&pty, inputbuf, in_n);
            /* Any typing clears a pending selection. */
            if (sel.active && !sel.dragging) sel.active = false;
        }

        if (g_title_dirty) { SetWindowTitle(g_title); g_title_dirty = false; }

        BeginDrawing();
        renderer_draw(&r, scr, GetTime(), IsWindowFocused(), &sel);
        EndDrawing();
    }

    // Cleanup
    if (pty.pid > 0) {
        kill(pty.pid, SIGHUP);
        int status;
        waitpid(pty.pid, &status, 0);
    }
    close(pty.fd);
    screen_free(scr);
    renderer_shutdown(&r);
    CloseWindow();
    return 0;
}
