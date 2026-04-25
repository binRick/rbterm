/* Multi-window implementation. Wraps GLFW + rlgl. The pattern
   matches the validated POC in tools/multiwin_poc.c — see that file
   for the rationale on call ordering. */
#include "multiwin.h"

#include "rlgl.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Track the original raylib window so `multiwin_end` can restore
   the GL context to it without the caller threading it through. */
static GLFWwindow *s_primary = NULL;

/* OpenGL VAOs are per-context, not shared even between contexts
   that share textures + buffers. raylib's rlgl creates one default
   render batch (with one VAO) at InitWindow — that VAO is valid
   only in the primary context. Drawing into a secondary context
   with that same VAO ID produces garbage (every glyph renders as
   a solid block, every textured rect samples the wrong texel).
   Fix: each secondary window owns its own rlRenderBatch, created
   while its context is current. multiwin_begin makes that batch
   active; multiwin_end restores the default (primary's) batch. */
typedef struct WinBatch {
    GLFWwindow    *handle;
    rlRenderBatch  batch;
    bool           initialised;
} WinBatch;

#define MULTIWIN_MAX_BATCHES 8
static WinBatch s_batches[MULTIWIN_MAX_BATCHES];
static int      s_batch_count = 0;

static WinBatch *find_batch(GLFWwindow *w) {
    for (int i = 0; i < s_batch_count; i++) {
        if (s_batches[i].handle == w) return &s_batches[i];
    }
    return NULL;
}

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
    if (s_batch_count >= MULTIWIN_MAX_BATCHES) {
        fprintf(stderr, "rbterm: multiwin batch slot exhausted\n");
        return NULL;
    }
    glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    GLFWwindow *w = glfwCreateWindow(width, height,
                                     title ? title : "rbterm",
                                     NULL, primary);
    if (!w) {
        fprintf(stderr, "rbterm: glfwCreateWindow failed (multi-window)\n");
        return NULL;
    }

    /* glfwCreateWindow leaves `w` current. Build a per-context
       rlRenderBatch HERE while we're current on `w` — its VAO
       belongs to this context. rlLoadRenderBatch's signature:
         (numBuffers, bufferElements). The same defaults raylib
         uses internally are RL_DEFAULT_BATCH_BUFFERS=1 and
         RL_DEFAULT_BATCH_BUFFER_ELEMENTS=8192. */
    WinBatch *wb = &s_batches[s_batch_count];
    wb->handle = w;
    wb->batch  = rlLoadRenderBatch(1, 8192);
    wb->initialised = true;
    s_batch_count++;

    /* Restore the primary context so raylib's per-frame
       BeginDrawing keeps drawing into it. */
    glfwMakeContextCurrent(primary);
    return w;
}

void multiwin_destroy(GLFWwindow *w) {
    if (!w) return;
    /* Free the per-window render batch first — its VAO/VBOs are
       owned by the window's GL context so we must do this while
       the context is still alive. */
    WinBatch *wb = find_batch(w);
    if (wb && wb->initialised) {
        glfwMakeContextCurrent(w);
        rlUnloadRenderBatch(wb->batch);
        wb->initialised = false;
        glfwMakeContextCurrent(multiwin_primary());
        /* Compact the slot so find_batch keeps working. */
        int idx = (int)(wb - s_batches);
        for (int i = idx; i + 1 < s_batch_count; i++) {
            s_batches[i] = s_batches[i + 1];
        }
        memset(&s_batches[s_batch_count - 1], 0, sizeof(WinBatch));
        s_batch_count--;
    }
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

void multiwin_get_winsize(GLFWwindow *w, int *w_out, int *h_out) {
    int sw = 0, sh = 0;
    if (w) glfwGetWindowSize(w, &sw, &sh);
    if (w_out) *w_out = sw;
    if (h_out) *h_out = sh;
}

/* Begin a render pass to a secondary window. Order matters:
     1. Flush the primary's pending rlgl batch with the *primary's*
        VAO still bound (we haven't switched contexts yet).
     2. Make the secondary GL context current.
     3. Set the secondary's per-context rlRenderBatch active so
        subsequent draw calls bind THIS context's VAO.
     4. Set viewport (framebuffer pixels) and ortho (window points)
        so app-level draw coords match raylib's screen-point
        convention — otherwise Retina displays render at half size. */
void multiwin_begin(GLFWwindow *w) {
    if (!w) return;
    rlDrawRenderBatchActive();
    glfwMakeContextCurrent(w);
    WinBatch *wb = find_batch(w);
    if (wb && wb->initialised) {
        rlSetRenderBatchActive(&wb->batch);
    }
    int fw = 0, fh = 0;
    glfwGetFramebufferSize(w, &fw, &fh);
    int sw = 0, sh = 0;
    glfwGetWindowSize(w, &sw, &sh);
    if (sw <= 0) sw = fw;
    if (sh <= 0) sh = fh;
    rlViewport(0, 0, fw, fh);
    rlMatrixMode(RL_PROJECTION);
    rlLoadIdentity();
    rlOrtho(0, sw, sh, 0, 0.0, 1.0);
    rlMatrixMode(RL_MODELVIEW);
    rlLoadIdentity();
}

/* Finish a secondary-window pass. Flush the secondary's rlgl batch
   (using its own VAO), swap, restore the primary GL context AND
   the default (primary-owned) rlRenderBatch so the next BeginDrawing
   uses the right VAO. */
void multiwin_end(GLFWwindow *w) {
    if (!w) return;
    rlDrawRenderBatchActive();
    glfwSwapBuffers(w);
    glfwMakeContextCurrent(multiwin_primary());
    /* NULL = restore raylib's default internal batch. */
    rlSetRenderBatchActive(NULL);
}

/* Read whatever GLFW callbacks raylib installed on the primary at
   InitWindow time, then re-register them on `w`. The trick: the
   GLFW setter/getter API returns the *previous* callback when you
   set a new one, so we set NULL temporarily to harvest the
   pointer, then immediately restore it on the primary, and set the
   same function on `w`.

   Why this works: raylib's callbacks update raylib's CORE input
   state with whatever event came in, regardless of which
   GLFWwindow* fired it. With the same callbacks on every window
   we own, IsKeyPressed / GetCharPressed / GetMousePosition / etc.
   see input from the OS-focused window automatically. */
void multiwin_install_input_callbacks(GLFWwindow *w) {
    if (!w) return;
    GLFWwindow *primary = multiwin_primary();
    if (!primary || w == primary) return;

#define MIRROR(SETTER, TYPE) do {                                \
        TYPE cb = SETTER(primary, NULL);                         \
        SETTER(primary, cb);                                     \
        SETTER(w, cb);                                           \
    } while (0)

    MIRROR(glfwSetKeyCallback,         GLFWkeyfun);
    MIRROR(glfwSetCharCallback,        GLFWcharfun);
    MIRROR(glfwSetMouseButtonCallback, GLFWmousebuttonfun);
    MIRROR(glfwSetCursorPosCallback,   GLFWcursorposfun);
    MIRROR(glfwSetCursorEnterCallback, GLFWcursorenterfun);
    MIRROR(glfwSetScrollCallback,      GLFWscrollfun);
    MIRROR(glfwSetDropCallback,        GLFWdropfun);
    MIRROR(glfwSetWindowSizeCallback,  GLFWwindowsizefun);
    MIRROR(glfwSetWindowMaximizeCallback, GLFWwindowmaximizefun);
    MIRROR(glfwSetWindowIconifyCallback,  GLFWwindowiconifyfun);
    MIRROR(glfwSetFramebufferSizeCallback, GLFWframebuffersizefun);
    MIRROR(glfwSetWindowFocusCallback, GLFWwindowfocusfun);

#undef MIRROR
}
