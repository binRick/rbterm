/* Animated WebP encoder. Wraps libwebp + libwebpmux's
   WebPAnimEncoder API with a streaming front-end that mirrors
   gif_encoder.h: open → add_frame*N → end. Used by the recording
   save path so WebP works without depending on a libwebp-enabled
   ffmpeg build. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct WebpEnc WebpEnc;

/* Open `path` for writing and prepare an encoder for `width`×`height`
   RGBA frames at `delay_ms` between frames (matches gif_begin's
   `delay_cs` semantics, but in milliseconds). Returns NULL on
   failure. */
WebpEnc *webp_begin(const char *path, int width, int height, int delay_ms);

/* Append one RGBA frame (top-row first, no padding). Returns false
   on encoder failure. */
bool     webp_add_frame(WebpEnc *e, const uint8_t *rgba);

/* Finalise the animation, write the file, and free `e`. NULL-safe. */
bool     webp_end(WebpEnc *e);
