/* Visual + temporal effects applied during recording playback.
   Settings live in a RecEffects struct attached to the save modal;
   rec_render_native consults them per frame.

   Implementation strategy:
     - Visual effects (CRT / phosphor / bloom / VHS / glitch / pixelate /
       grain / halftone) all share one fragment shader. Each is gated
       by a uniform; off → contributes nothing to the output.
     - The shader runs as a single fullscreen-quad pass that reads the
       freshly-rendered terminal frame and writes to a second
       RenderTexture. When every visual effect is off the pass is
       skipped (rec_effects_any_visual returns false) and the existing
       readback path is used unchanged.
     - Speed is purely temporal: rec_render_native multiplies its
       frame->event-time mapping by `speed`, so an 8s recording at
       speed=2 becomes 4s of output frames. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    PHOSPHOR_NONE = 0,
    PHOSPHOR_GREEN,
    PHOSPHOR_AMBER,
    PHOSPHOR_BLUE,
    PHOSPHOR_COUNT
} PhosphorMode;

typedef struct {
    /* Visual (shader-pass) effects. Each `float` is a 0..1 intensity
       knob; 0 means "off" so the shader can short-circuit. */
    float        crt;        /* scanlines + curvature + RGB mask + vignette + glow */
    PhosphorMode phosphor;   /* monochrome tint: green / amber / blue / none */
    float        bloom;      /* bright cells bleed into neighbours */
    float        vhs;        /* chromatic aberration + jitter + tape lines */
    float        glitch;     /* RGB shift + horizontal slice scramble */
    float        grain;      /* film grain + warm sepia tint */
    float        halftone;   /* dot-mask render */
    /* Temporal feedback. The shader blends `decay` of the previous
       output frame back over the current frame, producing a
       phosphor-trail / slow-fade look (HAL pod displays, Nostromo
       MU/TH/UR). Requires a per-target previous-frame texture; the
       caller passes it through rec_effects_apply_with_prev. */
    float        decay;

    /* Temporal effect (no shader). 1.0 = real time; 2.0 = play 2× faster. */
    float speed;
} RecEffects;

/* ---------- Curated movie / hardware presets ----------------------------
   Each preset is a hand-tuned RecEffects designed to evoke a specific
   piece of cinema or computing history. The Settings → Effects tab and
   the SSH form's Effects tab show them as a row of buttons; clicking
   one copies its values straight into the live effect set. The "Off"
   preset is just rec_effects_defaults under another name so a user
   can wipe a complicated tuning with one click. */

typedef enum {
    /* Movie / TV — high-recognition cinema looks. */
    EFX_PRESET_OFF = 0,
    EFX_PRESET_ALIEN_NOSTROMO,    /* Alien (1979) MU/TH/UR + Nostromo CRTs */
    EFX_PRESET_HAL_9000,          /* 2001 (1968) pod displays */
    EFX_PRESET_VT100,             /* DEC VT100 monochrome */
    EFX_PRESET_AMBER_CRT,         /* generic IBM-amber sibling */
    EFX_PRESET_VHS_TAPE,          /* consumer VHS tape */
    EFX_PRESET_TRON,              /* Tron (1982) — neon glow + trails */
    EFX_PRESET_MATRIX,            /* The Matrix operator — green decay */
    EFX_PRESET_WARGAMES,          /* WarGames (1983) WOPR — stark green CRT */
    EFX_PRESET_ROBOCOP,           /* RoboCop (1987) OCP HUD — amber + halftone */
    EFX_PRESET_BLACK_MIRROR,      /* Black Mirror title — light glitch + CRT */
    EFX_PRESET_DUNE,              /* Dune (Villeneuve) Atreides display panels */
    /* Hardware nostalgia — period-correct device looks. */
    EFX_PRESET_GAMEBOY,           /* Game Boy DMG — green dot-matrix LCD */
    EFX_PRESET_C64,               /* Commodore 64 monitor — blue CRT */
    EFX_PRESET_APOLLO_DSKY,       /* Apollo Guidance Computer DSKY readout */
    EFX_PRESET_NEWSROOM,          /* Cold-War newsroom CRT */
    /* Aesthetic vibes — no specific reference. */
    EFX_PRESET_CYBERPUNK,         /* heavy bloom + decay + slight glitch */
    EFX_PRESET_SURVEILLANCE,      /* lo-fi B&W security feed */
    EFX_PRESET_NEWSPRINT,         /* halftone print / risograph */
    EFX_PRESET_STATIC,            /* TV losing signal */
    EFX_PRESET_COUNT
} EfxPreset;

const char *rec_effects_preset_label(EfxPreset p);

/* Fill `e` with the named preset's values. No-op if `e` is NULL or
   `p` is out of range. */
void rec_effects_apply_preset(RecEffects *e, EfxPreset p);

/* Reset every field to its "off / neutral" value. */
void rec_effects_defaults(RecEffects *e);

/* True if any of the eight visual effects is enabled. Used to decide
   whether to allocate the second RenderTexture + run the shader pass. */
bool rec_effects_any_visual(const RecEffects *e);

/* Lazy-load the shared fragment shader (compiled on first use). Subsequent
   calls return the cached instance. Returns true iff the shader is
   ready; false on compilation failure (in which case the visual pass
   is silently skipped). */
bool rec_effects_shader_ready(void);

/* Set every effect uniform on the shared shader. Caller must already
   be inside a BeginShaderMode block. `time_s` advances per frame so
   animated effects (VHS / glitch / grain) move. No-op (returns false)
   if the shader isn't ready. The live render path uses this to bind
   uniforms before drawing the pane's RT to the screen.

   `prev_texture` is an optional Texture2D* — when non-NULL it is bound
   as the shader's previous-frame sampler so the `decay` blend can
   read from it. NULL forces decay to 0 (no trail). */
bool rec_effects_set_uniforms_for(const RecEffects *e,
                                  int width, int height, double time_s,
                                  void *prev_texture);

/* Begin / End a shader-mode block bound to the cached effects shader.
   Use these around a DrawTexture* call when you want to apply the
   effects to a texture you already have. begin returns false (and
   draws nothing) if the shader isn't ready; in that case end is a
   no-op too, so the caller can pair them blindly. */
bool rec_effects_begin_shader_mode(void);
void rec_effects_end_shader_mode(void);

/* Run one fullscreen-quad effects pass.
     - `src_texture` is a Texture2D* (opaque) to read from (the freshly-
       rendered terminal frame).
     - `dst_rendertex` is a RenderTexture2D* (opaque) to draw into.
     - `width` / `height` match both textures.
     - `time_s` advances per frame so animated effects (VHS jitter,
       grain, glitch) move.
   No-op if rec_effects_any_visual is false. The decay term is
   forced to 0 here (no previous frame available); use
   rec_effects_apply_with_prev for phosphor trails. */
void rec_effects_apply(void *src_texture, void *dst_rendertex,
                       int width, int height,
                       const RecEffects *e, double time_s);

/* Free the cached shader. Safe to call multiple times. */
void rec_effects_shutdown(void);

/* UI helper: human-readable label for a phosphor mode. */
const char *phosphor_label(PhosphorMode m);

/* ---------- Serialization ----------------------------------------------
   Helpers shared between the config-file persistence path and the
   per-host SSH stanza parser. Both formats serialize the same set of
   keys; they differ only in the wrapper (config: `effects.<key>=<v>`,
   SSH: `# rbterm-effects-<key>: <v>`). The caller iterates over
   rec_effects_keys[] and uses rec_effects_format / rec_effects_set
   to convert to / from a string per key.

   Keys: "crt", "phosphor", "bloom", "vhs", "glitch", "pixelate",
         "grain", "halftone", "speed". NULL-terminated. */
extern const char *const rec_effects_keys[];

/* Set a single effect field from its key name + string value.
   Returns true if the key was recognised and parsed. Whitespace
   around the value is tolerated. */
bool rec_effects_set(RecEffects *e, const char *key, const char *value);

/* Format a single effect's current value into `buf` as a string.
   Returns true if the key was recognised. */
bool rec_effects_format(const RecEffects *e, const char *key,
                        char *buf, size_t cap);

/* True if every field currently equals its default (off). Useful for
   skipping serialization when nothing has been customised. */
bool rec_effects_is_default(const RecEffects *e);
