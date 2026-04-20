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
    ST_CHARSET,
};

#define MAX_PARAMS 16

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
    int scroll_top, scroll_bot;
    Cell cur_attr;

    Cell *main;
    Cell *alt;
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
    uint32_t uni_cp;
    int uni_need;

    // OSC
    char osc[512];
    int osc_len;

    ScreenIO io;
};

/* ---------- Palette ---------- */

static uint32_t base16[16] = {
    0x000000, 0xCC0000, 0x4E9A06, 0xC4A000,
    0x3465A4, 0x75507B, 0x06989A, 0xD3D7CF,
    0x555753, 0xEF2929, 0x8AE234, 0xFCE94F,
    0x729FCF, 0xAD7FA8, 0x34E2E2, 0xEEEEEC,
};

static uint32_t palette256(int i) {
    if (i < 16) return base16[i];
    if (i < 232) {
        int n = i - 16;
        int r = (n / 36) % 6;
        int g = (n / 6) % 6;
        int b = n % 6;
        static const int lut[6] = {0, 95, 135, 175, 215, 255};
        return (lut[r] << 16) | (lut[g] << 8) | lut[b];
    }
    int v = 8 + (i - 232) * 10;
    return (v << 16) | (v << 8) | v;
}

/* ---------- Helpers ---------- */

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
    bool wide = cp_is_wide(cp);
    if (s->cx >= s->cols) s->cx = s->cols - 1;
    if (s->wrap_next && s->autowrap) {
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
            s->cur_attr.fg = DEFAULT_FG;
            s->cur_attr.bg = DEFAULT_BG;
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
            s->cur_attr.fg = base16[p - 30];
            s->cur_attr.attrs &= ~ATTR_DEFAULT_FG;
        }
        else if (p == 39) { s->cur_attr.fg = DEFAULT_FG; s->cur_attr.attrs |= ATTR_DEFAULT_FG; }
        else if (p >= 40 && p <= 47) {
            s->cur_attr.bg = base16[p - 40];
            s->cur_attr.attrs &= ~ATTR_DEFAULT_BG;
        }
        else if (p == 49) { s->cur_attr.bg = DEFAULT_BG; s->cur_attr.attrs |= ATTR_DEFAULT_BG; }
        else if (p >= 90 && p <= 97) {
            s->cur_attr.fg = base16[p - 90 + 8];
            s->cur_attr.attrs &= ~ATTR_DEFAULT_FG;
        }
        else if (p >= 100 && p <= 107) {
            s->cur_attr.bg = base16[p - 100 + 8];
            s->cur_attr.attrs &= ~ATTR_DEFAULT_BG;
        }
        else if (p == 38 || p == 48) {
            bool is_fg = (p == 38);
            if (i + 1 < n && s->params[i + 1] == 5 && i + 2 < n) {
                uint32_t c = palette256(s->params[i + 2] & 0xff);
                if (is_fg) { s->cur_attr.fg = c; s->cur_attr.attrs &= ~ATTR_DEFAULT_FG; }
                else       { s->cur_attr.bg = c; s->cur_attr.attrs &= ~ATTR_DEFAULT_BG; }
                i += 2;
            } else if (i + 1 < n && s->params[i + 1] == 2 && i + 4 < n) {
                uint32_t r = s->params[i + 2] & 0xff;
                uint32_t g = s->params[i + 3] & 0xff;
                uint32_t b = s->params[i + 4] & 0xff;
                uint32_t c = (r << 16) | (g << 8) | b;
                if (is_fg) { s->cur_attr.fg = c; s->cur_attr.attrs &= ~ATTR_DEFAULT_FG; }
                else       { s->cur_attr.bg = c; s->cur_attr.attrs &= ~ATTR_DEFAULT_BG; }
                i += 4;
            }
        }
    }
}

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
    case 1000: case 1002: case 1003: case 1006: case 1015:
        // mouse reporting - not implemented
        break;
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
    case 2004: /* bracketed paste - we set flag but don't need to act on it */ break;
    }
}

/* ---------- CSI ---------- */

static void handle_csi(Screen *s, uint8_t b) {
    if (b == '?') { s->priv = true; return; }
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
    case 'c':
        if (s->io.write) {
            const char *resp = "\x1b[?6c";
            s->io.write(s->io.user, (const uint8_t *)resp, strlen(resp));
        }
        break;
    case 's': s->saved_cx = s->cx; s->saved_cy = s->cy; break;
    case 'u': s->cx = s->saved_cx; s->cy = s->saved_cy; break;
    default: break;
    }
    s->pstate = ST_GROUND;
}

/* ---------- ESC ---------- */

static void finish_osc(Screen *s) {
    s->osc[s->osc_len] = 0;
    // Parse Ps;Pt
    int ps = 0;
    const char *p = s->osc;
    while (*p && *p != ';') { if (*p >= '0' && *p <= '9') ps = ps * 10 + (*p - '0'); p++; }
    if (*p == ';') p++;
    if ((ps == 0 || ps == 2) && s->io.set_title) s->io.set_title(s->io.user, p);
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
        s->cur_attr.fg = DEFAULT_FG; s->cur_attr.bg = DEFAULT_BG;
        s->cur_attr.attrs = ATTR_DEFAULT_FG | ATTR_DEFAULT_BG;
        for (int y = 0; y < s->rows; y++) clear_row(s, y);
        s->pstate = ST_GROUND; return;
    case '(': case ')': case '*': case '+':
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
    case 0x0E: case 0x0F: break; // SO/SI: charset - ignore
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
    for (size_t i = 0; i < n; i++) feed_byte(s, data[i]);
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
    s->cur_attr.fg = DEFAULT_FG;
    s->cur_attr.bg = DEFAULT_BG;
    s->cur_attr.attrs = ATTR_DEFAULT_FG | ATTR_DEFAULT_BG;
    s->main = calloc((size_t)cols * rows, sizeof(Cell));
    s->alt  = calloc((size_t)cols * rows, sizeof(Cell));
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
    free(s->main); free(s->alt); free(s->sb);
    free(s);
}

void screen_resize(Screen *s, int cols, int rows) {
    if (cols == s->cols && rows == s->rows) return;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;

    Cell *nmain = calloc((size_t)cols * rows, sizeof(Cell));
    Cell *nalt  = calloc((size_t)cols * rows, sizeof(Cell));
    Cell b = blank_cell(s);
    for (int y = 0; y < rows; y++)
        for (int x = 0; x < cols; x++) { nmain[y * cols + x] = b; nalt[y * cols + x] = b; }

    // Copy overlap (top-aligned, simple preservation)
    int cx = (cols < s->cols) ? cols : s->cols;
    int cy = (rows < s->rows) ? rows : s->rows;
    for (int y = 0; y < cy; y++) {
        memcpy(nmain + y * cols, s->main + y * s->cols, sizeof(Cell) * cx);
        memcpy(nalt  + y * cols, s->alt  + y * s->cols, sizeof(Cell) * cx);
    }

    // Rebuild scrollback at new width (truncate per row)
    Cell *nsb = NULL;
    if (s->sb_cap > 0) {
        nsb = calloc((size_t)s->sb_cap * cols, sizeof(Cell));
        for (int y = 0; y < s->sb_cap; y++)
            for (int x = 0; x < cols; x++) nsb[y * cols + x] = b;
        int ccols = (cols < s->cols) ? cols : s->cols;
        for (int i = 0; i < s->sb_len; i++) {
            int src = ((s->sb_head - s->sb_len + i) % s->sb_cap + s->sb_cap) % s->sb_cap;
            int dst = i;
            memcpy(nsb + dst * cols, s->sb + src * s->cols, sizeof(Cell) * ccols);
        }
        s->sb_head = s->sb_len % s->sb_cap;
    }

    free(s->main); free(s->alt); free(s->sb);
    s->main = nmain; s->alt = nalt; s->sb = nsb;
    s->cols = cols; s->rows = rows;
    s->scroll_top = 0;
    s->scroll_bot = rows - 1;
    if (s->cx >= cols) s->cx = cols - 1;
    if (s->cy >= rows) s->cy = rows - 1;
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

void screen_scroll_view(Screen *s, int delta_rows) {
    int v = s->view_off + delta_rows;
    if (v < 0) v = 0;
    if (v > s->sb_len) v = s->sb_len;
    s->view_off = v;
}

void screen_scroll_reset(Screen *s) { s->view_off = 0; }

Cell screen_view_cell(const Screen *s, int col, int vy) {
    if (col < 0 || col >= s->cols || vy < 0 || vy >= s->rows) {
        Cell e = {0, DEFAULT_FG, DEFAULT_BG, ATTR_DEFAULT_FG | ATTR_DEFAULT_BG};
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
