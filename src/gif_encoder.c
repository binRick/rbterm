/* GIF89a encoder. Public-domain-equivalent, written from the spec.
 *
 * Layout:
 *   "GIF89a"
 *   Logical Screen Descriptor (7 bytes; flags say global table is present)
 *   Global Colour Table (256 entries × 3 bytes)
 *   NETSCAPE 2.0 Application Extension (16 bytes; sets infinite loop)
 *   For each frame:
 *     Graphic Control Extension (8 bytes; carries the per-frame delay)
 *     Image Descriptor (10 bytes; full-frame at 0,0)
 *     LZW Image Data (sub-block-framed)
 *   Trailer (0x3B)
 *
 * Palette: a 6×6×6 RGB cube indices 0..215 plus a 40-step gray ramp
 * indices 216..255 (so neutral text pops without quantising to the
 * nearest cube tone). Quantisation is nearest-cube + nearest-gray
 * with an L1 distance fallback. Good enough for terminal recordings;
 * the original colours are already a 256-entry palette anyway.
 */
#include "gif_encoder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GIF_PALETTE_BITS  8
#define GIF_PALETTE_SIZE  256

/* LZW dict cap for GIF: 12-bit codes => 4096 entries before clear. */
#define LZW_MAX_CODE      4095
/* Hash table for the LZW dict. Size needs to comfortably exceed the
   max number of entries so collisions stay rare; 5021 is the
   classic compress(1) value. */
#define LZW_HASH_SIZE     5021

struct GifEnc {
    FILE *fp;
    int   width, height;
    int   delay_cs;
    /* Quantised-index buffer for the current frame (width*height). */
    uint8_t *idx_buf;
    /* LZW state, reset per frame. */
    int   min_code_size;
    int   clear_code;
    int   eoi_code;
    int   next_code;
    int   code_size;
    /* Bitstream accumulator + 255-byte sub-block. */
    uint32_t bit_buf;
    int      bit_count;
    uint8_t  sub_buf[255];
    int      sub_len;
    /* LZW dict: hash table of (prefix << 8 | suffix) → code. */
    int32_t *hash_keys;     /* -1 = empty slot */
    int     *hash_vals;
};

/* ---------- Palette ---------- */

/* Cube uses 6 levels per channel: 0, 51, 102, 153, 204, 255. */
static const uint8_t k_cube_levels[6] = { 0, 51, 102, 153, 204, 255 };

/* Find the cube level closest to channel value v. */
static int nearest_cube_level(int v) {
    int best = 0, best_d = 1 << 30;
    for (int i = 0; i < 6; i++) {
        int d = v - k_cube_levels[i];
        if (d < 0) d = -d;
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

/* Map an (r,g,b) triple to a palette index 0..255. The first 216
   entries are the 6×6×6 RGB cube; 216..255 are 40 gray steps. We
   pick the gray ramp when the channel spread is small (text on a
   single-color background almost always lands here). */
static int quantise_pixel(int r, int g, int b) {
    int spread = r;
    int lo = r, hi = r;
    if (g < lo) lo = g; if (g > hi) hi = g;
    if (b < lo) lo = b; if (b > hi) hi = b;
    spread = hi - lo;
    if (spread <= 12) {
        /* Treat as gray. Map luma to the 40-step ramp. */
        int luma = (r * 299 + g * 587 + b * 114) / 1000;
        int gi = (luma * 39 + 127) / 255;  /* 0..39 */
        if (gi < 0) gi = 0;
        if (gi > 39) gi = 39;
        return 216 + gi;
    }
    int ri = nearest_cube_level(r);
    int gi = nearest_cube_level(g);
    int bi = nearest_cube_level(b);
    return ri * 36 + gi * 6 + bi;
}

/* Write the 256×3 global colour table. */
static void write_palette(FILE *fp) {
    /* RGB cube. */
    for (int r = 0; r < 6; r++)
        for (int g = 0; g < 6; g++)
            for (int b = 0; b < 6; b++) {
                fputc(k_cube_levels[r], fp);
                fputc(k_cube_levels[g], fp);
                fputc(k_cube_levels[b], fp);
            }
    /* 40-step gray ramp. */
    for (int i = 0; i < 40; i++) {
        int v = (i * 255 + 19) / 39;
        fputc(v, fp);
        fputc(v, fp);
        fputc(v, fp);
    }
}

/* ---------- LZW bitstream ---------- */

static void lzw_flush_sub_block(GifEnc *g) {
    if (g->sub_len <= 0) return;
    fputc(g->sub_len, g->fp);
    fwrite(g->sub_buf, 1, g->sub_len, g->fp);
    g->sub_len = 0;
}

static void lzw_emit_byte(GifEnc *g, uint8_t b) {
    if (g->sub_len == 255) lzw_flush_sub_block(g);
    g->sub_buf[g->sub_len++] = b;
}

static void lzw_emit_code(GifEnc *g, int code) {
    g->bit_buf |= ((uint32_t)code) << g->bit_count;
    g->bit_count += g->code_size;
    while (g->bit_count >= 8) {
        lzw_emit_byte(g, (uint8_t)(g->bit_buf & 0xff));
        g->bit_buf >>= 8;
        g->bit_count -= 8;
    }
}

static void lzw_flush_remaining_bits(GifEnc *g) {
    if (g->bit_count > 0) {
        lzw_emit_byte(g, (uint8_t)(g->bit_buf & 0xff));
        g->bit_buf = 0;
        g->bit_count = 0;
    }
    /* End the chain of sub-blocks with a zero-length terminator. */
    lzw_flush_sub_block(g);
    fputc(0x00, g->fp);
}

/* ---------- LZW dict ---------- */

static void lzw_dict_clear(GifEnc *g) {
    for (int i = 0; i < LZW_HASH_SIZE; i++) g->hash_keys[i] = -1;
    g->next_code = g->eoi_code + 1;
    g->code_size = g->min_code_size + 1;
}

/* Open-addressed linear-probing hash for (prefix << 8 | suffix). */
static int lzw_dict_lookup(GifEnc *g, int prefix, int suffix) {
    int32_t key = (prefix << 8) | suffix;
    int slot = ((prefix * 313) ^ suffix) & (LZW_HASH_SIZE - 1);
    /* Hash size isn't a power of two — fall back to mod. */
    slot = ((unsigned)((prefix * 313) ^ suffix)) % LZW_HASH_SIZE;
    while (g->hash_keys[slot] != -1) {
        if (g->hash_keys[slot] == key) return g->hash_vals[slot];
        slot = (slot + 1) % LZW_HASH_SIZE;
    }
    return -1;
}

static void lzw_dict_insert(GifEnc *g, int prefix, int suffix, int code) {
    int32_t key = (prefix << 8) | suffix;
    int slot = ((unsigned)((prefix * 313) ^ suffix)) % LZW_HASH_SIZE;
    while (g->hash_keys[slot] != -1) {
        slot = (slot + 1) % LZW_HASH_SIZE;
    }
    g->hash_keys[slot] = key;
    g->hash_vals[slot] = code;
}

/* ---------- Encoder ---------- */

GifEnc *gif_begin(const char *path, int width, int height, int delay_cs) {
    if (width <= 0 || height <= 0) return NULL;
    GifEnc *g = calloc(1, sizeof(*g));
    if (!g) return NULL;
    g->fp = fopen(path, "wb");
    if (!g->fp) { free(g); return NULL; }
    g->width = width;
    g->height = height;
    g->delay_cs = delay_cs;
    g->idx_buf = malloc((size_t)width * height);
    g->hash_keys = malloc(sizeof(int32_t) * LZW_HASH_SIZE);
    g->hash_vals = malloc(sizeof(int)     * LZW_HASH_SIZE);
    if (!g->idx_buf || !g->hash_keys || !g->hash_vals) {
        gif_end(g);
        return NULL;
    }
    /* Header. */
    fwrite("GIF89a", 1, 6, g->fp);
    /* Logical Screen Descriptor. */
    fputc(width  & 0xff, g->fp); fputc((width  >> 8) & 0xff, g->fp);
    fputc(height & 0xff, g->fp); fputc((height >> 8) & 0xff, g->fp);
    /* Packed: gct=1, color resolution=7, sort=0, gct size=7 (256 entries). */
    fputc(0xF7, g->fp);
    fputc(0,    g->fp); /* bg color index */
    fputc(0,    g->fp); /* pixel aspect ratio */
    write_palette(g->fp);
    /* No NETSCAPE 2.0 application extension → the gif plays once
       and stops on the last frame, matching the user's request to
       not loop. (Adding the extension with loop=0 would loop
       forever; loop=1 would play once but is functionally the
       same as omitting the extension entirely.) */
    g->min_code_size = GIF_PALETTE_BITS;
    g->clear_code = 1 << g->min_code_size;
    g->eoi_code   = g->clear_code + 1;
    return g;
}

bool gif_add_frame(GifEnc *g, const uint8_t *rgba) {
    if (!g || !g->fp || !rgba) return false;
    int n = g->width * g->height;
    /* Quantise to palette indices. */
    for (int i = 0; i < n; i++) {
        const uint8_t *p = rgba + (size_t)i * 4;
        g->idx_buf[i] = (uint8_t)quantise_pixel(p[0], p[1], p[2]);
    }
    /* Graphic Control Extension: per-frame delay. */
    fputc(0x21, g->fp); fputc(0xF9, g->fp); fputc(0x04, g->fp);
    fputc(0x04, g->fp); /* dispose=1 (do not dispose), no transparency. */
    fputc(g->delay_cs & 0xff, g->fp);
    fputc((g->delay_cs >> 8) & 0xff, g->fp);
    fputc(0x00, g->fp); /* transparent index */
    fputc(0x00, g->fp); /* block terminator */
    /* Image Descriptor. */
    fputc(0x2C, g->fp);
    fputc(0, g->fp); fputc(0, g->fp);   /* left  */
    fputc(0, g->fp); fputc(0, g->fp);   /* top   */
    fputc(g->width  & 0xff, g->fp); fputc((g->width  >> 8) & 0xff, g->fp);
    fputc(g->height & 0xff, g->fp); fputc((g->height >> 8) & 0xff, g->fp);
    fputc(0, g->fp);                     /* no local color table */
    /* LZW image data. */
    fputc(g->min_code_size, g->fp);
    g->bit_buf = 0;
    g->bit_count = 0;
    g->sub_len = 0;
    lzw_dict_clear(g);
    /* Always start with a clear code. */
    lzw_emit_code(g, g->clear_code);
    int prefix = g->idx_buf[0];
    for (int i = 1; i < n; i++) {
        int suffix = g->idx_buf[i];
        int found = lzw_dict_lookup(g, prefix, suffix);
        if (found != -1) {
            prefix = found;
        } else {
            lzw_emit_code(g, prefix);
            if (g->next_code <= LZW_MAX_CODE) {
                lzw_dict_insert(g, prefix, suffix, g->next_code);
                /* Bump code size when next_code crosses a 2^N boundary. */
                if (g->next_code == (1 << g->code_size) && g->code_size < 12) {
                    g->code_size++;
                }
                g->next_code++;
            } else {
                /* Dict full — clear and restart. */
                lzw_emit_code(g, g->clear_code);
                lzw_dict_clear(g);
            }
            prefix = suffix;
        }
    }
    lzw_emit_code(g, prefix);
    lzw_emit_code(g, g->eoi_code);
    lzw_flush_remaining_bits(g);
    return !ferror(g->fp);
}

void gif_end(GifEnc *g) {
    if (!g) return;
    if (g->fp) {
        fputc(0x3B, g->fp);    /* GIF trailer */
        fclose(g->fp);
    }
    free(g->idx_buf);
    free(g->hash_keys);
    free(g->hash_vals);
    free(g);
}
