#pragma once
#include "screen.h"
#include <stdbool.h>

typedef struct {
    int font_size;
    int cell_w;           /* effective cell width (cell_w_base + cell_extra_w) */
    int cell_w_base;      /* natural width from the font's 'M' advance */
    int cell_extra_w;     /* user-configurable spacing added to cell_w */
    int cell_h;
    int pad_x;            /* horizontal padding in pixels around the grid */
    int pad_y;            /* vertical padding (applied above + below terminal) */
    float bg_alpha;       /* default-background opacity, 0..1 */
    char font_path[1024];
    void *font_data;      // Font* (opaque)
    /* Cache of the embedded blob the current font came from (NULL when
       it was loaded from a disk path). renderer_set_font_size can
       re-load from this without revisiting the filesystem. */
    const unsigned char *cur_data;
    int cur_data_size;
    char cur_ext[8];

    /* OpenType ligature shaping. Off by default. When enabled and the
       current font was loaded from an in-memory blob (cur_data != NULL,
       i.e. one of the bundled fonts), the renderer shapes each row
       through HarfBuzz before drawing — cells that participate in a
       ligature cluster get the rasterised ligature glyph instead of
       the per-codepoint atlas glyphs. Disk-path fonts don't shape
       (we don't keep their bytes around). The shape_font handle is
       a `ShapeFont *` (opaque to keep this header thin); rebuilt
       whenever the font / size changes. */
    bool  ligatures;
    void *shape_font;     /* ShapeFont *, owned by the renderer */
} Renderer;

typedef struct {
    bool active;    // a selection exists (highlight + available to copy)
    bool dragging;  // user is currently dragging mouse
    /* Anchor + current point in viewport coordinates, where row 0 is the top
       visible row (so scrollback history is addressable via view offset). */
    int a_col, a_row;
    int b_col, b_row;
} Selection;

bool renderer_init(Renderer *r, const char *font_path, int font_size);
bool renderer_init_with_data(Renderer *r, const unsigned char *data,
                             int data_size, const char *ext,
                             const char *display_path, int font_size);
void renderer_shutdown(Renderer *r);

// Reload font at a new size. Returns true on success.
bool renderer_set_font_size(Renderer *r, int font_size);
bool renderer_set_font_path(Renderer *r, const char *path);
// Load a font straight from a memory buffer (e.g. one of the
// .incbin-bundled assets). `ext` is "ttf" / "otf" / "ttc" so raylib
// picks the right loader. `display_path` is what we record as the
// renderer's `font_path` — typically "embedded:<NAME>" so save/load
// roundtrips, but any string that uniquely identifies the font is OK.
bool renderer_set_font_data(Renderer *r, const unsigned char *data,
                            int data_size, const char *ext,
                            const char *display_path);
// Install a broad-coverage backup font. When the primary font lacks a
// glyph for a codepoint, the renderer falls through to this font
// before declaring the cell unrenderable. Call once at startup with a
// font that covers a wide Unicode range (DejaVu Sans Mono is ideal).
// The caller owns `data`; the renderer keeps the pointer alive for the
// lifetime of the renderer.
void renderer_set_backup_font_data(const unsigned char *data,
                                   int data_size, const char *ext);
// Set extra pixels between cells horizontally (0..32). Updates cell_w in
// place; no atlas rebuild needed. The caller still owns reflowing tabs.
void renderer_set_cell_spacing(Renderer *r, int extra_w);

// Draw a screen's contents. `x_offset` / `y_offset` are the pixel offsets
// from the top-left of the window where the pane's top-left should land
// (y_offset leaves space for a tab bar; x_offset lets two panes share
// the window side-by-side).
// `hover_col` / `hover_row` identify the cell the mouse is over in
// viewport coordinates (or -1 / -1 to mean "no hover") — used to
// brighten OSC 8 hyperlink runs under the cursor.
void renderer_draw(Renderer *r, Screen *s, double time_sec, bool focused,
                   const Selection *sel, int x_offset, int y_offset,
                   int hover_col, int hover_row);

// Find a default system monospace font. Returns static buffer.
const char *renderer_find_default_font(void);

// Toggle OpenType ligature shaping. When `on` is true and HarfBuzz
// was linked at build time, the next renderer_draw will shape each
// row through HarfBuzz and substitute ligature glyphs (Fira Code's
// `=>`, JetBrains Mono's `!=`, etc.) for the matching cell pairs.
// No-op when shaping isn't compiled in.
void renderer_set_ligatures(Renderer *r, bool on);
