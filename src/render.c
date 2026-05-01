#include "render.h"
#include "emoji.h"
#include "raylib.h"
#include "shape.h"
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

/* --- Image texture cache ---
 *
 * Sixel + kitty images arrive as RGBA8 bitmaps attached to a Screen.
 * Uploading to a GPU texture every frame is wasteful, so we keep a
 * small cache keyed by (ScreenImage*, generation). `generation` is a
 * counter that never repeats within a run, so when an image is freed
 * and a new one is allocated at the same address it still misses
 * the cache. */
typedef struct {
    const ScreenImage *key;
    uint64_t gen;
    Texture2D tex;
    bool in_use;
    bool ok;
} ImgEntry;

#define IMG_CACHE_CAP 128
static ImgEntry g_img_cache[IMG_CACHE_CAP];

/* Look up an entry for this ScreenImage in the per-renderer image
   cache; if it's not there (or has been replaced), upload its RGBA
   pixels to a new GPU texture. Returns NULL on alloc failure or
   when the screen reports zero pixel dims. Mark-and-sweep
   eviction: in_use is reset by img_cache_begin_frame and slots
   that don't get their flag set during the draw pass are evicted
   by img_cache_prune_frame. */
static ImgEntry *img_cache_get_or_upload(const ScreenImage *img) {
    uint64_t g = screen_image_generation(img);
    for (int i = 0; i < IMG_CACHE_CAP; i++) {
        if (g_img_cache[i].ok && g_img_cache[i].key == img
            && g_img_cache[i].gen == g) {
            g_img_cache[i].in_use = true;
            return &g_img_cache[i];
        }
    }
    int slot = -1;
    for (int i = 0; i < IMG_CACHE_CAP; i++) {
        if (!g_img_cache[i].ok) { slot = i; break; }
    }
    if (slot < 0) {
        /* Cache full — evict the first entry not marked in use this frame.
           If every slot is in use (unlikely with cap=128), overwrite 0. */
        for (int i = 0; i < IMG_CACHE_CAP; i++) {
            if (!g_img_cache[i].in_use) { slot = i; break; }
        }
        if (slot < 0) slot = 0;
        if (g_img_cache[slot].ok) UnloadTexture(g_img_cache[slot].tex);
        memset(&g_img_cache[slot], 0, sizeof(g_img_cache[slot]));
    }
    int w = screen_image_px_w(img);
    int h = screen_image_px_h(img);
    const unsigned char *rgba = screen_image_rgba(img);
    if (w <= 0 || h <= 0 || !rgba) return NULL;
    Image im = { 0 };
    im.data = (void *)rgba;           /* shared; LoadTextureFromImage copies */
    im.width = w; im.height = h;
    im.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    im.mipmaps = 1;
    Texture2D tex = LoadTextureFromImage(im);
    if (tex.id == 0) return NULL;
    SetTextureFilter(tex, TEXTURE_FILTER_BILINEAR);
    g_img_cache[slot].key    = img;
    g_img_cache[slot].gen    = g;
    g_img_cache[slot].tex    = tex;
    g_img_cache[slot].in_use = true;
    g_img_cache[slot].ok     = true;
    return &g_img_cache[slot];
}

/* Drop GPU textures for image cache entries that weren't touched
   this frame — i.e. images that have scrolled off or been evicted
   from the screen's list. Called at end-of-frame after all in_use
   flags have been set by the draw pass. */
static void img_cache_prune_frame(void) {
    for (int i = 0; i < IMG_CACHE_CAP; i++) {
        if (g_img_cache[i].ok && !g_img_cache[i].in_use) {
            UnloadTexture(g_img_cache[i].tex);
            memset(&g_img_cache[i], 0, sizeof(g_img_cache[i]));
        }
    }
}

/* Mark every image-cache slot as "not yet seen" at the start of a
   frame. The draw pass sets in_use=true for slots it actually
   blits; img_cache_prune_frame later evicts the ones still false. */
static void img_cache_begin_frame(void) {
    for (int i = 0; i < IMG_CACHE_CAP; i++) g_img_cache[i].in_use = false;
}

/* Drop every cached glyph in `c`, freeing its GPU texture. Called
   when the font / size changes and the existing rasterisations are
   stale. */
static void glyph_cache_clear(GlyphCache *c) {
    for (int i = 0; i < c->count; i++) {
        if (c->items[i].ok) UnloadTexture(c->items[i].tex);
    }
    c->count = 0;
}

/* Drop both emoji + monochrome-fallback glyph caches together —
   typically called when font size changes and any rasterised
   substitute glyphs need re-baking at the new pixel size. */
static void emoji_cache_clear(void) {
    glyph_cache_clear(&g_emoji);
    glyph_cache_clear(&g_fallback);
}

/* Find or create a glyph cache entry for `cp` at `pixel_size`. If
   the cache's recorded size doesn't match, we drop everything in
   it (size changes are rare). Cache hits return immediately;
   misses call glyph_render (Core Text on macOS, no-op stub on
   other platforms) and upload the rasterised RGBA to a Texture2D.
   `attempted` is sticky so we don't keep retrying glyphs the
   rasteriser already declined. */
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

/* Hand-rasterised box-drawing glyphs. Fonts don't always draw these to the
   exact cell-edge, so adjacent cells would show thin gaps between "─" runs.
   Drawing them as rectangles guarantees every `─` butts against its
   neighbour. Returns true when the codepoint was handled. */
static bool draw_box_drawing(int x, int y, int cw, int ch,
                             uint32_t cp, Color col) {
    int t = 1;                    /* thin-line thickness */
    int t2 = 2;                   /* heavy-line thickness */
    int mx = x + cw / 2;
    int my = y + ch / 2;
    int left_w  = cw / 2 + t;     /* from x to middle inclusive */
    int right_w = cw - cw / 2;    /* from middle to x+cw */
    int up_h    = ch / 2 + t;
    int down_h  = ch - ch / 2;

    switch (cp) {
    /* Light lines */
    case 0x2500: DrawRectangle(x, my, cw, t, col); return true;                            /* ─ */
    case 0x2502: DrawRectangle(mx, y, t, ch, col); return true;                            /* │ */
    /* Corners */
    case 0x250C: DrawRectangle(mx, my, right_w, t, col); DrawRectangle(mx, my, t, down_h, col); return true; /* ┌ */
    case 0x2510: DrawRectangle(x, my, left_w, t, col); DrawRectangle(mx, my, t, down_h, col); return true;   /* ┐ */
    case 0x2514: DrawRectangle(mx, my, right_w, t, col); DrawRectangle(mx, y, t, up_h, col); return true;    /* └ */
    case 0x2518: DrawRectangle(x, my, left_w, t, col); DrawRectangle(mx, y, t, up_h, col); return true;      /* ┘ */
    /* Tees */
    case 0x251C: DrawRectangle(mx, y, t, ch, col); DrawRectangle(mx, my, right_w, t, col); return true;      /* ├ */
    case 0x2524: DrawRectangle(mx, y, t, ch, col); DrawRectangle(x, my, left_w, t, col); return true;        /* ┤ */
    case 0x252C: DrawRectangle(x, my, cw, t, col); DrawRectangle(mx, my, t, down_h, col); return true;       /* ┬ */
    case 0x2534: DrawRectangle(x, my, cw, t, col); DrawRectangle(mx, y, t, up_h, col); return true;          /* ┴ */
    /* Cross */
    case 0x253C: DrawRectangle(x, my, cw, t, col); DrawRectangle(mx, y, t, ch, col); return true;            /* ┼ */
    /* Heavy lines */
    case 0x2501: DrawRectangle(x, my - t2/2, cw, t2, col); return true;                    /* ━ */
    case 0x2503: DrawRectangle(mx - t2/2, y, t2, ch, col); return true;                    /* ┃ */
    /* Double lines (simple two-line approximation) */
    case 0x2550: DrawRectangle(x, my - 2, cw, t, col); DrawRectangle(x, my + 1, cw, t, col); return true;    /* ═ */
    case 0x2551: DrawRectangle(mx - 2, y, t, ch, col); DrawRectangle(mx + 1, y, t, ch, col); return true;    /* ║ */
    }
    return false;
}

/* Cheap heuristic: is this codepoint likely to be an emoji that
   should go through the colour-glyph path? Used to short-circuit
   font_missing() for the common case (any non-ASCII PUA / dingbat
   codepoint should try the emoji rasteriser before falling back to
   `?`). Not exhaustive — false negatives just mean an emoji draws
   monochrome via Menlo fallback, which is OK. */
static bool is_emoji_cp(uint32_t cp) {
    if (cp >= 0x1F000) return true;
    if (cp >= 0x2600  && cp <= 0x27BF) return true;
    return false;
}

/* Sorted set of codepoints the main font CAN render (loaded with a
   non-empty bitmap). Codepoints not present here are considered
   "missing" — even those that were never loaded into the atlas at
   all (e.g. CJK ideographs, since build_codepoints doesn't list
   them). Without this distinction, raylib's DrawTextCodepoint would
   draw the font's `.notdef` glyph (a tofu box or a literal "?"). */
typedef struct {
    uint32_t *cps;
    int count, cap;
} CodepointSet;

static CodepointSet g_present;

/* qsort comparator — ordered ascending so font_missing can binary-
   search the codepoint set. */
static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

/* Forget the present-codepoint set; called before re-building it
   for a new font. */
static void present_set_clear(void) {
    free(g_present.cps);
    g_present.cps = NULL;
    g_present.count = g_present.cap = 0;
}

/* Walk every glyph raylib successfully rasterised in `f` and record
   its codepoint into the present-set. Sorted afterwards so
   font_missing's binary search works. Called once per font load. */
static void present_set_build(Font *f) {
    present_set_clear();
    if (!f || !f->glyphs) return;
    for (int i = 0; i < f->glyphCount; i++) {
        uint32_t cp = (uint32_t)f->glyphs[i].value;
        /* Skip glyphs raylib loaded but couldn't rasterize. */
        if (f->glyphs[i].image.width <= 0 || f->glyphs[i].image.height <= 0)
            continue;
        if (g_present.count == g_present.cap) {
            g_present.cap = g_present.cap ? g_present.cap * 2 : 256;
            g_present.cps = realloc(g_present.cps, g_present.cap * sizeof(uint32_t));
        }
        g_present.cps[g_present.count++] = cp;
    }
    qsort(g_present.cps, g_present.count, sizeof(uint32_t), cmp_u32);
}

/* True iff `cp` is NOT in the main font's rasterised set, meaning
   the renderer should try emoji / Menlo fallback / `?` placeholder
   instead of drawing the font's tofu .notdef glyph. */
static bool font_missing(uint32_t cp) {
    /* Space + control chars: nothing to draw, treat as "present" so
       the renderer doesn't burn cycles trying to substitute them. */
    if (cp == 0 || cp == ' ' || cp < 0x20) return false;
    int lo = 0, hi = g_present.count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_present.cps[mid] == cp) return false;
        if (g_present.cps[mid] < cp) lo = mid + 1; else hi = mid - 1;
    }
    return true;
}

/* Build the codepoint list passed to LoadFontEx. Curated set:
   ASCII, Latin-1, Latin Ext-A/B, general punctuation, arrows, box
   drawing, block elements, dingbats, powerline. Caller frees. */
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

/* True if the path exists and is statable. Used by the font picker
   to skip non-existent system paths without spamming raylib's
   "couldn't load" log. */
static bool file_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

/* Return a sane default-monospace path or NULL. Walks a list of
   well-known per-OS install locations in priority order. Caller
   must NOT free — points into a static. */
const char *renderer_find_default_font(void) {
    static const char *candidates[] = {
#if defined(__EMSCRIPTEN__)
        /* WebAssembly demo: fonts are preloaded from assets/fonts/ into
           /fonts in MEMFS by the CMake --preload-file flag. */
        "/fonts/JetBrainsMono-Regular.ttf",
        "/fonts/FiraCode-Regular.ttf",
        "/fonts/SourceCodePro-Regular.ttf",
        "/fonts/Hack-Regular.ttf",
        "/fonts/IBMPlexMono-Regular.ttf",
        "/fonts/Inconsolata-Regular.ttf",
#endif
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

/* Cast helper — Renderer.font_data is a void* in the public header
   so consumers don't need to include raylib.h. */
static Font *as_font(Renderer *r) { return (Font *)r->font_data; }

/* Compute the cell pixel dimensions from the current font + size.
   Width is the rasterised "M" advance plus user-configured letter
   spacing; height is 1.2x the nominal point size — slightly tall so
   underlines and box-drawing don't crowd the next row. */
static void measure_cell(Renderer *r) {
    Font *f = as_font(r);
    Vector2 v = MeasureTextEx(*f, "M", (float)r->font_size, 0.0f);
    int w = (int)(v.x + 0.5f);
    if (w < 1) w = r->font_size / 2;
    int h = (int)((float)r->font_size * 1.2f + 0.5f);
    if (h < w) h = w;
    r->cell_w_base = w;
    r->cell_w = w + r->cell_extra_w;
    r->cell_h = h;
}

/* Adjust the per-cell pixel width by the configured letter-spacing
   delta (clamped 0..32). Wider cells trade horizontal density for
   readability on dense displays. */
void renderer_set_cell_spacing(Renderer *r, int extra_w) {
    if (extra_w < 0)  extra_w = 0;
    if (extra_w > 32) extra_w = 32;
    r->cell_extra_w = extra_w;
    r->cell_w = r->cell_w_base + extra_w;
}

/* Forward decl — definition follows install_font. */
static void backup_font_ensure(int atlas_size);

/* Install a freshly-loaded Font into the renderer, replacing whatever
   was loaded before. Shared by load_font_into / load_font_data_into.
   Records the path for future Settings-modal display, rebuilds the
   present-codepoint set, and reloads the broad backup font at the
   matching atlas size so missing-glyph fallback rasterises at the
   right resolution. */
static void install_font(Renderer *r, Font f, int size, const char *path) {
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
    present_set_build(&f);
    int atlas_size = size * 3;
    if (atlas_size > 144) atlas_size = 144;
    backup_font_ensure(atlas_size);
}

/* Broad-coverage backup font. Loaded once from main.c (typically the
   embedded DejaVu Sans Mono blob). When the primary font lacks a
   glyph, we draw it from this font instead of falling straight through
   to the emoji rasterizer / "?" placeholder. */
static const unsigned char *g_backup_data = NULL;
static int g_backup_data_size = 0;
static char g_backup_ext[8] = {0};
static Font g_backup_font = {0};
static int  g_backup_loaded_size = 0;     /* 0 = not yet loaded */

/* Drop the loaded backup font (if any), freeing its GPU texture
   and atlas. Idempotent. */
static void backup_font_unload(void) {
    if (g_backup_loaded_size > 0) {
        UnloadFont(g_backup_font);
        memset(&g_backup_font, 0, sizeof(g_backup_font));
        g_backup_loaded_size = 0;
    }
}

/* Lazy-load the backup font at the requested atlas size, replacing
   any previous load. No-op if (a) no backup blob is registered or
   (b) the backup is already at the right size. */
static void backup_font_ensure(int atlas_size) {
    if (!g_backup_data || g_backup_data_size <= 0) return;
    if (g_backup_loaded_size == atlas_size) return;
    backup_font_unload();
    int cp_count;
    int *cps = build_codepoints(&cp_count);
    char ft[8] = ".";
    strncat(ft, g_backup_ext[0] ? g_backup_ext : "ttf", sizeof(ft) - 2);
    g_backup_font = LoadFontFromMemory(ft, g_backup_data, g_backup_data_size,
                                       atlas_size, cps, cp_count);
    free(cps);
    if (g_backup_font.texture.id == 0) return;
    SetTextureFilter(g_backup_font.texture, TEXTURE_FILTER_BILINEAR);
    g_backup_loaded_size = atlas_size;
}

/* Register the embedded backup-font blob (DejaVu Sans Mono in this
   build) and trigger a lazy reload at the next size change. The
   backup is rasterised on first need so we don't waste atlas RAM
   if the primary font already has every glyph. */
void renderer_set_backup_font_data(const unsigned char *data, int data_size,
                                   const char *ext) {
    g_backup_data = data;
    g_backup_data_size = data_size;
    if (ext && *ext) {
        strncpy(g_backup_ext, ext, sizeof(g_backup_ext) - 1);
        g_backup_ext[sizeof(g_backup_ext) - 1] = 0;
    } else {
        g_backup_ext[0] = 0;
    }
    backup_font_unload();   /* lazy reload at next size change */
}

/* True iff the backup font has a non-empty rasterised glyph for
   `cp` — used by the missing-glyph fallback to decide whether to
   draw via the backup atlas before giving up to "?". */
static bool backup_font_has(uint32_t cp) {
    if (g_backup_loaded_size == 0 || !g_backup_font.glyphs) return false;
    int idx = GetGlyphIndex(g_backup_font, (int)cp);
    if (idx < 0 || idx >= g_backup_font.glyphCount) return false;
    GlyphInfo *gi = &g_backup_font.glyphs[idx];
    return (gi->image.width > 0 && gi->image.height > 0);
}

/* Free the cached font bytes if we own them, then null the slot. The
   embedded blobs we get from .incbin / RCDATA are read-only static
   data we mustn't free; the disk-loaded path mallocs its own copy
   and owns it. The owned flag distinguishes. */
static void forget_font_data(Renderer *r) {
    if (r->cur_data_owned && r->cur_data) {
        free((void *)r->cur_data);
    }
    r->cur_data       = NULL;
    r->cur_data_size  = 0;
    r->cur_data_owned = false;
    r->cur_ext[0] = 0;
}

/* Read a whole file into a heap buffer. Returns NULL on error; on
   success writes the byte count via *out_size. Caller frees. */
static unsigned char *slurp_file(const char *path, size_t *out_size) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long sz = ftell(fp);
    if (sz <= 0)                     { fclose(fp); return NULL; }
    if (fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return NULL; }
    unsigned char *buf = malloc((size_t)sz);
    if (!buf) { fclose(fp); return NULL; }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        free(buf); fclose(fp); return NULL;
    }
    fclose(fp);
    *out_size = (size_t)sz;
    return buf;
}

/* Forward decl: load_font_into reads the file then hands off to the
   shared in-memory loader so HarfBuzz has bytes to shape against. */
static bool load_font_data_into(Renderer *r, const unsigned char *data,
                                int data_size, const char *ext, int size,
                                const char *display_path);

/* Load a font from disk + install it. The bytes are slurped into a
   heap buffer first so they remain available for HarfBuzz shaping
   (which can't take a path) and so the size-change path doesn't
   re-read from disk. Returns false if the file's missing or fails
   to parse. */
static bool load_font_into(Renderer *r, const char *path, int size) {
    size_t sz = 0;
    unsigned char *buf = slurp_file(path, &sz);
    if (getenv("RBTERM_DEBUG")) {
        fprintf(stderr, "load_font_into: path=%s slurped=%zu bytes\n",
                path ? path : "(null)", sz);
        fflush(stderr);
    }
    if (!buf) return false;
    /* Pull the extension off the path so LoadFontFromMemory routes to
       the right parser. */
    const char *dot = strrchr(path, '.');
    const char *ext = dot ? dot + 1 : "ttf";
    bool ok = load_font_data_into(r, buf, (int)sz, ext, size, path);
    if (ok) {
        /* load_font_data_into now points cur_data at our heap buffer.
           Mark it as owned so the next forget_font_data frees it. */
        r->cur_data_owned = true;
    } else {
        free(buf);
    }
    return ok;
}

/* Load a font from an in-memory blob (embedded fonts) and install
   it. `display_path` is what gets shown in the font picker /
   settings — typically a synthetic "embedded:NAME" string. */
static bool load_font_data_into(Renderer *r, const unsigned char *data,
                                int data_size, const char *ext, int size,
                                const char *display_path) {
    int cp_count;
    int *cps = build_codepoints(&cp_count);
    int atlas_size = size * 3;
    if (atlas_size > 144) atlas_size = 144;
    char file_type[8] = ".";
    if (ext && *ext) {
        strncat(file_type, ext, sizeof(file_type) - 2);
    } else {
        strncat(file_type, "ttf", sizeof(file_type) - 2);
    }
    Font f = LoadFontFromMemory(file_type, data, data_size,
                                atlas_size, cps, cp_count);
    free(cps);
    if (getenv("RBTERM_DEBUG")) {
        fprintf(stderr,
                "load_font_data_into: ext=%s size=%d data_size=%d "
                "atlas=%d texture_id=%u glyphs=%d display=%s\n",
                ext ? ext : "?", size, data_size, atlas_size,
                f.texture.id, f.glyphCount,
                display_path ? display_path : "?");
        fflush(stderr);
    }
    if (f.texture.id == 0) return false;
    SetTextureFilter(f.texture, TEXTURE_FILTER_BILINEAR);
    install_font(r, f, size, display_path);
    /* Free any previously-owned bytes before pointing cur_data at the
       new blob. The disk loader's caller flips cur_data_owned back on
       after we return; embedded callers leave it false. */
    forget_font_data(r);
    r->cur_data = data;
    r->cur_data_size = data_size;
    r->cur_data_owned = false;
    if (ext && *ext) {
        strncpy(r->cur_ext, ext, sizeof(r->cur_ext) - 1);
        r->cur_ext[sizeof(r->cur_ext) - 1] = 0;
    } else {
        r->cur_ext[0] = 0;
    }
    /* Rebuild the shape handle if ligatures are on — the new font may
       have a different OpenType GSUB table, and the shape font's
       cached glyph bitmaps are tied to the old font/size. */
    if (r->shape_font) {
        shape_font_free((ShapeFont *)r->shape_font);
        r->shape_font = NULL;
    }
    if (r->ligatures) {
        r->shape_font = shape_font_open(data, data_size, size);
    }
    return true;
}

/* Bring a Renderer up from zero state with a font loaded from disk
   at the given size. Returns false if the font path doesn't open. */
bool renderer_init(Renderer *r, const char *font_path, int font_size) {
    memset(r, 0, sizeof(*r));
    if (font_size < 6) font_size = 14;
    r->bg_alpha = 1.0f;
    if (!font_path || !*font_path) font_path = renderer_find_default_font();
    if (!font_path) {
        fprintf(stderr, "rbterm: no system monospace font found; pass --font PATH\n");
        return false;
    }
    fprintf(stderr, "rbterm: using font %s @ %dpt\n", font_path, font_size);
    return load_font_into(r, font_path, font_size);
}

/* Bring a Renderer up using an in-memory font blob (the embedded
   fonts table). `display_path` is what the font picker shows to
   the user; pass "embedded:NAME" by convention. */
bool renderer_init_with_data(Renderer *r, const unsigned char *data,
                             int data_size, const char *ext,
                             const char *display_path, int font_size) {
    memset(r, 0, sizeof(*r));
    if (font_size < 6) font_size = 14;
    r->bg_alpha = 1.0f;
    if (!data || data_size <= 0) return false;
    fprintf(stderr, "rbterm: using embedded font %s @ %dpt\n",
            display_path ? display_path : "embedded:font", font_size);
    return load_font_data_into(r, data, data_size, ext, font_size,
                               display_path ? display_path : "embedded:font");
}

/* Free every GPU resource the renderer holds — atlas textures,
   glyph caches, present-codepoint set, backup font, primary font.
   Safe to call on an uninitialised Renderer. */
void renderer_shutdown(Renderer *r) {
    emoji_cache_clear();
    free(g_emoji.items);    g_emoji.items = NULL;    g_emoji.cap = 0;
    free(g_fallback.items); g_fallback.items = NULL; g_fallback.cap = 0;
    present_set_clear();
    backup_font_unload();
    if (!r || !r->font_data) return;
    if (r->shape_font) {
        shape_font_free((ShapeFont *)r->shape_font);
        r->shape_font = NULL;
    }
    forget_font_data(r);
    UnloadFont(*as_font(r));
    free(r->font_data);
    r->font_data = NULL;
}

/* Toggle ligature shaping. Building/freeing the shape handle is done
   here so callers don't have to know about ShapeFont. No-op when
   HarfBuzz wasn't linked at build time. */
void renderer_set_ligatures(Renderer *r, bool on) {
    if (!r) return;
    if (on == r->ligatures) return;
    r->ligatures = on;
    if (r->shape_font) {
        shape_font_free((ShapeFont *)r->shape_font);
        r->shape_font = NULL;
    }
    if (on && r->cur_data && r->cur_data_size > 0) {
        r->shape_font = shape_font_open(r->cur_data, r->cur_data_size,
                                        r->font_size);
    }
}

/* Re-rasterise the current font at a new pixel size. Clamps to
   6..96. Re-uses the cached embedded blob (if the current font came
   from memory) or re-reads from disk. Returns false on load fail —
   the renderer stays at its previous size. */
bool renderer_set_font_size(Renderer *r, int font_size) {
    if (font_size < 6) font_size = 6;
    if (font_size > 96) font_size = 96;
    emoji_cache_clear();
    /* Re-load from the cached embedded blob if the current font came
       from memory; otherwise re-read from disk. */
    if (r->cur_data && r->cur_data_size > 0) {
        return load_font_data_into(r, r->cur_data, r->cur_data_size,
                                   r->cur_ext, font_size, r->font_path);
    }
    return load_font_into(r, r->font_path, font_size);
}

/* Switch the active font to one at `path`, keeping the current
   size. Validates the path exists first. */
bool renderer_set_font_path(Renderer *r, const char *path) {
    if (getenv("RBTERM_DEBUG")) {
        fprintf(stderr, "renderer_set_font_path: path=%s exists=%d\n",
                path ? path : "(null)",
                (path && file_exists(path)) ? 1 : 0);
        fflush(stderr);
    }
    if (!path || !*path) return false;
    if (!file_exists(path)) return false;
    return load_font_into(r, path, r->font_size);
}

/* Switch the active font to one in memory. Same idea as
   renderer_init_with_data but for live-swap from the font picker. */
bool renderer_set_font_data(Renderer *r, const unsigned char *data,
                            int data_size, const char *ext,
                            const char *display_path) {
    if (!data || data_size <= 0) return false;
    emoji_cache_clear();
    return load_font_data_into(r, data, data_size, ext, r->font_size,
                               display_path ? display_path : "embedded:font");
}

/* 0xRRGGBB + alpha in 0..1 → raylib Color. Used everywhere a Cell's
   stored RGB needs to be drawn. */
static Color col_from_rgb(uint32_t v, float alpha) {
    Color c;
    c.r = (v >> 16) & 0xff;
    c.g = (v >> 8) & 0xff;
    c.b = v & 0xff;
    c.a = (unsigned char)(alpha * 255.0f + 0.5f);
    return c;
}

/* Resolve a cell's stored fg/bg to a concrete RGB. Cells hold a palette
   index when ATTR_*_INDEX is set, a default marker when ATTR_DEFAULT_*
   is set, and a raw RGB otherwise. Looking up on every draw means OSC 4
   palette changes retroactively recolour every cell on screen. */
static uint32_t resolve_fg(const Screen *s, Cell c) {
    if (c.attrs & ATTR_DEFAULT_FG) return screen_default_fg(s);
    if (c.attrs & ATTR_FG_INDEX)   return screen_palette(s, (int)(c.fg & 0xff));
    return c.fg;
}
/* Symmetric to resolve_fg — see the comment above. */
static uint32_t resolve_bg(const Screen *s, Cell c) {
    if (c.attrs & ATTR_DEFAULT_BG) return screen_default_bg(s);
    if (c.attrs & ATTR_BG_INDEX)   return screen_palette(s, (int)(c.bg & 0xff));
    return c.bg;
}

/* True iff the given (col, row) view position falls inside the
   active selection rectangle (which may run forward or backward —
   we normalise locally). Used to paint the selection overlay. */
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

/* Render one Screen into the window at (x_offset, y_offset). Walks
   the visible rows in three passes: backgrounds + selection overlay,
   cursor shape, then glyphs (with per-codepoint fallback to the
   emoji rasteriser, the embedded backup font, or a literal `?`).
   Also paints sixel/kitty image bitmaps (cached as Texture2D),
   underlines, the OSC 133 status gutter, and the right-edge
   scrollback indicator when view_offset > 0. `hover_col`/`hover_row`
   are -1 when the mouse isn't over this pane; otherwise they
   identify the cell to brighten for OSC 8 hyperlink hover. */
void renderer_draw(Renderer *r, Screen *s, double time_sec, bool focused,
                   const Selection *sel, int x_offset, int y_offset,
                   int hover_col, int hover_row) {
    Font *f = as_font(r);
    int cols = screen_cols(s);
    int rows = screen_rows(s);
    int cw = r->cell_w, ch = r->cell_h;
    int px = r->pad_x, py = r->pad_y;
    /* Let the screen know our current cell height so scroll accounting
       for pixel-heighted graphics lands on the right row. */
    screen_set_cell_h_px(s, ch);
    img_cache_begin_frame();
    float bga = r->bg_alpha;
    if (bga < 0.0f) bga = 0.0f;
    if (bga > 1.0f) bga = 1.0f;

    /* Backdrop: paint the whole terminal region (including padding) in
       the default bg, honouring the configured opacity. Non-default
       cell backgrounds drawn below still land at full alpha. */
    DrawRectangle(x_offset, y_offset,
                  cols * cw + 2 * px, rows * ch + 2 * py,
                  col_from_rgb(screen_default_bg(s), bga));

    Camera2D cam = {0};
    cam.offset = (Vector2){(float)(x_offset + px), (float)(y_offset + py)};
    cam.zoom = 1.0f;
    BeginMode2D(cam);

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
            uint16_t at = c.attrs;
            uint32_t bg = (at & ATTR_REVERSE) ? resolve_fg(s, c) : resolve_bg(s, c);
            int x2 = x + 1;
            while (x2 < cols) {
                Cell d = screen_view_cell(s, x2, y);
                uint32_t dbg = (d.attrs & ATTR_REVERSE) ? resolve_fg(s, d) : resolve_bg(s, d);
                if (dbg != bg) break;
                x2++;
            }
            if (bg != screen_default_bg(s) || (at & ATTR_REVERSE)) {
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

    // Cursor background. Shape comes from the per-Screen cursor style;
    // BLOCK_BLINK and DEFAULT toggle on/off at ~2Hz, the others are
    // steady. Unfocused windows get a hollow block regardless of style.
    if (show_cursor) {
        Color cc = col_from_rgb(screen_cursor_color(s), 1.0f);
        if (!focused) {
            DrawRectangleLines(cursor_vx * cw, cursor_vy * ch, cw, ch, cc);
        } else {
            CursorStyle cstyle = screen_cursor_style(s);
            bool blink_on = ((long long)(time_sec * 2.0) & 1) == 0;
            switch (cstyle) {
            case CURSOR_STYLE_BLOCK:
                DrawRectangle(cursor_vx * cw, cursor_vy * ch, cw, ch, cc);
                break;
            case CURSOR_STYLE_UNDERLINE: {
                int uh = ch / 6; if (uh < 2) uh = 2;
                DrawRectangle(cursor_vx * cw, cursor_vy * ch + ch - uh, cw, uh, cc);
                break;
            }
            case CURSOR_STYLE_BAR: {
                int bw = cw / 5; if (bw < 2) bw = 2;
                DrawRectangle(cursor_vx * cw, cursor_vy * ch, bw, ch, cc);
                break;
            }
            case CURSOR_STYLE_BLOCK_BLINK:
            case CURSOR_STYLE_DEFAULT:
            default:
                if (blink_on)
                    DrawRectangle(cursor_vx * cw, cursor_vy * ch, cw, ch, cc);
                break;
            }
        }
    }

    // Second pass: glyphs
    Vector2 pos;
    int glyph_y_offset = (ch - r->font_size) / 2;
    /* Rasterize emoji at 2x the displayed pixel height for crisper retina output. */
    int emoji_px = ch * 2;

    /* Per-row ligature map. lig_head[x] = -1 → cell x isn't part of a
       ligature cluster; >= 0 → cell x is consumed by a cluster whose
       head sits at column lig_head[x]. lig_glyph[head] holds the
       OpenType glyph_id to rasterise for the cluster's span.

       Static buffers because renderer_draw runs on the main thread
       only. 4096 cells is far above realistic terminal widths
       (vtebench / wide displays max out around 500-800 cols). */
    static int      lig_head[4096];
    static uint32_t lig_glyph[4096];
    static int      lig_span[4096];
    static int      lig_xoff[4096];
    static int      lig_yoff[4096];
    static uint32_t lig_cps[4096];
    static ShapedGlyph lig_out[4096];
    int lig_cols = (cols < 4096) ? cols : 4096;

    for (int y = 0; y < rows; y++) {
        /* Reset the per-row ligature map. */
        for (int i = 0; i < lig_cols; i++) lig_head[i] = -1;

        /* Shape the row when ligatures are on AND the font is
           shape-able (we only support shaping on embedded fonts that
           expose their raw bytes). hb_shape() is fast at this width;
           the cost when no ligatures fire is one shape call returning
           N-codepoint output that we walk and discard. */
        if (r->shape_font && lig_cols > 0) {
            for (int x = 0; x < lig_cols; x++) {
                Cell c0 = screen_view_cell(s, x, y);
                lig_cps[x] = (c0.attrs & ATTR_WIDE_CONT) ? 0 : (uint32_t)c0.cp;
            }
            int n = shape_row((ShapeFont *)r->shape_font, lig_cps, lig_cols,
                              lig_out, 4096);
            /* Strict allow-list: rbterm ligates only the arrow
               family (=>, ->, <-, <--, -->). Programmer fonts ship
               many other ligatures (==, !=, <=, >=, ||, &&, ::, ..,
               //, etc.) that look weird inside shell paths and
               command lines — and the rendering of split-glyph
               substitutions has alignment issues that make even
               "correct" ones (`==` etc.) read as broken. Suppressing
               anything outside the allow-list short-circuits all of
               that without a per-bug patch. */
            #define IS_ARROW_PAIR(a,b)                              \
                (((a) == '=' && (b) == '>') ||                      \
                 ((a) == '-' && (b) == '>') ||                      \
                 ((a) == '<' && (b) == '-'))
            #define IS_ARROW_TRIPLE(a,b,c)                          \
                (((a) == '<' && (b) == '-' && (c) == '-') ||        \
                 ((a) == '-' && (b) == '-' && (c) == '>'))

            for (int g = 0; g < n; g++) {
                if (lig_out[g].glyph_id == 0) continue;    /* .notdef → fall back */
                int head = lig_out[g].cell_col;
                int sp   = lig_out[g].cell_span;
                if (head < 0 || head >= lig_cols) continue;
                if (sp < 1) continue;
                if (head + sp > lig_cols) sp = lig_cols - head;

                /* Multi-cell merged cluster: input must spell out
                   an allowed arrow exactly. */
                if (sp >= 2) {
                    bool ok = false;
                    if (sp == 2) {
                        ok = IS_ARROW_PAIR(lig_cps[head], lig_cps[head+1]);
                    } else if (sp == 3) {
                        ok = IS_ARROW_TRIPLE(lig_cps[head],
                                             lig_cps[head+1],
                                             lig_cps[head+2]);
                    }
                    if (!ok) continue;
                }
                /* Single-cell split substitution: this glyph replaces
                   one of the two glyphs in an arrow pair. Only allow
                   when this cell IS part of an arrow context with a
                   valid neighbour. */
                if (sp == 1) {
                    uint32_t cp = lig_cps[head];
                    uint32_t prev = (head > 0) ? lig_cps[head-1] : 0;
                    uint32_t next = (head+1 < lig_cols) ? lig_cps[head+1] : 0;
                    bool arrow_ctx =
                        IS_ARROW_PAIR(cp, next) ||
                        IS_ARROW_PAIR(prev, cp);
                    if (!arrow_ctx) continue;
                }

                /* Two flavours of OpenType ligature substitution:
                     - multi-cell cluster (sp > 1): classic =>⇒. Always
                       rendered via the shape path.
                     - single-cell cluster (sp == 1) where the shaped
                       glyph_id differs from the cmap-natural glyph_id:
                       split / contextual substitution. Cascadia +
                       CaskaydiaCove use this — `=>` becomes two
                       glyphs `equal_start.seq` + `greater_equal_end.seq`,
                       each occupying its own cell, so the spans are
                       all 1 but every cell's glyph_id has changed.
                   Skip when sp == 1 AND the glyph_id matches the
                   natural one (no substitution → use the existing
                   atlas path). */
                if (sp == 1) {
                    uint32_t natural = shape_natural_glyph_id(
                        (ShapeFont *)r->shape_font, lig_cps[head]);
                    if (natural != 0 && natural == lig_out[g].glyph_id)
                        continue;
                }

                /* Pre-rasterise the glyph so we can confirm it loads
                   before marking the cluster as ours. If it fails, we
                   leave lig_head untouched and the cells render via
                   their normal codepoint paths — better than a blank. */
                ShapeGlyph *sg = shape_render_glyph(
                    (ShapeFont *)r->shape_font, lig_out[g].glyph_id);
                if (!sg || !sg->ok) continue;
                for (int k = 0; k < sp; k++) lig_head[head + k] = head;
                lig_glyph[head] = lig_out[g].glyph_id;
                lig_span[head]  = sp;
                /* HarfBuzz GPOS y_offset / x_offset — needed for
                   split-substitution ligatures like Cascadia's `==`
                   where the two `=` glyphs each carry a vertical
                   nudge to align their bars at the same height.
                   Without this, the bars sit at the font's natural
                   per-glyph baseline and look offset. */
                lig_xoff[head]  = lig_out[g].x_offset_px;
                lig_yoff[head]  = lig_out[g].y_offset_px;
            }
        }

        for (int x = 0; x < cols; x++) {
            Cell c = screen_view_cell(s, x, y);
            if (c.attrs & ATTR_WIDE_CONT) continue;  /* drawn as part of the head cell */
            if (c.attrs & ATTR_HIDDEN) continue;
            /* Blank/space cells skip glyph drawing but still get
               underline / strike / link decorations so an underlined
               "foo bar" doesn't drop the underline under the space.
               Unicode whitespace codepoints (NBSP, narrow-NBSP, en/em
               spaces, ideographic space, zero-width spaces, BOM) are
               treated as blanks too — without this, fonts that lack a
               glyph for e.g. U+00A0 fall through to the "?" placeholder.
               (Showed up in claude-code prompts which used NBSP after
               the `❯` glyph; iTerm2's Cocoa font substitution masked
               it.) */
            bool blank = (c.cp == 0 || c.cp == ' ' ||
                          c.cp == 0x00A0 ||                       /* NBSP */
                          c.cp == 0x1680 ||                       /* Ogham space */
                          (c.cp >= 0x2000 && c.cp <= 0x200B) ||   /* en/em/thin/.../zero-width */
                          c.cp == 0x202F ||                       /* narrow NBSP */
                          c.cp == 0x205F ||                       /* medium math space */
                          c.cp == 0x2060 ||                       /* word joiner */
                          c.cp == 0x3000 ||                       /* ideographic space */
                          c.cp == 0xFEFF);                        /* BOM / ZWNBSP */

            uint32_t fg = (c.attrs & ATTR_REVERSE) ? resolve_bg(s, c) : resolve_fg(s, c);

            /* Invert the glyph under the cursor only when the cursor
               actually covers it (block, blinking-block-on-tick).
               Underline and bar leave the glyph untouched. */
            CursorStyle cs_style = screen_cursor_style(s);
            bool covers_glyph =
                cs_style == CURSOR_STYLE_BLOCK ||
                ((cs_style == CURSOR_STYLE_BLOCK_BLINK || cs_style == CURSOR_STYLE_DEFAULT)
                 && ((long long)(time_sec * 2.0) & 1) == 0);
            bool at_cursor = (show_cursor && focused && covers_glyph &&
                              x == cursor_vx && y == cursor_vy);
            if (at_cursor) fg = screen_default_bg(s);

            float alpha = (c.attrs & ATTR_DIM) ? 0.6f : 1.0f;
            int span = (c.attrs & ATTR_WIDE) ? 2 : 1;

            /* Ligature-cluster handling. lig_head[x] tells us this
               cell is part of a multi-cell shaped cluster.
                 - x is the head: rasterise & draw the ligature glyph
                   spanning the full cluster width, then fall through
                   to glyph_drawn so underlines etc. still apply.
                 - x is a tail member: the head already drew, skip
                   the per-cell glyph but keep decorations. */
            if (lig_head[x] >= 0) {
                if (lig_head[x] == x) {
                    ShapeGlyph *sg = shape_render_glyph(
                        (ShapeFont *)r->shape_font, lig_glyph[x]);
                    if (sg && sg->ok && sg->texture) {
                        Texture2D *tex = (Texture2D *)sg->texture;
                        int sp = lig_span[x];
                        int total_w = sp * cw;
                        int ascent = shape_font_ascent_px(
                            (ShapeFont *)r->shape_font);
                        /* The font's baseline within a cell is at
                           cell_top + glyph_y_offset + ascent. The
                           glyph bitmap's top sits bearing_y above
                           that (bearing_y is negative for ascenders).
                           Horizontally we centre the bitmap inside
                           the cluster's footprint. Drawing at
                           display_w / display_h scales the
                           oversampled atlas back to 1×; raylib's
                           bilinear filter on the texture yields
                           the same crispness as the codepoint atlas. */
                        float dst_x = (float)(x * cw + (total_w - sg->display_w) / 2);
                        float dst_y = (float)(y * ch + glyph_y_offset
                                              + ascent + sg->bearing_y);
                        (void)lig_xoff; (void)lig_yoff;
                        Rectangle src = { 0, 0, (float)sg->width, (float)sg->height };
                        Rectangle dst = { dst_x, dst_y,
                                          (float)sg->display_w,
                                          (float)sg->display_h };
                        DrawTexturePro(*tex, src, dst, (Vector2){0, 0}, 0.0f,
                                       col_from_rgb(fg, alpha));
                    }
                }
                /* Whether head or tail, jump past the per-codepoint
                   draw — decorations follow. */
                goto glyph_drawn;
            }

            /* Skip glyph rendering for blank cells; jump straight to the
               decoration pass below so underlines / strike / hyperlink
               markers still draw. pos isn't needed for the decorations. */
            if (blank) goto glyph_drawn;

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
            /* Anything the main font lacks falls through to: (1) the
               broad backup font baked into the binary (DejaVu Sans
               Mono — wide Unicode coverage, works on every platform),
               then (2) the Core Text "Menlo" rasterizer on macOS,
               and finally (3) a "?" placeholder. */
            if (!main_has_glyph) {
                if (backup_font_has(c.cp)) {
                    pos.x = (float)(x * cw + r->cell_extra_w / 2);
                    pos.y = (float)(y * ch + glyph_y_offset);
                    DrawTextCodepoint(g_backup_font, (int)c.cp, pos,
                                      (float)r->font_size,
                                      col_from_rgb(fg, alpha));
                    goto glyph_drawn;
                }
                DRAW_GLYPH(g_fallback);
                /* Nothing in any font: visible placeholder. */
                pos.x = (float)(x * cw + r->cell_extra_w / 2);
                pos.y = (float)(y * ch + glyph_y_offset);
                DrawTextCodepoint(*f, '?', pos, (float)r->font_size,
                                  col_from_rgb(fg, alpha * 0.6f));
                continue;
            }

            /* Box-drawing: paint our own lines so they reach the cell
               edges regardless of font metrics. Must come before the
               main-font fallback or SF Mono's '─' leaves 1 px gaps. */
            if (c.cp >= 0x2500 && c.cp <= 0x2551 &&
                draw_box_drawing(x * cw, y * ch, cw, ch, c.cp, col_from_rgb(fg, alpha))) {
                goto glyph_drawn;
            }

            /* Centre the glyph horizontally within the cell so added
               letter spacing sits as equal whitespace on both sides,
               rather than leaving a visible gap only on the right. */
            int glyph_x_offset = r->cell_extra_w / 2;
            pos.x = (float)(x * cw + glyph_x_offset);
            pos.y = (float)(y * ch + glyph_y_offset);
            DrawTextCodepoint(*f, (int)c.cp, pos, (float)r->font_size, col_from_rgb(fg, alpha));
            /* Stroke-thickening pass: redraw the glyph half a pixel to
               the right. Compensates for sRGB-space alpha blending
               eating the edges of antialiased strokes, which otherwise
               makes "white" text read as gray. Same trick iTerm2 uses
               when "thin strokes" is off. */
            Vector2 p_thick = { pos.x + 0.5f, pos.y };
            DrawTextCodepoint(*f, (int)c.cp, p_thick, (float)r->font_size, col_from_rgb(fg, alpha));

        glyph_drawn:;
            #undef DRAW_GLYPH
            if (c.attrs & ATTR_BOLD) {
                Vector2 p2 = { pos.x + 1.0f, pos.y };
                DrawTextCodepoint(*f, (int)c.cp, p2, (float)r->font_size, col_from_rgb(fg, alpha));
            }
            if (c.attrs & ATTR_UNDERLINE) {
                /* Underline color: SGR 58 stores an explicit RGB in
                   ul_color; falls back to the cell's fg when 0. */
                uint32_t ulrgb = c.ul_color ? c.ul_color : fg;
                Color ulc = col_from_rgb(ulrgb, alpha);
                int ux  = x * cw;
                int uy  = y * ch + ch - 2;
                int uw  = cw * span;
                switch (UL_STYLE_GET(c.attrs)) {
                case UL_STYLE_DOUBLE:
                    DrawRectangle(ux, uy,     uw, 1, ulc);
                    DrawRectangle(ux, uy - 2, uw, 1, ulc);
                    break;
                case UL_STYLE_CURLY: {
                    /* Two-pixel-tall sine-ish wave — draw 1px squares
                       alternating between the upper and lower row. */
                    for (int dx = 0; dx < uw; dx++) {
                        int yy = uy + (((dx / 2) & 1) ? 1 : 0) - 1;
                        DrawRectangle(ux + dx, yy, 1, 1, ulc);
                    }
                    break;
                }
                case UL_STYLE_DOTTED:
                    for (int dx = 0; dx < uw; dx += 2) {
                        DrawRectangle(ux + dx, uy, 1, 1, ulc);
                    }
                    break;
                case UL_STYLE_DASHED:
                    for (int dx = 0; dx < uw; dx += 4) {
                        int seg = (dx + 2 <= uw) ? 2 : (uw - dx);
                        DrawRectangle(ux + dx, uy, seg, 1, ulc);
                    }
                    break;
                case UL_STYLE_SINGLE:
                default:
                    DrawRectangle(ux, uy, uw, 1, ulc);
                    break;
                }
            }
            if (c.attrs & ATTR_STRIKE) {
                DrawRectangle(x * cw, y * ch + ch / 2, cw * span, 1, col_from_rgb(fg, alpha));
            }
            /* OSC 8 hyperlink: always draw a thin underline under any
               cell with a link_id. When the mouse hovers over a cell
               that shares the same link_id, paint all cells in the
               hovered run thicker + accent so the whole link lights up. */
            if (c.link_id) {
                bool hot = false;
                if (hover_col >= 0 && hover_row >= 0) {
                    Cell hc = screen_view_cell(s, hover_col, hover_row);
                    if (hc.link_id == c.link_id) hot = true;
                }
                Color lc = hot ? (Color){120, 200, 255, 255}
                               : col_from_rgb(fg, alpha);
                int th = hot ? 2 : 1;
                DrawRectangle(x * cw, y * ch + ch - th, cw * span, th, lc);
            }
        }
    }

    /* Image pass — blit sixel / kitty bitmaps on top of text. Images
       occupy the rows they anchor to, so overlapping glyphs (if any)
       end up underneath the picture. Scroll offset shifts them up. */
    {
        int nimg = screen_image_count(s);
        int vo = screen_view_offset(s);
        bool on_alt = screen_on_alt(s);
        for (int i = 0; i < nimg; i++) {
            const ScreenImage *img = screen_image_at(s, i);
            if (!img) continue;
            /* Images are tagged at ingest with the screen they belong
               to; skip the ones that aren't for the current screen so
               tmux / less don't draw over each other's graphics. */
            if (screen_image_on_alt(img) != on_alt) continue;
            ImgEntry *e = img_cache_get_or_upload(img);
            if (!e) continue;
            int row = screen_image_anchor_row(img) + vo;
            int col = screen_image_anchor_col(img);
            int dx = col * cw;
            int dy = row * ch;
            int w = screen_image_px_w(img);
            int h = screen_image_px_h(img);
            Rectangle src = { 0, 0, (float)w, (float)h };
            Rectangle dst = { (float)dx, (float)dy, (float)w, (float)h };
            DrawTexturePro(e->tex, src, dst, (Vector2){0, 0}, 0.0f, WHITE);
        }
    }
    img_cache_prune_frame();

    // OSC 133 gutter: small badge in the pad area to the left of any
    // row whose pexit byte is set (green for exit 0, red otherwise).
    // pexit is exit_code+1, so 0 = "no info"; 1 = success; >1 = fail.
    // Invisible when the user's shell doesn't emit OSC 133.
    {
        int gutter_x = -4;
        for (int y = 0; y < rows; y++) {
            uint8_t enc = 0;
            screen_view_row_pmark(s, y, &enc);
            if (enc == 0) continue;
            Color c = (enc == 1) ? (Color){80, 200, 100, 220}
                                 : (Color){230, 80, 80, 240};
            int badge_h = ch - 4;
            DrawRectangle(gutter_x, y * ch + 2, 3, badge_h, c);
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

    EndMode2D();
}
