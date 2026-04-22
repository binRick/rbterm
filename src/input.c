#include "input.h"
#include "raylib.h"
#include <string.h>
#include <stdio.h>

static size_t app(uint8_t *out, size_t cap, size_t n, const char *s) {
    size_t len = strlen(s);
    if (n + len > cap) return n;
    memcpy(out + n, s, len);
    return n + len;
}

static size_t app_byte(uint8_t *out, size_t cap, size_t n, uint8_t b) {
    if (n + 1 > cap) return n;
    out[n++] = b;
    return n;
}

static size_t encode_utf8(uint32_t cp, uint8_t *out) {
    if (cp < 0x80) { out[0] = (uint8_t)cp; return 1; }
    if (cp < 0x800) { out[0] = 0xC0 | (cp >> 6); out[1] = 0x80 | (cp & 0x3F); return 2; }
    if (cp < 0x10000) { out[0] = 0xE0 | (cp >> 12); out[1] = 0x80 | ((cp >> 6) & 0x3F); out[2] = 0x80 | (cp & 0x3F); return 3; }
    out[0] = 0xF0 | (cp >> 18);
    out[1] = 0x80 | ((cp >> 12) & 0x3F);
    out[2] = 0x80 | ((cp >> 6) & 0x3F);
    out[3] = 0x80 | (cp & 0x3F);
    return 4;
}

static bool ctrl_down(void) {
    // Real Ctrl only. Cmd (KEY_LEFT_SUPER/RIGHT_SUPER) is tracked separately
    // so Ctrl+C can send SIGINT while Cmd+C copies on macOS.
    return IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
}

static bool cmd_down(void) {
#if defined(__APPLE__)
    return IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER);
#else
    return false;
#endif
}

static bool shift_down(void) {
    return IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
}

static bool alt_down(void) {
    return IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT);
}

struct SpecialKey {
    int key;
    const char *normal;
    const char *app;
};

static const struct SpecialKey special_keys[] = {
    { KEY_UP,    "\x1b[A", "\x1bOA" },
    { KEY_DOWN,  "\x1b[B", "\x1bOB" },
    { KEY_RIGHT, "\x1b[C", "\x1bOC" },
    { KEY_LEFT,  "\x1b[D", "\x1bOD" },
    { KEY_HOME,  "\x1b[H", "\x1bOH" },
    { KEY_END,   "\x1b[F", "\x1bOF" },
    { KEY_PAGE_UP,   "\x1b[5~", "\x1b[5~" },
    { KEY_PAGE_DOWN, "\x1b[6~", "\x1b[6~" },
    { KEY_INSERT,    "\x1b[2~", "\x1b[2~" },
    { KEY_DELETE,    "\x1b[3~", "\x1b[3~" },
    { KEY_F1,  "\x1bOP", "\x1bOP" },
    { KEY_F2,  "\x1bOQ", "\x1bOQ" },
    { KEY_F3,  "\x1bOR", "\x1bOR" },
    { KEY_F4,  "\x1bOS", "\x1bOS" },
    { KEY_F5,  "\x1b[15~", "\x1b[15~" },
    { KEY_F6,  "\x1b[17~", "\x1b[17~" },
    { KEY_F7,  "\x1b[18~", "\x1b[18~" },
    { KEY_F8,  "\x1b[19~", "\x1b[19~" },
    { KEY_F9,  "\x1b[20~", "\x1b[20~" },
    { KEY_F10, "\x1b[21~", "\x1b[21~" },
    { KEY_F11, "\x1b[23~", "\x1b[23~" },
    { KEY_F12, "\x1b[24~", "\x1b[24~" },
};

size_t input_poll(Screen *s, uint8_t *out, size_t cap, InputActions *actions) {
    actions->font_delta = 100;
    actions->copy = false;
    actions->paste = false;
    actions->select_all = false;
    actions->scroll_rows = 0;

    size_t n = 0;
    bool ctrl  = ctrl_down();
    bool cmd   = cmd_down();
    bool shift = shift_down();
    bool alt   = alt_down();

    // On macOS, app-level chords live on Cmd. Elsewhere, they live on Ctrl
    // (with Shift for copy/paste to leave bare Ctrl+C free for SIGINT).
#if defined(__APPLE__)
    bool ui = cmd;
    bool copy_chord  = cmd && IsKeyPressed(KEY_C);
    bool paste_chord = cmd && IsKeyPressed(KEY_V);
#else
    bool ui = ctrl;
    bool copy_chord  = ctrl && shift && IsKeyPressed(KEY_C);
    bool paste_chord = ctrl && shift && IsKeyPressed(KEY_V);
#endif

    // Mouse wheel scrolls viewport into scrollback (positive = wheel up = history up)
    float wheel = GetMouseWheelMove();
    if (wheel != 0.0f) {
        actions->scroll_rows = (int)(wheel * 3.0f);
    }

    if (ui) {
        if (IsKeyPressed(KEY_EQUAL) || IsKeyPressed(KEY_KP_ADD))     { actions->font_delta = +1; return n; }
        if (IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_KP_SUBTRACT)){ actions->font_delta = -1; return n; }
        if (IsKeyPressed(KEY_ZERO)  || IsKeyPressed(KEY_KP_0))       { actions->font_delta =  0; return n; }
    }
    if (copy_chord)  { actions->copy  = true; return n; }
    if (paste_chord) { actions->paste = true; return n; }

#if defined(__APPLE__)
    bool select_all_chord = cmd && IsKeyPressed(KEY_A);
#else
    bool select_all_chord = ctrl && shift && IsKeyPressed(KEY_A);
#endif
    if (select_all_chord) { actions->select_all = true; return n; }

    // History scroll chords (bare Ctrl, not Cmd, on all platforms).
    if (ctrl && shift && IsKeyPressed(KEY_UP))   { actions->scroll_rows = +1; return n; }
    if (ctrl && shift && IsKeyPressed(KEY_DOWN)) { actions->scroll_rows = -1; return n; }
    if (shift) {
        if (IsKeyPressed(KEY_PAGE_UP))   { actions->scroll_rows = +screen_rows(s) - 1; return n; }
        if (IsKeyPressed(KEY_PAGE_DOWN)) { actions->scroll_rows = -screen_rows(s) + 1; return n; }
    }

    // Ctrl+letter => C0 control byte. Handled via IsKeyPressed because
    // macOS/GLFW doesn't deliver a character event when Ctrl is held, so
    // GetCharPressed() never fires for Ctrl+A..Ctrl+Z.
    if (ctrl && !alt) {
        for (int k = KEY_A; k <= KEY_Z; k++) {
            if (IsKeyPressed(k) || IsKeyPressedRepeat(k)) {
                n = app_byte(out, cap, n, (uint8_t)(k - KEY_A + 1));
            }
        }
        // Other C0 chords
        if (IsKeyPressed(KEY_SPACE))         n = app_byte(out, cap, n, 0x00); // NUL
        if (IsKeyPressed(KEY_LEFT_BRACKET))  n = app_byte(out, cap, n, 0x1B); // ESC
        if (IsKeyPressed(KEY_BACKSLASH))     n = app_byte(out, cap, n, 0x1C); // FS
        if (IsKeyPressed(KEY_RIGHT_BRACKET)) n = app_byte(out, cap, n, 0x1D); // GS
    }

    // Text input (Unicode). Alphabetic chars are suppressed when Ctrl is
    // held — the C0 byte above covers them and we don't want duplicates.
    for (;;) {
        int cp = GetCharPressed();
        if (cp == 0) break;
        if (ctrl && !alt && cp >= 32 && cp < 127) {
            char ch = (char)cp;
            if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')) continue;
        }
        uint8_t buf[4];
        size_t len = encode_utf8((uint32_t)cp, buf);
        if (alt) n = app_byte(out, cap, n, 0x1b);
        for (size_t i = 0; i < len; i++) n = app_byte(out, cap, n, buf[i]);
    }

    // Enter / return
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
        if (alt) n = app_byte(out, cap, n, 0x1b);
        n = app_byte(out, cap, n, '\r');
    }
    // Tab
    if (IsKeyPressed(KEY_TAB)) {
        if (shift) n = app(out, cap, n, "\x1b[Z");
        else { if (alt) n = app_byte(out, cap, n, 0x1b); n = app_byte(out, cap, n, '\t'); }
    }
    // Backspace — send DEL (0x7f); Ctrl+Backspace -> BS (0x08) (word-erase in some shells)
    if (IsKeyPressed(KEY_BACKSPACE)) {
        if (alt) n = app_byte(out, cap, n, 0x1b);
        n = app_byte(out, cap, n, ctrl ? 0x08 : 0x7f);
    }
    // Escape
    if (IsKeyPressed(KEY_ESCAPE)) {
        n = app_byte(out, cap, n, 0x1b);
    }

    bool app_cursor = screen_app_cursor(s);
    for (size_t i = 0; i < sizeof(special_keys)/sizeof(special_keys[0]); i++) {
        if (IsKeyPressed(special_keys[i].key)) {
            const char *seq = app_cursor ? special_keys[i].app : special_keys[i].normal;
            if (alt) n = app_byte(out, cap, n, 0x1b);
            n = app(out, cap, n, seq);
        }
    }

    // Held-down repeat for arrows and backspace (raylib supplies IsKeyPressedRepeat in 5.x)
#if defined(IsKeyPressedRepeat)
    // noop: use built-in if available
#endif

    return n;
}
