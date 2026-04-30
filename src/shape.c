#include "shape.h"
#include "raylib.h"

#include <stdlib.h>
#include <string.h>

#ifdef RBTERM_HAVE_HARFBUZZ

#include <hb.h>

/* Vendored stb_truetype for glyph-id rasterisation. raylib bundles its
   own copy but with file-static linkage; defining the implementation
   in our own translation unit gives us a fresh, isolated set of symbols. */
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "../third_party/stb/stb_truetype.h"

#define GLYPH_CACHE_INIT 64

/* Glyph rasterisation oversample factor. Match raylib's atlas (which
   builds at font_size * 3 then downsamples) so ligature glyphs blend
   into surrounding text at the same crispness. */
#define SHAPE_OVERSAMPLE 3

struct ShapeFont {
    /* HarfBuzz: shape_row asks hb_shape() for the OpenType-driven
       cluster output. */
    hb_blob_t *blob;
    hb_face_t *face;
    hb_font_t *font;
    hb_buffer_t *buffer;     /* reused per call to avoid alloc churn */

    /* stb_truetype: rasterises ligature glyph bitmaps by glyph_id since
       raylib's atlas is keyed by codepoint and can't reach
       codepointless ligature glyphs. */
    stbtt_fontinfo info;
    float scale;             /* 1× px-per-em — used for bearings  */
    float scale_hi;          /* SHAPE_OVERSAMPLE× — used for bitmaps */
    int   pixel_size;
    int   ascent_px;         /* baseline offset within a cell (1×) */

    /* Per-glyph_id cache — small open-hash, linear probe. Glyph IDs
       are 16-bit OpenType so collisions are exceedingly rare even at
       moderate cache sizes. */
    ShapeGlyph *glyphs;
    int         glyph_cap;
    int         glyph_count;
};

bool shape_available(void) { return true; }

ShapeFont *shape_font_open(const unsigned char *font_data,
                           int data_size,
                           int pixel_size) {
    if (!font_data || data_size <= 0 || pixel_size <= 0) return NULL;
    ShapeFont *sf = calloc(1, sizeof(*sf));
    if (!sf) return NULL;

    /* HarfBuzz expects to own a hb_blob_t for the font bytes. The
       font_data pointer is owned by the caller (typically a font
       embedded in the binary's read-only section), so we wrap it
       with HB_MEMORY_MODE_READONLY and a no-op destroy. */
    sf->blob = hb_blob_create((const char *)font_data, (unsigned)data_size,
                              HB_MEMORY_MODE_READONLY, NULL, NULL);
    if (!sf->blob) goto fail;
    sf->face = hb_face_create(sf->blob, 0);
    if (!sf->face) goto fail;
    sf->font = hb_font_create(sf->face);
    if (!sf->font) goto fail;
    /* Set the pixel scale on the hb_font — needed so glyph offsets
       come back in pixel units rather than design-space units. */
    hb_font_set_scale(sf->font, pixel_size * 64, pixel_size * 64);
    sf->buffer = hb_buffer_create();
    if (!sf->buffer) goto fail;
    hb_buffer_set_direction(sf->buffer, HB_DIRECTION_LTR);
    hb_buffer_set_script(sf->buffer, HB_SCRIPT_LATIN);
    hb_buffer_set_language(sf->buffer, hb_language_from_string("en", -1));

    /* stb_truetype init for the same blob — different parser but
       reads the same bytes. */
    if (!stbtt_InitFont(&sf->info, font_data,
                        stbtt_GetFontOffsetForIndex(font_data, 0))) {
        goto fail;
    }
    sf->pixel_size = pixel_size;
    sf->scale    = stbtt_ScaleForPixelHeight(&sf->info, (float)pixel_size);
    sf->scale_hi = stbtt_ScaleForPixelHeight(&sf->info,
                                             (float)pixel_size * SHAPE_OVERSAMPLE);
    int ascent, descent, line_gap;
    stbtt_GetFontVMetrics(&sf->info, &ascent, &descent, &line_gap);
    sf->ascent_px = (int)((float)ascent * sf->scale);

    sf->glyph_cap = GLYPH_CACHE_INIT;
    sf->glyphs = calloc(sf->glyph_cap, sizeof(ShapeGlyph));
    if (!sf->glyphs) goto fail;
    return sf;

fail:
    shape_font_free(sf);
    return NULL;
}

void shape_font_free(ShapeFont *sf) {
    if (!sf) return;
    if (sf->glyphs) {
        for (int i = 0; i < sf->glyph_count; i++) {
            if (sf->glyphs[i].texture) {
                UnloadTexture(*(Texture2D *)sf->glyphs[i].texture);
                free(sf->glyphs[i].texture);
            }
        }
        free(sf->glyphs);
    }
    if (sf->buffer) hb_buffer_destroy(sf->buffer);
    if (sf->font)   hb_font_destroy(sf->font);
    if (sf->face)   hb_face_destroy(sf->face);
    if (sf->blob)   hb_blob_destroy(sf->blob);
    free(sf);
}

int shape_row(ShapeFont *sf,
              const uint32_t *codepoints,
              int n_codepoints,
              ShapedGlyph *out,
              int out_cap) {
    if (!sf || !codepoints || n_codepoints <= 0 || !out || out_cap <= 0)
        return 0;

    hb_buffer_reset(sf->buffer);
    hb_buffer_set_direction(sf->buffer, HB_DIRECTION_LTR);
    hb_buffer_set_script(sf->buffer, HB_SCRIPT_LATIN);
    hb_buffer_set_language(sf->buffer, hb_language_from_string("en", -1));
    /* Cluster index = the input cell column. We use this on the
       output side to map glyphs back to cells and detect ligatures
       (consecutive output glyphs with the same cluster index → single
       glyph spanning multiple cells). */
    for (int i = 0; i < n_codepoints; i++) {
        hb_buffer_add(sf->buffer, codepoints[i] ? codepoints[i] : ' ', i);
    }

    /* Force-enable the ligature features programmer fonts rely on.
       `calt` is the big one — Cascadia / CaskaydiaCove / Fira Code put
       most of their substitution lookups behind it, and HarfBuzz's
       default Latin shaping enables it but only for some font types.
       Enabling them explicitly avoids the "ligatures are silent"
       footgun where the font ships them but they don't fire.

       `dlig` (discretionary ligatures) is INTENTIONALLY off — it's
       where fonts hide `..` → ‥ / `...` style substitutions that
       look wrong in shell paths (`cd ../` rendering as three dots,
       fixed by leaving dlig at its disabled default). Standard
       programmer-font ligatures live in liga/clig/calt and aren't
       affected. */
    static const hb_feature_t k_features[] = {
        { HB_TAG('l','i','g','a'), 1, 0, (unsigned int)-1 },
        { HB_TAG('c','l','i','g'), 1, 0, (unsigned int)-1 },
        { HB_TAG('c','a','l','t'), 1, 0, (unsigned int)-1 },
        { HB_TAG('d','l','i','g'), 0, 0, (unsigned int)-1 },
    };
    hb_shape(sf->font, sf->buffer, k_features,
             sizeof(k_features) / sizeof(k_features[0]));

    unsigned int glyph_count = 0;
    hb_glyph_info_t     *gi = hb_buffer_get_glyph_infos(sf->buffer, &glyph_count);
    hb_glyph_position_t *gp = hb_buffer_get_glyph_positions(sf->buffer, &glyph_count);
    if (!gi || !gp) return 0;

    int written = 0;
    for (unsigned int g = 0; g < glyph_count && written < out_cap; g++) {
        unsigned int cluster = gi[g].cluster;
        /* Determine cell span: distance to next cluster OR the
           end of input. HarfBuzz emits clusters in input order, so
           the next cluster value tells us how many input cells this
           output glyph consumed. */
        unsigned int next_cluster = (g + 1 < glyph_count)
                                       ? gi[g + 1].cluster
                                       : (unsigned int)n_codepoints;
        if (next_cluster < cluster) next_cluster = cluster + 1;
        int span = (int)(next_cluster - cluster);
        if (span < 1) span = 1;

        out[written].glyph_id    = gi[g].codepoint;   /* HB calls the glyph_id 'codepoint' */
        out[written].cell_col    = (int)cluster;
        out[written].cell_span   = span;
        out[written].x_offset_px = gp[g].x_offset / 64;
        out[written].y_offset_px = -gp[g].y_offset / 64;  /* HB y-up → screen y-down */
        out[written].is_ligature = (span > 1);
        written++;
    }
    return written;
}

/* Linear-probe lookup. Capacity grows when load factor >0.7. */
static ShapeGlyph *glyph_cache_find(ShapeFont *sf, uint32_t glyph_id) {
    for (int i = 0; i < sf->glyph_count; i++) {
        if (sf->glyphs[i].glyph_id == glyph_id) return &sf->glyphs[i];
    }
    return NULL;
}

static ShapeGlyph *glyph_cache_alloc(ShapeFont *sf, uint32_t glyph_id) {
    if (sf->glyph_count + 1 >= sf->glyph_cap * 7 / 10) {
        int new_cap = sf->glyph_cap * 2;
        ShapeGlyph *bigger = realloc(sf->glyphs,
                                     (size_t)new_cap * sizeof(ShapeGlyph));
        if (!bigger) return NULL;
        memset(bigger + sf->glyph_cap, 0,
               (size_t)(new_cap - sf->glyph_cap) * sizeof(ShapeGlyph));
        sf->glyphs   = bigger;
        sf->glyph_cap = new_cap;
    }
    ShapeGlyph *e = &sf->glyphs[sf->glyph_count++];
    memset(e, 0, sizeof(*e));
    e->glyph_id = glyph_id;
    return e;
}

uint32_t shape_natural_glyph_id(ShapeFont *sf, uint32_t codepoint) {
    if (!sf) return 0;
    int gid = stbtt_FindGlyphIndex(&sf->info, (int)codepoint);
    return gid > 0 ? (uint32_t)gid : 0;
}

ShapeGlyph *shape_render_glyph(ShapeFont *sf, uint32_t glyph_id) {
    if (!sf) return NULL;
    ShapeGlyph *e = glyph_cache_find(sf, glyph_id);
    if (!e) e = glyph_cache_alloc(sf, glyph_id);
    if (!e) return NULL;
    if (e->ok || e->failed) return e;

    /* Rasterise via stb_truetype at SHAPE_OVERSAMPLE× the display
       size; the renderer draws at 1× and bilinear-filters down for
       the same crispness raylib gets from its 3× atlas. */
    int hx0, hy0, hx1, hy1;
    stbtt_GetGlyphBitmapBox(&sf->info, (int)glyph_id,
                            sf->scale_hi, sf->scale_hi,
                            &hx0, &hy0, &hx1, &hy1);
    int hw = hx1 - hx0, hh = hy1 - hy0;
    if (hw <= 0 || hh <= 0) {
        /* Empty glyph (e.g. .notdef). Mark failed so we skip on retry. */
        e->failed = true;
        return e;
    }
    unsigned char *alpha = malloc((size_t)hw * hh);
    if (!alpha) { e->failed = true; return e; }
    stbtt_MakeGlyphBitmap(&sf->info, alpha, hw, hh, hw,
                          sf->scale_hi, sf->scale_hi, (int)glyph_id);

    /* Convert single-channel alpha to RGBA so raylib's textured-quad
       path can blend it. R = G = B = 255 (white), A = the bitmap. */
    unsigned char *rgba = malloc((size_t)hw * hh * 4);
    if (!rgba) { free(alpha); e->failed = true; return e; }
    for (int p = 0; p < hw * hh; p++) {
        rgba[p * 4 + 0] = 255;
        rgba[p * 4 + 1] = 255;
        rgba[p * 4 + 2] = 255;
        rgba[p * 4 + 3] = alpha[p];
    }
    free(alpha);

    Image img = { rgba, hw, hh, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
    Texture2D tex = LoadTextureFromImage(img);
    UnloadImage(img);   /* frees rgba */
    if (tex.id == 0) { e->failed = true; return e; }
    SetTextureFilter(tex, TEXTURE_FILTER_BILINEAR);

    e->texture = malloc(sizeof(Texture2D));
    if (!e->texture) {
        UnloadTexture(tex);
        e->failed = true;
        return e;
    }
    *(Texture2D *)e->texture = tex;
    /* width/height = oversampled bitmap dims. display_w/h = the same
       in cell coordinates (i.e. divided by oversample) so the
       renderer's dst rect lands at 1× while sampling from the
       oversampled atlas. */
    e->width      = hw;
    e->height     = hh;
    e->display_w  = (hw + SHAPE_OVERSAMPLE - 1) / SHAPE_OVERSAMPLE;
    e->display_h  = (hh + SHAPE_OVERSAMPLE - 1) / SHAPE_OVERSAMPLE;
    e->bearing_x  = hx0 / SHAPE_OVERSAMPLE;
    e->bearing_y  = hy0 / SHAPE_OVERSAMPLE;
    e->ok         = true;
    return e;
}

int shape_font_ascent_px(ShapeFont *sf) {
    return sf ? sf->ascent_px : 0;
}

#else  /* !RBTERM_HAVE_HARFBUZZ — stub path */

bool shape_available(void) { return false; }

ShapeFont *shape_font_open(const unsigned char *font_data,
                           int data_size, int pixel_size) {
    (void)font_data; (void)data_size; (void)pixel_size;
    return NULL;
}
void shape_font_free(ShapeFont *sf) { (void)sf; }
int  shape_row(ShapeFont *sf, const uint32_t *codepoints, int n_codepoints,
               ShapedGlyph *out, int out_cap) {
    (void)sf; (void)codepoints; (void)n_codepoints; (void)out; (void)out_cap;
    return 0;
}
ShapeGlyph *shape_render_glyph(ShapeFont *sf, uint32_t glyph_id) {
    (void)sf; (void)glyph_id;
    return NULL;
}
uint32_t shape_natural_glyph_id(ShapeFont *sf, uint32_t codepoint) {
    (void)sf; (void)codepoint;
    return 0;
}
int shape_font_ascent_px(ShapeFont *sf) { (void)sf; return 0; }

#endif  /* RBTERM_HAVE_HARFBUZZ */
