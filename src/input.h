#pragma once
#include "screen.h"
#include <stdint.h>
#include <stddef.h>

/* Translate current frame's raylib input into bytes to send to the PTY.
   Writes up to cap bytes into out. Returns number of bytes written.
   `pty_font_cmd` is set to a font delta: -1 shrink, +1 grow, 0 reset, INT_MIN none. */

typedef struct {
    int font_delta;    // -1,0,+1 ; 100 = unset
    bool copy;         // Ctrl+Shift+C
    bool paste;        // Ctrl+Shift+V
    int scroll_rows;   // viewport scroll (positive = up into history)
} InputActions;

size_t input_poll(Screen *s, uint8_t *out, size_t cap, InputActions *actions);
