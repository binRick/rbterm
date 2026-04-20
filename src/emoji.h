#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Rasterize a codepoint from the named font to an RGBA bitmap.
 * `font_name` is a platform font name (e.g. "Apple Color Emoji", "Menlo").
 * Colour vs. monochrome output is determined by the font: colour bitmap
 * fonts like "Apple Color Emoji" ignore the fill colour; vector fonts
 * are filled white so the caller can tint at draw time.
 * On success, *out_rgba is top-down premultiplied R,G,B,A and must be
 * freed by the caller. Returns false if the font or glyph is missing. */
bool glyph_render(const char *font_name, uint32_t codepoint, int pixel_size,
                  uint8_t **out_rgba, int *out_w, int *out_h);

/* Convenience: emoji rasterization via the standard emoji font. */
static inline bool emoji_render(uint32_t cp, int px,
                                uint8_t **rgba, int *w, int *h) {
    return glyph_render("Apple Color Emoji", cp, px, rgba, w, h);
}
