#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Rasterize a codepoint from the named font to an RGBA bitmap.
 * `font_name` is a platform font name (e.g. "Apple Color Emoji", "Menlo").
 * Core Text may substitute a different installed font if the named one
 * lacks the glyph; `*out_colored` (optional) is set true when the
 * rasterized bitmap carries baked-in colour (i.e. it's a colour emoji),
 * and false when it's a monochrome alpha mask the caller should tint.
 * On success, *out_rgba is top-down premultiplied R,G,B,A and must be
 * freed by the caller. Returns false if the glyph could not be drawn. */
bool glyph_render(const char *font_name, uint32_t codepoint, int pixel_size,
                  uint8_t **out_rgba, int *out_w, int *out_h,
                  bool *out_colored);

/* Convenience: emoji rasterization via the standard emoji font. */
static inline bool emoji_render(uint32_t cp, int px,
                                uint8_t **rgba, int *w, int *h) {
    return glyph_render("Apple Color Emoji", cp, px, rgba, w, h, NULL);
}
