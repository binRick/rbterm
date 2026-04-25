/* No-op emoji renderer for non-Apple platforms. Always reports
   "couldn't rasterise" so the caller falls back to its glyph cache
   / `?` placeholder. The mac build links emoji_mac.m instead. */
#include "emoji.h"

/* See emoji.h for the contract. This stub never produces a bitmap
   — Linux + Windows would need a HarfBuzz/FreeType (or DirectWrite)
   port for real colour-emoji support. */
bool glyph_render(const char *font_name, uint32_t codepoint, int pixel_size,
                  uint8_t **out_rgba, int *out_w, int *out_h,
                  bool *out_colored) {
    (void)font_name; (void)codepoint; (void)pixel_size;
    if (out_rgba) *out_rgba = 0;
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    if (out_colored) *out_colored = false;
    return false;
}
