/* Multi-window implementation. Wraps GLFW + rlgl. The pattern
   matches the validated POC in tools/multiwin_poc.c — see that file
   for the rationale on call ordering. */
#include "multiwin.h"

#include "rlgl.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <stdio.h>

/* Track the original raylib window so `multiwin_end` can restore
   the GL context to it without the caller threading it through. */
static GLFWwindow *s_primary = NULL;

void multiwin_set_primary(GLFWwindow *w) {
    s_primary = w;
}

GLFWwindow *multiwin_primary(void) {
    if (s_primary) return s_primary;
    /* First-call before set_primary: falls back to current context
       (which is raylib's window, since InitWindow leaves it active). */
    return glfwGetCurrentContext();
}

GLFWwindow *multiwin_create(int width, int height, const char *title) {
    GLFWwindow *primary = multiwin_primary();
    if (!primary) return NULL;
    glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    GLFWwindow *w = glfwCreateWindow(width, height,
                                     title ? title : "rbterm",
                                     NULL, primary);
    if (!w) {
        fprintf(stderr, "rbterm: glfwCreateWindow failed (multi-window)\n");
        return NULL;
    }
    /* glfwCreateWindow leaves `w` current; restore the primary so
       raylib's per-frame BeginDrawing keeps drawing into it. */
    glfwMakeContextCurrent(primary);
    return w;
}

void multiwin_destroy(GLFWwindow *w) {
    if (!w) return;
    glfwDestroyWindow(w);
}

void multiwin_poll_events(void) {
    glfwPollEvents();
}

bool multiwin_should_close(GLFWwindow *w) {
    return w && glfwWindowShouldClose(w);
}

bool multiwin_is_focused(GLFWwindow *w) {
    return w && glfwGetWindowAttrib(w, GLFW_FOCUSED);
}

void multiwin_get_fbsize(GLFWwindow *w, int *w_out, int *h_out) {
    int fw = 0, fh = 0;
    if (w) glfwGetFramebufferSize(w, &fw, &fh);
    if (w_out) *w_out = fw;
    if (h_out) *h_out = fh;
}

/* Begin a render pass to a secondary window. Flushes the primary's
   pending rlgl batch, switches GL context, then sets up matrices in
   pixel space — same flow as the POC. */
void multiwin_begin(GLFWwindow *w) {
    if (!w) return;
    rlDrawRenderBatchActive();
    glfwMakeContextCurrent(w);
    int fw = 0, fh = 0;
    glfwGetFramebufferSize(w, &fw, &fh);
    rlViewport(0, 0, fw, fh);
    rlMatrixMode(RL_PROJECTION);
    rlLoadIdentity();
    rlOrtho(0, fw, fh, 0, 0.0, 1.0);
    rlMatrixMode(RL_MODELVIEW);
    rlLoadIdentity();
}

/* Finish a secondary-window pass. Flush the secondary's rlgl batch,
   swap, restore the primary GL context. */
void multiwin_end(GLFWwindow *w) {
    if (!w) return;
    rlDrawRenderBatchActive();
    glfwSwapBuffers(w);
    glfwMakeContextCurrent(multiwin_primary());
}
