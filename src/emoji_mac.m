/* Glyph rasterizer for macOS using Core Text + Core Graphics.
 * Produces RGBA (top-down, premultiplied alpha) suitable for uploading
 * to a raylib Texture2D with PIXELFORMAT_UNCOMPRESSED_R8G8B8A8.
 *
 * Uses CTLineDraw so Core Text's automatic font substitution picks up
 * any installed font that has the glyph when the preferred font doesn't.
 * Colour emoji fonts (SBIX) ignore the fill colour; vector fonts are
 * filled white so the caller can tint. */
#include "emoji.h"
#import <Cocoa/Cocoa.h>
#import <CoreText/CoreText.h>
#import <CoreGraphics/CoreGraphics.h>
#include <stdlib.h>
#include <string.h>

/* Strip the AppKit "Close Window" key equivalent so Cmd+W reaches our
 * keyboard polling instead of closing the only window. Walks every
 * top-level menu, blanking the keyEquivalent on items titled "Close"
 * or "Close Window". Idempotent — safe to call after raylib has set
 * up its own NSApp. */
void mac_disable_close_menu_item(void) {
    @autoreleasepool {
        NSMenu *mainMenu = [NSApp mainMenu];
        if (!mainMenu) return;
        for (NSMenuItem *topItem in [mainMenu itemArray]) {
            NSMenu *subMenu = [topItem submenu];
            if (!subMenu) continue;
            for (NSMenuItem *item in [subMenu itemArray]) {
                NSString *title = [item title];
                if ([title isEqualToString:@"Close"] ||
                    [title isEqualToString:@"Close Window"]) {
                    [item setKeyEquivalent:@""];
                    [item setKeyEquivalentModifierMask:0];
                }
            }
        }
    }
}

/* AppKit eats Ctrl+Tab in GLFW-backed windows (it's routed to the
   standard "selectNextTabViewItem:" / focus-switching machinery
   before our keyDown: override can see it). We install a local
   NSEvent monitor that inspects every NSEventTypeKeyDown, returns
   nil for Ctrl+Tab (swallowing it), and latches a flag the main
   loop polls each frame.
   Return values of mac_consume_ctrl_tab:
     0 = nothing happened
     1 = Ctrl+Tab (cycle forward)
     2 = Ctrl+Shift+Tab (cycle backward) */
static volatile int g_ctrl_tab_pending = 0;

void mac_install_ctrl_tab_monitor(void) {
    static bool installed = false;
    if (installed) return;
    installed = true;
    @autoreleasepool {
        [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                              handler:^NSEvent *(NSEvent *ev) {
            /* Tab's virtual key code is 0x30 on macOS. */
            if ([ev keyCode] != 0x30) return ev;
            NSEventModifierFlags m = [ev modifierFlags];
            if (!(m & NSEventModifierFlagControl)) return ev;
            g_ctrl_tab_pending = (m & NSEventModifierFlagShift) ? 2 : 1;
            return nil;   /* swallow — don't let AppKit / GLFW see it */
        }];
    }
}

int mac_consume_ctrl_tab(void) {
    int v = g_ctrl_tab_pending;
    g_ctrl_tab_pending = 0;
    return v;
}

/* Enter macOS native fullscreen on the first (primary) window — the
   window gets its own Space, so the user can three-finger swipe
   between it and other desktops. Contrast with raylib's
   ToggleFullscreen (exclusive fullscreen on the current Space). */
void mac_enter_native_fullscreen(void) {
    @autoreleasepool {
        NSArray *wins = [NSApp windows];
        for (NSWindow *w in wins) {
            if (![w isVisible]) continue;
            [w setCollectionBehavior:
                [w collectionBehavior] | NSWindowCollectionBehaviorFullScreenPrimary];
            if (!([w styleMask] & NSWindowStyleMaskFullScreen)) {
                [w toggleFullScreen:nil];
            }
            break;
        }
    }
}

/* Rasterise a single Unicode codepoint into an RGBA8 bitmap using
   Core Text. CTFontCreateForString picks a substitute font when the
   requested one lacks the glyph (so missing arrows / box-drawing /
   emoji still render). On success, fills *out_rgba (caller frees),
   *out_w / *out_h, and *out_colored — true means the output is a
   baked colour bitmap (SBIX) and the caller should NOT tint; false
   means the glyph is a monochrome vector and the caller should
   apply the cell's fg colour. Returns false if the rasteriser
   produced an empty (all-transparent) bitmap. */
bool glyph_render(const char *font_name, uint32_t codepoint, int pixel_size,
                  uint8_t **out_rgba, int *out_w, int *out_h,
                  bool *out_colored) {
    if (out_rgba) *out_rgba = NULL;
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    if (out_colored) *out_colored = false;
    if (pixel_size < 8) pixel_size = 8;
    if (!font_name) font_name = "Apple Color Emoji";

    /* Build a UTF-16 string for the codepoint. */
    UniChar chars[2];
    CFIndex nchars;
    if (codepoint <= 0xFFFF) {
        chars[0] = (UniChar)codepoint;
        nchars = 1;
    } else {
        uint32_t c = codepoint - 0x10000;
        chars[0] = (UniChar)(0xD800 | (c >> 10));
        chars[1] = (UniChar)(0xDC00 | (c & 0x3FF));
        nchars = 2;
    }
    CFStringRef str = CFStringCreateWithCharacters(NULL, chars, nchars);
    if (!str) return false;

    CFStringRef name = CFStringCreateWithCString(NULL, font_name,
                                                 kCFStringEncodingUTF8);
    if (!name) { CFRelease(str); return false; }
    CTFontRef primary = CTFontCreateWithName(name, (CGFloat)pixel_size, NULL);
    CFRelease(name);
    if (!primary) { CFRelease(str); return false; }

    /* Let Core Text pick the best installed font for *this* codepoint.
       Falls back automatically if `primary` lacks the glyph. */
    CFRange full = CFRangeMake(0, CFStringGetLength(str));
    CTFontRef best = CTFontCreateForString(primary, str, full);
    CFRelease(primary);
    if (!best) { CFRelease(str); return false; }

    CFMutableAttributedStringRef attr =
        CFAttributedStringCreateMutable(NULL, 0);
    CFAttributedStringReplaceString(attr, CFRangeMake(0, 0), str);
    CFAttributedStringSetAttribute(attr, full, kCTFontAttributeName, best);

    /* Fill color only affects vector glyphs (SBIX bitmaps ignore it). */
    CGColorRef white = CGColorCreateGenericRGB(1.0, 1.0, 1.0, 1.0);
    CFAttributedStringSetAttribute(attr, full,
                                   kCTForegroundColorAttributeName, white);
    CGColorRelease(white);

    CTLineRef line = CTLineCreateWithAttributedString(attr);
    if (!line) {
        CFRelease(attr);
        CFRelease(best);
        CFRelease(str);
        return false;
    }

    CGFloat ascent = 0, descent = 0, leading = 0;
    double w = CTLineGetTypographicBounds(line, &ascent, &descent, &leading);
    if (w <= 0) w = pixel_size;

    int size = pixel_size;
    size_t bytes = (size_t)size * size * 4;
    uint8_t *data = (uint8_t *)calloc(bytes, 1);
    if (!data) {
        CFRelease(line); CFRelease(attr); CFRelease(best); CFRelease(str);
        return false;
    }

    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(
        data, size, size, 8, (size_t)size * 4, cs,
        kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(cs);
    if (!ctx) {
        free(data);
        CFRelease(line); CFRelease(attr); CFRelease(best); CFRelease(str);
        return false;
    }

    CGContextSetShouldAntialias(ctx, true);
    CGContextSetShouldSmoothFonts(ctx, true);

    /* Flip so (0,0) is top-left; compensate on the text matrix so glyphs
       aren't drawn upside-down. */
    CGContextTranslateCTM(ctx, 0, size);
    CGContextScaleCTM(ctx, 1.0, -1.0);
    CGContextSetTextMatrix(ctx, CGAffineTransformMakeScale(1.0, -1.0));

    CGFloat dx = ((CGFloat)size - (CGFloat)w) / 2.0;
    CGFloat dy = ((CGFloat)size - (ascent + descent)) / 2.0 + ascent;
    CGContextSetTextPosition(ctx, dx, dy);
    CTLineDraw(line, ctx);

    CGContextRelease(ctx);
    CFRelease(line);
    CFRelease(attr);
    CFRelease(best);
    CFRelease(str);

    /* Classify the rasterized bitmap as colour vs. monochrome, and also
       detect whether *anything* got drawn. If every non-transparent pixel
       has roughly equal R/G/B, it's a vector glyph we can tint; otherwise
       it's a baked colour bitmap (e.g. SBIX emoji). */
    bool any_pixel = false;
    bool coloured = false;
    for (size_t i = 0; i < bytes; i += 4) {
        uint8_t a = data[i + 3];
        if (a < 8) continue;
        any_pixel = true;
        uint8_t r = data[i], g = data[i + 1], b = data[i + 2];
        int d1 = (int)r - (int)g; if (d1 < 0) d1 = -d1;
        int d2 = (int)r - (int)b; if (d2 < 0) d2 = -d2;
        int d3 = (int)g - (int)b; if (d3 < 0) d3 = -d3;
        if (d1 > 10 || d2 > 10 || d3 > 10) { coloured = true; break; }
    }
    if (!any_pixel) {
        free(data);
        return false;
    }

    *out_rgba = data;
    *out_w = size;
    *out_h = size;
    if (out_colored) *out_colored = coloured;
    return true;
}

/* Open a modal NSSavePanel and return the chosen save path in
   `out`. `suggested` pre-fills the filename field. Returns true on
   confirm, false on cancel or overflow. Used by the SFTP download
   modal to pick where to put the downloaded file. */
bool mac_pick_save_file(const char *suggested, char *out, size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = 0;
    __block bool ok = false;
    @autoreleasepool {
        NSSavePanel *panel = [NSSavePanel savePanel];
        if (suggested && *suggested) {
            [panel setNameFieldStringValue:[NSString stringWithUTF8String:suggested]];
        }
        [panel setTitle:@"Save downloaded file"];
        [panel setPrompt:@"Save"];
        if ([panel runModal] == NSModalResponseOK) {
            const char *p = [[[panel URL] path] UTF8String];
            if (p && strlen(p) + 1 <= cap) {
                strncpy(out, p, cap - 1);
                out[cap - 1] = 0;
                ok = true;
            }
        }
    }
    return ok;
}

/* Open a modal NSOpenPanel and return the chosen file's POSIX path
   in `out` (NUL-terminated). Returns true if the user picked a file,
   false on cancel or buffer overflow. Single-file selection only. */
bool mac_pick_open_file(char *out, size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = 0;
    __block bool ok = false;
    @autoreleasepool {
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        [panel setCanChooseFiles:YES];
        [panel setCanChooseDirectories:NO];
        [panel setAllowsMultipleSelection:NO];
        [panel setResolvesAliases:YES];
        [panel setTitle:@"Choose a file to upload"];
        [panel setPrompt:@"Choose"];
        if ([panel runModal] == NSModalResponseOK) {
            NSURL *url = [[panel URLs] firstObject];
            const char *p = [[url path] UTF8String];
            if (p && strlen(p) + 1 <= cap) {
                strncpy(out, p, cap - 1);
                out[cap - 1] = 0;
                ok = true;
            }
        }
    }
    return ok;
}
