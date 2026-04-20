#include "emoji.h"

bool emoji_render(uint32_t codepoint, int pixel_size,
                  uint8_t **out_rgba, int *out_w, int *out_h) {
    (void)codepoint; (void)pixel_size;
    if (out_rgba) *out_rgba = 0;
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    return false;
}
