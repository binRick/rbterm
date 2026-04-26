/* Fake-shell backend for the WebAssembly demo build.
 *
 * Implements the same local_*_impl contract as pty_unix.c / pty_win.c
 * but without a real process, PTY, or filesystem. Instead it runs a
 * tiny in-process line-editor + command interpreter over a small
 * in-memory tree. Just enough to feel like a real shell for a website
 * demo.
 *
 * Selected via -DRBTERM_WEB=ON in CMake; built with emscripten. */

#include "pty_internal.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------- Virtual filesystem ---------- */

typedef struct FNode FNode;
struct FNode {
    char   name[64];
    bool   is_dir;
    char  *content;       /* for files; NUL-terminated, malloc'd */
    FNode *child;
    FNode *sibling;
    FNode *parent;
};

static FNode *fnode_new(FNode *parent, const char *name, bool is_dir, const char *content) {
    FNode *n = calloc(1, sizeof(*n));
    strncpy(n->name, name, sizeof(n->name) - 1);
    n->is_dir = is_dir;
    if (content) n->content = strdup(content);
    n->parent = parent;
    if (parent) {
        n->sibling = parent->child;
        parent->child = n;
    }
    return n;
}

static void fnode_free(FNode *n) {
    if (!n) return;
    for (FNode *c = n->child, *next; c; c = next) { next = c->sibling; fnode_free(c); }
    free(n->content);
    free(n);
}

static FNode *fnode_child(FNode *parent, const char *name) {
    if (!parent || !parent->is_dir) return NULL;
    for (FNode *c = parent->child; c; c = c->sibling) {
        if (strcmp(c->name, name) == 0) return c;
    }
    return NULL;
}

/* ---------- Fake Pty state ---------- */

#define OUT_CAP   (64 * 1024)
#define LINE_MAX  1024
#define HIST_N    32

typedef struct {
    uint8_t outbuf[OUT_CAP];
    size_t  head, tail;

    char    line[LINE_MAX];
    int     line_len;
    int     cursor;           /* insertion point within line[], 0..line_len */

    FNode  *root;
    FNode  *cwd;

    char    history[HIST_N][LINE_MAX];
    int     history_len;
    int     history_cursor;   /* -1 = editing new line */

    int     cols, rows;
    bool    esc_mode;         /* parsing an ESC sequence from input */
    char    esc_buf[16];
    int     esc_len;

    bool    alive;
} FakePty;

/* ---------- Output helpers ---------- */

static void out_byte(FakePty *p, uint8_t b) {
    p->outbuf[p->head] = b;
    p->head = (p->head + 1) % OUT_CAP;
    if (p->head == p->tail) {
        /* drop oldest on overflow — fine for a demo shell */
        p->tail = (p->tail + 1) % OUT_CAP;
    }
}

static void out_str(FakePty *p, const char *s) {
    while (*s) out_byte(p, (uint8_t)*s++);
}

static void out_fmt(FakePty *p, const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    out_str(p, buf);
}

/* Compute "/home/demo/projects" style absolute path for `n`. */
static void fnode_path(const FNode *n, char *out, size_t cap) {
    if (!n || !n->parent) { snprintf(out, cap, "/"); return; }
    const FNode *chain[64];
    int depth = 0;
    for (const FNode *c = n; c && c->parent && depth < 64; c = c->parent) {
        chain[depth++] = c;
    }
    size_t off = 0;
    for (int i = depth - 1; i >= 0 && off + 1 < cap; i--) {
        out[off++] = '/';
        size_t nl = strlen(chain[i]->name);
        if (off + nl >= cap) nl = cap - off - 1;
        memcpy(out + off, chain[i]->name, nl);
        off += nl;
    }
    out[off] = 0;
    if (off == 0) { snprintf(out, cap, "/"); }
}

/* Print a colourful zsh-like prompt. */
static void prompt(FakePty *p) {
    char path[512];
    fnode_path(p->cwd, path, sizeof(path));
    /* Replace /home/demo with ~ for brevity. */
    const char *disp = path;
    if (strncmp(path, "/home/demo", 10) == 0) {
        char *tilde = path + 9;  /* point at 'o' of demo, overwrite */
        tilde[-1] = '~';
        disp = tilde - 1;
    }
    out_fmt(p, "\x1b[38;5;82m\xe2\x9e\x9c\x1b[0m "   /* green arrow */
               "\x1b[38;5;45m%s\x1b[0m $ ", disp);
}

/* ---------- Path resolution ---------- */

static FNode *resolve_path(FakePty *p, const char *path) {
    if (!path || !*path) return p->cwd;
    FNode *cur = (path[0] == '/') ? p->root : p->cwd;
    char buf[512];
    strncpy(buf, path, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    char *save = NULL;
    for (char *tok = strtok_r(buf, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
        if (strcmp(tok, ".") == 0) continue;
        if (strcmp(tok, "..") == 0) { if (cur->parent) cur = cur->parent; continue; }
        if (strcmp(tok, "~") == 0) {
            cur = fnode_child(p->root, "home");
            if (cur) cur = fnode_child(cur, "demo");
            if (!cur) return NULL;
            continue;
        }
        FNode *next = fnode_child(cur, tok);
        if (!next) return NULL;
        cur = next;
    }
    return cur;
}

/* ---------- Command implementations ---------- */

static void cmd_help(FakePty *p) {
    out_str(p,
      "\x1b[1mrbterm web demo\x1b[0m — a fake shell running entirely in your browser.\r\n"
      "\r\n"
      "Built-in commands:\r\n"
      "  \x1b[36mhelp\x1b[0m           show this message\r\n"
      "  \x1b[36mls\x1b[0m [path]      list a directory\r\n"
      "  \x1b[36mcd\x1b[0m [path]      change directory (default ~)\r\n"
      "  \x1b[36mpwd\x1b[0m            print working directory\r\n"
      "  \x1b[36mcat\x1b[0m <file>     print a file\r\n"
      "  \x1b[36mecho\x1b[0m <args>    print arguments\r\n"
      "  \x1b[36mclear\x1b[0m          clear the screen\r\n"
      "  \x1b[36mdate\x1b[0m           current date/time\r\n"
      "  \x1b[36mwhoami\x1b[0m         you\r\n"
      "  \x1b[36muname\x1b[0m          system\r\n"
      "  \x1b[36mhistory\x1b[0m        previous commands\r\n"
      "  \x1b[36mfortune\x1b[0m        a quote\r\n"
      "  \x1b[36mrainbow\x1b[0m        palette test\r\n"
      "  \x1b[36mexit\x1b[0m           (no-op in the demo)\r\n"
      "\r\n"
      "This isn't a real shell — there's no network, no processes, no disk. "
      "Everything runs client-side in WebAssembly.\r\n"
      "For the full terminal (real PTY, SSH, etc.) see "
      "\x1b[4;34mhttps://github.com/binRick/rbterm\x1b[0m\r\n");
}

static void cmd_pwd(FakePty *p) {
    char path[512];
    fnode_path(p->cwd, path, sizeof(path));
    out_str(p, path);
    out_str(p, "\r\n");
}

static void cmd_cd(FakePty *p, const char *path) {
    const char *target = (path && *path) ? path : "/home/demo";
    FNode *n = resolve_path(p, target);
    if (!n) { out_fmt(p, "cd: no such file or directory: %s\r\n", target); return; }
    if (!n->is_dir) { out_fmt(p, "cd: not a directory: %s\r\n", target); return; }
    p->cwd = n;
}

static void cmd_ls(FakePty *p, const char *path) {
    FNode *target = (path && *path) ? resolve_path(p, path) : p->cwd;
    if (!target) { out_fmt(p, "ls: no such file or directory: %s\r\n", path); return; }
    if (!target->is_dir) {
        out_str(p, target->name);
        out_str(p, "\r\n");
        return;
    }
    for (FNode *c = target->child; c; c = c->sibling) {
        if (c->is_dir) out_fmt(p, "\x1b[34m%s/\x1b[0m  ", c->name);
        else           out_fmt(p, "%s  ", c->name);
    }
    out_str(p, "\r\n");
}

static void cmd_cat(FakePty *p, const char *path) {
    if (!path || !*path) { out_str(p, "cat: missing operand\r\n"); return; }
    FNode *n = resolve_path(p, path);
    if (!n)         { out_fmt(p, "cat: %s: No such file or directory\r\n", path); return; }
    if (n->is_dir)  { out_fmt(p, "cat: %s: Is a directory\r\n", path); return; }
    if (n->content) {
        /* Convert plain \n to \r\n for terminal output. */
        for (const char *c = n->content; *c; c++) {
            if (*c == '\n') out_byte(p, '\r');
            out_byte(p, (uint8_t)*c);
        }
        if (*n->content && n->content[strlen(n->content) - 1] != '\n') out_str(p, "\r\n");
    }
}

static void cmd_echo(FakePty *p, int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        out_str(p, argv[i]);
        if (i + 1 < argc) out_byte(p, ' ');
    }
    out_str(p, "\r\n");
}

static void cmd_date(FakePty *p) {
    time_t t = time(NULL);
    out_str(p, ctime(&t));   /* ctime already ends with \n */
    /* Replace trailing \n with \r\n so the prompt lands on a fresh line. */
    out_byte(p, '\r'); /* harmless if already present */
}

static void cmd_rainbow(FakePty *p) {
    for (int i = 0; i < 16; i++) {
        out_fmt(p, "\x1b[48;5;%dm %02d \x1b[0m", i, i);
    }
    out_str(p, "\r\n");
    for (int i = 16; i < 256; i++) {
        out_fmt(p, "\x1b[48;5;%dm %3d \x1b[0m", i, i);
        if ((i - 15) % 6 == 0) out_str(p, "\r\n");
    }
    out_str(p, "\r\n");
}

static const char *fortunes[] = {
    "The best way to predict the future is to implement it.",
    "Simplicity is the ultimate sophistication.",
    "Readers who know nothing about Lisp should stop here.",
    "Any sufficiently advanced technology is indistinguishable from a rigged demo.",
    "The cheapest, fastest and most reliable components are those that aren't there.",
    "Controlling complexity is the essence of computer programming.",
    "Deleted code is debugged code.",
    NULL
};

static void cmd_fortune(FakePty *p) {
    int n = 0;
    while (fortunes[n]) n++;
    int idx = (int)(time(NULL) % n);
    out_fmt(p, "  \x1b[3m%s\x1b[0m\r\n", fortunes[idx]);
}

static void cmd_history(FakePty *p) {
    for (int i = 0; i < p->history_len; i++) {
        out_fmt(p, "%4d  %s\r\n", i + 1, p->history[i]);
    }
}

static void push_history(FakePty *p, const char *line) {
    if (!line || !*line) return;
    if (p->history_len == HIST_N) {
        memmove(p->history[0], p->history[1], sizeof(p->history[0]) * (HIST_N - 1));
        p->history_len--;
    }
    strncpy(p->history[p->history_len], line, LINE_MAX - 1);
    p->history[p->history_len][LINE_MAX - 1] = 0;
    p->history_len++;
    p->history_cursor = -1;
}

/* ---------- Command dispatcher ---------- */

static void run_line(FakePty *p, const char *line) {
    /* Skip leading whitespace. */
    while (*line == ' ' || *line == '\t') line++;
    if (!*line) return;

    push_history(p, line);

    /* Tokenise. Very naive — splits on whitespace, no quoting. */
    char buf[LINE_MAX];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    char *argv[32];
    int argc = 0;
    char *save = NULL;
    for (char *tok = strtok_r(buf, " \t", &save); tok && argc < 32;
         tok = strtok_r(NULL, " \t", &save))
        argv[argc++] = tok;
    if (argc == 0) return;

    if      (!strcmp(argv[0], "help"))    cmd_help(p);
    else if (!strcmp(argv[0], "pwd"))     cmd_pwd(p);
    else if (!strcmp(argv[0], "cd"))      cmd_cd(p, argc > 1 ? argv[1] : NULL);
    else if (!strcmp(argv[0], "ls"))      cmd_ls(p, argc > 1 ? argv[1] : NULL);
    else if (!strcmp(argv[0], "cat"))     cmd_cat(p, argc > 1 ? argv[1] : NULL);
    else if (!strcmp(argv[0], "echo"))    cmd_echo(p, argc, argv);
    else if (!strcmp(argv[0], "date"))    cmd_date(p);
    else if (!strcmp(argv[0], "clear"))   out_str(p, "\x1b[2J\x1b[H");
    else if (!strcmp(argv[0], "whoami"))  out_str(p, "demo\r\n");
    else if (!strcmp(argv[0], "uname"))   out_str(p, "rbterm-wasm 0.1.0 (demo)\r\n");
    else if (!strcmp(argv[0], "history")) cmd_history(p);
    else if (!strcmp(argv[0], "rainbow")) cmd_rainbow(p);
    else if (!strcmp(argv[0], "fortune")) cmd_fortune(p);
    else if (!strcmp(argv[0], "exit") ||
             !strcmp(argv[0], "logout")) {
        out_str(p, "logout (demo)\r\n");
        p->alive = false;
    }
    else                                  out_fmt(p, "rbterm: command not found: %s\r\n", argv[0]);
}

/* ---------- Input handling / line editor ---------- */

static void replace_line(FakePty *p, const char *newline) {
    /* Move the visual cursor to end of current line, then erase back to 0. */
    for (int i = p->cursor; i < p->line_len; i++) out_str(p, "\x1b[C");
    for (int i = 0; i < p->line_len; i++) out_str(p, "\b \b");
    p->line_len = 0;
    p->cursor = 0;
    size_t n = strlen(newline);
    if (n >= LINE_MAX) n = LINE_MAX - 1;
    memcpy(p->line, newline, n);
    p->line_len = (int)n;
    p->cursor = (int)n;
    p->line[n] = 0;
    for (size_t i = 0; i < n; i++) out_byte(p, (uint8_t)newline[i]);
}

static void feed_char(FakePty *p, uint8_t b) {
    /* Minimal ESC-sequence parser for arrow keys used by history. */
    if (p->esc_mode) {
        if (p->esc_len < (int)sizeof(p->esc_buf) - 1) p->esc_buf[p->esc_len++] = (char)b;
        if (p->esc_len >= 3 && p->esc_buf[0] == 0x1b && p->esc_buf[1] == '[') {
            char fn = p->esc_buf[2];
            p->esc_mode = false;
            p->esc_len = 0;
            if (fn == 'A') {  /* up */
                if (p->history_len == 0) return;
                if (p->history_cursor == -1) p->history_cursor = p->history_len - 1;
                else if (p->history_cursor > 0) p->history_cursor--;
                replace_line(p, p->history[p->history_cursor]);
                return;
            }
            if (fn == 'B') {  /* down */
                if (p->history_cursor == -1) return;
                if (p->history_cursor < p->history_len - 1) {
                    p->history_cursor++;
                    replace_line(p, p->history[p->history_cursor]);
                } else {
                    p->history_cursor = -1;
                    replace_line(p, "");
                }
                return;
            }
            if (fn == 'D') {  /* left */
                if (p->cursor > 0) { p->cursor--; out_str(p, "\x1b[D"); }
                return;
            }
            if (fn == 'C') {  /* right */
                if (p->cursor < p->line_len) { p->cursor++; out_str(p, "\x1b[C"); }
                return;
            }
            if (fn == 'H') {  /* home */
                while (p->cursor > 0) { p->cursor--; out_str(p, "\x1b[D"); }
                return;
            }
            if (fn == 'F') {  /* end */
                while (p->cursor < p->line_len) { p->cursor++; out_str(p, "\x1b[C"); }
                return;
            }
        }
        if (p->esc_len >= 3) { p->esc_mode = false; p->esc_len = 0; }
        return;
    }

    if (b == 0x1b) { p->esc_mode = true; p->esc_len = 0; p->esc_buf[p->esc_len++] = (char)b; return; }
    if (b == 0x03) {   /* Ctrl+C */
        out_str(p, "^C\r\n");
        p->line_len = 0;
        p->cursor = 0;
        prompt(p);
        return;
    }
    if (b == 0x04) {   /* Ctrl+D — EOF on empty line closes the
                          pane, matching how a real shell behaves. */
        if (p->line_len == 0) {
            out_str(p, "logout (demo)\r\n");
            p->alive = false;
        }
        return;
    }
    if (b == 0x0d || b == 0x0a) {  /* CR / LF — execute */
        out_str(p, "\r\n");
        p->line[p->line_len] = 0;
        run_line(p, p->line);
        p->line_len = 0;
        p->cursor = 0;
        prompt(p);
        return;
    }
    if (b == 0x7f || b == 0x08) {  /* backspace / DEL */
        if (p->cursor > 0) {
            memmove(&p->line[p->cursor - 1], &p->line[p->cursor],
                    p->line_len - p->cursor);
            p->cursor--;
            p->line_len--;
            p->line[p->line_len] = 0;
            /* Move visual cursor left, then delete the char there. */
            out_str(p, "\b\x1b[P");
        }
        return;
    }
    if (b == 0x01) {   /* Ctrl+A — beginning of line */
        while (p->cursor > 0) { p->cursor--; out_str(p, "\x1b[D"); }
        return;
    }
    if (b == 0x05) {   /* Ctrl+E — end of line */
        while (p->cursor < p->line_len) { p->cursor++; out_str(p, "\x1b[C"); }
        return;
    }
    if (b == 0x0b) {   /* Ctrl+K — kill to end of line */
        if (p->cursor < p->line_len) {
            p->line_len = p->cursor;
            p->line[p->line_len] = 0;
            out_str(p, "\x1b[K");
        }
        return;
    }
    if (b == 0x15) {   /* Ctrl+U — clear line */
        replace_line(p, "");
        return;
    }
    if (b == 0x0c) {   /* Ctrl+L — clear screen, redraw prompt + line */
        out_str(p, "\x1b[2J\x1b[H");
        prompt(p);
        if (p->line_len > 0) {
            for (int i = 0; i < p->line_len; i++) out_byte(p, (uint8_t)p->line[i]);
            /* Cursor ended up at end; move back to logical cursor. */
            for (int i = p->cursor; i < p->line_len; i++) out_str(p, "\x1b[D");
        }
        return;
    }
    if (b >= 0x20 && b < 0x7f) {
        if (p->line_len + 1 < LINE_MAX) {
            /* Insert b at p->cursor. */
            memmove(&p->line[p->cursor + 1], &p->line[p->cursor],
                    p->line_len - p->cursor);
            p->line[p->cursor] = (char)b;
            p->line_len++;
            p->cursor++;
            p->line[p->line_len] = 0;
            if (p->cursor == p->line_len) {
                /* Append — no shift needed. */
                out_byte(p, b);
            } else {
                /* Mid-line insert: open a slot with ICH, then write the char. */
                out_str(p, "\x1b[@");
                out_byte(p, b);
            }
        }
        return;
    }
}

/* ---------- Initial filesystem seed ---------- */

static const char *README_MD =
    "# rbterm web demo\r\n"
    "\r\n"
    "This is a browser build of rbterm — the terminal emulator I'm\r\n"
    "building with raylib.\r\n"
    "\r\n"
    "No real shell, no real filesystem, no network. Commands like\r\n"
    "`ls`, `cat`, `cd`, and `echo` are implemented here in the\r\n"
    "WebAssembly blob. Everything else you'd expect from a terminal\r\n"
    "(SSH, tabs, scrollback reflow, OSC palette, mouse reporting,\r\n"
    "session logging) is in the native builds.\r\n"
    "\r\n"
    "Grab the native binary from the releases page.\r\n";

static const char *TIPS_TXT =
    "Try: \r\n"
    "  help      — see every command\r\n"
    "  rainbow   — paint the 256-colour palette\r\n"
    "  fortune   — a random quote\r\n"
    "  cat ~/projects/notes.txt\r\n";

static const char *NOTES_TXT =
    "- tabs are in the desktop build\r\n"
    "- SSH modal talks to ~/.ssh/config\r\n"
    "- OSC 4 retroactively recolours existing cells\r\n"
    "- the box-drawing lookup is hand-drawn rectangles\r\n";

static FNode *build_demo_fs(void) {
    FNode *root = fnode_new(NULL, "/", true, NULL);
    FNode *home = fnode_new(root, "home", true, NULL);
    FNode *demo = fnode_new(home, "demo", true, NULL);
    fnode_new(demo, "README.md", false, README_MD);
    fnode_new(demo, "tips.txt",  false, TIPS_TXT);
    FNode *proj = fnode_new(demo, "projects", true, NULL);
    fnode_new(proj, "notes.txt", false, NOTES_TXT);
    fnode_new(root, "etc", true, NULL);
    fnode_new(root, "tmp", true, NULL);
    return root;
}

/* ---------- pty_internal.h backend ---------- */

void *local_open_impl(int cols, int rows, const char *cwd) {
    (void)cwd;   /* fake fs is virtual; cwd is always /home/demo */
    FakePty *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->root = build_demo_fs();
    FNode *home = fnode_child(p->root, "home");
    p->cwd = home ? fnode_child(home, "demo") : p->root;
    if (!p->cwd) p->cwd = p->root;
    p->cols = cols;
    p->rows = rows;
    p->history_cursor = -1;
    p->alive = true;

    /* Welcome banner. */
    out_str(p,
        "\x1b[1;38;5;45mrbterm web demo\x1b[0m  "
        "— a fake shell running in your browser.\r\n"
        "Type \x1b[36mhelp\x1b[0m for the list of commands.\r\n"
        "\r\n");
    prompt(p);
    return p;
}

void local_close_impl(void *impl) {
    FakePty *p = impl;
    if (!p) return;
    fnode_free(p->root);
    free(p);
}

bool local_alive_impl(void *impl) {
    FakePty *p = impl;
    return p && p->alive;
}

int local_read_impl(void *impl, uint8_t *buf, size_t cap) {
    FakePty *p = impl;
    if (!p) return -1;
    size_t n = 0;
    while (n < cap && p->tail != p->head) {
        buf[n++] = p->outbuf[p->tail];
        p->tail = (p->tail + 1) % OUT_CAP;
    }
    return (int)n;
}

void local_write_impl(void *impl, const uint8_t *buf, size_t n) {
    FakePty *p = impl;
    if (!p) return;
    for (size_t i = 0; i < n; i++) feed_char(p, buf[i]);
}

void local_resize_impl(void *impl, int cols, int rows) {
    FakePty *p = impl;
    if (!p) return;
    p->cols = cols;
    p->rows = rows;
}

bool local_cwd_impl(void *impl, char *out, size_t cap) {
    FakePty *p = impl;
    if (!p || !out || cap == 0) return false;
    fnode_path(p->cwd, out, cap);
    return true;
}

/* No reader thread on the wasm path → no fast-path DSR replies to
   support. Stub keeps the dispatch layer linkable. */
void local_snap_cursor_impl(void *impl, int cy, int cx) {
    (void)impl; (void)cy; (void)cx;
}
