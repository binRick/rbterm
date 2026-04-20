#include "render.h"
#include "emoji.h"
#include "raylib.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

/* --- Glyph texture caches ---
   Two caches backed by the same rasterizer:
   * g_emoji:    colour emoji (e.g. Apple Color Emoji) — tint WHITE at draw.
   * g_fallback: monochrome vector glyphs from a broad-coverage font
                 (e.g. Menlo) — tint with the cell's fg at draw. Used when
                 the main font is missing the glyph AND it's not a colour
                 emoji either. */

typedef struct {
    uint32_t cp;
    Texture2D tex;
    bool ok;
    bool attempted;
    bool colored;   /* true: SBIX/colour emoji — don't tint; false: vector mask — tint with fg */
} GlyphEntry;

typedef struct {
    GlyphEntry *items;
    int count, cap;
    int pixel_size;
    const char *font_name;
} GlyphCache;

static GlyphCache g_emoji    = { .font_name = "Apple Color Emoji" };
static GlyphCache g_fallback = { .font_name = "Menlo" };

static void glyph_cache_clear(GlyphCache *c) {
    for (int i = 0; i < c->count; i++) {
        if (c->items[i].ok) UnloadTexture(c->items[i].tex);
    }
    c->count = 0;
}

static void emoji_cache_clear(void) {
    glyph_cache_clear(&g_emoji);
    glyph_cache_clear(&g_fallback);
}

static GlyphEntry *glyph_lookup(GlyphCache *c, uint32_t cp, int pixel_size) {
    if (c->pixel_size != pixel_size) {
        glyph_cache_clear(c);
        c->pixel_size = pixel_size;
    }
    for (int i = 0; i < c->count; i++) {
        if (c->items[i].cp == cp) return &c->items[i];
    }
    if (c->count == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 32;
        c->items = realloc(c->items, sizeof(GlyphEntry) * c->cap);
    }
    GlyphEntry *e = &c->items[c->count++];
    e->cp = cp;
    e->ok = false;
    e->attempted = true;
    e->colored = false;
    uint8_t *rgba = NULL;
    int w = 0, h = 0;
    bool colored = false;
    if (glyph_render(c->font_name, cp, pixel_size, &rgba, &w, &h, &colored) && rgba) {
        Image img = {
            .data = rgba,
            .width = w, .height = h, .mipmaps = 1,
            .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8,
        };
        e->tex = LoadTextureFromImage(img);
        SetTextureFilter(e->tex, TEXTURE_FILTER_BILINEAR);
        free(rgba);
        e->ok = e->tex.id != 0;
        e->colored = colored;
    }
    return e;
}

static bool is_emoji_cp(uint32_t cp) {
    if (cp >= 0x1F000) return true;
    if (cp >= 0x2600  && cp <= 0x27BF) return true;
    return false;
}

/* Set of codepoints the main font failed to rasterize (stb_truetype leaves
   image.width == 0 for glyphs the font doesn't define). Built after every
   font load so the draw loop knows when to fall back to the emoji font. */
typedef struct {
    uint32_t *cps;
    int count, cap;
} MissingSet;

static MissingSet g_missing;

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static void missing_set_clear(void) {
    free(g_missing.cps);
    g_missing.cps = NULL;
    g_missing.count = g_missing.cap = 0;
}

static void missing_set_build(Font *f) {
    missing_set_clear();
    if (!f || !f->glyphs) return;
    for (int i = 0; i < f->glyphCount; i++) {
        uint32_t cp = (uint32_t)f->glyphs[i].value;
        if (cp == ' ' || cp == 0) continue;
        if (f->glyphs[i].image.width > 0 && f->glyphs[i].image.height > 0) continue;
        if (g_missing.count == g_missing.cap) {
            g_missing.cap = g_missing.cap ? g_missing.cap * 2 : 64;
            g_missing.cps = realloc(g_missing.cps, g_missing.cap * sizeof(uint32_t));
        }
        g_missing.cps[g_missing.count++] = cp;
    }
    qsort(g_missing.cps, g_missing.count, sizeof(uint32_t), cmp_u32);
    fprintf(stderr, "rbterm: %d codepoint(s) missing from main font\n",
            g_missing.count);
}

static bool font_missing(uint32_t cp) {
    int lo = 0, hi = g_missing.count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_missing.cps[mid] == cp) return true;
        if (g_missing.cps[mid] < cp) lo = mid + 1; else hi = mid - 1;
    }
    return false;
}

/* Curated codepoint set: ASCII, Latin-1, Latin Ext-A/B, punctuation,
   box drawing, block elements, powerline-ish arrows. */
static int *build_codepoints(int *out_count) {
    int ranges[][2] = {
        {0x20,   0x7E},
        {0xA0,   0x17F},
        {0x2000, 0x206F},
        {0x2190, 0x21FF},
        {0x2500, 0x259F},
        {0x25A0, 0x25FF},
        {0x2600, 0x26FF},
        {0x2700, 0x27BF},
        {0xE0A0, 0xE0B3}, // powerline
    };
    int total = 0;
    for (size_t i = 0; i < sizeof(ranges)/sizeof(ranges[0]); i++)
        total += ranges[i][1] - ranges[i][0] + 1;
    int *cps = malloc(sizeof(int) * total);
    int k = 0;
    for (size_t i = 0; i < sizeof(ranges)/sizeof(ranges[0]); i++)
        for (int cp = ranges[i][0]; cp <= ranges[i][1]; cp++) cps[k++] = cp;
    *out_count = total;
    return cps;
}

static bool file_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

const char *renderer_find_default_font(void) {
    static const char *candidates[] = {
        // Consolas first (Microsoft). Common install paths on all platforms.
        "/Library/Fonts/Consolas.ttf",
        "/Library/Fonts/Microsoft/Consolas.ttf",
        "C:/Windows/Fonts/consola.ttf",
        "/usr/share/fonts/truetype/msttcorefonts/Consolas.ttf",
#if defined(__APPLE__)
        // TTFs preferred over TTCs — stb_truetype in raylib has trouble
        // locating glyphs in font collections, which makes box-drawing and
        // other extended Unicode ranges render as '?'.
        "/System/Library/Fonts/SFNSMono.ttf",
        "/System/Library/Fonts/Monaco.ttf",
        "/Library/Fonts/Monaco.ttf",
        "/System/Library/Fonts/Menlo.ttc",
        "/System/Library/Fonts/Courier New.ttf",
#else
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
        "/usr/share/fonts/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/google-noto/NotoSansMono-Regular.ttf",
#endif
        NULL
    };
    for (int i = 0; candidates[i]; i++)
        if (file_exists(candidates[i])) return candidates[i];
    return NULL;
}

static Font *as_font(Renderer *r) { return (Font *)r->font_data; }

static void measure_cell(Renderer *r) {
    Font *f = as_font(r);
    Vector2 v = MeasureTextEx(*f, "M", (float)r->font_size, 0.0f);
    int w = (int)(v.x + 0.5f);
    if (w < 1) w = r->font_size / 2;
    // Use slightly taller line height for nicer spacing.
    int h = (int)((float)r->font_size * 1.2f + 0.5f);
    if (h < w) h = w;
    r->cell_w = w;
    r->cell_h = h;
}

static bool load_font_into(Renderer *r, const char *path, int size) {
    int cp_count;
    int *cps = build_codepoints(&cp_count);
    Font f = LoadFontEx(path, size, cps, cp_count);
    free(cps);
    if (f.texture.id == 0) return false;
    SetTextureFilter(f.texture, TEXTURE_FILTER_BILINEAR);
    if (r->font_data) {
        UnloadFont(*as_font(r));
    } else {
        r->font_data = malloc(sizeof(Font));
    }
    *as_font(r) = f;
    r->font_size = size;
    strncpy(r->font_path, path, sizeof(r->font_path) - 1);
    r->font_path[sizeof(r->font_path) - 1] = 0;
    measure_cell(r);
    missing_set_build(&f);
    return true;
}

bool renderer_init(Renderer *r, const char *font_path, int font_size) {
    memset(r, 0, sizeof(*r));
    if (font_size < 6) font_size = 14;
    if (!font_path || !*font_path) font_path = renderer_find_default_font();
    if (!font_path) {
        fprintf(stderr, "rbterm: no system monospace font found; pass --font PATH\n");
        return false;
    }
    fprintf(stderr, "rbterm: using font %s @ %dpt\n", font_path, font_size);
    return load_font_into(r, font_path, font_size);
}

void renderer_shutdown(Renderer *r) {
    emoji_cache_clear();
    free(g_emoji.items);    g_emoji.items = NULL;    g_emoji.cap = 0;
    free(g_fallback.items); g_fallback.items = NULL; g_fallback.cap = 0;
    missing_set_clear();
    if (!r || !r->font_data) return;
    UnloadFont(*as_font(r));
    free(r->font_data);
    r->font_data = NULL;
}

bool renderer_set_font_size(Renderer *r, int font_size) {
    if (font_size < 6) font_size = 6;
    if (font_size > 96) font_size = 96;
    emoji_cache_clear();
    return load_font_into(r, r->font_path, font_size);
}

bool renderer_set_font_path(Renderer *r, const char *path) {
    if (!path || !*path) return false;
    if (!file_exists(path)) return false;
    return load_font_into(r, path, r->font_size);
}

static Color col_from_rgb(uint32_t v, float alpha) {
    Color c;
    c.r = (v >> 16) & 0xff;
    c.g = (v >> 8) & 0xff;
    c.b = v & 0xff;
    c.a = (unsigned char)(alpha * 255.0f + 0.5f);
    return c;
}

static bool sel_contains(const Selection *sel, int col, int row) {
    if (!sel || !sel->active) return false;
    int r1 = sel->a_row, c1 = sel->a_col;
    int r2 = sel->b_row, c2 = sel->b_col;
    if (r1 > r2 || (r1 == r2 && c1 > c2)) {
        int tr = r1, tc = c1; r1 = r2; c1 = c2; r2 = tr; c2 = tc;
    }
    if (row < r1 || row > r2) return false;
    if (row == r1 && col < c1) return false;
    if (row == r2 && col > c2) return false;
    return true;
}

void renderer_draw(Renderer *r, Screen *s, double time_sec, bool focused,
                   const Selection *sel) {
    Font *f = as_font(r);
    int cols = screen_cols(s);
    int rows = screen_rows(s);
    int cw = r->cell_w, ch = r->cell_h;

    // Clear background with default bg
    ClearBackground(col_from_rgb(DEFAULT_BG, 1.0f));

    int cursor_vx = screen_cursor_x(s);
    int cursor_vy = screen_cursor_y(s) + screen_view_offset(s);
    bool show_cursor = screen_cursor_visible(s)
                        && screen_view_offset(s) == 0
                        && cursor_vy >= 0 && cursor_vy < rows;

    // First pass: backgrounds (with selection overlay)
    Color sel_color = {62, 68, 96, 200};  // translucent blue
    for (int y = 0; y < rows; y++) {
        int x = 0;
        while (x < cols) {
            Cell c = screen_view_cell(s, x, y);
            uint32_t bg = c.bg;
            uint16_t at = c.attrs;
            if (at & ATTR_REVERSE) { uint32_t t = bg; bg = c.fg; (void)t; }
            int x2 = x + 1;
            while (x2 < cols) {
                Cell d = screen_view_cell(s, x2, y);
                uint32_t dbg = (d.attrs & ATTR_REVERSE) ? d.fg : d.bg;
                if (dbg != bg) break;
                x2++;
            }
            if (bg != DEFAULT_BG || (at & ATTR_REVERSE)) {
                DrawRectangle(x * cw, y * ch, (x2 - x) * cw, ch, col_from_rgb(bg, 1.0f));
            }
            x = x2;
        }
        // Selection overlay as a second run on the same row
        int sx = 0;
        while (sx < cols) {
            if (!sel_contains(sel, sx, y)) { sx++; continue; }
            int sx2 = sx + 1;
            while (sx2 < cols && sel_contains(sel, sx2, y)) sx2++;
            DrawRectangle(sx * cw, y * ch, (sx2 - sx) * cw, ch, sel_color);
            sx = sx2;
        }
    }

    // Cursor background (block)
    if (show_cursor) {
        bool blink_on = ((long long)(time_sec * 2.0) & 1) == 0;
        if (!focused) {
            DrawRectangleLines(cursor_vx * cw, cursor_vy * ch, cw, ch, col_from_rgb(CURSOR_COLOR, 1.0f));
        } else if (blink_on) {
            DrawRectangle(cursor_vx * cw, cursor_vy * ch, cw, ch, col_from_rgb(CURSOR_COLOR, 1.0f));
        }
    }

    // Second pass: glyphs
    Vector2 pos;
    int glyph_y_offset = (ch - r->font_size) / 2;
    /* Rasterize emoji at 2x the displayed pixel height for crisper retina output. */
    int emoji_px = ch * 2;
    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < cols; x++) {
            Cell c = screen_view_cell(s, x, y);
            if (c.attrs & ATTR_WIDE_CONT) continue;  /* drawn as part of the head cell */
            if (c.cp == 0 || c.cp == ' ') continue;
            if (c.attrs & ATTR_HIDDEN) continue;

            uint32_t fg = c.fg;
            if (c.attrs & ATTR_REVERSE) fg = c.bg;

            bool at_cursor = (show_cursor && x == cursor_vx && y == cursor_vy
                              && ((long long)(time_sec * 2.0) & 1) == 0 && focused);
            if (at_cursor) fg = DEFAULT_BG;

            float alpha = (c.attrs & ATTR_DIM) ? 0.6f : 1.0f;
            int span = (c.attrs & ATTR_WIDE) ? 2 : 1;

            bool main_has_glyph = !font_missing(c.cp);

            /* Helper: draw a cached glyph texture. Tint with WHITE for
               colour bitmap glyphs (preserves baked colours), or with the
               cell's foreground for monochrome vector masks. */
            #define DRAW_GLYPH(cache) do {                                           \
                GlyphEntry *_e = glyph_lookup(&(cache), c.cp, emoji_px);             \
                if (_e && _e->ok) {                                                  \
                    Color _tint = _e->colored                                        \
                        ? col_from_rgb(0xFFFFFF, alpha)                              \
                        : col_from_rgb(fg, alpha);                                   \
                    float _dw = (float)(cw * span), _dh = (float)ch;                 \
                    float _s = _dh / (float)_e->tex.height;                          \
                    if ((float)_e->tex.width * _s > _dw) _s = _dw / (float)_e->tex.width; \
                    float _w = (float)_e->tex.width * _s, _h = (float)_e->tex.height * _s; \
                    Rectangle _src = {0, 0, (float)_e->tex.width, (float)_e->tex.height}; \
                    Rectangle _dst = { (float)(x * cw) + (_dw - _w) / 2.0f,          \
                                       (float)(y * ch) + (_dh - _h) / 2.0f, _w, _h };\
                    DrawTexturePro(_e->tex, _src, _dst, (Vector2){0, 0}, 0.0f, _tint);\
                    goto glyph_drawn;                                                \
                }                                                                    \
            } while (0)

            /* Try emoji cache first for high-plane emoji (always), or for any
               codepoint the main font can't render. */
            if (c.cp >= 0x1F000 || !main_has_glyph) {
                DRAW_GLYPH(g_emoji);
            }
            /* Try Menlo fallback for anything the main font doesn't have. */
            if (!main_has_glyph) {
                DRAW_GLYPH(g_fallback);
                /* Nothing in any font: visible placeholder. */
                pos.x = (float)(x * cw);
                pos.y = (float)(y * ch + glyph_y_offset);
                DrawTextCodepoint(*f, '?', pos, (float)r->font_size,
                                  col_from_rgb(fg, alpha * 0.6f));
                continue;
            }

            pos.x = (float)(x * cw);
            pos.y = (float)(y * ch + glyph_y_offset);
            DrawTextCodepoint(*f, (int)c.cp, pos, (float)r->font_size, col_from_rgb(fg, alpha));

        glyph_drawn:;
            #undef DRAW_GLYPH
            if (c.attrs & ATTR_BOLD) {
                Vector2 p2 = { pos.x + 1.0f, pos.y };
                DrawTextCodepoint(*f, (int)c.cp, p2, (float)r->font_size, col_from_rgb(fg, alpha));
            }
            if (c.attrs & ATTR_UNDERLINE) {
                DrawRectangle(x * cw, y * ch + ch - 2, cw * span, 1, col_from_rgb(fg, alpha));
            }
            if (c.attrs & ATTR_STRIKE) {
                DrawRectangle(x * cw, y * ch + ch / 2, cw * span, 1, col_from_rgb(fg, alpha));
            }
        }
    }

    // Scrollback indicator
    if (screen_view_offset(s) > 0) {
        int W = cols * cw;
        int H = rows * ch;
        DrawRectangle(W - 4, 0, 4, H, (Color){100, 100, 100, 120});
        int sb_len = screen_scrollback_len(s);
        if (sb_len > 0) {
            int off = screen_view_offset(s);
            float frac_pos = (float)(sb_len - off) / (float)sb_len;
            int bar_h = H / 10; if (bar_h < 20) bar_h = 20;
            int bar_y = (int)((H - bar_h) * frac_pos);
            DrawRectangle(W - 4, bar_y, 4, bar_h, (Color){200, 200, 200, 200});
        }
    }
}
