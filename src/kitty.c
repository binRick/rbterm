/* Kitty graphics protocol decoder.
 *
 * APC payload layout (bytes between ESC _ and ESC \):
 *
 *   G <keys>;<payload>
 *
 * Where <keys> is comma-separated key=value pairs (e.g.
 * "a=T,f=100,m=0") and <payload> is base64-encoded image data.
 *
 * v1 supports only:
 *   - f=100 (PNG) — decoded via raylib's LoadImageFromMemory
 *   - a=T (transmit + display). a=t (transmit only) is treated as
 *     display-anyway since most generators use a=T.
 *
 * Chunking (m=1) is handled in screen.c: it buffers payloads across
 * APC messages and calls this decoder once at finalisation with the
 * concatenated payload. So from this module's point of view the
 * input is always a single "G<keys>;<full-payload>" blob.
 *
 * Everything else (raw RGBA f=24/32, delete commands a=d,
 * animation a=a, response protocol q=) is intentionally ignored for
 * now — the protocol is sprawling and we only need "show me the
 * picture" to claim support. */

#include "kitty.h"
#include "raylib.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Base64 alphabet reverse-lookup table. 0xFF = invalid. */
static const unsigned char b64_lut[256] = {
    ['A']=0,['B']=1,['C']=2,['D']=3,['E']=4,['F']=5,['G']=6,['H']=7,
    ['I']=8,['J']=9,['K']=10,['L']=11,['M']=12,['N']=13,['O']=14,['P']=15,
    ['Q']=16,['R']=17,['S']=18,['T']=19,['U']=20,['V']=21,['W']=22,['X']=23,
    ['Y']=24,['Z']=25,
    ['a']=26,['b']=27,['c']=28,['d']=29,['e']=30,['f']=31,['g']=32,['h']=33,
    ['i']=34,['j']=35,['k']=36,['l']=37,['m']=38,['n']=39,['o']=40,['p']=41,
    ['q']=42,['r']=43,['s']=44,['t']=45,['u']=46,['v']=47,['w']=48,['x']=49,
    ['y']=50,['z']=51,
    ['0']=52,['1']=53,['2']=54,['3']=55,['4']=56,['5']=57,['6']=58,['7']=59,
    ['8']=60,['9']=61,
    ['+']=62,['/']=63,
    /* Kitty also permits URL-safe base64. */
    ['-']=62,['_']=63,
};

/* Decode a base64 string (characters only — '=' padding and whitespace
   are skipped). Returns a malloc'd byte buffer and writes its length
   into *out_len. NULL on out-of-memory. */
static unsigned char *b64_decode(const char *src, size_t src_len, size_t *out_len) {
    /* Worst-case output size: ceil(src_len/4)*3. */
    size_t cap = (src_len / 4 + 1) * 3 + 4;
    unsigned char *out = malloc(cap);
    if (!out) return NULL;
    size_t n = 0;
    uint32_t buf = 0;
    int bits = 0;
    for (size_t i = 0; i < src_len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '=' || c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        unsigned char v = b64_lut[c];
        /* Distinguishing "valid decodes to 0" from "invalid" without a
           separate presence table: only 'A', '-', and '_' decode to 0
           /62/63, so check explicitly. */
        if (v == 0 && c != 'A') continue;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[n++] = (unsigned char)((buf >> bits) & 0xff);
        }
    }
    if (out_len) *out_len = n;
    return out;
}

/* Find `key=value` in the keys section. Returns true if found; copies
   up to cap-1 bytes of value into dst (NUL-terminated).
   `keys` is the comma-delimited list with length `klen`. */
static bool keys_get(const char *keys, size_t klen,
                     const char *key, char *dst, size_t cap) {
    size_t key_n = strlen(key);
    size_t i = 0;
    while (i < klen) {
        /* Match key= at current position. */
        if (i + key_n + 1 <= klen
            && memcmp(keys + i, key, key_n) == 0
            && keys[i + key_n] == '=') {
            size_t j = i + key_n + 1;
            size_t k = 0;
            while (j < klen && keys[j] != ',' && k + 1 < cap) {
                dst[k++] = keys[j++];
            }
            dst[k] = 0;
            return true;
        }
        /* Advance past current pair. */
        while (i < klen && keys[i] != ',') i++;
        if (i < klen) i++;
    }
    return false;
}

unsigned char *kitty_decode(const unsigned char *p, size_t n,
                            int *out_w, int *out_h) {
    if (!p || n < 2 || p[0] != 'G') return NULL;
    /* Keys section runs from after 'G' up to the first ';'. */
    size_t i = 1;
    while (i < n && p[i] != ';') i++;
    const char *keys = (const char *)(p + 1);
    size_t klen = i - 1;
    const unsigned char *payload = (i < n) ? p + i + 1 : p + n;
    size_t plen = (i < n) ? n - i - 1 : 0;

    /* Only PNG for v1. */
    char fbuf[8] = {0};
    if (keys_get(keys, klen, "f", fbuf, sizeof(fbuf))) {
        if (strcmp(fbuf, "100") != 0) return NULL;
    } else {
        /* Default per spec is RGBA (f=32); we don't support that. */
        return NULL;
    }

    size_t binlen = 0;
    unsigned char *bin = b64_decode((const char *)payload, plen, &binlen);
    if (!bin || binlen < 8) { free(bin); return NULL; }

    Image img = LoadImageFromMemory(".png", bin, (int)binlen);
    free(bin);
    if (img.data == NULL || img.width <= 0 || img.height <= 0) {
        if (img.data) UnloadImage(img);
        return NULL;
    }
    ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    int w = img.width, h = img.height;
    size_t sz = (size_t)w * h * 4;
    unsigned char *rgba = malloc(sz);
    if (!rgba) { UnloadImage(img); return NULL; }
    memcpy(rgba, img.data, sz);
    UnloadImage(img);
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return rgba;
}
