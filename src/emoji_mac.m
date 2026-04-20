/* Glyph rasterizer for macOS using Core Text + Core Graphics.
 * Produces RGBA (top-down, premultiplied alpha) suitable for uploading
 * to a raylib Texture2D with PIXELFORMAT_UNCOMPRESSED_R8G8B8A8.
 *
 * Uses CTLineDraw so Core Text's automatic font substitution picks up
 * any installed font that has the glyph when the preferred font doesn't.
 * Colour emoji fonts (SBIX) ignore the fill colour; vector fonts are
 * filled white so the caller can tint. */
#include "emoji.h"
#import <CoreText/CoreText.h>
#import <CoreGraphics/CoreGraphics.h>
#include <stdlib.h>
#include <string.h>

bool glyph_render(const char *font_name, uint32_t codepoint, int pixel_size,
                  uint8_t **out_rgba, int *out_w, int *out_h) {
    if (out_rgba) *out_rgba = NULL;
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
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

    *out_rgba = data;
    *out_w = size;
    *out_h = size;
    return true;
}
