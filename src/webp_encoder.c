/* Animated WebP encoder built on libwebp + libwebpmux. The
   WebPAnimEncoder API takes one decoded WebPPicture per frame
   along with a timestamp in ms (cumulative from start), then
   assembles the muxed container at end. We hide that behind a
   streaming open/add/end so the caller's loop matches the gif
   encoder's. Compiled out on platforms where libwebp isn't
   linked — see Makefile guard. */
#include "webp_encoder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <webp/encode.h>
#include <webp/mux.h>

struct WebpEnc {
    WebPAnimEncoder *enc;
    WebPConfig       cfg;
    char            *path;       /* heap-owned destination path */
    int              width, height;
    int              delay_ms;
    int              t_ms;       /* cumulative timestamp of the next frame */
    bool             failed;
};

WebpEnc *webp_begin(const char *path, int width, int height, int delay_ms) {
    if (!path || width <= 0 || height <= 0 || delay_ms <= 0) return NULL;
    WebpEnc *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    WebPAnimEncoderOptions opts;
    if (!WebPAnimEncoderOptionsInit(&opts)) { free(e); return NULL; }
    /* Default options work fine for screen recordings; libwebp picks
       sensible per-frame disposal + blending. We *do* want explicit
       no-loop (kbgcolor stays 0 → transparent, anim_params set
       below). */
    e->enc = WebPAnimEncoderNew(width, height, &opts);
    if (!e->enc) { free(e); return NULL; }
    if (!WebPConfigInit(&e->cfg)) { WebPAnimEncoderDelete(e->enc); free(e); return NULL; }
    /* Lossy at quality 75 keeps file size sane for terminal
       recordings (lots of large flat areas — lossless balloons fast).
       Method 4 is the libwebp default; faster than 6, slightly
       worse ratio. */
    e->cfg.lossless = 0;
    e->cfg.quality  = 75.0f;
    e->cfg.method   = 4;
    if (!WebPValidateConfig(&e->cfg)) {
        WebPAnimEncoderDelete(e->enc);
        free(e);
        return NULL;
    }
    e->path = strdup(path);
    if (!e->path) {
        WebPAnimEncoderDelete(e->enc);
        free(e);
        return NULL;
    }
    e->width    = width;
    e->height   = height;
    e->delay_ms = delay_ms;
    e->t_ms     = 0;
    e->failed   = false;
    return e;
}

bool webp_add_frame(WebpEnc *e, const uint8_t *rgba) {
    if (!e || e->failed || !rgba) return false;
    WebPPicture pic;
    if (!WebPPictureInit(&pic)) { e->failed = true; return false; }
    pic.width    = e->width;
    pic.height   = e->height;
    pic.use_argb = 1;   /* required by the animator */
    if (!WebPPictureImportRGBA(&pic, rgba, e->width * 4)) {
        WebPPictureFree(&pic);
        e->failed = true;
        return false;
    }
    if (!WebPAnimEncoderAdd(e->enc, &pic, e->t_ms, &e->cfg)) {
        WebPPictureFree(&pic);
        e->failed = true;
        return false;
    }
    WebPPictureFree(&pic);
    e->t_ms += e->delay_ms;
    return true;
}

bool webp_end(WebpEnc *e) {
    if (!e) return false;
    bool ok = !e->failed;
    if (ok) {
        /* Final null-frame sentinel marks the end of the timeline. */
        if (!WebPAnimEncoderAdd(e->enc, NULL, e->t_ms, NULL)) ok = false;
    }
    WebPData data;
    WebPDataInit(&data);
    if (ok) {
        if (!WebPAnimEncoderAssemble(e->enc, &data)) ok = false;
    }
    if (ok) {
        /* Patch in loop-count = 1 (play once) by re-muxing through
           WebPMux. Cheaper than re-encoding; we just rewrite the
           ANIM chunk's loop_count. */
        WebPMux *mux = WebPMuxCreate(&data, 1);
        if (mux) {
            WebPMuxAnimParams params;
            if (WebPMuxGetAnimationParams(mux, &params) == WEBP_MUX_OK) {
                params.loop_count = 1;
                WebPMuxSetAnimationParams(mux, &params);
                WebPData out;
                WebPDataInit(&out);
                if (WebPMuxAssemble(mux, &out) == WEBP_MUX_OK) {
                    WebPDataClear(&data);
                    data = out;
                }
            }
            WebPMuxDelete(mux);
        }
        FILE *fp = fopen(e->path, "wb");
        if (!fp) ok = false;
        else {
            if (fwrite(data.bytes, 1, data.size, fp) != data.size) ok = false;
            if (fclose(fp) != 0) ok = false;
        }
    }
    WebPDataClear(&data);
    WebPAnimEncoderDelete(e->enc);
    free(e->path);
    free(e);
    return ok;
}
