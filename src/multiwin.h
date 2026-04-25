/* Multi-window glue. Wraps the GLFW + rlgl calls needed to host
   multiple raylib windows in one OS process. Hides <GLFW/glfw3.h>
   and <rlgl.h> from main.c so its include surface stays small. */
#pragma once

#include <stdbool.h>

/* Opaque handle — actually `GLFWwindow *`. main.c only stores and
   passes these; never dereferences. */
typedef struct GLFWwindow GLFWwindow;

/* Pin the primary GLFWwindow* (the one raylib's InitWindow made)
   so multiwin_end can restore the GL context to it without the
   caller threading the handle through. Call once at startup. */
void multiwin_set_primary(GLFWwindow *w);

/* Returns the primary window's GLFWwindow handle (the one raylib's
   InitWindow created). Call after InitWindow. */
GLFWwindow *multiwin_primary(void);

/* Open another GLFWwindow that shares the primary's GL context, so
   raylib draw primitives (which use a single rlgl batch) can write
   into either window via context switching. NULL on failure. */
GLFWwindow *multiwin_create(int width, int height, const char *title);

/* Close + free a window we created via multiwin_create. NULL-safe. */
void multiwin_destroy(GLFWwindow *w);

/* Pump GLFW events for all windows once per frame. Call in the
   main loop. */
void multiwin_poll_events(void);

/* True iff the user clicked the close button on this window since
   the last call to multiwin_clear_should_close. */
bool multiwin_should_close(GLFWwindow *w);

/* Returns true iff `w` is the OS-focused window (key window in
   AppKit terms). Used to decide which Window in main.c receives
   keyboard / mouse input each frame. */
bool multiwin_is_focused(GLFWwindow *w);

/* Framebuffer size in pixels (NOT screen-points; on Retina they
   differ by 2x). Mostly used to set rlViewport for the secondary
   pass. */
void multiwin_get_fbsize(GLFWwindow *w, int *w_out, int *h_out);

/* Begin / end a render pass to a non-primary window. Sandwiches
   the user's draw calls between the rlgl batch flush + context
   swap on entry and the matching swap-buffers + context-restore
   on exit. */
void multiwin_begin(GLFWwindow *w);
void multiwin_end(GLFWwindow *w);
