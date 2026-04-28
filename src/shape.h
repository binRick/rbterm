/* OpenType ligature shaping for the renderer.

   When the user enables Settings → Font → Ligatures, the renderer
   shapes each visible row through HarfBuzz before drawing. The result
   is an array of *shaped glyphs*: each glyph carries its glyph_id
   (no longer a codepoint), the cell column it visually starts at, and
   how many cells of input it consumed. Ligature glyphs (e.g. Fira
   Code's `=>` glyph) consume 2+ cells but draw as one glyph spanning
   the same horizontal extent as those cells.

   When HarfBuzz isn't available at build time (RBTERM_HAVE_HARFBUZZ
   not defined), every function in this file becomes a no-op that
   reports "no shaping available". The renderer's existing per-codepoint
   draw path runs unchanged, so users without the dev headers still get
   a functional terminal.

   Glyph rasterisation for ligature glyph_ids uses a vendored copy of
   stb_truetype (third_party/stb/stb_truetype.h) since raylib bundles
   its own copy with file-static linkage. The two copies don't conflict
   because each is in its own translation unit. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One shaped glyph in the output of shape_row. Caller iterates the
   array and asks shape_render_glyph() to get the rasterised texture. */
typedef struct {
    uint32_t glyph_id;       /* OpenType glyph index — opaque, font-local */
    int      cell_col;       /* leftmost cell this glyph covers */
    int      cell_span;      /* how many cells the glyph horizontally spans */
    int      x_offset_px;    /* sub-cell horizontal offset (from HarfBuzz) */
    int      y_offset_px;    /* sub-cell vertical offset */
    bool     is_ligature;    /* true when cell_span > 1 (multi-cell cluster) */
} ShapedGlyph;

/* Opaque per-font shaping handle. One per loaded font; created lazily
   on first shape_row() call against that font, freed by
   shape_font_free(). */
typedef struct ShapeFont ShapeFont;

/* Per-glyph rasterised bitmap cache entry. Glyph_id keyed; reused for
   the lifetime of the font. */
typedef struct {
    uint32_t glyph_id;
    void *texture;           /* raylib Texture2D* (opaque to keep this header raylib-free) */
    int   width;             /* pixel dims of the rasterised bitmap */
    int   height;
    int   bearing_x;         /* x advance from cluster origin to bitmap left */
    int   bearing_y;         /* y advance from cluster origin to bitmap top  */
    bool  ok;
    bool  failed;            /* sticky — don't keep retrying on miss */
} ShapeGlyph;

/* Compile-time presence — read from main.c so the Settings UI knows
   whether to expose the Ligatures toggle. */
bool shape_available(void);

/* Allocate / look up the per-font shaping handle for `font_data` (a
   raw .ttf / .otf blob with `data_size` bytes) at the given pixel
   size. Returns NULL on failure (e.g. malformed font, HarfBuzz not
   available). The caller owns the returned pointer and must call
   shape_font_free when the font is unloaded. */
ShapeFont *shape_font_open(const unsigned char *font_data,
                           int data_size,
                           int pixel_size);
void       shape_font_free(ShapeFont *sf);

/* Shape a row of codepoints into glyphs. Writes up to `out_cap`
   ShapedGlyph entries to `out`; returns how many were written.
   Returns 0 if shaping isn't available or the input is empty.

   The caller owns the input cells array. Wide-char continuation
   cells (the second half of a CJK character) should be passed as
   codepoint=0 so HarfBuzz skips them — or just omitted entirely. */
int shape_row(ShapeFont *sf,
              const uint32_t *codepoints,
              int n_codepoints,
              ShapedGlyph *out,
              int out_cap);

/* Rasterise a glyph_id into a Texture2D and cache it. Repeated calls
   for the same glyph_id return the same cached entry. Returns NULL
   on failure. The returned pointer is owned by the ShapeFont and
   freed when shape_font_free is called. */
ShapeGlyph *shape_render_glyph(ShapeFont *sf, uint32_t glyph_id);

/* Return the OpenType glyph_id this codepoint maps to with no
   shaping applied (cmap lookup only). The renderer compares this
   against shape_row's output to detect single-cell substitutions:
   when shape_row returns glyph_id != natural, the font has done a
   contextual / stylistic / split-ligature substitution that should
   be rendered instead of the per-codepoint atlas glyph. Returns 0
   if no glyph maps to this codepoint or shape isn't available. */
uint32_t shape_natural_glyph_id(ShapeFont *sf, uint32_t codepoint);
