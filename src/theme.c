#include "theme.h"
#include "themes_embedded.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

/* Mutable state. themes_load_builtins() populates this once at startup,
 * then it's treated as read-only. */
#define THEMES_MAX 512
static Theme   g_themes[THEMES_MAX];
static int     g_theme_count = 0;
static bool    g_themes_loaded = false;

/* Hex digit -> 0..15. Returns -1 for non-hex chars so callers can
   detect malformed colour specs. */
static int hexval_(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Accept "#RRGGBB" / "RRGGBB" / "rgb:RR/GG/BB" (pal uses the bare-hex form).
 * Returns true on success + writes 0xRRGGBB into *out. */
static bool parse_hex_rgb(const char *s, uint32_t *out) {
    if (!s) return false;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '#') s++;
    if (strncmp(s, "rgb:", 4) == 0) {
        s += 4;
        int r1 = hexval_(s[0]), r2 = hexval_(s[1]);
        if (r1 < 0 || r2 < 0 || s[2] != '/') return false;
        int g1 = hexval_(s[3]), g2 = hexval_(s[4]);
        if (g1 < 0 || g2 < 0 || s[5] != '/') return false;
        int b1 = hexval_(s[6]), b2 = hexval_(s[7]);
        if (b1 < 0 || b2 < 0) return false;
        *out = (uint32_t)(((r1 << 4 | r2) << 16) |
                          ((g1 << 4 | g2) << 8)  |
                           (b1 << 4 | b2));
        return true;
    }
    if (strlen(s) < 6) return false;
    int v[6];
    for (int i = 0; i < 6; i++) { v[i] = hexval_(s[i]); if (v[i] < 0) return false; }
    *out = (uint32_t)(((v[0] << 4 | v[1]) << 16) |
                      ((v[2] << 4 | v[3]) << 8)  |
                       (v[4] << 4 | v[5]));
    return true;
}

/* Parse one kfc/dark-style body into `t`. Returns true iff at least
 * foreground + background were set — themes missing that aren't useful. */
static bool parse_theme_body(const char *body, Theme *t) {
    bool got_fg = false, got_bg = false;
    const char *p = body;
    char line[256];
    while (*p) {
        /* Read next line into the buffer. */
        size_t n = 0;
        while (*p && *p != '\n' && n + 1 < sizeof(line)) line[n++] = *p++;
        line[n] = 0;
        if (*p == '\n') p++;
        /* Trim leading whitespace + skip blank / comment lines. */
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == 0 || *s == '#' || *s == '!') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        const char *key = s;
        const char *val = eq + 1;
        while (*val == ' ' || *val == '\t') val++;
        uint32_t col;
        if (!parse_hex_rgb(val, &col)) continue;

        if (strcmp(key, "background") == 0)      { t->bg = col; got_bg = true; }
        else if (strcmp(key, "foreground") == 0) { t->fg = col; got_fg = true; }
        else if (strcmp(key, "cursor") == 0)     { t->cursor = col; }
        else if (strncmp(key, "color", 5) == 0) {
            int idx = atoi(key + 5);
            if (idx >= 0 && idx < 16) t->palette[idx] = col;
        }
    }
    /* Cursor defaults to foreground if the theme didn't specify one. */
    if (got_fg && t->cursor == 0) t->cursor = t->fg;
    return got_fg && got_bg;
}

/* Parse every theme baked into themes_embedded.h into g_themes.
   Idempotent — guarded by g_themes_loaded so repeated calls are
   no-ops. Failed parses (themes without a foreground+background)
   are silently skipped. */
void themes_load_builtins(void) {
    if (g_themes_loaded) return;
    g_themes_loaded = true;
    for (int i = 0; i < k_embedded_theme_count && g_theme_count < THEMES_MAX; i++) {
        Theme t = {0};
        strncpy(t.name, k_embedded_themes[i].name, sizeof(t.name) - 1);
        if (parse_theme_body(k_embedded_themes[i].body, &t)) {
            g_themes[g_theme_count++] = t;
        }
    }
}

/* Number of successfully-parsed builtin themes. */
int          themes_count(void)      { return g_theme_count; }
/* Heap-stable pointer to the themes array (length = themes_count()). */
const Theme *themes_all(void)        { return g_themes; }

/* Linear-search lookup by exact name. Returns NULL if not found
   or `name` is empty. */
const Theme *theme_find_by_name(const char *name) {
    if (!name || !*name) return NULL;
    for (int i = 0; i < g_theme_count; i++) {
        if (strcmp(g_themes[i].name, name) == 0) return &g_themes[i];
    }
    return NULL;
}

/* Push a parsed Theme into a Screen — sets default fg/bg/cursor and
   the 16-entry colour palette. Per-screen, so applying a theme to
   one pane doesn't bleed into others. */
void screen_apply_theme(Screen *s, const Theme *t) {
    if (!s || !t) return;
    screen_set_default_fg(s, t->fg);
    screen_set_default_bg(s, t->bg);
    screen_set_cursor_color(s, t->cursor);
    for (int i = 0; i < 16; i++) screen_set_palette_entry(s, i, t->palette[i]);
}
