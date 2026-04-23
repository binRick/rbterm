#pragma once
#include <stddef.h>

/* Decode a DCS sixel payload (the bytes between "ESC P" and "ESC \",
   WITHOUT the ESC P / ESC \ framing) into a heap-allocated RGBA8
   bitmap. Caller owns the returned buffer and must free() it.
   Returns NULL on malformed input or out-of-memory.
   On success, *out_w / *out_h receive the pixel dimensions. */
unsigned char *sixel_decode(const unsigned char *p, size_t n,
                            int *out_w, int *out_h);
