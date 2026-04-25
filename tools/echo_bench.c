/* echo_bench — fully automated terminal round-trip latency benchmark.
 *
 * Measures the time between writing an ANSI Device Status Report
 * query (CSI 6n) and reading the terminal's response. The terminal
 * has to parse the query, format a CSI <row>;<col>R reply, and put
 * it back on stdin. The wall-clock delta covers:
 *
 *   - kernel write to PTY
 *   - terminal's read + VT parser
 *   - terminal's write back to PTY
 *   - kernel read into our buffer
 *
 * It is *not* keystroke-to-pixel latency — that needs OS-native
 * event injection + screen capture, which only typometer-style
 * tools provide. But it's a clean, automated proxy for "how
 * snappy is this terminal's parser + I/O path", and unlike
 * vtebench it captures backpressure / ack timing rather than
 * raw drain throughput.
 *
 * Usage: ./echo_bench [N] (defaults to 1000 samples)
 *
 * Output: min / median / avg / max / stdev in ms, plus a histogram.
 * Can be compared head-to-head by running the same binary inside
 * each terminal you want to test.
 *
 * Requires a TTY on stdin/stdout. No external deps. POSIX only
 * (Linux + macOS); Windows would need a DSR + ReadConsole port.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static int            g_tty_fd = -1;
static struct termios g_saved_tio;

/* Restore cooked-mode TTY on exit so a botched run doesn't leave
   the user's terminal stuck in raw. */
static void restore_tty(void) {
    if (g_tty_fd >= 0) {
        tcsetattr(g_tty_fd, TCSANOW, &g_saved_tio);
    }
}

static long long now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int cmp_ll(const void *a, const void *b) {
    long long da = *(const long long *)a, db = *(const long long *)b;
    return da < db ? -1 : da > db ? 1 : 0;
}

int main(int argc, char **argv) {
    int n = (argc > 1) ? atoi(argv[1]) : 1000;
    if (n < 10) n = 10;
    if (n > 100000) n = 100000;

    g_tty_fd = open("/dev/tty", O_RDWR);
    if (g_tty_fd < 0) {
        fprintf(stderr, "open(/dev/tty): %s\n", strerror(errno));
        return 1;
    }

    if (tcgetattr(g_tty_fd, &g_saved_tio) != 0) {
        fprintf(stderr, "tcgetattr: %s\n", strerror(errno));
        return 1;
    }
    atexit(restore_tty);

    /* Raw-ish mode: no echo, no canonical line buffering, no signals,
       so the CSI 6n response lands in our read directly. */
    struct termios raw = g_saved_tio;
    raw.c_lflag &= ~(ICANON | ECHO | ISIG);
    raw.c_iflag &= ~(IXON | ICRNL);
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 1;   /* 100 ms read timeout per call */
    if (tcsetattr(g_tty_fd, TCSANOW, &raw) != 0) {
        fprintf(stderr, "tcsetattr: %s\n", strerror(errno));
        return 1;
    }

    /* Drain any pending input bytes (e.g. mouse motion, kitty
       graphics responses) so they don't pollute the first sample. */
    char drain[4096];
    while (read(g_tty_fd, drain, sizeof(drain)) > 0) { }

    fprintf(stderr, "echo_bench: %d samples × CSI 6n round-trip ", n);
    fflush(stderr);

    long long *samples = malloc(sizeof(long long) * n);
    if (!samples) { fprintf(stderr, "oom\n"); return 1; }

    long long deadline_total_ns = (long long)5 * 1000000000LL;  /* 5s timeout */

    int collected = 0;
    long long t_first = now_ns();
    for (int i = 0; i < n; i++) {
        long long t0 = now_ns();
        if (write(g_tty_fd, "\x1b[6n", 4) != 4) break;

        /* Read until we see 'R' (CSI 6n's terminator). The full
           response is "\e[<row>;<col>R" — we don't parse it, just
           wait for the terminator byte. */
        long long elapsed = 0;
        char ch;
        while (1) {
            ssize_t r = read(g_tty_fd, &ch, 1);
            if (r == 1) {
                if (ch == 'R') break;
                continue;
            }
            if (r < 0 && errno != EINTR) goto giveup;
            elapsed = now_ns() - t0;
            if (elapsed > 500000000LL) goto giveup;   /* 500 ms / sample */
        }
        long long t1 = now_ns();
        samples[collected++] = t1 - t0;

        /* Cheap progress dot every 100 samples. */
        if ((collected % 100) == 0) {
            fputc('.', stderr);
            fflush(stderr);
        }
        if (now_ns() - t_first > deadline_total_ns) break;
        continue;
giveup:
        fprintf(stderr, "\n  no response after %lld ms — bailing at %d/%d samples\n",
                elapsed / 1000000LL, collected, n);
        break;
    }
    fputc('\n', stderr);

    if (collected < 1) {
        fprintf(stderr, "no samples collected — does this terminal handle CSI 6n?\n");
        free(samples);
        return 1;
    }

    qsort(samples, collected, sizeof(long long), cmp_ll);
    double sum = 0;
    for (int i = 0; i < collected; i++) sum += (double)samples[i];
    double mean = sum / collected;
    double sqdiff = 0;
    for (int i = 0; i < collected; i++) {
        double d = (double)samples[i] - mean;
        sqdiff += d * d;
    }
    double stdev = (collected > 1) ? (sqdiff / (collected - 1)) : 0;
    /* Manual sqrt (avoid -lm dependency): use Newton's method.
       At this scale double precision converges fast enough. */
    double sqrt_stdev;
    if (stdev <= 0) sqrt_stdev = 0;
    else {
        double x = stdev;
        for (int k = 0; k < 30; k++) x = 0.5 * (x + stdev / x);
        sqrt_stdev = x;
    }

    /* Report in ms. */
    double min_ms    = (double)samples[0]               / 1e6;
    double max_ms    = (double)samples[collected-1]     / 1e6;
    double med_ms    = (double)samples[collected/2]     / 1e6;
    double p99_ms    = (double)samples[(collected*99)/100] / 1e6;
    double mean_ms   = mean / 1e6;
    double stdev_ms  = sqrt_stdev / 1e6;

    const char *term  = getenv("TERM_PROGRAM");
    if (!term) term = "?";

    printf("term=%-12s n=%d  min=%.3f  med=%.3f  mean=%.3f  p99=%.3f  max=%.3f  sd=%.3f  ms\n",
           term, collected, min_ms, med_ms, mean_ms, p99_ms, max_ms, stdev_ms);

    free(samples);
    return 0;
}
