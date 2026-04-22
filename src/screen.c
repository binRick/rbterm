#include "screen.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

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
};

#define MAX_PARAMS 16

/* Seed values copied into every new Screen. OSC 10/11/12 mutate the
   per-screen copy, not these, so one pane's theme doesn't bleed into
   the others. */
#define SEED_DEFAULT_FG    0xFFFFFFu   /* pure white — was 0xEAEAEA, felt gray */
#define SEED_DEFAULT_BG    0x111111u
#define SEED_CURSOR_COLOR  0xFFFFFFu

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
    bool on_alt;

    // Scrollback ring buffer: rows of `cols` cells
    Cell *sb;
    int sb_cap;
    int sb_len;
    int sb_head;     // next write slot
    int view_off;    // rows scrolled up (>=0, <= sb_len)

    // Parser
    int pstate;
    int params[MAX_PARAMS];
    bool param_set[MAX_PARAMS];
    int param_cnt;
    bool priv;       // '?' seen
    bool inter_gt;   // '>' seen  (secondary DA etc.)
    bool inter_eq;   // '=' seen  (tertiary DA)

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

    /* Per-screen palette (mutable via OSC 4/104). Kept per-screen so
       a tab/pane rewriting its colours doesn't bleed into the others. */
    uint32_t palette[256];
    /* Per-screen default colours (mutable via OSC 10/11/12). Same
       reason — a theme's default-fg/bg change shouldn't escape the
       pane that applied it. */
    uint32_t default_fg;
    uint32_t default_bg;
    uint32_t cursor_color;

    ScreenIO io;
};

uint32_t screen_default_fg(const Screen *s)    { return s ? s->default_fg    : SEED_DEFAULT_FG; }
uint32_t screen_default_bg(const Screen *s)    { return s ? s->default_bg    : SEED_DEFAULT_BG; }
uint32_t screen_cursor_color(const Screen *s)  { return s ? s->cursor_color  : SEED_CURSOR_COLOR; }

/* ---------- Palette (mutable — can be set by OSC 4) ---------- */

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

static uint32_t pal(const Screen *s, int i) {
    if (!s || i < 0 || i > 255) return SEED_DEFAULT_FG;
    return s->palette[i];
}

/* Parse one OSC 4 colour spec: "#RGB" / "#RRGGBB" / "#RRRRGGGGBBBB"
   or "rgb:R/G/B" with 1..4 hex digits each. Returns true + rgb. */
static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
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

static Cell blank_cell(Screen *s) {
    Cell c;
    c.cp = 0;
    c.fg = s->cur_attr.fg;
    c.bg = s->cur_attr.bg;
    c.attrs = s->cur_attr.attrs & (ATTR_DEFAULT_FG | ATTR_DEFAULT_BG);
    return c;
}

static Cell *active(Screen *s) { return s->on_alt ? s->alt : s->main; }

static Cell *row_ptr(Screen *s, int y) { return active(s) + y * s->cols; }

static void clear_row(Screen *s, int y) {
    Cell b = blank_cell(s);
    Cell *r = row_ptr(s, y);
    for (int x = 0; x < s->cols; x++) r[x] = b;
    if (!s->on_alt && s->main_wrap && y >= 0 && y < s->rows) s->main_wrap[y] = 0;
}

static void push_scrollback(Screen *s, const Cell *row) {
    if (s->on_alt || s->sb_cap == 0) return;
    memcpy(s->sb + s->sb_head * s->cols, row, sizeof(Cell) * s->cols);
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
        for (int i = 0; i < n; i++) push_scrollback(s, base + (top + i) * s->cols);
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
    }
    for (int i = 0; i < n; i++) clear_row(s, top + i);
}

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
    Cell *row = row_ptr(s, s->cy);
    Cell *c = row + s->cx;
    c->cp = cp;
    c->fg = s->cur_attr.fg;
    c->bg = s->cur_attr.bg;
    c->attrs = s->cur_attr.attrs | (wide ? ATTR_WIDE : 0);
    if (wide) {
        Cell *c2 = row + s->cx + 1;
        c2->cp = 0;
        c2->fg = s->cur_attr.fg;
        c2->bg = s->cur_attr.bg;
        c2->attrs = s->cur_attr.attrs | ATTR_WIDE_CONT;
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

static int clamp(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static void reset_params(Screen *s) {
    for (int i = 0; i < MAX_PARAMS; i++) { s->params[i] = 0; s->param_set[i] = false; }
    s->param_cnt = 0;
    s->priv = false;
    s->inter_gt = false;
    s->inter_eq = false;
    s->osc_len = 0;
}

static int pget(Screen *s, int i, int dflt) {
    if (i >= s->param_cnt || !s->param_set[i]) return dflt;
    return s->params[i];
}

/* ---------- SGR ---------- */

static void sgr(Screen *s) {
    int n = s->param_cnt ? s->param_cnt : 1;
    for (int i = 0; i < n; i++) {
        int p = s->param_set[i] ? s->params[i] : 0;
        if (p == 0) {
            s->cur_attr.fg = s->default_fg;
            s->cur_attr.bg = s->default_bg;
            s->cur_attr.attrs = ATTR_DEFAULT_FG | ATTR_DEFAULT_BG;
        } else if (p == 1)  s->cur_attr.attrs |= ATTR_BOLD;
        else if (p == 2)    s->cur_attr.attrs |= ATTR_DIM;
        else if (p == 3)    s->cur_attr.attrs |= ATTR_ITALIC;
        else if (p == 4)    s->cur_attr.attrs |= ATTR_UNDERLINE;
        else if (p == 7)    s->cur_attr.attrs |= ATTR_REVERSE;
        else if (p == 8)    s->cur_attr.attrs |= ATTR_HIDDEN;
        else if (p == 9)    s->cur_attr.attrs |= ATTR_STRIKE;
        else if (p == 22)   s->cur_attr.attrs &= ~(ATTR_BOLD | ATTR_DIM);
        else if (p == 23)   s->cur_attr.attrs &= ~ATTR_ITALIC;
        else if (p == 24)   s->cur_attr.attrs &= ~ATTR_UNDERLINE;
        else if (p == 27)   s->cur_attr.attrs &= ~ATTR_REVERSE;
        else if (p == 28)   s->cur_attr.attrs &= ~ATTR_HIDDEN;
        else if (p == 29)   s->cur_attr.attrs &= ~ATTR_STRIKE;
        else if (p >= 30 && p <= 37) {
            /* Store the palette INDEX and flag it; rendering resolves the
               current RGB each frame, so OSC 4 palette changes retro-
               actively recolour existing cells. */
            s->cur_attr.fg = (uint32_t)(p - 30);
            s->cur_attr.attrs = (uint16_t)((s->cur_attr.attrs & ~ATTR_DEFAULT_FG) | ATTR_FG_INDEX);
        }
        else if (p == 39) {
            s->cur_attr.fg = s->default_fg;
            s->cur_attr.attrs = (uint16_t)((s->cur_attr.attrs | ATTR_DEFAULT_FG) & ~ATTR_FG_INDEX);
        }
        else if (p >= 40 && p <= 47) {
            s->cur_attr.bg = (uint32_t)(p - 40);
            s->cur_attr.attrs = (uint16_t)((s->cur_attr.attrs & ~ATTR_DEFAULT_BG) | ATTR_BG_INDEX);
        }
        else if (p == 49) {
            s->cur_attr.bg = s->default_bg;
            s->cur_attr.attrs = (uint16_t)((s->cur_attr.attrs | ATTR_DEFAULT_BG) & ~ATTR_BG_INDEX);
        }
        else if (p >= 90 && p <= 97) {
            s->cur_attr.fg = (uint32_t)(p - 90 + 8);
            s->cur_attr.attrs = (uint16_t)((s->cur_attr.attrs & ~ATTR_DEFAULT_FG) | ATTR_FG_INDEX);
        }
        else if (p >= 100 && p <= 107) {
            s->cur_attr.bg = (uint32_t)(p - 100 + 8);
            s->cur_attr.attrs = (uint16_t)((s->cur_attr.attrs & ~ATTR_DEFAULT_BG) | ATTR_BG_INDEX);
        }
        else if (p == 38 || p == 48) {
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
        }
    }
}

uint32_t screen_palette(const Screen *s, int i) { return pal(s, i); }

/* ---------- Erase ---------- */

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
    }
}

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
        } else if (!on && s->on_alt) {
            s->on_alt = false;
            s->cx = s->saved_cx; s->cy = s->saved_cy;
        }
        s->wrap_next = false;
        break;
    }
    case 2004: s->bracketed_paste = on; break;
    }
}

/* ---------- CSI ---------- */

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
        if (s->param_cnt < MAX_PARAMS) s->param_cnt++;
        return;
    }
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
            const char *resp = "\x1b[?65;1;9c";
            s->io.write(s->io.user, (const uint8_t *)resp, strlen(resp));
        }
        break;
    }
    case 's': s->saved_cx = s->cx; s->saved_cy = s->cy; break;
    case 'u': s->cx = s->saved_cx; s->cy = s->saved_cy; break;
    default: break;
    }
    s->pstate = ST_GROUND;
}

/* ---------- ESC ---------- */

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

static void handle_esc(Screen *s, uint8_t b) {
    switch (b) {
    case '[': s->pstate = ST_CSI; reset_params(s); return;
    case ']': s->pstate = ST_OSC; s->osc_len = 0; return;
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
    s->main_wrap = calloc((size_t)rows, 1);
    for (int y = 0; y < rows; y++) {
        Cell b = blank_cell(s);
        for (int x = 0; x < cols; x++) s->main[y * cols + x] = b;
        for (int x = 0; x < cols; x++) s->alt[y * cols + x] = b;
    }
    s->sb_cap = scrollback;
    if (scrollback > 0) s->sb = calloc((size_t)scrollback * cols, sizeof(Cell));
    s->io = io;
    return s;
}

void screen_free(Screen *s) {
    if (!s) return;
    free(s->main); free(s->alt); free(s->sb); free(s->main_wrap);
    free(s);
}

void screen_set_io_user(Screen *s, void *user) {
    if (!s) return;
    s->io.user = user;
}

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
    if (s->sb_cap > 0) {
        nsb = calloc((size_t)s->sb_cap * cols, sizeof(Cell));
        for (int y = 0; y < s->sb_cap; y++)
            for (int x = 0; x < cols; x++) nsb[y * cols + x] = blank;
        int ccols = (cols < old_cols) ? cols : old_cols;
        for (int i = 0; i < s->sb_len; i++) {
            int src = ((s->sb_head - s->sb_len + i) % s->sb_cap + s->sb_cap) % s->sb_cap;
            memcpy(nsb + (size_t)i * cols, s->sb + (size_t)src * old_cols,
                   sizeof(Cell) * ccols);
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
        int saved_cols = s->cols;
        s->sb = nsb;
        s->cols = cols;
        for (int r = 0; r < start_row; r++) {
            push_scrollback(s, scratch + (size_t)r * cols);
        }
        s->cols = saved_cols;
        s->sb = saved_sb_was;
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
    free(s->main); free(s->alt); free(s->sb); free(s->main_wrap);
    s->main = nmain; s->alt = nalt; s->sb = nsb; s->main_wrap = nwrap;
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

void screen_scroll_view(Screen *s, int delta_rows) {
    int v = s->view_off + delta_rows;
    if (v < 0) v = 0;
    if (v > s->sb_len) v = s->sb_len;
    s->view_off = v;
}

void screen_scroll_reset(Screen *s) { s->view_off = 0; }

Cell screen_view_cell(const Screen *s, int col, int vy) {
    if (col < 0 || col >= s->cols || vy < 0 || vy >= s->rows) {
        Cell e = {0, s->default_fg, s->default_bg, ATTR_DEFAULT_FG | ATTR_DEFAULT_BG};
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
