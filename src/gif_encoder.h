/* Minimal GIF89a encoder — public-domain-equivalent, written from
   spec. Produces an animated GIF from a sequence of RGBA frames.
   Each call to gif_add_frame quantises the input to a fixed
   6x6x6 RGB cube + 40 gray ramp palette (256 entries total) and
   LZW-compresses the resulting indices. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct GifEnc GifEnc;

/* Open `path` for writing and emit the header + global colour table.
   `delay_cs` is the per-frame delay in hundredths of a second
   (e.g. 6 → 60ms → ~16 fps). Returns NULL on file-open failure. */
GifEnc *gif_begin(const char *path, int width, int height, int delay_cs);

/* Append one frame. `rgba` must point to width*height*4 bytes
   in row-major order, top-down. Returns false on write error. */
bool    gif_add_frame(GifEnc *g, const uint8_t *rgba);

/* Write the trailer and close the file. NULL-safe. */
void    gif_end(GifEnc *g);
