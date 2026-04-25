/* Parser-only microbenchmark for screen.c.
 *
 * Produces a deterministic byte stream that mirrors a chosen
 * vtebench benchmark, then feeds it through a Screen (no PTY, no
 * raylib, no rendering) in a tight loop and reports throughput.
 *
 * Usage:
 *   make parser_bench   # build
 *   ./parser_bench [bench]
 *
 * Bench names: dense_cells (default), scrolling, unicode.
 *
 * Why a microbench: vtebench measures the whole rbterm pipeline
 * including raylib, vsync, GUI input, etc. To iterate on parser
 * perf cleanly we want to isolate screen_feed() and the per-cell
 * write path. Run this under `sample`, `samply`, or Instruments
 * to find hot frames.
 */

#include "../src/screen.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Build a 1 MiB-ish dense_cells stream: \e[38;5;F;48;5;B;1;3;4m<C>
   per cell over an 80×24 grid, then repeat with the next letter. */
static size_t gen_dense_cells(uint8_t *buf, size_t cap, int cols, int rows) {
    size_t n = 0;
    int offset = 0;
    while (n + 32 < cap) {
        n += (size_t)snprintf((char *)buf + n, cap - n, "\x1b[H");
        for (int line = 1; line <= rows && n + 32 < cap; line++) {
            for (int col = 1; col <= cols && n + 32 < cap; col++) {
                int idx = line + col + offset;
                int fg  = idx % 156 + 100;
                int bg  = 255 - idx % 156 + 100;
                int c   = 'A' + (offset % 26);
                n += (size_t)snprintf((char *)buf + n, cap - n,
                                      "\x1b[38;5;%d;48;5;%d;1;3;4m%c",
                                      fg, bg, c);
            }
        }
        offset++;
    }
    return n;
}

/* Big block of unicode codepoints to exercise UTF-8 multi-byte
   parsing. Mixes 1/2/3-byte runs. */
static size_t gen_unicode(uint8_t *buf, size_t cap) {
    static const char *snippets[] = {
        "Здравствуй, мир! ",
        "你好，世界。",
        "مرحبا بالعالم. ",
        "Γειά σου Κόσμε! ",
        "café résumé naïve façade ",
        "the quick brown fox jumps over the lazy dog\n",
    };
    size_t n = 0;
    int idx = 0;
    while (n < cap) {
        const char *s = snippets[idx++ % (sizeof(snippets) / sizeof(*snippets))];
        size_t l = strlen(s);
        if (n + l > cap) break;
        memcpy(buf + n, s, l);
        n += l;
    }
    return n;
}

/* Long printable-ASCII runs separated by newlines — simulates
   `cat /usr/share/dict/words` or any program flooding text. This
   is the workload the NEON printable-run scanner is designed for. */
static size_t gen_long_text(uint8_t *buf, size_t cap) {
    static const char *lines[] = {
        "the quick brown fox jumps over the lazy dog every single morning\n",
        "abandon abolish abrupt absence absolute abstract abundance accept\n",
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz!@#\n",
        "loremipsumdolorsitametconsecteturadipiscingelitseddoeiusmodtempor\n",
    };
    size_t n = 0;
    int idx = 0;
    while (n < cap) {
        const char *s = lines[idx++ % (sizeof(lines) / sizeof(*lines))];
        size_t l = strlen(s);
        if (n + l > cap) break;
        memcpy(buf + n, s, l);
        n += l;
    }
    return n;
}

/* y\n repeated, like vtebench's scrolling benchmark. */
static size_t gen_scrolling(uint8_t *buf, size_t cap) {
    size_t n = 0;
    while (n + 2 <= cap) {
        buf[n++] = 'y';
        buf[n++] = '\n';
    }
    return n;
}

int main(int argc, char **argv) {
    const char *bench = (argc > 1) ? argv[1] : "dense_cells";

    /* 1 MiB working buffer to match vtebench's --min-bytes default. */
    size_t cap = 1 << 20;
    uint8_t *buf = malloc(cap);
    if (!buf) { perror("malloc"); return 1; }

    size_t stream_len = 0;
    if (!strcmp(bench, "dense_cells")) {
        stream_len = gen_dense_cells(buf, cap, 80, 24);
    } else if (!strcmp(bench, "unicode")) {
        stream_len = gen_unicode(buf, cap);
    } else if (!strcmp(bench, "scrolling")) {
        stream_len = gen_scrolling(buf, cap);
    } else if (!strcmp(bench, "long_text")) {
        stream_len = gen_long_text(buf, cap);
    } else {
        fprintf(stderr, "unknown bench: %s (try dense_cells / unicode / scrolling / long_text)\n", bench);
        free(buf);
        return 1;
    }
    fprintf(stderr, "bench=%s stream=%zu bytes\n", bench, stream_len);

    /* Hidden Screen — no IO callbacks. */
    ScreenIO io = {0};
    Screen *s = screen_new(80, 24, 5000, io);
    if (!s) { fprintf(stderr, "screen_new failed\n"); free(buf); return 1; }

    /* Warmup. */
    for (int w = 0; w < 5; w++) screen_feed(s, buf, stream_len);

    /* Measured run: feed enough times to total >= ~512 MiB so the
       wall-time number stabilises. Reports MiB/s and ms-per-MiB to
       compare directly against vtebench's drain-rate metric. */
    size_t total_bytes = 0;
    size_t target = (size_t)512 << 20;   /* 512 MiB */
    int iters = 0;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (total_bytes < target) {
        screen_feed(s, buf, stream_len);
        total_bytes += stream_len;
        iters++;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);

    double elapsed_s = (double)(t1.tv_sec - t0.tv_sec)
                     + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    double mib       = (double)total_bytes / (1024.0 * 1024.0);
    double mibs      = mib / elapsed_s;
    double ms_per_mib = (elapsed_s * 1000.0) / mib;

    printf("bench=%s iters=%d total=%.1f MiB elapsed=%.3f s "
           "throughput=%.1f MiB/s (%.3f ms/MiB)\n",
           bench, iters, mib, elapsed_s, mibs, ms_per_mib);

    screen_free(s);
    free(buf);
    return 0;
}
