#include "screen.h"
#include "sixel.h"
#include "kitty.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>

enum {
    ST_GROUND = 0,
    ST_ESC,
    ST_CSI,
    ST_OSC,
    ST_OSC_ESC,
    ST_UTF8,
    ST_CHARSET,      /* ESC * X or ESC + X — G2/G3, we ignore the payload */
    ST_CHARSET_G0,   /* ESC ( X — pick G0 charset */
    ST_CHARSET_G1,   /* ESC ) X — pick G1 charset */
    ST_DCS,          /* ESC P … ST  — sixel lives here */
    ST_DCS_ESC,      /* saw ESC while collecting DCS, waiting for \ */
    ST_APC,          /* ESC _ … ST  — kitty graphics lives here */
    ST_APC_ESC,      /* saw ESC while collecting APC, waiting for \ */
};

#define MAX_PARAMS 16

/* Seed values copied into every new Screen. OSC 10/11/12 mutate the
   per-screen copy, not these, so one pane's theme doesn't bleed into
   the others. */
#define SEED_DEFAULT_FG    0xFFFFFFu   /* pure white — was 0xEAEAEA, felt gray */
#define SEED_DEFAULT_BG    0x111111u
#define SEED_CURSOR_COLOR  0xFFFFFFu

/* Forward declaration; full definition lives below. */
typedef struct ScreenImage ScreenImage_t;

/* Max concurrent images per screen. When a new image pushes us over,
   the oldest is evicted (FIFO). 32 is generous — most sessions show
   one image at a time and discard it on scroll. */
#define SCREEN_IMAGE_CAP 32

struct ScreenImage {
    unsigned char *rgba;       /* owned; w*h*4 bytes */
    int px_w, px_h;
    int anchor_row;            /* viewport row (0 = top); may go < 0 off-screen */
    int anchor_col;
    uint64_t gen;              /* unique id; never reused across the run */
    bool on_alt;               /* true when image was ingested on alt screen;
                                  render skips images whose flag doesn't match
                                  the current screen. tmux + less clear cleanly. */
};

struct Screen {
    int cols, rows;
    int cx, cy;
    int saved_cx, saved_cy;
    uint16_t saved_attrs_flags;
    uint32_t saved_fg, saved_bg;
    bool cursor_visible;
    bool wrap_next;
    bool app_cursor;
    bool app_keypad;
    bool origin_mode;
    bool autowrap;
    bool focus_report;   /* DECSET 1004 */
    bool sync_update;    /* DECSET 2026 */
    int  mouse_mode;     /* 0, 1000, 1002, 1003 */
    bool mouse_sgr;      /* DECSET 1006 — SGR encoding on mouse reports */
    bool bracketed_paste;/* DECSET 2004 */
    int scroll_top, scroll_bot;
    Cell cur_attr;

    Cell *main;
    Cell *alt;
    uint8_t *main_wrap;   /* size rows: main_wrap[y]=1 means row y ended
                             by auto-wrapping into row y+1 (as opposed to a
                             natural line terminator). Used by resize reflow. */
    /* OSC 133 (semantic prompt marks) per-row metadata. Type values:
       0 = none, 'A' = prompt start, 'B' = prompt end / command edit
       start, 'C' = command output start, 'D' = command finished.
       `pexit` is exit_code+1 (so 0 = "no exit known", 1 = success,
       2..255 = failure exit codes 1..254). Stored separately from
       pmark so D's exit info survives an immediately-following A
       overwriting the same row (zsh emits both from precmd back-to-
       back without any intervening cursor motion). The arrays are
       parallel to main_wrap and sb_wrap; resize and scroll
       bookkeeping touches them in lockstep. */
    uint8_t *main_pmark;
    uint8_t *main_pexit;
    /* Exit code from the most recent OSC 133;D, encoded as
       exit_code + 1 (0 = none pending). Carried forward to the
       NEXT A mark so the prompt row inherits the previous
       command's status when D and A land on different rows. */
    uint8_t pending_pexit;
    /* Wall-clock time (seconds) when the most recent OSC 133;C was
       parsed. Used to compute command duration on D and fire a
       slow-command notification via io.notify. 0 = no command
       currently running. */
    time_t c_time;
    bool on_alt;

    // Scrollback ring buffer: rows of `cols` cells
    Cell *sb;
    uint8_t *sb_wrap; /* parallel ring, 1 byte per row: row ended by
                         auto-wrap into the next row. Mirrors main_wrap
                         so selection can cross wrap boundaries back
                         into scrollback. */
    uint8_t *sb_pmark;
    uint8_t *sb_pexit;
    int sb_cap;
    int sb_len;
    int sb_head;     // next write slot
    int view_off;    // rows scrolled up (>=0, <= sb_len)

    // Parser
    int pstate;
    int params[MAX_PARAMS];
    bool param_set[MAX_PARAMS];
    /* True when params[i] was introduced by a ':' (sub-parameter of
       the previous param), false when it was a normal ';' separator.
       Used by the SGR handler to distinguish e.g. 4:3 (curly underline)
       from 4;3 (underline + italic). */
    bool param_colon[MAX_PARAMS];
    int param_cnt;
    bool priv;       // '?' seen
    bool inter_gt;   // '>' seen  (secondary DA etc.)
    bool inter_eq;   // '=' seen  (tertiary DA)
    bool inter_sp;   // ' ' seen  (DECSCUSR cursor-style, `CSI Ps SP q`)

    /* Charset state for DEC Special Graphics / Line Drawing. tmux
       selects this via `ESC ( 0` (G0 = line drawing) and then prints
       the lowercase letters q/x/j/k/l/m/n/t/u/v/w for the lines and
       corners of its pane borders. */
    char charset_g0;      /* 'B' = US-ASCII (default), '0' = DEC graphics */
    char charset_g1;
    int  charset_active;  /* 0 = G0, 1 = G1 (toggled by SI/SO) */
    uint32_t uni_cp;
    int uni_need;

    // OSC
    char osc[512];
    int osc_len;

    /* Graphics — sixel / kitty images anchored to viewport rows. */
    ScreenImage *images[SCREEN_IMAGE_CAP];
    int          nimages;
    uint64_t     next_image_gen;
    /* Renderer tells the screen its cell pixel height so scroll
       accounting can translate pixel-heighted images into cell-rows.
       Seed at 20 so things work before any draw call. */
    int          cell_h_px;

    /* DCS/APC payload buffer for sixel (DCS) and kitty (APC). Grown
       as needed so we don't blow the stack on large images. */
    unsigned char *dpayload;
    size_t         dpayload_len;
    size_t         dpayload_cap;

    /* Kitty chunked-message accumulator. Each chunk arrives as a
       separate APC; we stitch them together here and decode on the
       final chunk (m=0 or m absent). The buffer holds a synthetic
       "G<keys-from-chunk-1>;<accumulated-base64>" blob so kitty_decode
       doesn't need to know about chunking. */
    unsigned char *kpending;
    size_t         kpending_len;
    size_t         kpending_cap;
    bool           kpending_active;

    /* OSC 8 hyperlink URL pool. Cells reference a URL via a 16-bit
       link_id (1-based; 0 = no link). Interning keeps Cell at 16 bytes
       and scrollback size bounded even with many links. Max 65535
       unique URLs per screen; new entries past that recycle slot 1
       (oldest) — cheap, effectively unbounded for real use. */
    char   **urls;
    uint16_t urls_count;
    uint32_t urls_cap;

    /* OSC 8 link id and SGR 58 underline color now live inside
       cur_attr (.link_id / .ul_color). Carrying them on Screen
       separately forced put_cp to do six discrete stores per cell;
       folding them in lets put_cp emit a single struct copy. */

    /* Per-screen palette (mutable via OSC 4/104). Kept per-screen so
       a tab/pane rewriting its colours doesn't bleed into the others. */
    uint32_t palette[256];
    /* Per-screen default colours (mutable via OSC 10/11/12). Same
       reason — a theme's default-fg/bg change shouldn't escape the
       pane that applied it. */
    uint32_t default_fg;
    uint32_t default_bg;
    uint32_t cursor_color;
    /* Cursor presentation — mutable via screen_set_cursor_style. */
    CursorStyle cursor_style;

    ScreenIO io;
};

uint32_t screen_default_fg(const Screen *s)    { return s ? s->default_fg    : SEED_DEFAULT_FG; }
uint32_t screen_default_bg(const Screen *s)    { return s ? s->default_bg    : SEED_DEFAULT_BG; }
uint32_t screen_cursor_color(const Screen *s)  { return s ? s->cursor_color  : SEED_CURSOR_COLOR; }

/* ---------- Palette (mutable — can be set by OSC 4) ---------- */

/* Seed `dst` with the standard 256-colour xterm palette: 16 system
   colours, then a 6×6×6 RGB cube (indices 16..231), then 24 grays.
   Anything OSC 4 changes will overwrite specific entries on top. */
static void palette_fill_defaults(uint32_t dst[256]) {
    static const uint32_t base[16] = {
        0x000000, 0xCC0000, 0x4E9A06, 0xC4A000,
        0x3465A4, 0x75507B, 0x06989A, 0xD3D7CF,
        0x555753, 0xEF2929, 0x8AE234, 0xFCE94F,
        0x729FCF, 0xAD7FA8, 0x34E2E2, 0xEEEEEC,
    };
    for (int i = 0; i < 16; i++) dst[i] = base[i];
    static const int lut[6] = {0, 95, 135, 175, 215, 255};
    for (int i = 16; i < 232; i++) {
        int n = i - 16;
        int r = (n / 36) % 6, gg = (n / 6) % 6, bb = n % 6;
        dst[i] = (lut[r] << 16) | (lut[gg] << 8) | lut[bb];
    }
    for (int i = 232; i < 256; i++) {
        int v = 8 + (i - 232) * 10;
        dst[i] = (v << 16) | (v << 8) | v;
    }
}

/* Bounds-checked palette lookup. Returns the seed default-fg for
   out-of-range indices so a malformed SGR can't crash. */
static uint32_t pal(const Screen *s, int i) {
    if (!s || i < 0 || i > 255) return SEED_DEFAULT_FG;
    return s->palette[i];
}

/* Hex digit → 0..15. -1 for non-hex, so callers can detect end of
   a colour spec or malformed input. */
static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
/* Parse one OSC 4 / OSC 10/11/12 colour spec. Accepts both
   "#RRGGBB" / "#RGB" / "#RRRRGGGGBBBB" and the X11 "rgb:R/G/B"
   form (with 1..4 hex digits per component, scaled to 8-bit).
   Writes 0xRRGGBB into *out and returns true on success. */
static bool parse_color_spec(const char *s, uint32_t *out) {
    if (!s || !*s) return false;
    if (*s == '#') {
        s++;
        int n = (int)strlen(s);
        if (n == 3 || n == 6 || n == 12) {
            int dig = n / 3;
            int r = 0, g = 0, b = 0;
            for (int i = 0; i < dig; i++) {
                int vr = hexval(s[i]);
                int vg = hexval(s[i + dig]);
                int vb = hexval(s[i + 2 * dig]);
                if (vr < 0 || vg < 0 || vb < 0) return false;
                r = (r << 4) | vr;
                g = (g << 4) | vg;
                b = (b << 4) | vb;
            }
            if (dig == 1) { r *= 0x11; g *= 0x11; b *= 0x11; }
            else if (dig == 4) { r >>= 8; g >>= 8; b >>= 8; }
            *out = ((uint32_t)(r & 0xff) << 16) | ((uint32_t)(g & 0xff) << 8) | (uint32_t)(b & 0xff);
            return true;
        }
        return false;
    }
    if (strncmp(s, "rgb:", 4) == 0) {
        s += 4;
        int comps[3] = {0, 0, 0};
        int ci = 0;
        int v = 0, vdig = 0;
        for (; *s && ci < 3; s++) {
            if (*s == '/') {
                if (vdig == 0) return false;
                /* Scale to 8-bit */
                int shift = (vdig - 1) * 4;
                comps[ci++] = (v >> shift) & 0xff;
                v = 0; vdig = 0;
                if (ci == 3) break;
            } else {
                int h = hexval(*s);
                if (h < 0) return false;
                v = (v << 4) | h;
                vdig++;
            }
        }
        if (ci == 2 && vdig > 0) {
            int shift = (vdig - 1) * 4;
            comps[ci++] = (v >> shift) & 0xff;
        }
        if (ci != 3) return false;
        *out = ((uint32_t)comps[0] << 16) | ((uint32_t)comps[1] << 8) | (uint32_t)comps[2];
        return true;
    }
    return false;
}

/* ---------- Helpers ---------- */

/* Translate a 7-bit ASCII code through the DEC Special Graphics set.
   Only printable codes 0x60..0x7E have interesting mappings; anything
   else passes through unchanged. */
static uint32_t dec_graphic(uint32_t cp) {
    if (cp < 0x60 || cp > 0x7E) return cp;
    static const uint32_t table[0x7F - 0x60 + 1] = {
        0x25C6, /* ` diamond            */
        0x2592, /* a checkerboard       */
        0x2409, /* b HT                 */
        0x240C, /* c FF                 */
        0x240D, /* d CR                 */
        0x240A, /* e LF                 */
        0x00B0, /* f degree             */
        0x00B1, /* g plus/minus         */
        0x2424, /* h NL                 */
        0x240B, /* i VT                 */
        0x2518, /* j ┘                  */
        0x2510, /* k ┐                  */
        0x250C, /* l ┌                  */
        0x2514, /* m └                  */
        0x253C, /* n ┼                  */
        0x23BA, /* o ⎺                  */
        0x23BB, /* p ⎻                  */
        0x2500, /* q ─                  */
        0x23BC, /* r ⎼                  */
        0x23BD, /* s ⎽                  */
        0x251C, /* t ├                  */
        0x2524, /* u ┤                  */
        0x2534, /* v ┴                  */
        0x252C, /* w ┬                  */
        0x2502, /* x │                  */
        0x2264, /* y ≤                  */
        0x2265, /* z ≥                  */
        0x03C0, /* { π                  */
        0x2260, /* | ≠                  */
        0x00A3, /* } £                  */
        0x00B7, /* ~ ·                  */
    };
    return table[cp - 0x60];
}

/* Construct a blank Cell carrying the current SGR-state fg/bg —
   so backgrounds that scroll into a cleared row keep whatever
   palette colour was active. cp=0 means "no glyph drawn here". */
static Cell blank_cell(Screen *s) {
    Cell c;
    c.cp = 0;
    c.fg = s->cur_attr.fg;
    c.bg = s->cur_attr.bg;
    c.ul_color = 0;
    c.attrs = s->cur_attr.attrs & (ATTR_DEFAULT_FG | ATTR_DEFAULT_BG);
    c.link_id = 0;
    return c;
}

/* Pick whichever cell buffer is currently being displayed (alt or
   main). All grid mutators go through this. */
static Cell *active(Screen *s) { return s->on_alt ? s->alt : s->main; }

/* ---------- Images ---------- */

/* Free a single ScreenImage's pixel data and struct. NULL-safe. */
static void image_free_one(ScreenImage *img) {
    if (!img) return;
    free(img->rgba);
    free(img);
}

/* Free every image owned by a screen. Called from screen_free and
   on hard-reset / clear-everything paths. */
static void images_free_all(Screen *s) {
    for (int i = 0; i < s->nimages; i++) image_free_one(s->images[i]);
    s->nimages = 0;
}

/* Take ownership of rgba (caller must not free). anchor_row/col are
   viewport coordinates (row 0 = top of visible grid). Returns the
   new image on success, NULL if we're full or out of memory. When
   we're at capacity, drop the oldest to make room. */
static ScreenImage *image_append(Screen *s, unsigned char *rgba,
                                 int px_w, int px_h,
                                 int anchor_row, int anchor_col) {
    if (!rgba || px_w <= 0 || px_h <= 0) { free(rgba); return NULL; }
    if (s->nimages >= SCREEN_IMAGE_CAP) {
        image_free_one(s->images[0]);
        memmove(s->images, s->images + 1,
                sizeof(s->images[0]) * (SCREEN_IMAGE_CAP - 1));
        s->nimages = SCREEN_IMAGE_CAP - 1;
    }
    ScreenImage *img = calloc(1, sizeof(*img));
    if (!img) { free(rgba); return NULL; }
    img->rgba = rgba;
    img->px_w = px_w;
    img->px_h = px_h;
    img->anchor_row = anchor_row;
    img->anchor_col = anchor_col;
    img->gen = ++s->next_image_gen;
    img->on_alt = s->on_alt;
    s->images[s->nimages++] = img;
    return img;
}

/* Drop every image whose `on_alt` flag equals `which`. Used on alt/main
   transitions and on CSI 2J / CSI 3J clears. */
static void images_drop_where(Screen *s, bool which) {
    for (int i = 0; i < s->nimages; ) {
        if (s->images[i]->on_alt == which) {
            image_free_one(s->images[i]);
            memmove(s->images + i, s->images + i + 1,
                    sizeof(s->images[0]) * (s->nimages - i - 1));
            s->nimages--;
            continue;
        }
        i++;
    }
}

/* Move every image's anchor_row by -dy rows (viewport scrolled up).
   When an image fully scrolls above row 0 it's dropped — rbterm
   doesn't persist images into scrollback (v1 simplification). */
static void images_scroll(Screen *s, int dy) {
    if (dy == 0 || s->nimages == 0) return;
    int ch = s->cell_h_px > 0 ? s->cell_h_px : 20;
    for (int i = 0; i < s->nimages; ) {
        ScreenImage *img = s->images[i];
        img->anchor_row -= dy;
        int h_rows = (img->px_h + ch - 1) / ch;
        if (img->anchor_row + h_rows <= 0) {
            image_free_one(img);
            memmove(s->images + i, s->images + i + 1,
                    sizeof(s->images[0]) * (s->nimages - i - 1));
            s->nimages--;
            continue;
        }
        i++;
    }
}

static Cell *row_ptr(Screen *s, int y) { return active(s) + y * s->cols; }

static void clear_row(Screen *s, int y) {
    Cell b = blank_cell(s);
    Cell *r = row_ptr(s, y);
    for (int x = 0; x < s->cols; x++) r[x] = b;
    if (!s->on_alt && y >= 0 && y < s->rows) {
        if (s->main_wrap)  s->main_wrap[y]  = 0;
        if (s->main_pmark) s->main_pmark[y] = 0;
        if (s->main_pexit) s->main_pexit[y] = 0;
    }
}

static void push_scrollback(Screen *s, const Cell *row, bool wrapped,
                            uint8_t pmark, uint8_t pexit) {
    if (s->on_alt || s->sb_cap == 0) return;
    memcpy(s->sb + s->sb_head * s->cols, row, sizeof(Cell) * s->cols);
    if (s->sb_wrap)  s->sb_wrap[s->sb_head]  = wrapped ? 1 : 0;
    if (s->sb_pmark) s->sb_pmark[s->sb_head] = pmark;
    if (s->sb_pexit) s->sb_pexit[s->sb_head] = pexit;
    s->sb_head = (s->sb_head + 1) % s->sb_cap;
    if (s->sb_len < s->sb_cap) s->sb_len++;
    // Keep view anchored if we're at bottom; otherwise drift one row.
    if (s->view_off > 0 && s->view_off < s->sb_len) s->view_off++;
    if (s->view_off > s->sb_len) s->view_off = s->sb_len;
}

static void scroll_up_region(Screen *s, int top, int bot, int n) {
    if (n <= 0) return;
    int span = bot - top + 1;
    if (n > span) n = span;
    Cell *base = active(s);
    if (!s->on_alt && top == 0) {
        for (int i = 0; i < n; i++) {
            bool w  = s->main_wrap  ? s->main_wrap[top + i] != 0 : false;
            uint8_t pm = s->main_pmark ? s->main_pmark[top + i] : 0;
            uint8_t pe = s->main_pexit ? s->main_pexit[top + i] : 0;
            push_scrollback(s, base + (top + i) * s->cols, w, pm, pe);
        }
        images_scroll(s, n);
    }
    if (span - n > 0) {
        memmove(base + top * s->cols,
                base + (top + n) * s->cols,
                sizeof(Cell) * (span - n) * s->cols);
        if (!s->on_alt && s->main_wrap) {
            memmove(s->main_wrap + top,
                    s->main_wrap + top + n,
                    (size_t)(span - n));
        }
        if (!s->on_alt && s->main_pmark) {
            memmove(s->main_pmark + top,
                    s->main_pmark + top + n,
                    (size_t)(span - n));
        }
        if (!s->on_alt && s->main_pexit) {
            memmove(s->main_pexit + top,
                    s->main_pexit + top + n,
                    (size_t)(span - n));
        }
    }
    for (int i = 0; i < n; i++) clear_row(s, bot - i);
}

static void scroll_down_region(Screen *s, int top, int bot, int n) {
    if (n <= 0) return;
    int span = bot - top + 1;
    if (n > span) n = span;
    Cell *base = active(s);
    if (span - n > 0) {
        memmove(base + (top + n) * s->cols,
                base + top * s->cols,
                sizeof(Cell) * (span - n) * s->cols);
        if (!s->on_alt && s->main_wrap) {
            memmove(s->main_wrap + top + n,
                    s->main_wrap + top,
                    (size_t)(span - n));
        }
        if (!s->on_alt && s->main_pmark) {
            memmove(s->main_pmark + top + n,
                    s->main_pmark + top,
                    (size_t)(span - n));
        }
        if (!s->on_alt && s->main_pexit) {
            memmove(s->main_pexit + top + n,
                    s->main_pexit + top,
                    (size_t)(span - n));
        }
    }
    for (int i = 0; i < n; i++) clear_row(s, top + i);
}

/* Move the cursor down one row. If we're on the last row of the
   active scroll region, scroll the region up by one (pushing the
   top row to scrollback for the main screen). */
static void newline(Screen *s) {
    if (s->cy == s->scroll_bot) {
        scroll_up_region(s, s->scroll_top, s->scroll_bot, 1);
    } else if (s->cy < s->rows - 1) {
        s->cy++;
    }
}

/* Crude wcwidth — conservative: only codepoints that are *always* wide
   are flagged. Extend as needed. */
static bool cp_is_wide(uint32_t cp) {
    if (cp < 0x1100) return false;
    if (cp >= 0x1100  && cp <= 0x115F)  return true;   /* Hangul Jamo */
    if (cp >= 0x2E80  && cp <= 0x303E)  return true;   /* CJK Radicals, Kangxi */
    if (cp >= 0x3041  && cp <= 0x33FF)  return true;   /* Hiragana..CJK Compat */
    if (cp >= 0x3400  && cp <= 0x4DBF)  return true;   /* CJK Ext A */
    if (cp >= 0x4E00  && cp <= 0x9FFF)  return true;   /* CJK Unified */
    if (cp >= 0xA000  && cp <= 0xA4CF)  return true;   /* Yi */
    if (cp >= 0xAC00  && cp <= 0xD7A3)  return true;   /* Hangul Syllables */
    if (cp >= 0xF900  && cp <= 0xFAFF)  return true;   /* CJK Compat */
    if (cp >= 0xFE30  && cp <= 0xFE4F)  return true;   /* CJK Compat Forms */
    if (cp >= 0xFF00  && cp <= 0xFF60)  return true;   /* Fullwidth */
    if (cp >= 0xFFE0  && cp <= 0xFFE6)  return true;
    if (cp >= 0x1F300 && cp <= 0x1F64F) return true;   /* emoji */
    if (cp >= 0x1F680 && cp <= 0x1F6FF) return true;
    if (cp >= 0x1F700 && cp <= 0x1F77F) return true;
    if (cp >= 0x1F780 && cp <= 0x1F7FF) return true;
    if (cp >= 0x1F800 && cp <= 0x1F8FF) return true;
    if (cp >= 0x1F900 && cp <= 0x1F9FF) return true;
    if (cp >= 0x1FA00 && cp <= 0x1FAFF) return true;
    if (cp >= 0x20000 && cp <= 0x2FFFD) return true;   /* CJK Ext B..F */
    if (cp >= 0x30000 && cp <= 0x3FFFD) return true;   /* CJK Ext G */
    return false;
}

/* Write one codepoint into the cell at the cursor position with
   the current SGR attrs / OSC 8 link / SGR 58 underline colour.
   Handles auto-wrap-on-write (deferred from the previous put_cp
   if the cursor was already at column cols-1), wide-character
   handling (writes both the head cell with ATTR_WIDE and the
   continuation cell with ATTR_WIDE_CONT), and DEC Special Graphics
   charset translation when SI/SO has selected G0=0. Resets
   view_off so any new output rolls the viewport back to the live
   bottom. */
static void put_cp(Screen *s, uint32_t cp) {
    /* Apply the active charset mapping before any width/wrap logic. */
    char active = (s->charset_active == 1) ? s->charset_g1 : s->charset_g0;
    if (active == '0' && cp >= 0x20 && cp < 0x7F) cp = dec_graphic(cp);
    bool wide = cp_is_wide(cp);
    if (s->cx >= s->cols) s->cx = s->cols - 1;
    if (s->wrap_next && s->autowrap) {
        /* Mark the current row as having auto-wrapped so that a later
           resize can rejoin it with its continuation. */
        if (!s->on_alt && s->main_wrap && s->cy >= 0 && s->cy < s->rows)
            s->main_wrap[s->cy] = 1;
        s->cx = 0;
        newline(s);
        s->wrap_next = false;
    }
    if (wide && s->cx + 1 >= s->cols) {
        if (s->autowrap) {
            s->cx = 0;
            newline(s);
            s->wrap_next = false;
        } else {
            /* No room and no autowrap: drop. */
            return;
        }
    }
    /* Build the cell on the stack, then copy as one struct write.
       cur_attr already carries fg/bg/ul_color/attrs/link_id, so the
       per-cell setup is just stamping cp + the wide flag. The
       compiler lowers `*c = t` to two stores (16 + 4 bytes) instead
       of the six it'd emit for individual field assignments. */
    Cell *row = row_ptr(s, s->cy);
    Cell *c = row + s->cx;
    Cell t = s->cur_attr;
    t.cp = cp;
    if (wide) t.attrs |= ATTR_WIDE;
    *c = t;
    if (wide) {
        Cell *c2 = row + s->cx + 1;
        t.cp = 0;
        t.attrs = (uint16_t)((t.attrs & ~ATTR_WIDE) | ATTR_WIDE_CONT);
        *c2 = t;
    }
    int advance = wide ? 2 : 1;
    int next = s->cx + advance;
    if (next >= s->cols) {
        s->cx = s->cols - 1;
        s->wrap_next = true;
    } else {
        s->cx = next;
    }
    s->view_off = 0;
}

/* Generic int clamp helper used throughout the parser. */
static int clamp(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Wipe the CSI parameter buffer + private/intermediate flags +
   the OSC payload length. Called at the start of each new
   sequence and on parser-state transitions. */
static void reset_params(Screen *s) {
    for (int i = 0; i < MAX_PARAMS; i++) {
        s->params[i] = 0;
        s->param_set[i] = false;
        s->param_colon[i] = false;
    }
    s->param_cnt = 0;
    s->priv = false;
    s->inter_gt = false;
    s->inter_eq = false;
    s->inter_sp = false;
    s->osc_len = 0;
}

/* Read parameter `i` from the current CSI, falling back to `dflt`
   when omitted. Most VT controls use this so missing params take
   their per-spec defaults (usually 1 — "move 1 cell" etc.). */
static int pget(Screen *s, int i, int dflt) {
    if (i >= s->param_cnt || !s->param_set[i]) return dflt;
    return s->params[i];
}

/* ---------- SGR ---------- */

/* Helper for sub-param consumption — returns true and fills *out_sub
   when params[i+1] is a colon-introduced sub-param. */
static bool sgr_has_sub(Screen *s, int i, int n, int *out_sub) {
    if (i + 1 >= n) return false;
    if (!s->param_colon[i + 1]) return false;
    *out_sub = s->params[i + 1];
    return true;
}

/* Update the 3-bit underline-style field embedded in cur_attr.attrs
   without disturbing the other flag bits. */
static void set_underline_style(Screen *s, UlStyle st) {
    s->cur_attr.attrs = (uint16_t)((s->cur_attr.attrs & ~ATTR_UL_STYLE_MASK)
                                   | ((uint16_t)st << ATTR_UL_STYLE_SHIFT));
}

/* Apply one CSI SGR (Select Graphic Rendition) — the catch-all
   styling sequence. Walks the parameter list updating
   s->cur_attr (used by subsequent put_cp calls). Handles both the
   classic 30..37/90..97 palette indices, 38/48 with sub-arg colour
   space (5;N for indexed, 2;R;G;B for true colour), the 4:N
   underline-style sub-form, and 58 underline colour. Anything we
   don't recognise is silently skipped. */
static void sgr(Screen *s) {
    /* Switch dispatch on the param value lets the compiler emit a
       jump table for the dense ranges (0..9, 21..29, 30..49, 58..59,
       90..107) instead of a sequential if/else chain. Hot path on
       benchmarks that flood per-cell SGR (vtebench dense_cells:
       9-param SGR + 1 char per cell). */
    int n = s->param_cnt ? s->param_cnt : 1;
    for (int i = 0; i < n; i++) {
        /* If this entry is itself a colon sub-param, it was consumed
           by the previous iteration — skip. (Only relevant when the
           sub-param wasn't recognised; else the consumer already
           advanced past it.) */
        if (s->param_colon[i]) continue;
        int p = s->param_set[i] ? s->params[i] : 0;
        switch (p) {
        case 0:
            s->cur_attr.fg = s->default_fg;
            s->cur_attr.bg = s->default_bg;
            s->cur_attr.attrs = ATTR_DEFAULT_FG | ATTR_DEFAULT_BG;
            s->cur_attr.ul_color = 0;
            break;
        case 1: s->cur_attr.attrs |= ATTR_BOLD;   break;
        case 2: s->cur_attr.attrs |= ATTR_DIM;    break;
        case 3: s->cur_attr.attrs |= ATTR_ITALIC; break;
        case 4: {
            /* SGR 4 — plain underline OR 4:N for style variant. */
            int sub;
            if (sgr_has_sub(s, i, n, &sub)) {
                if (sub == 0) {
                    s->cur_attr.attrs &= ~(ATTR_UNDERLINE | ATTR_UL_STYLE_MASK);
                } else {
                    UlStyle st = (sub >= 1 && sub <= 5) ? (UlStyle)(sub - 1)
                                                        : UL_STYLE_SINGLE;
                    s->cur_attr.attrs |= ATTR_UNDERLINE;
                    set_underline_style(s, st);
                }
                while (i + 1 < n && s->param_colon[i + 1]) i++;
            } else {
                s->cur_attr.attrs |= ATTR_UNDERLINE;
                set_underline_style(s, UL_STYLE_SINGLE);
            }
            break;
        }
        case 7: s->cur_attr.attrs |= ATTR_REVERSE; break;
        case 8: s->cur_attr.attrs |= ATTR_HIDDEN;  break;
        case 9: s->cur_attr.attrs |= ATTR_STRIKE;  break;
        case 21:
            s->cur_attr.attrs |= ATTR_UNDERLINE;
            set_underline_style(s, UL_STYLE_DOUBLE);
            break;
        case 22: s->cur_attr.attrs &= ~(ATTR_BOLD | ATTR_DIM); break;
        case 23: s->cur_attr.attrs &= ~ATTR_ITALIC;            break;
        case 24: s->cur_attr.attrs &= ~(ATTR_UNDERLINE | ATTR_UL_STYLE_MASK); break;
        case 27: s->cur_attr.attrs &= ~ATTR_REVERSE; break;
        case 28: s->cur_attr.attrs &= ~ATTR_HIDDEN;  break;
        case 29: s->cur_attr.attrs &= ~ATTR_STRIKE;  break;
        case 30: case 31: case 32: case 33:
        case 34: case 35: case 36: case 37:
            /* 16-color fg. Store the palette INDEX and flag it;
               render path resolves the current RGB each frame so
               OSC 4 palette changes retro-actively recolour existing cells. */
            s->cur_attr.fg = (uint32_t)(p - 30);
            s->cur_attr.attrs = (uint16_t)((s->cur_attr.attrs & ~ATTR_DEFAULT_FG) | ATTR_FG_INDEX);
            break;
        case 38:
        case 48: {
            bool is_fg = (p == 38);
            if (i + 1 < n && s->params[i + 1] == 5 && i + 2 < n) {
                /* 256-color: store index, flag it. */
                uint32_t idx = (uint32_t)(s->params[i + 2] & 0xff);
                if (is_fg) {
                    s->cur_attr.fg = idx;
                    s->cur_attr.attrs = (uint16_t)((s->cur_attr.attrs & ~ATTR_DEFAULT_FG) | ATTR_FG_INDEX);
                } else {
                    s->cur_attr.bg = idx;
                    s->cur_attr.attrs = (uint16_t)((s->cur_attr.attrs & ~ATTR_DEFAULT_BG) | ATTR_BG_INDEX);
                }
                i += 2;
            } else if (i + 1 < n && s->params[i + 1] == 2 && i + 4 < n) {
                /* 24-bit truecolor: store the RGB literal, clear index flag. */
                uint32_t r = s->params[i + 2] & 0xff;
                uint32_t g = s->params[i + 3] & 0xff;
                uint32_t b = s->params[i + 4] & 0xff;
                uint32_t c = (r << 16) | (g << 8) | b;
                if (is_fg) {
                    s->cur_attr.fg = c;
                    s->cur_attr.attrs = (uint16_t)(s->cur_attr.attrs & ~(ATTR_DEFAULT_FG | ATTR_FG_INDEX));
                } else {
                    s->cur_attr.bg = c;
                    s->cur_attr.attrs = (uint16_t)(s->cur_attr.attrs & ~(ATTR_DEFAULT_BG | ATTR_BG_INDEX));
                }
                i += 4;
            }
            break;
        }
        case 39:
            s->cur_attr.fg = s->default_fg;
            s->cur_attr.attrs = (uint16_t)((s->cur_attr.attrs | ATTR_DEFAULT_FG) & ~ATTR_FG_INDEX);
            break;
        case 40: case 41: case 42: case 43:
        case 44: case 45: case 46: case 47:
            s->cur_attr.bg = (uint32_t)(p - 40);
            s->cur_attr.attrs = (uint16_t)((s->cur_attr.attrs & ~ATTR_DEFAULT_BG) | ATTR_BG_INDEX);
            break;
        case 49:
            s->cur_attr.bg = s->default_bg;
            s->cur_attr.attrs = (uint16_t)((s->cur_attr.attrs | ATTR_DEFAULT_BG) & ~ATTR_BG_INDEX);
            break;
        case 58:
            /* Explicit underline color. Both 58:2:R:G:B / 58;2;R;G;B
               (truecolor) and 58:5:N / 58;5;N (256). Sub-params come
               via colon (modern) or semicolon (legacy xterm). */
            if (i + 1 < n && s->params[i + 1] == 2 && i + 4 < n) {
                uint32_t r = s->params[i + 2] & 0xff;
                uint32_t g = s->params[i + 3] & 0xff;
                uint32_t b = s->params[i + 4] & 0xff;
                s->cur_attr.ul_color = (r << 16) | (g << 8) | b;
                if (s->cur_attr.ul_color == 0) s->cur_attr.ul_color = 0x010101u;
                i += 4;
            } else if (i + 1 < n && s->params[i + 1] == 5 && i + 2 < n) {
                int idx = s->params[i + 2] & 0xff;
                s->cur_attr.ul_color = pal(s, idx);
                if (s->cur_attr.ul_color == 0) s->cur_attr.ul_color = 0x010101u;
                i += 2;
            }
            while (i + 1 < n && s->param_colon[i + 1]) i++;
            break;
        case 59:
            /* Reset underline color — fall back to fg. */
            s->cur_attr.ul_color = 0;
            break;
        case 90: case 91: case 92: case 93:
        case 94: case 95: case 96: case 97:
            s->cur_attr.fg = (uint32_t)(p - 90 + 8);
            s->cur_attr.attrs = (uint16_t)((s->cur_attr.attrs & ~ATTR_DEFAULT_FG) | ATTR_FG_INDEX);
            break;
        case 100: case 101: case 102: case 103:
        case 104: case 105: case 106: case 107:
            s->cur_attr.bg = (uint32_t)(p - 100 + 8);
            s->cur_attr.attrs = (uint16_t)((s->cur_attr.attrs & ~ATTR_DEFAULT_BG) | ATTR_BG_INDEX);
            break;
        }
    }
}

uint32_t screen_palette(const Screen *s, int i) { return pal(s, i); }

/* ---------- OSC 8 URL intern pool ---------- */

/* Find an existing URL or add a new one. Returns the 1-based id or 0
   on failure / empty URL. Strings are owned by the pool. */
static uint16_t url_intern(Screen *s, const char *url) {
    if (!url || !*url) return 0;
    for (uint16_t i = 0; i < s->urls_count; i++) {
        if (s->urls[i] && strcmp(s->urls[i], url) == 0) return (uint16_t)(i + 1);
    }
    if (s->urls_count >= 65535) {
        /* Recycle slot 0 rather than refusing; keeps runaway link
           spam from silently dropping after 64K distinct URLs. */
        free(s->urls[0]);
        s->urls[0] = strdup(url);
        return 1;
    }
    if (s->urls_count + 1 > s->urls_cap) {
        uint32_t nc = s->urls_cap ? s->urls_cap * 2 : 16;
        char **nb = realloc(s->urls, nc * sizeof(char *));
        if (!nb) return 0;
        s->urls = nb;
        s->urls_cap = nc;
    }
    char *dup = strdup(url);
    if (!dup) return 0;
    s->urls[s->urls_count++] = dup;
    return s->urls_count;   /* 1-based — links_count after increment */
}

/* Look up an OSC 8 hyperlink target by its 1-based link_id. Returns
   the interned URL string (NUL-terminated, owned by the screen) or
   NULL for out-of-range / no-link cells. */
const char *screen_link_url(const Screen *s, uint16_t link_id) {
    if (!s || link_id == 0 || link_id > s->urls_count) return NULL;
    return s->urls[link_id - 1];
}

/* ---------- Erase ---------- */

/* Implement CSI Ps J — erase in display. Modes:
     0 = cursor to end of screen
     1 = start of screen to cursor
     2/3 = whole screen (and drop any sixel/kitty images on it).
   Existing cells are replaced with blanks carrying the current
   SGR-state bg, so a coloured background scrolls in cleanly. */
static void erase_in_display(Screen *s, int mode) {
    Cell b = blank_cell(s);
    if (mode == 0) {
        // from cursor to end
        Cell *row = row_ptr(s, s->cy);
        for (int x = s->cx; x < s->cols; x++) row[x] = b;
        for (int y = s->cy + 1; y < s->rows; y++) clear_row(s, y);
    } else if (mode == 1) {
        for (int y = 0; y < s->cy; y++) clear_row(s, y);
        Cell *row = row_ptr(s, s->cy);
        for (int x = 0; x <= s->cx && x < s->cols; x++) row[x] = b;
    } else if (mode == 2 || mode == 3) {
        for (int y = 0; y < s->rows; y++) clear_row(s, y);
        /* Ctrl-L / `clear` also wipes out any sixel/kitty images on
           the current screen. Matches xterm/iTerm/kitty. */
        images_drop_where(s, s->on_alt);
    }
}

/* Implement CSI Ps K — erase in line. 0 = cursor→eol, 1 = bol→
   cursor, 2 = whole row. */
static void erase_in_line(Screen *s, int mode) {
    Cell b = blank_cell(s);
    Cell *row = row_ptr(s, s->cy);
    if (mode == 0) {
        for (int x = s->cx; x < s->cols; x++) row[x] = b;
    } else if (mode == 1) {
        for (int x = 0; x <= s->cx && x < s->cols; x++) row[x] = b;
    } else if (mode == 2) {
        for (int x = 0; x < s->cols; x++) row[x] = b;
    }
}

/* ---------- DEC modes ---------- */

/* Apply DECSET/DECRST (CSI ? Pn h / l) for one private mode. We
   only handle the modes rbterm actually consumes — most are
   feature flags (app-cursor, autowrap, mouse reporting, focus
   reports, bracketed paste, alt screen, sync updates). Unknown
   modes are silently ignored. */
static void set_mode(Screen *s, int p, bool on) {
    if (!s->priv) return;
    switch (p) {
    case 1:    s->app_cursor = on; break;
    case 6:    s->origin_mode = on; s->cx = 0; s->cy = on ? s->scroll_top : 0; break;
    case 7:    s->autowrap = on; break;
    case 25:   s->cursor_visible = on; break;
    case 1000: s->mouse_mode = on ? 1000 : 0; break;
    case 1002: s->mouse_mode = on ? 1002 : 0; break;
    case 1003: s->mouse_mode = on ? 1003 : 0; break;
    case 1004: s->focus_report = on; break;
    case 1006: s->mouse_sgr = on; break;
    case 1015: /* urxvt mouse encoding — unused */ break;
    case 2026: s->sync_update = on; break;  /* synchronized updates */
    case 1049: {
        if (on && !s->on_alt) {
            s->saved_cx = s->cx; s->saved_cy = s->cy;
            s->on_alt = true;
            for (int y = 0; y < s->rows; y++) clear_row(s, y);
            s->cx = 0; s->cy = 0;
            /* Entering alt — drop any stale alt-tagged images left
               over from a previous fullscreen app (shouldn't exist
               normally, but cheap insurance). */
            images_drop_where(s, true);
        } else if (!on && s->on_alt) {
            s->on_alt = false;
            s->cx = s->saved_cx; s->cy = s->saved_cy;
            /* Leaving alt — drop anything the alt-screen app drew.
               Images tagged main survive and reappear automatically. */
            images_drop_where(s, true);
        }
        s->wrap_next = false;
        break;
    }
    case 2004: s->bracketed_paste = on; break;
    }
}

/* ---------- CSI ---------- */

/* Drive the CSI state machine one byte at a time. Accumulates
   parameter digits, tracks separators (';' for new param, ':' for
   sub-param) and intermediates ('?' '>' '=' SP), then dispatches
   on the final byte to apply the actual operation (cursor moves,
   erases, scroll regions, mode sets, SGR, the lot). Resets to
   ground when the operation finishes. */
static void handle_csi(Screen *s, uint8_t b) {
    if (b == '?') { s->priv = true; return; }
    if (b == '>') { s->inter_gt = true; return; }
    if (b == '=') { s->inter_eq = true; return; }
    if (b >= '0' && b <= '9') {
        if (s->param_cnt == 0) s->param_cnt = 1;
        int i = s->param_cnt - 1;
        s->params[i] = s->params[i] * 10 + (b - '0');
        s->param_set[i] = true;
        return;
    }
    if (b == ';' || b == ':') {
        if (s->param_cnt < MAX_PARAMS) {
            s->param_cnt++;
            /* The new (currently empty) param's colon flag tells the
               SGR handler "this is a sub-param of the previous one". */
            if (b == ':' && s->param_cnt > 0) {
                s->param_colon[s->param_cnt - 1] = true;
            }
        }
        return;
    }
    /* Track ' ' (0x20) as a DECSCUSR intermediate so the `q` final byte
       below can distinguish "cursor style" from other 'q' semantics. */
    if (b == ' ') { s->inter_sp = true; return; }
    if (b < 0x40) return; // other intermediate bytes: ignore for now
    // Final byte.
    int p0 = pget(s, 0, 0);
    switch (b) {
    case 'A': s->cy = clamp(s->cy - (p0 ? p0 : 1), 0, s->rows - 1); s->wrap_next = false; break;
    case 'B': case 'e': s->cy = clamp(s->cy + (p0 ? p0 : 1), 0, s->rows - 1); s->wrap_next = false; break;
    case 'C': case 'a': s->cx = clamp(s->cx + (p0 ? p0 : 1), 0, s->cols - 1); s->wrap_next = false; break;
    case 'D': s->cx = clamp(s->cx - (p0 ? p0 : 1), 0, s->cols - 1); s->wrap_next = false; break;
    case 'E': s->cy = clamp(s->cy + (p0 ? p0 : 1), 0, s->rows - 1); s->cx = 0; s->wrap_next = false; break;
    case 'F': s->cy = clamp(s->cy - (p0 ? p0 : 1), 0, s->rows - 1); s->cx = 0; s->wrap_next = false; break;
    case 'G': case '`': s->cx = clamp((p0 ? p0 : 1) - 1, 0, s->cols - 1); s->wrap_next = false; break;
    case 'H': case 'f': {
        int r = pget(s, 0, 1);
        int c = pget(s, 1, 1);
        s->cy = clamp(r - 1, 0, s->rows - 1);
        s->cx = clamp(c - 1, 0, s->cols - 1);
        s->wrap_next = false;
        break;
    }
    case 'J': erase_in_display(s, p0); break;
    case 'K': erase_in_line(s, p0); break;
    case 'L': scroll_down_region(s, s->cy, s->scroll_bot, p0 ? p0 : 1); break;
    case 'M': scroll_up_region(s, s->cy, s->scroll_bot, p0 ? p0 : 1); break;
    case 'P': { // DCH: delete chars
        int n = p0 ? p0 : 1;
        if (n > s->cols - s->cx) n = s->cols - s->cx;
        Cell *row = row_ptr(s, s->cy);
        memmove(row + s->cx, row + s->cx + n, sizeof(Cell) * (s->cols - s->cx - n));
        Cell b2 = blank_cell(s);
        for (int x = s->cols - n; x < s->cols; x++) row[x] = b2;
        break;
    }
    case '@': { // ICH: insert chars
        int n = p0 ? p0 : 1;
        if (n > s->cols - s->cx) n = s->cols - s->cx;
        Cell *row = row_ptr(s, s->cy);
        memmove(row + s->cx + n, row + s->cx, sizeof(Cell) * (s->cols - s->cx - n));
        Cell b2 = blank_cell(s);
        for (int x = s->cx; x < s->cx + n; x++) row[x] = b2;
        break;
    }
    case 'X': { // ECH: erase chars
        int n = p0 ? p0 : 1;
        if (n > s->cols - s->cx) n = s->cols - s->cx;
        Cell *row = row_ptr(s, s->cy);
        Cell b2 = blank_cell(s);
        for (int x = s->cx; x < s->cx + n; x++) row[x] = b2;
        break;
    }
    case 'S': scroll_up_region(s, s->scroll_top, s->scroll_bot, p0 ? p0 : 1); break;
    case 'T': scroll_down_region(s, s->scroll_top, s->scroll_bot, p0 ? p0 : 1); break;
    case 'd': s->cy = clamp((p0 ? p0 : 1) - 1, 0, s->rows - 1); s->wrap_next = false; break;
    case 'h': for (int i = 0; i < s->param_cnt; i++) set_mode(s, s->params[i], true); break;
    case 'l': for (int i = 0; i < s->param_cnt; i++) set_mode(s, s->params[i], false); break;
    case 'm': sgr(s); break;
    case 'r': {
        int top = pget(s, 0, 1);
        int bot = pget(s, 1, s->rows);
        if (top < 1) top = 1;
        if (bot > s->rows) bot = s->rows;
        if (top >= bot) { top = 1; bot = s->rows; }
        s->scroll_top = top - 1;
        s->scroll_bot = bot - 1;
        s->cx = 0; s->cy = 0;
        break;
    }
    case 'n':
        if (p0 == 6 && s->io.write) {
            char buf[32];
            int n = snprintf(buf, sizeof(buf), "\x1b[%d;%dR", s->cy + 1, s->cx + 1);
            s->io.write(s->io.user, (const uint8_t *)buf, n);
        }
        break;
    case 'c': {
        /* Device Attributes. Three flavours, distinguished by the CSI
           intermediate byte:
             CSI  c     — Primary DA   → `CSI ? 65 ; 1 ; 9 c` (VT520 + printer + UTF-8)
             CSI >c     — Secondary DA → `CSI > 0 ; 95 ; 0 c` (firmware 95, matches xterm)
             CSI =c     — Tertiary DA  → ignored (some terminals reply "rbTM\\")
           Answering Primary-DA to a Secondary-DA query made tmux's
           inner shell display the reply literally. */
        if (!s->io.write) break;
        if (s->inter_gt) {
            const char *resp = "\x1b[>0;95;0c";
            s->io.write(s->io.user, (const uint8_t *)resp, strlen(resp));
        } else if (s->inter_eq) {
            /* Intentionally silent — xterm responds with a DCS id which
               some shells also mis-parse. */
        } else {
            /* Advertise sixel support via "4" so img2sixel / ranger /
               gnuplot pick rbterm as a sixel-capable terminal:
               65 = VT525, 1 = 132-col, 4 = sixel, 9 = NRCS. */
            const char *resp = "\x1b[?65;1;4;9c";
            s->io.write(s->io.user, (const uint8_t *)resp, strlen(resp));
        }
        break;
    }
    case 's': s->saved_cx = s->cx; s->saved_cy = s->cy; break;
    case 'u': s->cx = s->saved_cx; s->cy = s->saved_cy; break;
    case 'q':
        /* DECSCUSR — "CSI Ps SP q" sets the cursor style. We only
           accept the form with a SPACE intermediate so plain `q`
           (reserved) doesn't misfire. Values follow xterm:
             0 / 1 = blinking block (our DEFAULT)
             2     = steady block
             3     = blinking underline
             4     = steady underline  (same as 3 for us — no blink distinction yet)
             5     = blinking bar
             6     = steady bar        (same as 5 similarly) */
        if (s->inter_sp) {
            CursorStyle st = CURSOR_STYLE_DEFAULT;
            switch (p0) {
            case 0: case 1: st = CURSOR_STYLE_BLOCK_BLINK; break;
            case 2:         st = CURSOR_STYLE_BLOCK;       break;
            case 3: case 4: st = CURSOR_STYLE_UNDERLINE;   break;
            case 5: case 6: st = CURSOR_STYLE_BAR;         break;
            }
            s->cursor_style = st;
        }
        break;
    default: break;
    }
    s->pstate = ST_GROUND;
}

/* ---------- ESC ---------- */

/* Called when the OSC string parser has just consumed the
   terminator (ESC \ or BEL). Parses the leading numeric "Ps" out
   of s->osc, then dispatches to whichever OSC code (0/2 title,
   4 palette, 7 cwd, 8 hyperlink, 9/777 notification, 10/11/12
   default colours, 52 clipboard, 104 palette reset, 133 prompt
   marks). Unknown codes are silently dropped. */
static void finish_osc(Screen *s) {
    s->osc[s->osc_len] = 0;
    int ps = 0;
    const char *p = s->osc;
    while (*p && *p != ';') { if (*p >= '0' && *p <= '9') ps = ps * 10 + (*p - '0'); p++; }
    if (*p == ';') p++;

    if (ps == 0 || ps == 2) {
        if (s->io.set_title) s->io.set_title(s->io.user, p);
        return;
    }
    if (ps == 4) {
        /* OSC 4;N;spec[;N;spec...] — set one or more palette entries
           on this screen only. */
        while (*p) {
            int idx = 0; bool any = false;
            while (*p >= '0' && *p <= '9') { idx = idx * 10 + (*p - '0'); any = true; p++; }
            if (!any || *p != ';') break;
            p++;
            char spec[128]; int si = 0;
            while (*p && *p != ';' && si + 1 < (int)sizeof(spec)) spec[si++] = *p++;
            spec[si] = 0;
            uint32_t col;
            if (idx >= 0 && idx < 256 && parse_color_spec(spec, &col)) {
                s->palette[idx] = col;
            }
            if (*p == ';') p++; else break;
        }
        return;
    }
    if (ps == 8) {
        /* OSC 8 ; <params> ; <URL> — start or end a hyperlink. An
           empty URL closes the current link. Params (e.g. id=foo) are
           ignored; we just set cur_link_id so subsequent put_cp stamps
           cells with the link. */
        /* p currently points just past the first ';'. Skip params,
           find the second ';' — whatever follows it (possibly empty)
           is the URL. */
        const char *q = p;
        while (*q && *q != ';') q++;
        if (*q == ';') q++;
        const char *url = q;
        if (!*url) {
            s->cur_attr.link_id = 0;
        } else {
            s->cur_attr.link_id = url_intern(s, url);
        }
        return;
    }
    if (ps == 133) {
        /* OSC 133 — semantic prompt marks (FinalTerm / iTerm2 spec).
           Format: OSC 133 ; X [ ; <args> ] ST  where X is one of:
              A — prompt about to draw (row above is end of prev cmd)
              B — prompt finished, user is editing
              C — Enter pressed, command output starts
              D [;<exit_code>] — command finished
           We record the mark on the current cursor row of the main
           screen so the gutter renderer can show success/fail badges
           and future passes can do prompt-to-prompt navigation. Alt
           screen marks are dropped (vim/less don't run the shell's
           prompt hooks anyway). */
        if (!s->on_alt && s->main_pmark && s->cy >= 0 && s->cy < s->rows) {
            char kind = *p ? *p : 0;
            if (kind == 'A' || kind == 'B' || kind == 'C' || kind == 'D') {
                if (kind == 'C') {
                    s->main_pmark[s->cy] = 'C';
                    s->c_time = time(NULL);
                } else if (kind == 'D') {
                    const char *q = p + 1;
                    if (*q == ';') q++;
                    int v = 0;
                    while (*q >= '0' && *q <= '9') { v = v * 10 + (*q - '0'); q++; }
                    if (v < 0)   v = 0;
                    if (v > 254) v = 254;
                    uint8_t enc = (uint8_t)(v + 1);  /* 1 = exit 0; >1 = failure */
                    s->main_pmark[s->cy] = 'D';
                    s->main_pexit[s->cy] = enc;
                    s->pending_pexit     = enc;
                    /* Command finished — clear the "running" timer so
                       the tab-bar spinner stops. */
                    s->c_time = 0;
                } else if (kind == 'A') {
                    s->main_pmark[s->cy] = 'A';
                    /* Carry the previous D's exit status onto the prompt row.
                       If the previous A already consumed it, leave whatever
                       was there (likely from a same-row D earlier this hook). */
                    if (s->pending_pexit > 0) {
                        s->main_pexit[s->cy] = s->pending_pexit;
                        s->pending_pexit = 0;
                    }
                } else {
                    /* B: prompt-end, command-edit area. Doesn't
                       disturb pexit. */
                    s->main_pmark[s->cy] = (uint8_t)kind;
                }
            }
        }
        return;
    }
    if (ps == 7) {
        /* OSC 7 — current working directory, emitted by most shells on
           every prompt. Format: file://<host>/<urlencoded-path>. We
           ignore <host> (for SSH the remote sends its own hostname,
           which isn't useful to us) and URL-decode the path. */
        if (strncmp(p, "file://", 7) == 0) {
            const char *q = p + 7;
            const char *slash = strchr(q, '/');
            if (slash) {
                char out[1024];
                size_t oi = 0;
                const char *r = slash;
                while (*r && oi + 1 < sizeof(out)) {
                    if (*r == '%' && r[1] && r[2] &&
                        isxdigit((unsigned char)r[1]) &&
                        isxdigit((unsigned char)r[2])) {
                        int h1 = hexval(r[1]), h2 = hexval(r[2]);
                        out[oi++] = (char)((h1 << 4) | h2);
                        r += 3;
                    } else {
                        out[oi++] = *r++;
                    }
                }
                out[oi] = 0;
                if (s->io.set_cwd) s->io.set_cwd(s->io.user, out);
            }
        }
        return;
    }
    if (ps == 10 || ps == 11 || ps == 12) {
        /* OSC 10/11/12 — default fg / bg / cursor colour. `?` queries;
           anything else sets. */
        uint32_t *slot = (ps == 10) ? &s->default_fg
                       : (ps == 11) ? &s->default_bg
                                    : &s->cursor_color;
        if (*p == '?') {
            char buf[80];
            uint32_t c = *slot;
            int n = snprintf(buf, sizeof(buf),
                             "\x1b]%d;rgb:%02x%02x/%02x%02x/%02x%02x\x1b\\",
                             ps,
                             (c >> 16) & 0xff, (c >> 16) & 0xff,
                             (c >>  8) & 0xff, (c >>  8) & 0xff,
                             c & 0xff, c & 0xff);
            if (s->io.write) s->io.write(s->io.user, (const uint8_t *)buf, n);
        } else {
            uint32_t col;
            if (parse_color_spec(p, &col)) *slot = col;
        }
        return;
    }
    if (ps == 9) {
        /* OSC 9 — iTerm2-style desktop notification. The whole tail
           after the first ';' is the message body (no title). */
        if (s->io.notify && *p) s->io.notify(s->io.user, p);
        return;
    }
    if (ps == 777) {
        /* OSC 777 — urxvt notification. Format: notify;<title>;<body>.
           We collapse to "<title>: <body>" for the OS handler. */
        if (s->io.notify && strncmp(p, "notify;", 7) == 0) {
            const char *q = p + 7;
            const char *semi = strchr(q, ';');
            char buf[512];
            if (semi) {
                int tlen = (int)(semi - q);
                snprintf(buf, sizeof(buf), "%.*s: %s", tlen, q, semi + 1);
            } else {
                snprintf(buf, sizeof(buf), "%s", q);
            }
            s->io.notify(s->io.user, buf);
        }
        return;
    }
    if (ps == 52) {
        /* OSC 52 — set the system clipboard from base64 (the Pc selector
           is ignored here; we always target the system clipboard).
           `OSC 52 ; Pc ; ?` is a query and is ignored (programs mostly
           use this to paranoidly check and we don't want to leak). */
        while (*p && *p != ';') p++;
        if (*p == ';') p++;
        if (*p == 0 || *p == '?') return;
        /* base64 decode */
        static const int8_t b64tab[256] = {
            ['A']= 0,['B']= 1,['C']= 2,['D']= 3,['E']= 4,['F']= 5,['G']= 6,['H']= 7,
            ['I']= 8,['J']= 9,['K']=10,['L']=11,['M']=12,['N']=13,['O']=14,['P']=15,
            ['Q']=16,['R']=17,['S']=18,['T']=19,['U']=20,['V']=21,['W']=22,['X']=23,
            ['Y']=24,['Z']=25,
            ['a']=26,['b']=27,['c']=28,['d']=29,['e']=30,['f']=31,['g']=32,['h']=33,
            ['i']=34,['j']=35,['k']=36,['l']=37,['m']=38,['n']=39,['o']=40,['p']=41,
            ['q']=42,['r']=43,['s']=44,['t']=45,['u']=46,['v']=47,['w']=48,['x']=49,
            ['y']=50,['z']=51,
            ['0']=52,['1']=53,['2']=54,['3']=55,['4']=56,['5']=57,['6']=58,['7']=59,
            ['8']=60,['9']=61,
            ['+']=62,['/']=63,
        };
        size_t in = strlen(p);
        char *out = malloc(in); /* overestimate */
        if (!out) return;
        size_t n = 0;
        int acc = 0, bits = 0;
        for (size_t i = 0; i < in; i++) {
            unsigned char c = (unsigned char)p[i];
            if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
            int v = b64tab[c];
            if (v == 0 && c != 'A') {
                /* Table initialised to 0 for unset slots; reject anything
                   that isn't explicitly 'A'. */
                continue;
            }
            acc = (acc << 6) | v;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                out[n++] = (acc >> bits) & 0xff;
            }
        }
        out[n] = 0;
        /* SetClipboardText needs to be done by the caller via io.write?
           No — we can call raylib here but screen.c shouldn't know about
           raylib. Route through io.set_title? That's abused. Let's add
           a dedicated callback for clipboard. For now, borrow set_title:
           no, that'd change window title. Instead, do it via a new
           io callback. */
        if (s->io.set_clipboard) s->io.set_clipboard(s->io.user, out);
        free(out);
        return;
    }
    if (ps == 104) {
        /* OSC 104 — reset palette entries on this screen (all if no
           arg; otherwise listed). */
        if (!*p) {
            palette_fill_defaults(s->palette);
            return;
        }
        /* Per-entry reset: rebuild the default table once and copy
           only the requested index back into this screen. */
        uint32_t defaults[256];
        palette_fill_defaults(defaults);
        while (*p) {
            int idx = 0; bool any = false;
            while (*p >= '0' && *p <= '9') { idx = idx * 10 + (*p - '0'); any = true; p++; }
            if (any && idx >= 0 && idx < 256) {
                s->palette[idx] = defaults[idx];
            }
            if (*p == ';') p++; else break;
        }
        return;
    }
}

/* Forward decls for DCS/APC dispatch (payload decoders live below). */
static void dispatch_dcs(Screen *s, const unsigned char *p, size_t n);
static void dispatch_apc(Screen *s, const unsigned char *p, size_t n);
static void advance_cursor_past_image(Screen *s, int h_px);

/* DCS payload dispatcher. Sixel payloads start with optional
   numeric params and a `q`. Other DCS types (DECRQSS response,
   xterm termcap queries) we ignore silently. */
static void dispatch_dcs(Screen *s, const unsigned char *p, size_t n) {
    if (!p || n == 0) return;
    /* A sixel payload's header is digits/semicolons then 'q'. Skip
       digits/semicolons and check: if we land on 'q' it's sixel. */
    size_t i = 0;
    while (i < n && ((p[i] >= '0' && p[i] <= '9') || p[i] == ';')) i++;
    if (i >= n || p[i] != 'q') return;
    int w = 0, h = 0;
    unsigned char *rgba = sixel_decode(p, n, &w, &h);
    if (!rgba) return;
    if (image_append(s, rgba, w, h, s->cy, s->cx) == NULL) return;
    /* rgba ownership transferred to image_append on success. */
    advance_cursor_past_image(s, h);
}

/* Append `n` bytes to the kitty chunk accumulator, growing as needed. */
static void kpending_append(Screen *s, const unsigned char *buf, size_t n) {
    if (s->kpending_len + n > (1u << 26)) return;   /* 64MB safety cap */
    if (s->kpending_len + n > s->kpending_cap) {
        size_t nc = s->kpending_cap ? s->kpending_cap * 2 : 4096;
        while (nc < s->kpending_len + n) nc *= 2;
        unsigned char *nb = realloc(s->kpending, nc);
        if (!nb) return;
        s->kpending = nb;
        s->kpending_cap = nc;
    }
    memcpy(s->kpending + s->kpending_len, buf, n);
    s->kpending_len += n;
}

/* Wipe the kitty chunk accumulator after a final-chunk decode (or
   on error). Doesn't free the buffer — kept for the next image. */
static void kpending_reset(Screen *s) {
    s->kpending_len = 0;
    s->kpending_active = false;
}

/* Find the `m=` value in a kitty keys block. Returns 0 (m=0, "last
   chunk") when absent — that matches spec's default and also the
   single-message case. */
static int kitty_m_value(const unsigned char *keys, size_t klen) {
    for (size_t i = 0; i + 1 < klen; i++) {
        if (keys[i] == 'm' && keys[i + 1] == '=') {
            size_t j = i + 2;
            if (j < klen && keys[j] >= '0' && keys[j] <= '9')
                return keys[j] - '0';
            return 0;
        }
    }
    return 0;
}

/* APC payload dispatcher. Kitty graphics start with 'G' then
   key=value pairs. Supports chunked (m=1/m=0) multi-message transfer
   by buffering bytes across APC calls until the final chunk arrives. */
static void dispatch_apc(Screen *s, const unsigned char *p, size_t n) {
    if (!p || n == 0) return;
    if (p[0] != 'G') return;

    /* Locate the ';' that splits keys from the base64 payload. */
    size_t sep = 1;
    while (sep < n && p[sep] != ';') sep++;
    const unsigned char *keys = p + 1;
    size_t klen = sep - 1;
    const unsigned char *pload = (sep < n) ? p + sep + 1 : p + n;
    size_t plen = (sep < n) ? n - sep - 1 : 0;

    int m = kitty_m_value(keys, klen);

    /* First chunk (no pending yet): save the full "G<keys>;<payload>"
       prefix so the decoder sees the metadata from the first message
       only. Subsequent chunks contribute only their payload bytes. */
    if (!s->kpending_active) {
        /* Include the full "G<keys>;" header plus this chunk's payload. */
        kpending_append(s, p, (size_t)(pload - p));  /* G<keys>; */
        kpending_append(s, pload, plen);
        s->kpending_active = true;
    } else {
        /* Mid/last chunk — append only the payload bytes, discarding
           the (minimal) keys section that tails like `m=0;`. */
        kpending_append(s, pload, plen);
    }

    if (m == 1) return;   /* more chunks follow */

    /* Final chunk — decode and clear. */
    int w = 0, h = 0;
    unsigned char *rgba = kitty_decode(s->kpending, s->kpending_len, &w, &h);
    kpending_reset(s);
    if (!rgba) return;
    if (image_append(s, rgba, w, h, s->cy, s->cx) == NULL) return;
    advance_cursor_past_image(s, h);
}

/* Advance the cursor past an image that occupies `h_px` vertical
   pixels, anchored at the current cursor position. The cursor lands
   on a fresh row below the image so subsequent text doesn't draw on
   top. Triggers scroll if the image extends past the bottom. */
static void advance_cursor_past_image(Screen *s, int h_px) {
    int ch = s->cell_h_px > 0 ? s->cell_h_px : 20;
    int h_rows = (h_px + ch - 1) / ch;
    if (h_rows < 1) h_rows = 1;
    s->cx = 0;
    for (int i = 0; i < h_rows; i++) {
        if (s->cy >= s->scroll_bot) {
            scroll_up_region(s, s->scroll_top, s->scroll_bot, 1);
        } else {
            s->cy++;
        }
    }
    s->wrap_next = false;
}

/* Append a single byte to the DCS/APC payload buffer, growing if needed.
   Caps at 16MB — sixels bigger than that are almost certainly a bug. */
static void dpayload_push(Screen *s, uint8_t b) {
    if (s->dpayload_len >= (1u << 24)) return;
    if (s->dpayload_len + 1 > s->dpayload_cap) {
        size_t nc = s->dpayload_cap ? s->dpayload_cap * 2 : 4096;
        if (nc < s->dpayload_len + 1) nc = s->dpayload_len + 1;
        unsigned char *nb = realloc(s->dpayload, nc);
        if (!nb) return;
        s->dpayload = nb;
        s->dpayload_cap = nc;
    }
    s->dpayload[s->dpayload_len++] = b;
}

/* Wipe the DCS/APC payload accumulator. Buffer kept for re-use. */
static void dpayload_reset(Screen *s) { s->dpayload_len = 0; }

/* Handle the byte immediately following an unescaped ESC (0x1b).
   Dispatches into CSI / OSC / DCS / APC submachines (`[`, `]`,
   `P`, `_`), stand-alone two-byte ESC codes (D/E/M/c/=/>), the
   DEC save/restore cursor pair (7/8), and charset designation
   `( )`. Anything unrecognised drops back to ground. */
static void handle_esc(Screen *s, uint8_t b) {
    switch (b) {
    case '[': s->pstate = ST_CSI; reset_params(s); return;
    case ']': s->pstate = ST_OSC; s->osc_len = 0; return;
    case 'P': s->pstate = ST_DCS; dpayload_reset(s); return;
    case '_': s->pstate = ST_APC; dpayload_reset(s); return;
    case '7': s->saved_cx = s->cx; s->saved_cy = s->cy;
              s->saved_fg = s->cur_attr.fg; s->saved_bg = s->cur_attr.bg;
              s->saved_attrs_flags = s->cur_attr.attrs;
              s->pstate = ST_GROUND; return;
    case '8': s->cx = s->saved_cx; s->cy = s->saved_cy;
              s->cur_attr.fg = s->saved_fg; s->cur_attr.bg = s->saved_bg;
              s->cur_attr.attrs = s->saved_attrs_flags;
              s->pstate = ST_GROUND; return;
    case '=': s->app_keypad = true;  s->pstate = ST_GROUND; return;
    case '>': s->app_keypad = false; s->pstate = ST_GROUND; return;
    case 'D': newline(s); s->pstate = ST_GROUND; return;
    case 'E': s->cx = 0; newline(s); s->pstate = ST_GROUND; return;
    case 'M':
        if (s->cy == s->scroll_top) scroll_down_region(s, s->scroll_top, s->scroll_bot, 1);
        else if (s->cy > 0) s->cy--;
        s->pstate = ST_GROUND; return;
    case 'c':
        // RIS: reset
        s->cx = 0; s->cy = 0; s->cursor_visible = true; s->autowrap = true;
        s->scroll_top = 0; s->scroll_bot = s->rows - 1;
        s->cur_attr.fg = s->default_fg; s->cur_attr.bg = s->default_bg;
        s->cur_attr.attrs = ATTR_DEFAULT_FG | ATTR_DEFAULT_BG;
        s->charset_g0 = 'B'; s->charset_g1 = 'B'; s->charset_active = 0;
        for (int y = 0; y < s->rows; y++) clear_row(s, y);
        s->pstate = ST_GROUND; return;
    case '(': s->pstate = ST_CHARSET_G0; return;
    case ')': s->pstate = ST_CHARSET_G1; return;
    case '*': case '+':
        /* G2/G3 — we don't implement these yet, just swallow the next byte. */
        s->pstate = ST_CHARSET; return;
    default:
        s->pstate = ST_GROUND; return;
    }
}

/* ---------- C0 ---------- */

/* Handle one C0 control byte (0x00..0x1F). BEL fires the bell
   callback, BS/HT/LF/VT/FF/CR move the cursor as expected, SO/SI
   switch the active charset between G1 and G0. ESC (0x1B) is
   handled by feed_byte before reaching here. */
static void handle_c0(Screen *s, uint8_t b) {
    switch (b) {
    case 0x07: if (s->io.bell) s->io.bell(s->io.user); break;
    case 0x08: // BS
        if (s->wrap_next) s->wrap_next = false;
        else if (s->cx > 0) s->cx--;
        break;
    case 0x09: { // HT
        int next = ((s->cx / 8) + 1) * 8;
        if (next >= s->cols) next = s->cols - 1;
        s->cx = next;
        break;
    }
    case 0x0A: case 0x0B: case 0x0C: // LF/VT/FF
        newline(s);
        s->wrap_next = false;
        break;
    case 0x0D: // CR
        s->cx = 0;
        s->wrap_next = false;
        break;
    case 0x0E: s->charset_active = 1; break; /* SO: shift to G1 */
    case 0x0F: s->charset_active = 0; break; /* SI: shift to G0 */
    default: break;
    }
    s->view_off = 0;
}

/* ---------- Feed ---------- */

/* The single-byte parser core. Routes the byte through whichever
   parser sub-state we're in (ground / OSC / OSC_ESC / DCS / DCS_ESC
   / APC / APC_ESC / CSI / CHARSET) and into the right handler.
   Maintains UTF-8 multi-byte assembly inline; emitted codepoints
   end up as cells via put_cp. */
static void feed_byte(Screen *s, uint8_t b) {
    // Always handle CAN/SUB/ESC
    if (s->pstate == ST_OSC) {
        if (b == 0x07) { finish_osc(s); s->pstate = ST_GROUND; return; }
        if (b == 0x1b) { s->pstate = ST_OSC_ESC; return; }
        if (s->osc_len + 1 < (int)sizeof(s->osc)) s->osc[s->osc_len++] = (char)b;
        return;
    }
    if (s->pstate == ST_OSC_ESC) {
        if (b == '\\') { finish_osc(s); s->pstate = ST_GROUND; return; }
        s->pstate = ST_OSC; return;
    }
    /* DCS — sixel and DECRQSS live here. Terminated by ESC \ or BEL. */
    if (s->pstate == ST_DCS) {
        if (b == 0x1b) { s->pstate = ST_DCS_ESC; return; }
        if (b == 0x07) { dispatch_dcs(s, s->dpayload, s->dpayload_len);
                         dpayload_reset(s); s->pstate = ST_GROUND; return; }
        dpayload_push(s, b);
        return;
    }
    if (s->pstate == ST_DCS_ESC) {
        if (b == '\\') { dispatch_dcs(s, s->dpayload, s->dpayload_len);
                         dpayload_reset(s); s->pstate = ST_GROUND; return; }
        /* Bogus ESC inside DCS — treat as literal ESC in payload and
           resume collection. Matches xterm leniency for broken senders. */
        dpayload_push(s, 0x1b);
        dpayload_push(s, b);
        s->pstate = ST_DCS;
        return;
    }
    /* APC — kitty graphics payloads. Same ESC \ termination. */
    if (s->pstate == ST_APC) {
        if (b == 0x1b) { s->pstate = ST_APC_ESC; return; }
        if (b == 0x07) { dispatch_apc(s, s->dpayload, s->dpayload_len);
                         dpayload_reset(s); s->pstate = ST_GROUND; return; }
        dpayload_push(s, b);
        return;
    }
    if (s->pstate == ST_APC_ESC) {
        if (b == '\\') { dispatch_apc(s, s->dpayload, s->dpayload_len);
                         dpayload_reset(s); s->pstate = ST_GROUND; return; }
        dpayload_push(s, 0x1b);
        dpayload_push(s, b);
        s->pstate = ST_APC;
        return;
    }
    if (s->pstate == ST_CHARSET) { s->pstate = ST_GROUND; return; }
    if (s->pstate == ST_CHARSET_G0) { s->charset_g0 = (char)b; s->pstate = ST_GROUND; return; }
    if (s->pstate == ST_CHARSET_G1) { s->charset_g1 = (char)b; s->pstate = ST_GROUND; return; }
    if (b == 0x18 || b == 0x1a) { s->pstate = ST_GROUND; return; }
    if (b == 0x1b) { s->pstate = ST_ESC; reset_params(s); return; }

    switch (s->pstate) {
    case ST_ESC: handle_esc(s, b); return;
    case ST_CSI: handle_csi(s, b); return;
    case ST_UTF8:
        if ((b & 0xc0) == 0x80) {
            s->uni_cp = (s->uni_cp << 6) | (b & 0x3f);
            if (--s->uni_need == 0) {
                put_cp(s, s->uni_cp);
                s->pstate = ST_GROUND;
            }
            return;
        }
        put_cp(s, 0xFFFD);
        s->pstate = ST_GROUND;
        break; // fallthrough below
    }

    // GROUND (or fallthrough)
    if (b < 0x20) { handle_c0(s, b); return; }
    if (b == 0x7f) return;
    if (b < 0x80) { put_cp(s, b); return; }
    if ((b & 0xe0) == 0xc0) { s->uni_cp = b & 0x1f; s->uni_need = 1; s->pstate = ST_UTF8; return; }
    if ((b & 0xf0) == 0xe0) { s->uni_cp = b & 0x0f; s->uni_need = 2; s->pstate = ST_UTF8; return; }
    if ((b & 0xf8) == 0xf0) { s->uni_cp = b & 0x07; s->uni_need = 3; s->pstate = ST_UTF8; return; }
    put_cp(s, 0xFFFD);
}

/* Feed `n` bytes from the PTY into the parser. Public entry point —
   called from the main loop after pty_read fills a buffer. Includes
   a fast path for runs of printable ASCII in GROUND state to avoid
   the per-byte parser dispatch when commands flood the PTY (e.g.
   `find /usr`). */
void screen_feed(Screen *s, const uint8_t *data, size_t n) {
    /* Hot path: when we're in GROUND state and the next bytes are all
       printable ASCII, write them straight into the current row without
       the state-machine per-byte dispatch. This matters for commands
       like `find /usr` that flood the PTY with newline-terminated text
       — the vanilla loop pays ~5x the cycles per byte and then the
       shell stalls on full PTY buffers. */
    size_t i = 0;
    while (i < n) {
        if (s->pstate == ST_GROUND) {
            uint8_t b = data[i];
            if (b >= 0x20 && b < 0x7f) {
                size_t j = i + 1;
                while (j < n && data[j] >= 0x20 && data[j] < 0x7f) j++;
                for (size_t k = i; k < j; k++) put_cp(s, data[k]);
                i = j;
                continue;
            }
        }
        feed_byte(s, data[i]);
        i++;
    }
}

/* ---------- Construction ---------- */

/* Allocate a fresh Screen with `cols` columns, `rows` rows, and
   `scrollback` rows of history (0 disables scrollback — used for
   the alt-screen-only buffer). The IO callbacks let the screen
   call back into the host for things it can't do itself: writing
   responses to the PTY, setting the window title, posting
   notifications, etc. */
Screen *screen_new(int cols, int rows, int scrollback, ScreenIO io) {
    Screen *s = calloc(1, sizeof(*s));
    s->cols = cols;
    s->rows = rows;
    s->scroll_top = 0;
    s->scroll_bot = rows - 1;
    s->cursor_visible = true;
    s->autowrap = true;
    /* Seed the per-screen defaults + palette first — cur_attr reads from them. */
    s->default_fg   = SEED_DEFAULT_FG;
    s->default_bg   = SEED_DEFAULT_BG;
    s->cursor_color = SEED_CURSOR_COLOR;
    palette_fill_defaults(s->palette);
    s->cur_attr.fg = s->default_fg;
    s->cur_attr.bg = s->default_bg;
    s->cur_attr.attrs = ATTR_DEFAULT_FG | ATTR_DEFAULT_BG;
    s->charset_g0 = 'B';
    s->charset_g1 = 'B';
    s->charset_active = 0;
    s->main = calloc((size_t)cols * rows, sizeof(Cell));
    s->alt  = calloc((size_t)cols * rows, sizeof(Cell));
    s->main_wrap  = calloc((size_t)rows, 1);
    s->main_pmark = calloc((size_t)rows, 1);
    s->main_pexit = calloc((size_t)rows, 1);
    for (int y = 0; y < rows; y++) {
        Cell b = blank_cell(s);
        for (int x = 0; x < cols; x++) s->main[y * cols + x] = b;
        for (int x = 0; x < cols; x++) s->alt[y * cols + x] = b;
    }
    s->sb_cap = scrollback;
    if (scrollback > 0) {
        s->sb = calloc((size_t)scrollback * cols, sizeof(Cell));
        s->sb_wrap  = calloc((size_t)scrollback, 1);
        s->sb_pmark = calloc((size_t)scrollback, 1);
        s->sb_pexit = calloc((size_t)scrollback, 1);
    }
    s->cell_h_px = 20;   /* best-effort until renderer sets the real value */
    s->io = io;
    return s;
}

/* Tear down a Screen and free every owned allocation: cells, alt
   buffer, scrollback ring, parallel wrap / pmark / pexit arrays,
   image bitmaps, OSC 8 URL pool, DCS / APC payload buffers. */
void screen_free(Screen *s) {
    if (!s) return;
    images_free_all(s);
    free(s->dpayload);
    free(s->kpending);
    for (uint16_t i = 0; i < s->urls_count; i++) free(s->urls[i]);
    free(s->urls);
    free(s->main); free(s->alt); free(s->sb); free(s->sb_wrap); free(s->main_wrap);
    free(s->sb_pmark); free(s->sb_pexit);
    free(s->main_pmark); free(s->main_pexit);
    free(s);
}

/* ---------- Image accessors (see header for the contract). ---------- */

/* Number of images (sixel + kitty) currently anchored on the
   visible viewport for this screen. */
int screen_image_count(const Screen *s) { return s ? s->nimages : 0; }
/* Indexed lookup (0..count-1). NULL on out-of-range. */
const ScreenImage *screen_image_at(const Screen *s, int i) {
    if (!s || i < 0 || i >= s->nimages) return NULL;
    return s->images[i];
}
const unsigned char *screen_image_rgba(const ScreenImage *img) { return img ? img->rgba : NULL; }
int screen_image_px_w(const ScreenImage *img)       { return img ? img->px_w : 0; }
int screen_image_px_h(const ScreenImage *img)       { return img ? img->px_h : 0; }
int screen_image_anchor_row(const ScreenImage *img) { return img ? img->anchor_row : 0; }
int screen_image_anchor_col(const ScreenImage *img) { return img ? img->anchor_col : 0; }
uint64_t screen_image_generation(const ScreenImage *img) { return img ? img->gen : 0; }
bool screen_image_on_alt(const ScreenImage *img) { return img ? img->on_alt : false; }
bool screen_on_alt(const Screen *s) { return s ? s->on_alt : false; }
/* Renderer-side hint: tell the screen how many pixels tall each
   cell currently is, so image scroll bookkeeping can translate
   pixel-heighted graphics into cell-rows correctly. */
void screen_set_cell_h_px(Screen *s, int cell_h_px) {
    if (s && cell_h_px > 0) s->cell_h_px = cell_h_px;
}

/* Update the IO callback's `user` pointer (typically a Pane*) on
   an existing screen — used when a pane is realloc'd. */
void screen_set_io_user(Screen *s, void *user) {
    if (!s) return;
    s->io.user = user;
}

void screen_set_default_fg(Screen *s, uint32_t rgb)   { if (s) s->default_fg = rgb; }
void screen_set_default_bg(Screen *s, uint32_t rgb)   { if (s) s->default_bg = rgb; }
void screen_set_cursor_color(Screen *s, uint32_t rgb) { if (s) s->cursor_color = rgb; }
void screen_set_palette_entry(Screen *s, int i, uint32_t rgb) {
    if (!s || i < 0 || i >= 256) return;
    s->palette[i] = rgb;
}

void        screen_set_cursor_style(Screen *s, CursorStyle st) { if (s) s->cursor_style = st; }
CursorStyle screen_cursor_style(const Screen *s) { return s ? s->cursor_style : CURSOR_STYLE_DEFAULT; }

/* Resize the main + alt grids to (cols, rows). The main screen is
   reflowed: existing logical lines (groups of rows joined by
   main_wrap[]) are concatenated, rewrapped at the new width, and
   then re-split. Overflow off the top of the new viewport is
   pushed into scrollback. The alt screen is *not* reflowed (full-
   screen apps redraw on SIGWINCH). Per-row wrap / pmark / pexit
   arrays are rebuilt; some scrollback wrap info is approximate
   after resize — accepted v1 lossiness. */
void screen_resize(Screen *s, int cols, int rows) {
    if (cols == s->cols && rows == s->rows) return;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    int old_cols = s->cols;
    int old_rows = s->rows;
    Cell blank = blank_cell(s);

    /* --- Main screen: reflow. Collect logical lines, then rewrap at new_cols.
           If the emitted content exceeds new_rows, the top overflow is pushed
           into scrollback (pre-reflow scrollback is kept as-is). --- */
    int line_cap = old_rows;
    Cell **lines = calloc(line_cap, sizeof(Cell *));
    int *line_lens = calloc(line_cap, sizeof(int));
    int num_lines = 0;
    {
        int y = 0;
        while (y < old_rows) {
            int start = y;
            int total = 0;
            int last = y;
            while (last < old_rows) {
                bool wrapped = s->main_wrap && s->main_wrap[last];
                int rl = old_cols;
                if (!wrapped) {
                    while (rl > 0) {
                        Cell c = s->main[last * old_cols + rl - 1];
                        if (c.cp != 0 && c.cp != ' ') break;
                        rl--;
                    }
                }
                total += rl;
                if (!wrapped) break;
                last++;
            }
            if (total > 0) {
                Cell *line = malloc(sizeof(Cell) * total);
                int pos = 0;
                for (int yy = start; yy <= last && yy < old_rows; yy++) {
                    bool wrapped = s->main_wrap && s->main_wrap[yy];
                    int rl = old_cols;
                    if (!wrapped) {
                        while (rl > 0) {
                            Cell c = s->main[yy * old_cols + rl - 1];
                            if (c.cp != 0 && c.cp != ' ') break;
                            rl--;
                        }
                    }
                    memcpy(line + pos, s->main + yy * old_cols, sizeof(Cell) * rl);
                    pos += rl;
                }
                lines[num_lines] = line;
                line_lens[num_lines] = total;
            } else {
                lines[num_lines] = NULL;
                line_lens[num_lines] = 0;
            }
            num_lines++;
            y = last + 1;
        }
    }

    /* Emit lines into a scratch buffer wide new_cols. */
    int max_rows = 1;
    for (int i = 0; i < num_lines; i++) {
        int n = line_lens[i];
        max_rows += n == 0 ? 1 : (n + cols - 1) / cols;
    }
    if (max_rows < rows) max_rows = rows;
    Cell *scratch = calloc((size_t)max_rows * cols, sizeof(Cell));
    uint8_t *scratch_wrap = calloc((size_t)max_rows, 1);
    for (int r = 0; r < max_rows; r++)
        for (int x = 0; x < cols; x++) scratch[r * cols + x] = blank;
    int emit = 0;
    for (int i = 0; i < num_lines; i++) {
        int len = line_lens[i];
        if (len == 0) { emit++; continue; }
        int pos = 0;
        while (pos < len && emit < max_rows) {
            int take = len - pos;
            if (take > cols) take = cols;
            memcpy(scratch + (size_t)emit * cols, lines[i] + pos, sizeof(Cell) * take);
            pos += take;
            if (pos < len) scratch_wrap[emit] = 1;
            emit++;
        }
    }

    /* Copy the last `rows` emitted rows into the new main buffer; shove
       overflow into the (un-reflowed) scrollback so nothing is lost. */
    Cell *nmain = calloc((size_t)cols * rows, sizeof(Cell));
    uint8_t *nwrap = calloc((size_t)rows, 1);
    for (int r = 0; r < rows; r++)
        for (int x = 0; x < cols; x++) nmain[r * cols + x] = blank;
    int start_row = emit - rows;
    if (start_row < 0) start_row = 0;

    /* Rebuild scrollback at new width (preserve its existing content; just
       re-bucket into the new col count). */
    Cell *nsb = NULL;
    uint8_t *nsbw = NULL;
    uint8_t *nsbpm = NULL;
    uint8_t *nsbpe = NULL;
    if (s->sb_cap > 0) {
        nsb = calloc((size_t)s->sb_cap * cols, sizeof(Cell));
        nsbw  = calloc((size_t)s->sb_cap, 1);
        nsbpm = calloc((size_t)s->sb_cap, 1);
        nsbpe = calloc((size_t)s->sb_cap, 1);
        for (int y = 0; y < s->sb_cap; y++)
            for (int x = 0; x < cols; x++) nsb[y * cols + x] = blank;
        int ccols = (cols < old_cols) ? cols : old_cols;
        for (int i = 0; i < s->sb_len; i++) {
            int src = ((s->sb_head - s->sb_len + i) % s->sb_cap + s->sb_cap) % s->sb_cap;
            memcpy(nsb + (size_t)i * cols, s->sb + (size_t)src * old_cols,
                   sizeof(Cell) * ccols);
            nsbw[i]  = s->sb_wrap  ? s->sb_wrap[src]  : 0;
            nsbpm[i] = s->sb_pmark ? s->sb_pmark[src] : 0;
            nsbpe[i] = s->sb_pexit ? s->sb_pexit[src] : 0;
        }
        s->sb_head = s->sb_len % s->sb_cap;
    }
    /* Push overflow to scrollback by writing into nsb directly. We
       temporarily point s->sb at the new buffer (and s->cols at the
       new width) so push_scrollback writes into nsb with the right
       stride. **Both must be restored before the teardown below
       free()s s->sb** — otherwise we'd free the new buffer, then
       reinstall the freed pointer, and screen_free would later
       double-free it. */
    if (s->sb_cap > 0 && start_row > 0) {
        Cell *saved_sb_was = s->sb;
        uint8_t *saved_sbw_was = s->sb_wrap;
        uint8_t *saved_sbpm_was = s->sb_pmark;
        uint8_t *saved_sbpe_was = s->sb_pexit;
        int saved_cols = s->cols;
        s->sb = nsb;
        s->sb_wrap  = nsbw;
        s->sb_pmark = nsbpm;
        s->sb_pexit = nsbpe;
        s->cols = cols;
        for (int r = 0; r < start_row; r++) {
            /* Reflow loses per-row pmark/pexit on overflow rows —
               accept v1 lossiness; new marks land correctly. */
            push_scrollback(s, scratch + (size_t)r * cols,
                            scratch_wrap[r] != 0, 0, 0);
        }
        s->cols = saved_cols;
        s->sb = saved_sb_was;
        s->sb_wrap  = saved_sbw_was;
        s->sb_pmark = saved_sbpm_was;
        s->sb_pexit = saved_sbpe_was;
    }
    int copy_count = emit - start_row;
    if (copy_count > rows) copy_count = rows;
    for (int r = 0; r < copy_count; r++) {
        memcpy(nmain + (size_t)r * cols, scratch + (size_t)(start_row + r) * cols,
               sizeof(Cell) * cols);
        nwrap[r] = scratch_wrap[start_row + r];
    }

    /* Alt screen: copy overlap with top-left alignment (full-screen apps will
       redraw on SIGWINCH). */
    Cell *nalt = calloc((size_t)cols * rows, sizeof(Cell));
    for (int r = 0; r < rows; r++)
        for (int x = 0; x < cols; x++) nalt[r * cols + x] = blank;
    int ox = (cols < old_cols) ? cols : old_cols;
    int oy = (rows < old_rows) ? rows : old_rows;
    for (int y = 0; y < oy; y++) {
        memcpy(nalt + (size_t)y * cols, s->alt + (size_t)y * old_cols,
               sizeof(Cell) * ox);
    }

    /* Install. */
    for (int i = 0; i < num_lines; i++) free(lines[i]);
    free(lines);
    free(line_lens);
    free(scratch);
    free(scratch_wrap);
    /* Fresh per-row pmark/pexit for the new main grid — we accept
       reflow-time loss; new shell marks repopulate correctly. */
    uint8_t *npmark = calloc((size_t)rows, 1);
    uint8_t *npexit = calloc((size_t)rows, 1);
    free(s->main); free(s->alt); free(s->sb); free(s->sb_wrap); free(s->main_wrap);
    free(s->sb_pmark); free(s->sb_pexit);
    free(s->main_pmark); free(s->main_pexit);
    s->main = nmain; s->alt = nalt; s->sb = nsb; s->sb_wrap = nsbw; s->main_wrap = nwrap;
    s->sb_pmark = nsbpm; s->sb_pexit = nsbpe;
    s->main_pmark = npmark; s->main_pexit = npexit;
    s->cols = cols; s->rows = rows;
    s->scroll_top = 0;
    s->scroll_bot = rows - 1;
    if (s->cx >= cols) s->cx = cols - 1;
    if (s->cy >= rows) s->cy = rows - 1;
    /* Move cursor to the end of the last row that holds real content — lets
       bash redraw its prompt in place after SIGWINCH. */
    int last_content_row = -1;
    for (int y = rows - 1; y >= 0; y--) {
        for (int x = 0; x < cols; x++) {
            uint32_t cp = s->main[y * cols + x].cp;
            if (cp != 0 && cp != ' ') { last_content_row = y; break; }
        }
        if (last_content_row >= 0) break;
    }
    if (last_content_row >= 0) {
        s->cy = last_content_row;
        int cx_end = 0;
        for (int x = 0; x < cols; x++) {
            uint32_t cp = s->main[last_content_row * cols + x].cp;
            if (cp != 0 && cp != ' ') cx_end = x + 1;
        }
        if (cx_end >= cols) cx_end = cols - 1;
        s->cx = cx_end;
    }
    s->wrap_next = false;
    if (s->view_off > s->sb_len) s->view_off = s->sb_len;
}

/* ---------- Accessors ---------- */

int  screen_cols(const Screen *s)           { return s->cols; }
int  screen_rows(const Screen *s)           { return s->rows; }
int  screen_cursor_x(const Screen *s)       { return s->cx; }
int  screen_cursor_y(const Screen *s)       { return s->cy; }
bool screen_cursor_visible(const Screen *s) { return s->cursor_visible; }
int  screen_view_offset(const Screen *s)    { return s->view_off; }
int  screen_scrollback_len(const Screen *s) { return s->sb_len; }
bool screen_app_cursor(const Screen *s)     { return s->app_cursor; }
bool screen_app_keypad(const Screen *s)     { return s->app_keypad; }
bool screen_focus_report(const Screen *s)   { return s->focus_report; }
bool screen_bracketed_paste(const Screen *s){ return s->bracketed_paste; }
int  screen_mouse_mode(const Screen *s)     { return s->mouse_mode; }
bool screen_mouse_sgr(const Screen *s)      { return s->mouse_sgr; }

/* Adjust the scrollback view offset by `delta_rows` (positive scrolls
   into older history, negative back toward the live grid).
   Clamped to [0, sb_len] — out-of-range deltas are silent no-ops. */
void screen_scroll_view(Screen *s, int delta_rows) {
    int v = s->view_off + delta_rows;
    if (v < 0) v = 0;
    if (v > s->sb_len) v = s->sb_len;
    s->view_off = v;
}

/* Snap the viewport back to the live grid (offset = 0). */
void screen_scroll_reset(Screen *s) { s->view_off = 0; }

Cell screen_view_cell(const Screen *s, int col, int vy) {
    if (col < 0 || col >= s->cols || vy < 0 || vy >= s->rows) {
        Cell e = {0, s->default_fg, s->default_bg, 0, ATTR_DEFAULT_FG | ATTR_DEFAULT_BG, 0};
        return e;
    }
    // view_off rows from the bottom of scrollback replace the top rows of the live screen
    int off = s->view_off;
    if (vy < off) {
        // From scrollback
        int sb_index = s->sb_len - off + vy;
        int ring = ((s->sb_head - s->sb_len + sb_index) % s->sb_cap + s->sb_cap) % s->sb_cap;
        return s->sb[ring * s->cols + col];
    }
    int y = vy - off;
    Cell *base = s->on_alt ? s->alt : s->main;
    return base[y * s->cols + col];
}

int screen_total_rows(const Screen *s) {
    if (!s) return 0;
    /* Alt screen has no scrollback; main screen walks scrollback then live. */
    return s->on_alt ? s->rows : (s->sb_len + s->rows);
}

Cell screen_cell_abs(const Screen *s, int col, int abs_row) {
    Cell e = {0, 0, 0, 0, 0, 0};
    if (!s || col < 0 || col >= s->cols || abs_row < 0) return e;
    if (s->on_alt) {
        if (abs_row >= s->rows) return e;
        return s->alt[abs_row * s->cols + col];
    }
    if (abs_row < s->sb_len) {
        int ring = ((s->sb_head - s->sb_len + abs_row) % s->sb_cap + s->sb_cap) % s->sb_cap;
        return s->sb[ring * s->cols + col];
    }
    int y = abs_row - s->sb_len;
    if (y >= s->rows) return e;
    return s->main[y * s->cols + col];
}

bool screen_command_running(const Screen *s) {
    return s && s->c_time != 0;
}

uint8_t screen_pmark_at_abs(const Screen *s, int abs_row, uint8_t *out_exit) {
    if (out_exit) *out_exit = 0;
    if (!s || abs_row < 0 || s->on_alt) return 0;
    if (abs_row < s->sb_len) {
        if (!s->sb_pmark || s->sb_cap == 0) return 0;
        int ring = ((s->sb_head - s->sb_len + abs_row) % s->sb_cap + s->sb_cap) % s->sb_cap;
        if (out_exit && s->sb_pexit) *out_exit = s->sb_pexit[ring];
        return s->sb_pmark[ring];
    }
    int y = abs_row - s->sb_len;
    if (y < 0 || y >= s->rows || !s->main_pmark) return 0;
    if (out_exit && s->main_pexit) *out_exit = s->main_pexit[y];
    return s->main_pmark[y];
}

uint8_t screen_view_row_pmark(const Screen *s, int vy, uint8_t *out_exit) {
    if (out_exit) *out_exit = 0;
    if (!s || vy < 0 || vy >= s->rows || s->on_alt) return 0;
    int off = s->view_off;
    if (vy < off) {
        if (!s->sb_pmark || s->sb_cap == 0) return 0;
        int sb_index = s->sb_len - off + vy;
        int ring = ((s->sb_head - s->sb_len + sb_index) % s->sb_cap + s->sb_cap) % s->sb_cap;
        if (out_exit && s->sb_pexit) *out_exit = s->sb_pexit[ring];
        return s->sb_pmark[ring];
    }
    int y = vy - off;
    if (!s->main_pmark) return 0;
    if (out_exit && s->main_pexit) *out_exit = s->main_pexit[y];
    return s->main_pmark[y];
}

bool screen_view_row_wrapped(const Screen *s, int vy) {
    if (!s || vy < 0 || vy >= s->rows || s->on_alt) return false;
    int off = s->view_off;
    if (vy < off) {
        if (!s->sb_wrap || s->sb_cap == 0) return false;
        int sb_index = s->sb_len - off + vy;
        int ring = ((s->sb_head - s->sb_len + sb_index) % s->sb_cap + s->sb_cap) % s->sb_cap;
        return s->sb_wrap[ring] != 0;
    }
    int y = vy - off;
    if (!s->main_wrap) return false;
    return s->main_wrap[y] != 0;
}
