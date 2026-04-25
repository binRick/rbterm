/* Sixel decoder — parses DCS payload into an RGBA8 bitmap.
 *
 * Input layout (bytes between ESC P and ESC \):
 *   P1;P2;P3 q [raster-attrs] [color-defs] [pixel-rows]
 *
 * Pixel rows are made of sixel chars (? .. ~). Each char encodes 6
 * vertical pixels for one column of the current sixel row:
 *   bit 0 (LSB) = topmost pixel of the 6
 *   ...
 *   bit 5       = bottommost pixel of the 6
 *
 * Specials in the pixel-data stream:
 *   #Pc                          — select color register Pc
 *   #Pc;Pu;Px;Py;Pz              — define color Pc (Pu=1 HLS, Pu=2 RGB)
 *   "Pan;Pad;Ph;Pv               — raster attributes (aspect + target dims)
 *   !Pr <sixel-char>             — repeat sixel char Pr times
 *   $                            — carriage return (x=0; stay on same sixel row)
 *   -                            — newline (advance sixel row, x=0)
 *
 * Strategy: two-pass. Pass 1 walks the stream to find max X extent
 * and sixel-row count; Pass 2 allocates the RGBA buffer and rasterises.
 * Simpler than a growable RGBA canvas and handles arbitrary sizes. */

#include "sixel.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define COLOR_REGS 256
#define MAX_W 4096
#define MAX_H 4096

/* xterm / libsixel default palette. 16 entries, RGB 0..255. Sixel
   programs typically ignore this and define their own colors up-front,
   but a few common generators (e.g. old DECTerm examples) rely on it. */
static const uint32_t default_palette[16] = {
    0x000000u, 0x202080u, 0x802020u, 0x208020u,
    0x802080u, 0x208080u, 0x808020u, 0x808080u,
    0x404040u, 0x4060C0u, 0xC04040u, 0x40C040u,
    0xC040C0u, 0x40C0C0u, 0xC0C040u, 0xFFFFFFu,
};

/* HLS → RGB conversion per sixel spec: H in 0..360 (0=blue!), L,S in 0..100.
   Sixel uses the "DEC HLS" convention where hue 0 is blue, 120 red, 240 green. */
static uint32_t hls_to_rgb(int h, int l, int s) {
    if (h < 0) h = 0; if (h >= 360) h %= 360;
    if (l < 0) l = 0; if (l > 100) l = 100;
    if (s < 0) s = 0; if (s > 100) s = 100;
    double H = (double)h + 120.0;  /* rotate so red=0 for standard math */
    if (H >= 360.0) H -= 360.0;
    double L = (double)l / 100.0;
    double S = (double)s / 100.0;
    double C = (1.0 - (L >= 0.5 ? (2.0 * L - 1.0) : -(2.0 * L - 1.0))) * S;
    double Hp = H / 60.0;
    double X = C * (1.0 - (Hp - (int)Hp > 0.5 ? 1.0 - (Hp - (int)Hp) : (Hp - (int)Hp)));
    /* Simplified piecewise — doesn't need to be exact, sixel art is
       garish anyway. */
    double r=0,g=0,b=0;
    if      (Hp < 1.0) { r=C; g=X; b=0; }
    else if (Hp < 2.0) { r=X; g=C; b=0; }
    else if (Hp < 3.0) { r=0; g=C; b=X; }
    else if (Hp < 4.0) { r=0; g=X; b=C; }
    else if (Hp < 5.0) { r=X; g=0; b=C; }
    else               { r=C; g=0; b=X; }
    double m = L - C/2.0;
    int R = (int)((r+m) * 255.0 + 0.5);
    int G = (int)((g+m) * 255.0 + 0.5);
    int B = (int)((b+m) * 255.0 + 0.5);
    if (R < 0) R = 0; if (R > 255) R = 255;
    if (G < 0) G = 0; if (G > 255) G = 255;
    if (B < 0) B = 0; if (B > 255) B = 255;
    return ((uint32_t)R << 16) | ((uint32_t)G << 8) | (uint32_t)B;
}

/* Parse an unsigned integer starting at *pp. Advance *pp past it.
   Returns 0 if no digits present. */
static int parse_uint(const unsigned char **pp, const unsigned char *end) {
    const unsigned char *p = *pp;
    int v = 0;
    bool any = false;
    while (p < end && *p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        p++;
        any = true;
    }
    *pp = p;
    (void)any;
    return v;
}

/* Parse `;N;N;N...` semicolon-delimited numeric list. Returns count
   read, stopping at max_n. Advances *pp past the list (up to but not
   including the first non-digit non-';' byte). Starts BEFORE the first
   expected digit — caller must have consumed the leading '#' or '"'. */
static int parse_num_list(const unsigned char **pp, const unsigned char *end,
                          int *nums, int max_n) {
    int count = 0;
    while (count < max_n && *pp < end) {
        /* Read optional number. */
        int v = parse_uint(pp, end);
        nums[count++] = v;
        if (*pp < end && **pp == ';') (*pp)++;
        else break;
    }
    return count;
}

/* Skip past the "Pan;Pad;Ph;Pv raster attributes (if present). They
   start with '"' and continue through digits and semicolons. */
static void skip_raster(const unsigned char **pp, const unsigned char *end) {
    if (*pp < end && **pp == '"') {
        (*pp)++;
        int dummy[4];
        parse_num_list(pp, end, dummy, 4);
    }
}

/* Find the 'q' that terminates the DCS header (P1;P2;P3q). Everything
   before 'q' is the header; everything after is the pixel-data stream. */
static const unsigned char *skip_header(const unsigned char *p,
                                        const unsigned char *end) {
    while (p < end && *p != 'q') p++;
    if (p < end) p++;
    return p;
}

/* First pass: walk the pixel stream and record the maximum X extent
   and sixel-row count. */
static void scan_extents(const unsigned char *p, const unsigned char *end,
                         int *out_w, int *out_rows) {
    int x = 0, max_x = 0;
    int rows = 0;
    int rep = 1;
    while (p < end) {
        unsigned char c = *p;
        if (c >= '?' && c <= '~') {
            x += rep;
            if (x > max_x) max_x = x;
            rep = 1;
            p++;
        } else if (c == '!') {
            p++;
            int r = parse_uint(&p, end);
            rep = r > 0 ? r : 1;
        } else if (c == '$') {
            x = 0; rep = 1; p++;
        } else if (c == '-') {
            if (x > max_x) max_x = x;
            rows++; x = 0; rep = 1; p++;
        } else if (c == '#') {
            p++;
            int dummy[5];
            parse_num_list(&p, end, dummy, 5);
        } else if (c == '"') {
            skip_raster(&p, end);
        } else {
            /* Ignore unknown / whitespace. */
            p++;
        }
    }
    /* Account for the final (possibly unterminated) sixel row. */
    if (x > 0 || rows == 0) rows++;
    if (out_w)    *out_w    = max_x;
    if (out_rows) *out_rows = rows;
}

/* Decode a complete sixel DCS payload into an RGBA8 bitmap. Two
   passes: scan_extents walks the stream to find the final width
   and sixel-row count, then a second pass allocates the buffer
   and rasterises it. Returns a malloc'd buffer (caller frees) and
   writes pixel dims into out_w/out_h. NULL on alloc failure or
   empty/garbage input. */
unsigned char *sixel_decode(const unsigned char *p, size_t n,
                            int *out_w, int *out_h) {
    if (!p || n == 0) return NULL;
    const unsigned char *end = p + n;

    /* Skip header: "P1;P2;P3 q" (anything before 'q'). */
    const unsigned char *body = skip_header(p, end);

    /* Optional raster attributes `"Pan;Pad;Ph;Pv` up-front. We honour
       Ph/Pv as the target pixel dimensions when both are present and
       positive — some encoders don't emit the sixels for the full
       advertised area, but declare it here. */
    int rast_h = 0, rast_v = 0;
    if (body < end && *body == '"') {
        const unsigned char *q = body + 1;
        int nums[4] = {0,0,0,0};
        parse_num_list(&q, end, nums, 4);
        rast_h = nums[2];
        rast_v = nums[3];
        body = q;
    }

    /* Pass 1: measure. */
    int w_scan = 0, rows_scan = 0;
    scan_extents(body, end, &w_scan, &rows_scan);
    int w = w_scan;
    int h = rows_scan * 6;
    if (rast_h > w) w = rast_h;
    if (rast_v > h) h = rast_v;
    if (w <= 0 || h <= 0) return NULL;
    if (w > MAX_W) w = MAX_W;
    if (h > MAX_H) h = MAX_H;

    /* Allocate opaque-black RGBA. Undrawn pixels stay at alpha=0
       so the terminal background shows through in transparent mode. */
    unsigned char *rgba = calloc((size_t)w * h, 4);
    if (!rgba) return NULL;

    /* Seed color registers with the default palette, rest black. */
    uint32_t reg[COLOR_REGS];
    for (int i = 0; i < COLOR_REGS; i++) {
        reg[i] = (i < 16) ? default_palette[i] : 0x000000u;
    }

    /* Pass 2: rasterise. */
    int x = 0;
    int row = 0;           /* sixel row index; each row is 6 px tall */
    int rep = 1;
    int cur = 0;           /* current color register */

    #define PUT_SIXEL(sx, col_rgb) do {                                       \
        if ((sx) >= '?' && (sx) <= '~' && (col_rgb) != 0xFFFFFFFFu) {         \
            int bits = (sx) - '?';                                            \
            int base_x = x;                                                   \
            for (int rpt = 0; rpt < rep; rpt++) {                             \
                int dstx = base_x + rpt;                                      \
                if (dstx >= w) break;                                         \
                for (int b = 0; b < 6; b++) {                                 \
                    if (!(bits & (1 << b))) continue;                         \
                    int dsty = row * 6 + b;                                   \
                    if (dsty >= h) break;                                     \
                    unsigned char *px = rgba + ((size_t)dsty * w + dstx) * 4; \
                    px[0] = (unsigned char)(((col_rgb) >> 16) & 0xff);        \
                    px[1] = (unsigned char)(((col_rgb) >>  8) & 0xff);        \
                    px[2] = (unsigned char)((col_rgb) & 0xff);                \
                    px[3] = 255;                                              \
                }                                                             \
            }                                                                 \
            x += rep;                                                         \
        }                                                                     \
    } while (0)

    const unsigned char *q = body;
    while (q < end) {
        unsigned char c = *q;
        if (c >= '?' && c <= '~') {
            PUT_SIXEL(c, reg[cur]);
            rep = 1;
            q++;
        } else if (c == '!') {
            q++;
            int r = parse_uint(&q, end);
            rep = r > 0 ? r : 1;
        } else if (c == '$') {
            x = 0; rep = 1; q++;
        } else if (c == '-') {
            row++; x = 0; rep = 1; q++;
        } else if (c == '#') {
            q++;
            int nums[5] = {0,0,0,0,0};
            int cnt = parse_num_list(&q, end, nums, 5);
            int idx = nums[0];
            if (idx < 0) idx = 0;
            if (idx >= COLOR_REGS) idx = COLOR_REGS - 1;
            cur = idx;
            if (cnt >= 5) {
                /* Definition form. Pu=1 HLS, Pu=2 RGB. */
                int sys = nums[1];
                int a = nums[2], b = nums[3], cc = nums[4];
                if (sys == 1) {
                    reg[idx] = hls_to_rgb(a, b, cc);
                } else if (sys == 2) {
                    int r = a * 255 / 100;
                    int g = b * 255 / 100;
                    int bl = cc * 255 / 100;
                    if (r < 0) r = 0; if (r > 255) r = 255;
                    if (g < 0) g = 0; if (g > 255) g = 255;
                    if (bl < 0) bl = 0; if (bl > 255) bl = 255;
                    reg[idx] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
                }
            }
        } else if (c == '"') {
            skip_raster(&q, end);
        } else {
            q++;
        }
    }

    #undef PUT_SIXEL

    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return rgba;
}
