#define _DARWIN_C_SOURCE
#define _GNU_SOURCE
#include "pty_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#if defined(__APPLE__)
  #include <util.h>
  #include <libproc.h>
#else
  #include <pty.h>
#endif

#define RING_CAP (1u << 20)   /* 1 MB per tab */

typedef struct {
    pid_t pid;
    int   fd;
    bool  alive;

    /* Reader thread keeps the PTY drained into a ring buffer regardless
       of the frame rate. Without this, bursty commands like `find /usr`
       stall waiting for the main loop's next frame to drain the kernel's
       PTY buffer. */
    pthread_t       reader;
    pthread_mutex_t lock;
    uint8_t        *ring;
    size_t          head;
    size_t          tail;
    volatile int    stop;
    volatile int    eof;
} LocalPty;

static void *local_reader(void *arg) {
    LocalPty *p = (LocalPty *)arg;
    uint8_t buf[16384];
    while (!__atomic_load_n(&p->stop, __ATOMIC_RELAXED)) {
        ssize_t n = read(p->fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        pthread_mutex_lock(&p->lock);
        for (ssize_t i = 0; i < n; i++) {
            p->ring[p->head] = buf[i];
            p->head = (p->head + 1) & (RING_CAP - 1);
            if (p->head == p->tail) {
                /* overrun — drop oldest byte */
                p->tail = (p->tail + 1) & (RING_CAP - 1);
            }
        }
        pthread_mutex_unlock(&p->lock);
    }
    __atomic_store_n(&p->eof, 1, __ATOMIC_RELAXED);
    return NULL;
}

void *local_open_impl(int cols, int rows) {
    struct winsize ws = { .ws_row = (unsigned short)rows,
                          .ws_col = (unsigned short)cols };
    int master;
    pid_t pid = forkpty(&master, NULL, NULL, &ws);
    if (pid < 0) { perror("forkpty"); return NULL; }
    if (pid == 0) {
        signal(SIGCHLD, SIG_DFL);
        signal(SIGHUP,  SIG_DFL);
        signal(SIGINT,  SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGALRM, SIG_DFL);
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        unsetenv("TERM_PROGRAM");
        unsetenv("TERM_SESSION_ID");
        /* Ensure the shell sees a UTF-8 locale so tools like tmux use
           Unicode box-drawing instead of falling back to '-' / '|'.
           Only set if the parent didn't explicitly set a locale — if
           someone runs LANG=C on purpose, we respect it. */
        if (!getenv("LC_ALL") && !getenv("LC_CTYPE")) {
            setenv("LC_CTYPE", "en_US.UTF-8", 1);
        }
        if (!getenv("LANG")) {
            setenv("LANG", "en_US.UTF-8", 1);
        }

        const char *home = getenv("HOME");
        if (!home || !*home) {
            struct passwd *pw = getpwuid(getuid());
            if (pw && pw->pw_dir) home = pw->pw_dir;
        }
        if (home && *home) {
            (void)chdir(home);
            setenv("PWD", home, 1);
        }

        const char *shell = getenv("SHELL");
        if (!shell || !*shell) shell = "/bin/bash";
        const char *argv0 = strrchr(shell, '/');
        argv0 = argv0 ? argv0 + 1 : shell;
        char dash[64];
        snprintf(dash, sizeof(dash), "-%s", argv0);
        execl(shell, dash, (char *)NULL);
        perror("execl");
        _exit(127);
    }
    /* Reader thread uses blocking reads — leave the fd in blocking mode. */
    LocalPty *p = calloc(1, sizeof(*p));
    p->pid = pid;
    p->fd  = master;
    p->alive = true;
    p->ring = malloc(RING_CAP);
    if (!p->ring) { close(master); free(p); return NULL; }
    pthread_mutex_init(&p->lock, NULL);
    if (pthread_create(&p->reader, NULL, local_reader, p) != 0) {
        pthread_mutex_destroy(&p->lock);
        close(master);
        free(p->ring);
        free(p);
        return NULL;
    }
    return p;
}

void local_close_impl(void *impl) {
    LocalPty *p = impl;
    if (!p) return;
    __atomic_store_n(&p->stop, 1, __ATOMIC_RELAXED);
    if (p->pid > 0) {
        kill(p->pid, SIGHUP);
        int status;
        waitpid(p->pid, &status, 0);
    }
    if (p->fd >= 0) {
        close(p->fd);           /* unblocks any pending read() in the thread */
        p->fd = -1;
    }
    if (p->reader) pthread_join(p->reader, NULL);
    pthread_mutex_destroy(&p->lock);
    free(p->ring);
    free(p);
}

bool local_alive_impl(void *impl) {
    LocalPty *p = impl;
    if (!p || !p->alive) return false;
    int status;
    pid_t w = waitpid(p->pid, &status, WNOHANG);
    if (w == p->pid) { p->alive = false; return false; }
    return true;
}

int local_read_impl(void *impl, uint8_t *buf, size_t cap) {
    LocalPty *p = impl;
    if (!p) return -1;
    size_t n = 0;
    pthread_mutex_lock(&p->lock);
    while (n < cap && p->tail != p->head) {
        buf[n++] = p->ring[p->tail];
        p->tail = (p->tail + 1) & (RING_CAP - 1);
    }
    pthread_mutex_unlock(&p->lock);
    if (n > 0) return (int)n;
    /* No data right now — but the child / reader may have ended. */
    if (__atomic_load_n(&p->eof, __ATOMIC_RELAXED) && !local_alive_impl(p)) return -1;
    return 0;
}

void local_write_impl(void *impl, const uint8_t *buf, size_t n) {
    LocalPty *p = impl;
    if (!p || p->fd < 0) return;
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(p->fd, buf + off, n - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            return;
        }
        off += (size_t)w;
    }
}

void local_resize_impl(void *impl, int cols, int rows) {
    LocalPty *p = impl;
    if (!p || p->fd < 0) return;
    struct winsize ws = { .ws_row = (unsigned short)rows,
                          .ws_col = (unsigned short)cols };
    ioctl(p->fd, TIOCSWINSZ, &ws);
}

bool local_cwd_impl(void *impl, char *out, size_t cap) {
    LocalPty *p = impl;
    if (!p || !out || cap == 0 || p->pid <= 0) return false;
#if defined(__APPLE__)
    struct proc_vnodepathinfo vpi;
    int r = proc_pidinfo((int)p->pid, PROC_PIDVNODEPATHINFO, 0, &vpi, sizeof(vpi));
    if (r <= 0) return false;
    strncpy(out, vpi.pvi_cdir.vip_path, cap - 1);
    out[cap - 1] = 0;
    return out[0] != 0;
#else
    char link[64];
    snprintf(link, sizeof(link), "/proc/%d/cwd", (int)p->pid);
    ssize_t n = readlink(link, out, cap - 1);
    if (n <= 0) return false;
    out[n] = 0;
    return true;
#endif
}
