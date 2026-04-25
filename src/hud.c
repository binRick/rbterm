/* System-info HUD: local-host probe + formatter. Platform paths:
     macOS: gethostname / getloadavg / sysctlbyname(hw.memsize +
            vm_stat) / statfs / SCNetworkConfiguration-or-getifaddrs.
     Linux: gethostname / getloadavg / /proc/meminfo / statvfs /
            getifaddrs.
     Windows: GetComputerName / GlobalMemoryStatusEx / GetDiskFreeSpaceEx /
              GetAdaptersAddresses (TODO when we ship the Windows hud).

   All calls are non-blocking and complete in microseconds, so the
   caller can poll once a second on the main thread without worrying
   about stalling the render loop. */
#include "hud.h"

#include <stdio.h>
#include <string.h>
#ifdef _WIN32
  /* GetComputerNameA — the only Windows-side syscall we use here.
     NOGDI / NOUSER mirror main.c so raylib's Rectangle / DrawText
     don't collide if the headers ever cross-include. */
  #define NOGDI
  #define NOUSER
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <unistd.h>      /* gethostname; not on MSVC */
#endif

#if defined(__APPLE__)
  #include <sys/sysctl.h>
  #include <sys/mount.h>
  #include <mach/mach.h>
  #include <mach/mach_host.h>
  #include <mach/host_info.h>
  #include <mach/vm_statistics.h>
  #include <ifaddrs.h>
  #include <netdb.h>
  #include <net/if.h>          /* IFF_LOOPBACK / IFF_UP */
#elif defined(__linux__)
  #include <sys/statvfs.h>
  #include <sys/sysinfo.h>
  #include <ifaddrs.h>
  #include <netdb.h>
  #include <net/if.h>
#endif

#include <stdlib.h>

/* Best-effort: pick the first non-loopback IPv4 address from the
   interface list. Falls back to the empty string if nothing's up. */
static void pick_local_ipv4(char *out, int cap) {
    if (cap > 0) out[0] = 0;
#if defined(__APPLE__) || defined(__linux__)
    struct ifaddrs *ifa = NULL;
    if (getifaddrs(&ifa) != 0 || !ifa) return;
    for (struct ifaddrs *cur = ifa; cur; cur = cur->ifa_next) {
        if (!cur->ifa_addr) continue;
        if (cur->ifa_addr->sa_family != AF_INET) continue;
        if ((cur->ifa_flags & IFF_LOOPBACK) != 0) continue;
        if ((cur->ifa_flags & IFF_UP) == 0) continue;
        char host[NI_MAXHOST] = {0};
        if (getnameinfo(cur->ifa_addr, sizeof(struct sockaddr_in),
                        host, sizeof(host), NULL, 0,
                        NI_NUMERICHOST) == 0) {
            snprintf(out, (size_t)cap, "%s", host);
            break;
        }
    }
    freeifaddrs(ifa);
#else
    (void)cap;
#endif
}

#if defined(__APPLE__)
/* Return the system's free memory in MB. Mach gives us a per-page
   breakdown; "free + inactive + speculative" is what most OS
   monitors report as "available". Pages × pagesize / 1MB. */
static long macos_mem_free_mb(void) {
    mach_port_t host = mach_host_self();
    vm_size_t page_size = 0;
    if (host_page_size(host, &page_size) != KERN_SUCCESS) return -1;
    vm_statistics64_data_t st;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(host, HOST_VM_INFO64,
                          (host_info64_t)&st, &count) != KERN_SUCCESS) return -1;
    uint64_t free_pages = (uint64_t)st.free_count + st.inactive_count
                                                  + st.speculative_count;
    uint64_t bytes = free_pages * (uint64_t)page_size;
    return (long)(bytes / (1024ULL * 1024ULL));
}
#endif

#if defined(__linux__)
/* Read MemAvailable from /proc/meminfo (kB) and convert to MB. */
static long linux_mem_free_mb(void) {
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) return -1;
    char line[256];
    long mb = -1;
    while (fgets(line, sizeof(line), fp)) {
        long kb;
        if (sscanf(line, "MemAvailable: %ld kB", &kb) == 1) {
            mb = kb / 1024;
            break;
        }
    }
    fclose(fp);
    return mb;
}
#endif

/* Free disk percentage on the root volume. We surface "% free"
   (not absolute MB) because rooms differ wildly in size and the
   percent is the metric users actually feel ("am I running out?"). */
static int root_disk_free_pct(void) {
#if defined(__APPLE__)
    struct statfs st;
    if (statfs("/", &st) != 0) return -1;
    if (st.f_blocks == 0) return -1;
    return (int)((100ULL * st.f_bavail) / st.f_blocks);
#elif defined(__linux__)
    struct statvfs st;
    if (statvfs("/", &st) != 0) return -1;
    if (st.f_blocks == 0) return -1;
    return (int)((100ULL * st.f_bavail) / st.f_blocks);
#else
    return -1;
#endif
}

void hud_local_poll(char *out_hostname, int hostname_cap,
                    char *out_ip, int ip_cap,
                    double *out_load1,
                    long *out_mem_free_mb,
                    int *out_disk_free_pct) {
#ifdef _WIN32
    /* Windows HUD is intentionally a stub for now (see TODO at top
       of file). Fill safe defaults so callers don't display garbage. */
    (void)hostname_cap;
    (void)ip_cap;
    if (out_hostname && hostname_cap > 0) {
        DWORD len = (DWORD)hostname_cap;
        if (!GetComputerNameA(out_hostname, &len)) {
            snprintf(out_hostname, (size_t)hostname_cap, "?");
        }
    }
    if (out_ip && ip_cap > 0) out_ip[0] = 0;
    if (out_load1)        *out_load1        = -1.0;
    if (out_mem_free_mb)  *out_mem_free_mb  = -1;
    if (out_disk_free_pct)*out_disk_free_pct= -1;
#else
    if (out_hostname && hostname_cap > 0) {
        if (gethostname(out_hostname, (size_t)hostname_cap) != 0) {
            snprintf(out_hostname, (size_t)hostname_cap, "%s", "?");
        }
        out_hostname[hostname_cap - 1] = 0;
        /* Strip the trailing .local that Bonjour glues onto macOS
           hostnames — most users don't care to see it. */
        size_t hl = strlen(out_hostname);
        if (hl > 6 && strcmp(out_hostname + hl - 6, ".local") == 0) {
            out_hostname[hl - 6] = 0;
        }
    }
    pick_local_ipv4(out_ip, ip_cap);
    if (out_load1) {
        double avg[3] = {0, 0, 0};
        if (getloadavg(avg, 3) >= 1) *out_load1 = avg[0];
        else                         *out_load1 = -1.0;
    }
    if (out_mem_free_mb) {
#if defined(__APPLE__)
        *out_mem_free_mb = macos_mem_free_mb();
#elif defined(__linux__)
        *out_mem_free_mb = linux_mem_free_mb();
#else
        *out_mem_free_mb = -1;
#endif
    }
    if (out_disk_free_pct) *out_disk_free_pct = root_disk_free_pct();
#endif
}

/* Format helper: render the unit suffix that fits the magnitude.
   Used for memory: < 1024 MB stays MB, larger jumps to GB with one
   decimal so 5.4 GB and 12 GB both render compactly. */
static void format_mem(long mb, char *buf, int cap) {
    if (mb < 0)            snprintf(buf, (size_t)cap, "?");
    else if (mb < 1024)    snprintf(buf, (size_t)cap, "%ld MB", mb);
    else                   snprintf(buf, (size_t)cap, "%.1f GB", mb / 1024.0);
}

/* Cumulative CPU ticks across all cores. macOS exposes
   host_cpu_load_info (USER, SYSTEM, IDLE, NICE counters);
   Linux exposes /proc/stat with the same buckets plus a few
   extras we lump into "busy". */
bool hud_read_cpu_ticks(unsigned long long *out_busy,
                        unsigned long long *out_total) {
    if (out_busy)  *out_busy = 0;
    if (out_total) *out_total = 0;
#if defined(__APPLE__)
    host_cpu_load_info_data_t info;
    mach_msg_type_number_t cnt = HOST_CPU_LOAD_INFO_COUNT;
    if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO,
                        (host_info_t)&info, &cnt) != KERN_SUCCESS) {
        return false;
    }
    unsigned long long user = info.cpu_ticks[CPU_STATE_USER];
    unsigned long long sys  = info.cpu_ticks[CPU_STATE_SYSTEM];
    unsigned long long nice = info.cpu_ticks[CPU_STATE_NICE];
    unsigned long long idle = info.cpu_ticks[CPU_STATE_IDLE];
    unsigned long long busy = user + sys + nice;
    if (out_busy)  *out_busy = busy;
    if (out_total) *out_total = busy + idle;
    return true;
#elif defined(__linux__)
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) return false;
    /* "cpu  user nice system idle iowait irq softirq steal guest guest_nice" */
    unsigned long long user, nice, sys, idle;
    unsigned long long iowait = 0, irq = 0, softirq = 0, steal = 0;
    int n = fscanf(fp, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &user, &nice, &sys, &idle,
                   &iowait, &irq, &softirq, &steal);
    fclose(fp);
    if (n < 4) return false;
    unsigned long long busy = user + nice + sys + irq + softirq + steal;
    if (out_busy)  *out_busy = busy;
    if (out_total) *out_total = busy + idle + iowait;
    return true;
#else
    return false;
#endif
}

int hud_format(char *buf, int cap,
               const char *hostname, const char *ip,
               double load1, long mem_free_mb, int disk_free_pct,
               bool show_host, bool show_ip, bool show_load,
               bool show_mem,  bool show_disk) {
    char load_s[24], mem_s[24], disk_s[24];
    if (load1 < 0)              snprintf(load_s, sizeof(load_s), "?");
    else                        snprintf(load_s, sizeof(load_s), "%.2f", load1);
    format_mem(mem_free_mb, mem_s, (int)sizeof(mem_s));
    if (disk_free_pct < 0)      snprintf(disk_s, sizeof(disk_s), "?");
    else                        snprintf(disk_s, sizeof(disk_s), "%d%% free", disk_free_pct);

    int n = 0;
    #define APPEND(...) do { \
        int wrote = snprintf(buf + n, (size_t)(cap - n), __VA_ARGS__); \
        if (wrote > 0) n += wrote; \
    } while (0)
    if (show_host) APPEND("%s%s", n ? "\n" : "", (hostname && *hostname) ? hostname : "?");
    if (show_ip)   APPEND("%s%s", n ? "\n" : "", (ip       && *ip)       ? ip       : "?");
    if (show_load) APPEND("%sload %s", n ? "\n" : "", load_s);
    if (show_mem)  APPEND("%smem %s",  n ? "\n" : "", mem_s);
    if (show_disk) APPEND("%sdisk %s", n ? "\n" : "", disk_s);
    #undef APPEND
    return n;
}
