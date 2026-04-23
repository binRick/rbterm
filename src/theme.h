#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "screen.h"

/* A parsed palette theme. `name` is the filename in
 * third_party/pal/palettes/kfc/dark/ (e.g. "dracula", "zenburn"). */
typedef struct {
    char name[64];
    uint32_t bg;
    uint32_t fg;
    uint32_t cursor;
    uint32_t palette[16];
} Theme;

/* Parse every theme baked into the binary at startup (src/themes_embedded.h).
 * Idempotent: subsequent calls are no-ops. */
void themes_load_builtins(void);

/* Read-only access to the parsed theme list. */
int           themes_count(void);
const Theme  *themes_all(void);
const Theme  *theme_find_by_name(const char *name);

/* Write every colour in `t` into `s` (bg, fg, cursor, palette[0..15]). */
void screen_apply_theme(Screen *s, const Theme *t);
