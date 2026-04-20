#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    uint32_t cp;     // Unicode code point (0 = blank)
    uint32_t fg;     // 0xRRGGBB
    uint32_t bg;     // 0xRRGGBB
    uint16_t attrs;  // ATTR_*
} Cell;

enum {
    ATTR_BOLD      = 1u << 0,
    ATTR_DIM       = 1u << 1,
    ATTR_ITALIC    = 1u << 2,
    ATTR_UNDERLINE = 1u << 3,
    ATTR_REVERSE   = 1u << 4,
    ATTR_STRIKE    = 1u << 5,
    ATTR_HIDDEN    = 1u << 6,
    ATTR_WIDE      = 1u << 7,   /* first half of a double-width character */
    ATTR_WIDE_CONT = 1u << 8,   /* right-hand continuation of a wide char  */
    ATTR_DEFAULT_FG = 1u << 14,
    ATTR_DEFAULT_BG = 1u << 15,
};

#define DEFAULT_FG 0xEAEAEAu
#define DEFAULT_BG 0x111111u
#define CURSOR_COLOR 0xEAEAEAu

typedef struct Screen Screen;

typedef struct {
    void *user;
    void (*write)(void *user, const uint8_t *buf, size_t n);
    void (*set_title)(void *user, const char *title);
    void (*bell)(void *user);
} ScreenIO;

Screen *screen_new(int cols, int rows, int scrollback, ScreenIO io);
void    screen_free(Screen *s);
void    screen_resize(Screen *s, int cols, int rows);
void    screen_feed(Screen *s, const uint8_t *data, size_t n);

int     screen_cols(const Screen *s);
int     screen_rows(const Screen *s);
int     screen_cursor_x(const Screen *s);
int     screen_cursor_y(const Screen *s);
bool    screen_cursor_visible(const Screen *s);
int     screen_view_offset(const Screen *s);
int     screen_scrollback_len(const Screen *s);
void    screen_scroll_view(Screen *s, int delta_rows);
void    screen_scroll_reset(Screen *s);

/* Returns cell at viewport position (vy = 0 is top row of visible area).
   Takes current view offset into scrollback into account. */
Cell    screen_view_cell(const Screen *s, int col, int vy);
bool    screen_app_cursor(const Screen *s);
bool    screen_app_keypad(const Screen *s);
