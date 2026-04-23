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
    ATTR_FG_INDEX  = 1u << 9,   /* fg holds a palette index (0..255), not RGB */
    ATTR_BG_INDEX  = 1u << 10,  /* bg holds a palette index (0..255), not RGB */
    ATTR_DEFAULT_FG = 1u << 14,
    ATTR_DEFAULT_BG = 1u << 15,
};

typedef struct Screen Screen;

/* Palette index → RGB. Each Screen owns its own palette (mutable via
   OSC 4), so changes in one tab/pane don't bleed into others. Safe for
   any i (clamped). */
uint32_t screen_palette(const Screen *s, int i);

/* Per-screen default colours. OSC 10/11/12 mutate these, so a theme
   applied in one pane doesn't bleed into the others. */
uint32_t screen_default_fg(const Screen *s);
uint32_t screen_default_bg(const Screen *s);
uint32_t screen_cursor_color(const Screen *s);

typedef struct {
    void *user;
    void (*write)(void *user, const uint8_t *buf, size_t n);
    void (*set_title)(void *user, const char *title);
    void (*bell)(void *user);
    void (*set_clipboard)(void *user, const char *utf8);   /* OSC 52 */
    void (*set_cwd)(void *user, const char *path);         /* OSC 7 */
} ScreenIO;

Screen *screen_new(int cols, int rows, int scrollback, ScreenIO io);
void    screen_free(Screen *s);
/* Retarget the screen's IO callbacks to a different `user` pointer.
   Used when a Pane moves in memory (e.g. after a pane-0 close in a
   two-pane tab slides pane-1 into slot 0). */
void    screen_set_io_user(Screen *s, void *user);

/* Theme application lives in theme.c — exported here so screen.c
   internals stay private. */
void    screen_set_default_fg(Screen *s, uint32_t rgb);
void    screen_set_default_bg(Screen *s, uint32_t rgb);
void    screen_set_cursor_color(Screen *s, uint32_t rgb);
void    screen_set_palette_entry(Screen *s, int i, uint32_t rgb);

/* Cursor presentation. Per-Screen so a host's preferred shape can be
   applied without affecting other tabs/panes. CURSOR_STYLE_DEFAULT
   means "use the renderer's default" (a solid block). */
typedef enum {
    CURSOR_STYLE_DEFAULT = 0,
    CURSOR_STYLE_BLOCK,
    CURSOR_STYLE_UNDERLINE,
    CURSOR_STYLE_BAR,
    CURSOR_STYLE_BLOCK_BLINK
} CursorStyle;
void        screen_set_cursor_style(Screen *s, CursorStyle st);
CursorStyle screen_cursor_style(const Screen *s);
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
bool    screen_focus_report(const Screen *s);
bool    screen_bracketed_paste(const Screen *s);
int     screen_mouse_mode(const Screen *s);   /* 0, 1000, 1002, 1003 */
bool    screen_mouse_sgr(const Screen *s);    /* true when DECSET 1006 active */

/* --- Graphics (sixel / kitty) -------------------------------------------
 *
 * Images are decoded at the parser and attached to the Screen with an
 * anchor row+col (in viewport row coordinates). When the viewport
 * scrolls, anchors move with the content; images that fall above the
 * live grid (anchor_row + img_rows <= 0) are dropped. The Renderer
 * reads the list each frame and uploads RGBA buffers to textures
 * lazily (generation-number cache). */
typedef struct ScreenImage ScreenImage;
int                   screen_image_count(const Screen *s);
const ScreenImage    *screen_image_at(const Screen *s, int i);
const unsigned char  *screen_image_rgba(const ScreenImage *img);
int                   screen_image_px_w(const ScreenImage *img);
int                   screen_image_px_h(const ScreenImage *img);
int                   screen_image_anchor_row(const ScreenImage *img);
int                   screen_image_anchor_col(const ScreenImage *img);
uint64_t              screen_image_generation(const ScreenImage *img);
/* Renderer tells the screen its cell pixel height so scroll accounting
   can translate pixel-heighted images into cell rows. Call from the
   renderer whenever cell_h changes (font size change, init). */
void                  screen_set_cell_h_px(Screen *s, int cell_h_px);
