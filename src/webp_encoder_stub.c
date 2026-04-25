/* WebP encoder stub for builds where libwebp + libwebpmux aren't
   available — currently the Emscripten/WebAssembly build, where the
   recording feature itself is a no-op anyway (no PTY → no recording
   → no save modal). The desktop build uses webp_encoder.c with a
   real libwebp link. */
#include "webp_encoder.h"

#include <stddef.h>

WebpEnc *webp_begin(const char *path, int width, int height, int delay_ms) {
    (void)path; (void)width; (void)height; (void)delay_ms;
    return NULL;
}

bool webp_add_frame(WebpEnc *e, const uint8_t *rgba) {
    (void)e; (void)rgba;
    return false;
}

bool webp_end(WebpEnc *e) {
    (void)e;
    return false;
}
