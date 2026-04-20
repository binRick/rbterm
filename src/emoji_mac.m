/* Glyph rasterizer for macOS using Core Text + Core Graphics.
 * Produces RGBA (top-down, premultiplied alpha) suitable for uploading
 * to a raylib Texture2D with PIXELFORMAT_UNCOMPRESSED_R8G8B8A8.
 *
 * Colour emoji fonts (SBIX) ignore the fill colour and draw their bitmap
 * directly, so callers can tint with WHITE. Vector fonts fill in white,
 * so callers tint with the desired foreground color. */
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

    CFStringRef name = CFStringCreateWithCString(NULL, font_name,
                                                 kCFStringEncodingUTF8);
    if (!name) return false;
    CTFontRef font = CTFontCreateWithName(name, (CGFloat)pixel_size, NULL);
    CFRelease(name);
    if (!font) return false;

    /* Convert codepoint -> UTF-16 code units */
    UniChar chars[2];
    CGGlyph glyphs[2] = {0, 0};
    CFIndex nchars = 0;
    if (codepoint <= 0xFFFF) {
        chars[0] = (UniChar)codepoint;
        nchars = 1;
    } else {
        uint32_t c = codepoint - 0x10000;
        chars[0] = (UniChar)(0xD800 | (c >> 10));
        chars[1] = (UniChar)(0xDC00 | (c & 0x3FF));
        nchars = 2;
    }
    if (!CTFontGetGlyphsForCharacters(font, chars, glyphs, nchars) || glyphs[0] == 0) {
        CFRelease(font);
        return false;
    }
    CGGlyph g = glyphs[0];

    int size = pixel_size;
    size_t bytes = (size_t)size * size * 4;
    uint8_t *data = (uint8_t *)calloc(bytes, 1);
    if (!data) { CFRelease(font); return false; }

    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(
        data, size, size, 8, (size_t)size * 4, cs,
        kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(cs);
    if (!ctx) { free(data); CFRelease(font); return false; }

    CGContextSetShouldAntialias(ctx, true);
    CGContextSetShouldSmoothFonts(ctx, true);
    /* White fill so vector glyphs render as alpha masks we can tint later.
     * SBIX colour bitmaps ignore this. */
    CGContextSetRGBFillColor(ctx, 1.0, 1.0, 1.0, 1.0);

    /* Flip so (0,0) is top-left to match raylib's texture orientation. */
    CGContextTranslateCTM(ctx, 0, size);
    CGContextScaleCTM(ctx, 1.0, -1.0);
    CGContextSetTextMatrix(ctx, CGAffineTransformMakeScale(1.0, -1.0));

    CGRect bbox;
    CTFontGetBoundingRectsForGlyphs(font, kCTFontOrientationHorizontal,
                                    &g, &bbox, 1);
    CGFloat dx = ((CGFloat)size - bbox.size.width)  / 2.0 - bbox.origin.x;
    CGFloat dy = ((CGFloat)size - bbox.size.height) / 2.0 + bbox.size.height + bbox.origin.y;
    CGPoint pos = CGPointMake(dx, dy);
    CTFontDrawGlyphs(font, &g, &pos, 1, ctx);

    CGContextRelease(ctx);
    CFRelease(font);

    *out_rgba = data;
    *out_w = size;
    *out_h = size;
    return true;
}
