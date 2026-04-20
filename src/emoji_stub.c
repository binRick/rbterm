#include "emoji.h"

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
