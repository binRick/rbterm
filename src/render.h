#pragma once
#include "screen.h"

typedef struct {
    int font_size;
    int cell_w;
    int cell_h;
    char font_path[1024];
    void *font_data; // Font* (opaque)
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
void renderer_shutdown(Renderer *r);

// Reload font at a new size. Returns true on success.
bool renderer_set_font_size(Renderer *r, int font_size);
bool renderer_set_font_path(Renderer *r, const char *path);

// Draw a screen's contents. `y_offset` is the pixel offset from the top of
// the window where row 0 should be drawn (used to leave space for a tab bar).
void renderer_draw(Renderer *r, Screen *s, double time_sec, bool focused,
                   const Selection *sel, int y_offset);

// Find a default system monospace font. Returns static buffer.
const char *renderer_find_default_font(void);
