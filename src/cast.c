/* Asciinema v2 (.cast) parser — see cast.h. The format is one JSON
   value per line:
     line 1 (header):  {"version": 2, "width": W, "height": H, ...}
     line N (event):   [<seconds>, "o", "<utf-8 bytes with JSON escapes>"]
   We only need width/height + the "o" data, so the parser is
   intentionally minimal — no general JSON support. */
#include "cast.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Find an integer following `key` in the header object (e.g. find
   `"width": 80` after passing key="width"). Returns the value or
   `dflt` if not found. */
static int header_int(const char *line, const char *key, int dflt) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(line, needle);
    if (!p) return dflt;
    p += strlen(needle);
    while (*p && (*p == ' ' || *p == ':' || *p == '\t')) p++;
    if (!*p) return dflt;
    int sign = 1;
    if (*p == '-') { sign = -1; p++; }
    int v = 0; bool any = false;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; any = true; }
    return any ? v * sign : dflt;
}

/* Hex digit → 0..15, or -1. */
static int hexv(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Encode a Unicode codepoint as UTF-8 into `out`, return bytes
   written (1..4). Caller reserves >= 4 bytes. */
static size_t utf8_emit(uint32_t cp, uint8_t *out) {
    if (cp < 0x80)    { out[0] = (uint8_t)cp; return 1; }
    if (cp < 0x800)   { out[0] = 0xC0 | (cp >> 6);
                        out[1] = 0x80 | (cp & 0x3F); return 2; }
    if (cp < 0x10000) { out[0] = 0xE0 | (cp >> 12);
                        out[1] = 0x80 | ((cp >> 6) & 0x3F);
                        out[2] = 0x80 | (cp & 0x3F); return 3; }
    out[0] = 0xF0 | (cp >> 18);
    out[1] = 0x80 | ((cp >> 12) & 0x3F);
    out[2] = 0x80 | ((cp >> 6) & 0x3F);
    out[3] = 0x80 | (cp & 0x3F);
    return 4;
}

/* Un-escape a JSON string body (between but not including the
   surrounding quotes) into a fresh heap buffer. Returns NULL +
   *out_n = 0 on alloc failure. Tolerates malformed escapes by
   passing them through. */
static uint8_t *json_unescape(const char *src, size_t src_len, size_t *out_n) {
    uint8_t *buf = malloc(src_len + 1);
    if (!buf) { *out_n = 0; return NULL; }
    size_t n = 0;
    for (size_t i = 0; i < src_len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c != '\\' || i + 1 >= src_len) { buf[n++] = c; continue; }
        unsigned char e = (unsigned char)src[++i];
        switch (e) {
        case '"':  buf[n++] = '"';  break;
        case '\\': buf[n++] = '\\'; break;
        case '/':  buf[n++] = '/';  break;
        case 'b':  buf[n++] = 0x08; break;
        case 'f':  buf[n++] = 0x0C; break;
        case 'n':  buf[n++] = 0x0A; break;
        case 'r':  buf[n++] = 0x0D; break;
        case 't':  buf[n++] = 0x09; break;
        case 'u': {
            if (i + 4 >= src_len) { buf[n++] = e; break; }
            int h0 = hexv(src[i + 1]);
            int h1 = hexv(src[i + 2]);
            int h2 = hexv(src[i + 3]);
            int h3 = hexv(src[i + 4]);
            if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0) {
                buf[n++] = e;
            } else {
                uint32_t cp = (uint32_t)((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
                /* Codepoints we encode are <0x80 / control-only; just in
                   case a producer uses higher BMP values, emit valid UTF-8. */
                n += utf8_emit(cp, buf + n);
                i += 4;
            }
            break;
        }
        default:   buf[n++] = e; break;
        }
    }
    *out_n = n;
    return buf;
}

/* Parse one event line: [<num>, "o"|"i", "<string>"]. Returns true on
   success and fills *out_t / *out_kind / *out_data / *out_n. Caller
   frees data. Marker streams ("m" etc.) are rejected. */
static bool parse_event(const char *line, size_t len,
                        double *out_t, char *out_kind,
                        uint8_t **out_data, size_t *out_n) {
    /* Find leading '['. */
    size_t i = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
    if (i >= len || line[i] != '[') return false;
    i++;
    /* Parse timestamp as a double. */
    char *endp = NULL;
    double t = strtod(line + i, &endp);
    if (!endp || endp == line + i) return false;
    i = (size_t)(endp - line);
    while (i < len && (line[i] == ' ' || line[i] == '\t' || line[i] == ',')) i++;
    /* Expect "o" or "i". Anything else (markers, future) is skipped. */
    if (i + 3 > len || line[i] != '"') return false;
    char tag = line[i + 1];
    if (line[i + 2] != '"') return false;
    if (tag != 'o' && tag != 'i') return false;
    i += 3;
    while (i < len && (line[i] == ' ' || line[i] == '\t' || line[i] == ',')) i++;
    /* Expect "<data>". */
    if (i >= len || line[i] != '"') return false;
    i++;
    size_t start = i;
    while (i < len && line[i] != '"') {
        if (line[i] == '\\' && i + 1 < len) i++;   /* skip escape */
        i++;
    }
    size_t data_len = i - start;
    *out_t = t;
    *out_kind = tag;
    *out_data = json_unescape(line + start, data_len, out_n);
    return *out_data != NULL || *out_n == 0;
}

CastFile *cast_load(const char *path, char *err, size_t errsz) {
    if (err && errsz) err[0] = 0;
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        if (err && errsz) snprintf(err, errsz, "open %s", path);
        return NULL;
    }
    CastFile *cf = calloc(1, sizeof(*cf));
    if (!cf) { fclose(fp); return NULL; }
    cf->cols = 80;
    cf->rows = 24;

    char *line = NULL;
    size_t cap = 0;
    bool first = true;
    while (1) {
        /* getline isn't portable; do it ourselves with a growable buf. */
        size_t n = 0;
        for (;;) {
            if (n + 1 >= cap) {
                size_t nc = cap ? cap * 2 : 256;
                char *nb = realloc(line, nc);
                if (!nb) { fclose(fp); cast_free(cf); free(line); return NULL; }
                line = nb;
                cap = nc;
            }
            int c = fgetc(fp);
            if (c == EOF) break;
            if (c == '\n') { line[n] = 0; break; }
            line[n++] = (char)c;
        }
        if (n == 0 && feof(fp)) break;
        line[n] = 0;
        if (first) {
            first = false;
            cf->cols = header_int(line, "width", cf->cols);
            cf->rows = header_int(line, "height", cf->rows);
            continue;
        }
        if (n == 0) continue;   /* trailing blank line */
        double t = 0;
        char kind = 'o';
        uint8_t *data = NULL;
        size_t dn = 0;
        if (!parse_event(line, n, &t, &kind, &data, &dn)) continue;
        if (cf->count == cf->cap) {
            size_t nc = cf->cap ? cf->cap * 2 : 256;
            CastEvent *ne = realloc(cf->events, nc * sizeof(*ne));
            if (!ne) { free(data); break; }
            cf->events = ne;
            cf->cap = nc;
        }
        cf->events[cf->count++] = (CastEvent){ t, kind, data, dn };
        if (t > cf->duration_s) cf->duration_s = t;
    }
    free(line);
    fclose(fp);
    if (cf->count == 0) {
        if (err && errsz) snprintf(err, errsz, "no events in %s", path);
        cast_free(cf);
        return NULL;
    }
    return cf;
}

void cast_free(CastFile *cf) {
    if (!cf) return;
    for (size_t i = 0; i < cf->count; i++) free(cf->events[i].data);
    free(cf->events);
    free(cf);
}
