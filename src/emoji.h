#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Rasterize an emoji codepoint to an RGBA bitmap.
 * On success, fills *out_rgba (top-down, tightly packed R,G,B,A premultiplied),
 * *out_w, *out_h, and returns true. Caller must free(*out_rgba).
 * Returns false if no renderer is available or the glyph isn't in the font. */
bool emoji_render(uint32_t codepoint, int pixel_size,
                  uint8_t **out_rgba, int *out_w, int *out_h);
