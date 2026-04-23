#pragma once
#include <stddef.h>

/* Decode a Kitty graphics APC payload (the bytes between "ESC _" and
   "ESC \", INCLUDING the leading 'G' but nothing else) into a
   heap-allocated RGBA8 bitmap. Returns NULL on malformed input,
   unsupported format, or out-of-memory.
   On success, *out_w / *out_h receive the pixel dimensions.
   Caller owns the returned buffer and must free() it.

   v1 supports: f=100 (PNG), single-message (m=0 or absent),
                a=T (transmit + display). */
unsigned char *kitty_decode(const unsigned char *p, size_t n,
                            int *out_w, int *out_h);
