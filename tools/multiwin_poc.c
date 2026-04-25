/* Two-window POC for rbterm's planned same-process multi-window
 * support. Validates the rlgl + GLFW context-switching pattern
 * before we rip up main.c.
 *
 * Pattern (matches what main.c will eventually do per frame):
 *   - InitWindow creates window 1, owns the GL context.
 *   - glfwCreateWindow(..., share=primary_ctx) creates window 2
 *     that shares the GL context (so glyph atlas etc. work from
 *     both contexts).
 *   - Per frame:
 *       BeginDrawing/EndDrawing renders to window 1 the usual way.
 *       For each extra window:
 *         rlDrawRenderBatchActive();           flush primary's batch
 *         glfwMakeContextCurrent(extra);       switch to its context
 *         rlViewport(...) + rlOrtho(...)       set up matrices
 *         rlClearColor + rlClearScreenBuffers  fresh background
 *         <draw with raylib primitives>
 *         rlDrawRenderBatchActive();           flush extra's batch
 *         glfwSwapBuffers(extra);              present
 *         glfwMakeContextCurrent(primary);     return
 *
 * Build:
 *   cmake --build build --target multiwin_poc
 *   ./build/multiwin_poc
 *
 * If two windows pop up showing different colours and respond to
 * close events independently, the architecture works. If you see
 * "may cause spurious casting failures" warnings or one window
 * is blank/stuttering, USE_EXTERNAL_GLFW didn't take. */

#include "raylib.h"
#include "rlgl.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <stdio.h>

int main(void) {
    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(640, 360, "rbterm-multiwin: window 1 (primary)");
    SetTargetFPS(60);

    GLFWwindow *primary = glfwGetCurrentContext();
    if (!primary) {
        fprintf(stderr, "no primary GLFW context — InitWindow didn't take\n");
        return 1;
    }

    /* Create window 2 sharing primary's GL context. */
    glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    GLFWwindow *extra = glfwCreateWindow(
        640, 360, "rbterm-multiwin: window 2 (extra, shared ctx)",
        NULL, primary);
    if (!extra) {
        fprintf(stderr, "glfwCreateWindow failed for extra window\n");
        return 1;
    }
    glfwSetWindowPos(extra, 700, 200);

    int frame = 0;
    while (!WindowShouldClose() && !glfwWindowShouldClose(extra)) {
        /* Window 1 — normal raylib pass. */
        BeginDrawing();
            ClearBackground((Color){30, 30, 50, 255});
            DrawText("rbterm multi-window POC", 20, 20, 24, RAYWHITE);
            DrawText("primary window (raylib)", 20, 56, 18, (Color){180, 200, 255, 255});
            DrawCircle(320 + (frame % 200) - 100, 200, 24, RED);
            int w = GetScreenWidth(), h = GetScreenHeight();
            char buf[64];
            snprintf(buf, sizeof(buf), "frame %d   %dx%d", frame, w, h);
            DrawText(buf, 20, h - 30, 16, GRAY);
        EndDrawing();

        /* Window 2 — manual rlgl pass after switching context. */
        rlDrawRenderBatchActive();
        glfwMakeContextCurrent(extra);
        int ew, eh;
        glfwGetFramebufferSize(extra, &ew, &eh);
        rlViewport(0, 0, ew, eh);
        rlMatrixMode(RL_PROJECTION);
        rlLoadIdentity();
        rlOrtho(0, ew, eh, 0, 0.0, 1.0);
        rlMatrixMode(RL_MODELVIEW);
        rlLoadIdentity();
        rlClearColor(50, 30, 30, 255);
        rlClearScreenBuffers();
        DrawText("extra window (rlgl direct)", 20, 56, 18,
                 (Color){255, 200, 180, 255});
        DrawRectangle(20 + (frame % 300), 120, 60, 60, GREEN);
        rlDrawRenderBatchActive();
        glfwSwapBuffers(extra);
        glfwMakeContextCurrent(primary);

        glfwPollEvents();
        frame++;
    }

    glfwDestroyWindow(extra);
    CloseWindow();
    return 0;
}
