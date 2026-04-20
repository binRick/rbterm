/*
 * gen_icon — render the rbterm app icon to a PNG using raylib.
 * Run once; commit the generated PNG. Regenerate when tweaking the design.
 *   ./tools/gen_icon assets/icon.png
 */
#include "raylib.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    const char *out = argc > 1 ? argv[1] : "assets/icon.png";
    int S = 1024;

    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(S, S, "gen_icon");

    Color bg     = (Color){26, 27, 38, 255};
    Color shadow = (Color){0, 0, 0, 80};
    Color fg     = (Color){125, 207, 255, 255};   // soft blue, like tokyonight
    Color green  = (Color){158, 206, 106, 255};
    Color amber  = (Color){224, 175, 104, 255};

    RenderTexture2D rt = LoadRenderTexture(S, S);
    BeginTextureMode(rt);
    ClearBackground((Color){0, 0, 0, 0});

    // Soft drop shadow
    DrawRectangleRounded((Rectangle){16, 28, S - 32, S - 32}, 0.22f, 32, shadow);
    // Main squircle body
    DrawRectangleRounded((Rectangle){0, 0, S, S}, 0.22f, 32, bg);

    // A subtle top highlight bar (stylistic)
    Color hi = (Color){60, 62, 85, 255};
    DrawRectangleRounded((Rectangle){S*0.08f, S*0.15f, S*0.84f, S*0.04f}, 0.9f, 16, hi);

    // Three window control dots (iTerm-ish touch) along the top
    float dot_r = S * 0.025f;
    float dot_y = S * 0.11f;
    DrawCircle((int)(S*0.16f), (int)dot_y, dot_r, (Color){231, 114, 112, 255}); // red
    DrawCircle((int)(S*0.22f), (int)dot_y, dot_r, amber);
    DrawCircle((int)(S*0.28f), (int)dot_y, dot_r, green);

    // Big ">_" prompt glyph using a real monospace font for clean edges.
    const char *font_candidates[] = {
        "/System/Library/Fonts/SFNSMono.ttf",
        "/System/Library/Fonts/Monaco.ttf",
        "/System/Library/Fonts/Menlo.ttc",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        NULL
    };
    Font font = {0};
    float fs = S * 0.55f;
    int cps[] = { '>', '_' };
    for (int i = 0; font_candidates[i]; i++) {
        if (FileExists(font_candidates[i])) {
            font = LoadFontEx(font_candidates[i], (int)fs, cps, 2);
            if (font.texture.id) break;
        }
    }
    if (!font.texture.id) font = GetFontDefault();
    SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR);

    const char *text = ">_";
    float spacing = fs / 20.0f;
    Vector2 sz = MeasureTextEx(font, text, fs, spacing);
    Vector2 pos = { (S - sz.x) / 2.0f, (S - sz.y) / 2.0f + S * 0.02f };
    DrawTextEx(font, text, pos, fs, spacing, fg);

    EndTextureMode();

    Image img = LoadImageFromTexture(rt.texture);
    ImageFlipVertical(&img);  // RenderTexture is y-flipped
    if (!ExportImage(img, out)) {
        fprintf(stderr, "gen_icon: failed to write %s\n", out);
        return 1;
    }
    printf("gen_icon: wrote %s (%dx%d)\n", out, img.width, img.height);

    UnloadImage(img);
    UnloadRenderTexture(rt);
    CloseWindow();
    return 0;
}
