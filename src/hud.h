/* System-info HUD: per-pane overlay with hostname, IP, load avg,
   free memory, and free disk space. Local panes get the data from
   cheap syscalls (see hud_local_poll). SSH panes get it from a
   dedicated probe thread that runs commands on the live libssh
   session (see hud_ssh.c — wired up later). */
#pragma once

#include <stdbool.h>

/* Plain-old-data snapshot. main.c stamps these fields directly on
   the active Pane; the renderer formats and draws them. */

/* Refresh the host info for `out_*` from the local machine. Cheap
   syscalls only — no blocking, no allocation. Safe to call every
   frame; throttled by a deadline in the caller. */
void hud_local_poll(char *out_hostname, int hostname_cap,
                    char *out_ip, int ip_cap,
                    double *out_load1,
                    long *out_mem_free_mb,
                    int *out_disk_free_pct);

/* Format the HUD into `buf` for the renderer — up to five short
   lines, depending on the show_* booleans (each independently
   suppressible). No trailing newline. Caller draws with monospace
   on a translucent slab. Returns bytes written (excluding NUL). */
int  hud_format(char *buf, int cap,
                const char *hostname, const char *ip,
                double load1, long mem_free_mb, int disk_free_pct,
                bool show_host, bool show_ip, bool show_load,
                bool show_mem,  bool show_disk);
