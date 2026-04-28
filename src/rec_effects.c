#include "rec_effects.h"
#include "raylib.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- Effect-pass shader ----------------------------------------
   Single fragment shader that bundles all eight visual effects. Each
   effect is gated by a uniform; when every gate is zero the shader's
   per-effect branches short-circuit and the result is a straight blit
   of the input texture. The order matters — pixelation runs first
   (it changes uv sampling), curvature next (it can produce out-of-
   bounds black borders), chromatic aberration / bloom / halftone
   colour-shift the sampled value, scanlines + RGB-mask + vignette
   tint per-pixel, grain + sepia warm everything up, and phosphor
   collapses to monochrome at the very end so it always wins.

   Written for OpenGL 3.3 (the desktop / Apple-via-Metal target). raylib
   handles the matching default vertex shader. */
static const char *k_effect_fs =
"#version 330\n"
"in vec2 fragTexCoord;\n"
"in vec4 fragColor;\n"
"out vec4 finalColor;\n"
"\n"
"uniform sampler2D texture0;\n"
"uniform sampler2D u_prev;\n"
"uniform vec4 colDiffuse;\n"
"\n"
"uniform vec2  u_resolution;\n"
"uniform float u_time;\n"
"\n"
"uniform float u_crt;\n"
"uniform int   u_phosphor;\n"
"uniform float u_bloom;\n"
"uniform float u_vhs;\n"
"uniform float u_glitch;\n"
"uniform float u_grain;\n"
"uniform float u_halftone;\n"
"uniform float u_decay;\n"
"\n"
"float rand(vec2 co) {\n"
"    return fract(sin(dot(co.xy, vec2(12.9898, 78.233))) * 43758.5453);\n"
"}\n"
"\n"
"vec2 barrel(vec2 uv, float k) {\n"
"    vec2 c = uv - 0.5;\n"
"    float r2 = dot(c, c);\n"
"    return uv + c * r2 * k;\n"
"}\n"
"\n"
"void main() {\n"
"    vec2 uv = fragTexCoord;\n"
"\n"
"    /* VHS horizontal jitter — left/right warp per scanline. */\n"
"    if (u_vhs > 0.0) {\n"
"        float j = rand(vec2(floor(uv.y * u_resolution.y * 0.5), u_time)) - 0.5;\n"
"        uv.x += j * 0.012 * u_vhs;\n"
"    }\n"
"\n"
"    /* Glitch: occasional horizontal slice scramble. */\n"
"    if (u_glitch > 0.0) {\n"
"        float band = floor(uv.y * 12.0 + u_time * 0.7);\n"
"        if (rand(vec2(band, floor(u_time * 4.0))) < 0.08 * u_glitch) {\n"
"            uv.x = fract(uv.x + (rand(vec2(band, u_time)) - 0.5) * 0.3 * u_glitch);\n"
"        }\n"
"    }\n"
"\n"
"    /* CRT barrel curvature. Out-of-bounds samples become solid\n"
"       black so the curved corners read clean. */\n"
"    if (u_crt > 0.0) {\n"
"        uv = barrel(uv, 0.18 * u_crt);\n"
"        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {\n"
"            finalColor = vec4(0.0, 0.0, 0.0, 1.0);\n"
"            return;\n"
"        }\n"
"    }\n"
"\n"
"    /* Chromatic aberration: split R / G / B across a small offset.\n"
"       Driven by VHS + glitch + CRT — they all want some. */\n"
"    float ca = 0.003 * u_vhs + 0.002 * u_glitch + 0.0015 * u_crt;\n"
"    vec3 col;\n"
"    if (ca > 0.0) {\n"
"        col.r = texture(texture0, uv + vec2(ca, 0.0)).r;\n"
"        col.g = texture(texture0, uv).g;\n"
"        col.b = texture(texture0, uv - vec2(ca, 0.0)).b;\n"
"    } else {\n"
"        col = texture(texture0, uv).rgb;\n"
"    }\n"
"\n"
"    /* Bloom: cheap 4-tap radial sample of bright neighbours. The\n"
"       smoothstep gate means dim cells contribute ~nothing. */\n"
"    if (u_bloom > 0.0) {\n"
"        vec3 acc = vec3(0.0);\n"
"        float sp = 6.0 / max(u_resolution.x, 1.0);\n"
"        for (int i = 0; i < 4; i++) {\n"
"            float a = float(i) * 1.5708;\n"
"            vec2 d = vec2(cos(a), sin(a)) * sp;\n"
"            vec3 s = texture(texture0, uv + d).rgb;\n"
"            float lum = max(s.r, max(s.g, s.b));\n"
"            acc += s * smoothstep(0.6, 1.0, lum);\n"
"        }\n"
"        col += acc * 0.20 * u_bloom;\n"
"    }\n"
"\n"
"    /* Halftone: dot mask, area scaled by per-pixel darkness. */\n"
"    if (u_halftone > 0.0) {\n"
"        vec2 g = uv * u_resolution / 4.0;\n"
"        vec2 cell = fract(g) - 0.5;\n"
"        float lum = (col.r + col.g + col.b) / 3.0;\n"
"        float r = (1.0 - lum) * 0.5;\n"
"        float d = length(cell);\n"
"        float m = smoothstep(r + 0.05, r, d);\n"
"        col = mix(col, vec3(m), u_halftone);\n"
"    }\n"
"\n"
"    /* CRT scanlines + RGB aperture mask + vignette. */\n"
"    if (u_crt > 0.0) {\n"
"        float sl = 0.5 + 0.5 * cos(uv.y * u_resolution.y * 3.14159);\n"
"        col *= mix(1.0, 0.78 + 0.22 * sl, u_crt);\n"
"        float pix = mod(uv.x * u_resolution.x, 3.0);\n"
"        vec3 mask;\n"
"        if      (pix < 1.0) mask = vec3(1.10, 0.92, 0.92);\n"
"        else if (pix < 2.0) mask = vec3(0.92, 1.10, 0.92);\n"
"        else                mask = vec3(0.92, 0.92, 1.10);\n"
"        col *= mix(vec3(1.0), mask, u_crt * 0.6);\n"
"        vec2 vc = uv - 0.5;\n"
"        float v = 1.0 - dot(vc, vc) * 1.4;\n"
"        col *= mix(1.0, v, u_crt * 0.5);\n"
"    }\n"
"\n"
"    /* VHS tape tracking line. */\n"
"    if (u_vhs > 0.0) {\n"
"        float band = sin(uv.y * 80.0 + u_time * 6.0);\n"
"        col += vec3(0.05) * smoothstep(0.97, 1.0, band) * u_vhs;\n"
"    }\n"
"\n"
"    /* Film grain + warm sepia tint. */\n"
"    if (u_grain > 0.0) {\n"
"        float g = rand(uv + vec2(u_time)) - 0.5;\n"
"        col += vec3(g) * 0.18 * u_grain;\n"
"        vec3 sep;\n"
"        sep.r = dot(col, vec3(0.393, 0.769, 0.189));\n"
"        sep.g = dot(col, vec3(0.349, 0.686, 0.168));\n"
"        sep.b = dot(col, vec3(0.272, 0.534, 0.131));\n"
"        col = mix(col, sep, u_grain * 0.6);\n"
"    }\n"
"\n"
"    /* Phosphor monochrome — runs last so it always wins the look. */\n"
"    if (u_phosphor > 0) {\n"
"        float lum = dot(col, vec3(0.299, 0.587, 0.114));\n"
"        vec3 tint;\n"
"        if      (u_phosphor == 1) tint = vec3(0.30, 1.00, 0.40);\n"
"        else if (u_phosphor == 2) tint = vec3(1.00, 0.75, 0.30);\n"
"        else                      tint = vec3(0.50, 0.80, 1.00);\n"
"        col = lum * tint;\n"
"    }\n"
"\n"
"    /* Phosphor decay / ghost trail: max-blend in the previous output\n"
"       frame so bright pixels persist as fading trails, then darken\n"
"       slightly each frame so the trail eventually clears. uv is\n"
"       sampled un-curved so trails don't twist on a curved CRT. */\n"
"    if (u_decay > 0.0) {\n"
"        vec3 prev = texture(u_prev, fragTexCoord).rgb * u_decay;\n"
"        col = max(col, prev * 0.95);\n"
"    }\n"
"\n"
"    finalColor = vec4(clamp(col, 0.0, 1.0), 1.0);\n"
"}\n";

/* ---------- Cached shader handle -------------------------------------- */

static Shader g_shader;
static bool   g_shader_loaded = false;
static bool   g_shader_failed = false;

/* Uniform locations resolved on first load and reused per frame. */
static int    u_resolution = -1;
static int    u_time       = -1;
static int    u_crt        = -1;
static int    u_phosphor   = -1;
static int    u_bloom      = -1;
static int    u_vhs        = -1;
static int    u_glitch     = -1;
static int    u_grain      = -1;
static int    u_halftone   = -1;
static int    u_decay      = -1;
static int    u_prev_loc   = -1;

static bool load_shader_once(void) {
    if (g_shader_loaded) return true;
    if (g_shader_failed) return false;
    g_shader = LoadShaderFromMemory(NULL, k_effect_fs);
    if (g_shader.id == 0) {
        g_shader_failed = true;
        TraceLog(LOG_WARNING, "rec_effects: shader compilation failed; effects disabled");
        return false;
    }
    u_resolution = GetShaderLocation(g_shader, "u_resolution");
    u_time       = GetShaderLocation(g_shader, "u_time");
    u_crt        = GetShaderLocation(g_shader, "u_crt");
    u_phosphor   = GetShaderLocation(g_shader, "u_phosphor");
    u_bloom      = GetShaderLocation(g_shader, "u_bloom");
    u_vhs        = GetShaderLocation(g_shader, "u_vhs");
    u_glitch     = GetShaderLocation(g_shader, "u_glitch");
    u_grain      = GetShaderLocation(g_shader, "u_grain");
    u_halftone   = GetShaderLocation(g_shader, "u_halftone");
    u_decay      = GetShaderLocation(g_shader, "u_decay");
    u_prev_loc   = GetShaderLocation(g_shader, "u_prev");
    g_shader_loaded = true;
    return true;
}

/* ---------- Public API ------------------------------------------------ */

void rec_effects_defaults(RecEffects *e) {
    if (!e) return;
    memset(e, 0, sizeof(*e));
    e->phosphor = PHOSPHOR_NONE;
    e->speed    = 1.0f;
}

bool rec_effects_any_visual(const RecEffects *e) {
    if (!e) return false;
    return e->crt      > 0.001f
        || e->phosphor != PHOSPHOR_NONE
        || e->bloom    > 0.001f
        || e->vhs      > 0.001f
        || e->glitch   > 0.001f
        || e->grain    > 0.001f
        || e->halftone > 0.001f
        || e->decay    > 0.001f;
}

bool rec_effects_shader_ready(void) {
    return load_shader_once();
}

bool rec_effects_set_uniforms_for(const RecEffects *e,
                                  int width, int height, double time_s,
                                  void *prev_texture) {
    if (!e) return false;
    if (!load_shader_once()) return false;
    float res[2] = { (float)width, (float)height };
    float t      = (float)time_s;
    float crt    = e->crt;
    int   phos   = (int)e->phosphor;
    float bloom  = e->bloom;
    float vhs    = e->vhs;
    float glitch = e->glitch;
    float grain  = e->grain;
    float halft  = e->halftone;
    /* Decay is silently forced to 0 if the caller didn't hand us a
       previous-frame texture — sampling u_prev when it isn't bound to
       a real texture would read garbage. */
    float decay  = (prev_texture != NULL) ? e->decay : 0.0f;
    SetShaderValue(g_shader, u_resolution, res,    SHADER_UNIFORM_VEC2);
    SetShaderValue(g_shader, u_time,       &t,     SHADER_UNIFORM_FLOAT);
    SetShaderValue(g_shader, u_crt,        &crt,   SHADER_UNIFORM_FLOAT);
    SetShaderValue(g_shader, u_phosphor,   &phos,  SHADER_UNIFORM_INT);
    SetShaderValue(g_shader, u_bloom,      &bloom, SHADER_UNIFORM_FLOAT);
    SetShaderValue(g_shader, u_vhs,        &vhs,   SHADER_UNIFORM_FLOAT);
    SetShaderValue(g_shader, u_glitch,     &glitch,SHADER_UNIFORM_FLOAT);
    SetShaderValue(g_shader, u_grain,      &grain, SHADER_UNIFORM_FLOAT);
    SetShaderValue(g_shader, u_halftone,   &halft, SHADER_UNIFORM_FLOAT);
    SetShaderValue(g_shader, u_decay,      &decay, SHADER_UNIFORM_FLOAT);
    if (prev_texture && u_prev_loc >= 0) {
        SetShaderValueTexture(g_shader, u_prev_loc, *(Texture2D *)prev_texture);
    }
    return true;
}

bool rec_effects_begin_shader_mode(void) {
    if (!load_shader_once()) return false;
    BeginShaderMode(g_shader);
    return true;
}

void rec_effects_end_shader_mode(void) {
    if (g_shader_loaded) EndShaderMode();
}

void rec_effects_apply(void *src_texture, void *dst_rendertex,
                       int width, int height,
                       const RecEffects *e, double time_s) {
    if (!src_texture || !dst_rendertex || !e) return;
    if (!rec_effects_any_visual(e)) return;
    if (!load_shader_once())        return;

    Texture2D       *tex = (Texture2D *)src_texture;
    RenderTexture2D *rt  = (RenderTexture2D *)dst_rendertex;

    float res[2] = { (float)width, (float)height };
    float t      = (float)time_s;
    float crt    = e->crt;
    int   phos   = (int)e->phosphor;
    float bloom  = e->bloom;
    float vhs    = e->vhs;
    float glitch = e->glitch;
    float grain  = e->grain;
    float halft  = e->halftone;

    SetShaderValue(g_shader, u_resolution, res,    SHADER_UNIFORM_VEC2);
    SetShaderValue(g_shader, u_time,       &t,     SHADER_UNIFORM_FLOAT);
    SetShaderValue(g_shader, u_crt,        &crt,   SHADER_UNIFORM_FLOAT);
    SetShaderValue(g_shader, u_phosphor,   &phos,  SHADER_UNIFORM_INT);
    SetShaderValue(g_shader, u_bloom,      &bloom, SHADER_UNIFORM_FLOAT);
    SetShaderValue(g_shader, u_vhs,        &vhs,   SHADER_UNIFORM_FLOAT);
    SetShaderValue(g_shader, u_glitch,     &glitch,SHADER_UNIFORM_FLOAT);
    SetShaderValue(g_shader, u_grain,      &grain, SHADER_UNIFORM_FLOAT);
    SetShaderValue(g_shader, u_halftone,   &halft, SHADER_UNIFORM_FLOAT);
    /* No previous frame in the recording-render path → decay forced
       to 0. The recorder is single-pass-per-frame and doesn't keep a
       ping-pong buffer; live render does. */
    float decay0 = 0.0f;
    SetShaderValue(g_shader, u_decay,      &decay0, SHADER_UNIFORM_FLOAT);

    BeginTextureMode(*rt);
        ClearBackground(BLACK);
        BeginShaderMode(g_shader);
            /* Source RT textures have GL-native (bottom-up) storage.
               raylib's draw routines assume top-up textures, so we
               flip during sampling via a negative source height —
               the result lands in `rt` with the same bottom-up
               storage layout that rec_render_native's readback loop
               already inverts when copying into `pixels`. */
            DrawTexturePro(*tex,
                           (Rectangle){ 0.0f, 0.0f, (float)tex->width, -(float)tex->height },
                           (Rectangle){ 0.0f, 0.0f, (float)width,       (float)height      },
                           (Vector2){ 0.0f, 0.0f }, 0.0f, WHITE);
        EndShaderMode();
    EndTextureMode();
}

void rec_effects_shutdown(void) {
    if (g_shader_loaded) {
        UnloadShader(g_shader);
        g_shader_loaded = false;
    }
    g_shader_failed = false;
}

const char *phosphor_label(PhosphorMode m) {
    switch (m) {
    case PHOSPHOR_GREEN: return "Green";
    case PHOSPHOR_AMBER: return "Amber";
    case PHOSPHOR_BLUE:  return "Blue";
    case PHOSPHOR_NONE:
    default:             return "None";
    }
}

/* ---------- Serialization helpers ------------------------------------- */

const char *const rec_effects_keys[] = {
    "crt", "phosphor", "bloom", "vhs", "glitch",
    "grain", "halftone", "decay", "speed",
    NULL
};

static PhosphorMode phosphor_from_string(const char *v) {
    if (!v || !*v)               return PHOSPHOR_NONE;
    if (strcmp(v, "green") == 0) return PHOSPHOR_GREEN;
    if (strcmp(v, "amber") == 0) return PHOSPHOR_AMBER;
    if (strcmp(v, "blue")  == 0) return PHOSPHOR_BLUE;
    return PHOSPHOR_NONE;
}

static const char *phosphor_to_string(PhosphorMode m) {
    switch (m) {
    case PHOSPHOR_GREEN: return "green";
    case PHOSPHOR_AMBER: return "amber";
    case PHOSPHOR_BLUE:  return "blue";
    case PHOSPHOR_NONE:
    default:             return "none";
    }
}

/* Trim leading whitespace and parse as float, clamped to 0..1. */
static float clamp01_atof(const char *v) {
    while (*v == ' ' || *v == '\t') v++;
    float f = (float)atof(v);
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    return f;
}

bool rec_effects_set(RecEffects *e, const char *key, const char *value) {
    if (!e || !key || !value) return false;
    /* Skip leading whitespace on value once for every callsite. */
    while (*value == ' ' || *value == '\t') value++;
    if (strcmp(key, "crt") == 0)        { e->crt      = clamp01_atof(value); return true; }
    if (strcmp(key, "phosphor") == 0)   { e->phosphor = phosphor_from_string(value); return true; }
    if (strcmp(key, "bloom") == 0)      { e->bloom    = clamp01_atof(value); return true; }
    if (strcmp(key, "vhs") == 0)        { e->vhs      = clamp01_atof(value); return true; }
    if (strcmp(key, "glitch") == 0)     { e->glitch   = clamp01_atof(value); return true; }
    if (strcmp(key, "grain") == 0)      { e->grain    = clamp01_atof(value); return true; }
    if (strcmp(key, "halftone") == 0)   { e->halftone = clamp01_atof(value); return true; }
    if (strcmp(key, "decay") == 0)      { e->decay    = clamp01_atof(value); return true; }
    if (strcmp(key, "speed") == 0) {
        float s = (float)atof(value);
        if (s < 0.05f) s = 1.0f;
        if (s > 16.0f) s = 16.0f;
        e->speed = s;
        return true;
    }
    return false;
}

bool rec_effects_format(const RecEffects *e, const char *key,
                        char *buf, size_t cap) {
    if (!e || !key || !buf || cap == 0) return false;
    if      (strcmp(key, "crt") == 0)      { snprintf(buf, cap, "%.3f", e->crt);      return true; }
    else if (strcmp(key, "phosphor") == 0) { snprintf(buf, cap, "%s",   phosphor_to_string(e->phosphor)); return true; }
    else if (strcmp(key, "bloom") == 0)    { snprintf(buf, cap, "%.3f", e->bloom);    return true; }
    else if (strcmp(key, "vhs") == 0)      { snprintf(buf, cap, "%.3f", e->vhs);      return true; }
    else if (strcmp(key, "glitch") == 0)   { snprintf(buf, cap, "%.3f", e->glitch);   return true; }
    else if (strcmp(key, "grain") == 0)    { snprintf(buf, cap, "%.3f", e->grain);    return true; }
    else if (strcmp(key, "halftone") == 0) { snprintf(buf, cap, "%.3f", e->halftone); return true; }
    else if (strcmp(key, "decay") == 0)    { snprintf(buf, cap, "%.3f", e->decay);    return true; }
    else if (strcmp(key, "speed") == 0)    { snprintf(buf, cap, "%.3f", e->speed);    return true; }
    return false;
}

/* ---------- Curated presets -------------------------------------------- */

const char *rec_effects_preset_label(EfxPreset p) {
    switch (p) {
    case EFX_PRESET_OFF:            return "Off";
    case EFX_PRESET_ALIEN_NOSTROMO: return "Nostromo";
    case EFX_PRESET_HAL_9000:       return "HAL 9000";
    case EFX_PRESET_VT100:          return "VT100";
    case EFX_PRESET_AMBER_CRT:      return "Amber CRT";
    case EFX_PRESET_VHS_TAPE:       return "VHS";
    case EFX_PRESET_TRON:           return "Tron";
    case EFX_PRESET_MATRIX:         return "Matrix";
    case EFX_PRESET_WARGAMES:       return "WarGames";
    case EFX_PRESET_ROBOCOP:        return "RoboCop";
    case EFX_PRESET_BLACK_MIRROR:   return "Black Mirror";
    case EFX_PRESET_DUNE:           return "Dune";
    case EFX_PRESET_GAMEBOY:        return "Game Boy";
    case EFX_PRESET_C64:            return "C64";
    case EFX_PRESET_APOLLO_DSKY:    return "Apollo DSKY";
    case EFX_PRESET_NEWSROOM:       return "Newsroom";
    case EFX_PRESET_CYBERPUNK:      return "Cyberpunk";
    case EFX_PRESET_SURVEILLANCE:   return "Surveillance";
    case EFX_PRESET_NEWSPRINT:      return "Newsprint";
    case EFX_PRESET_STATIC:         return "Static";
    default:                        return "?";
    }
}

void rec_effects_apply_preset(RecEffects *e, EfxPreset p) {
    if (!e) return;
    rec_effects_defaults(e);
    switch (p) {
    case EFX_PRESET_OFF:
        /* defaults already left things neutral */
        break;
    case EFX_PRESET_ALIEN_NOSTROMO:
        /* MU/TH/UR-style green CRT in the Nostromo: heavy scanlines,
           strong phosphor glow, mild grain, slow trail. The grain is
           held back so the IBM 3270-style block characters still
           read at small sizes. */
        e->crt      = 0.65f;
        e->phosphor = PHOSPHOR_GREEN;
        e->bloom    = 0.70f;
        e->vhs      = 0.10f;
        e->grain    = 0.18f;
        e->decay    = 0.45f;
        break;
    case EFX_PRESET_HAL_9000:
        /* 2001's pod / centrifuge displays: very mild scanlines (the
           original is more "projected slide" than CRT), modest blue
           phosphor, heavy decay so trails are the headline effect.
           No glitch — HAL is *calm*. */
        e->crt      = 0.20f;
        e->phosphor = PHOSPHOR_BLUE;
        e->bloom    = 0.45f;
        e->grain    = 0.12f;
        e->decay    = 0.70f;
        break;
    case EFX_PRESET_VT100:
        /* Clean DEC-monochrome look — minimal everything, just enough
           CRT + green phosphor that it reads as a 1978 terminal. */
        e->crt      = 0.40f;
        e->phosphor = PHOSPHOR_GREEN;
        e->bloom    = 0.30f;
        e->decay    = 0.20f;
        break;
    case EFX_PRESET_AMBER_CRT:
        /* IBM-amber sibling of VT100. Same restraint, different tint. */
        e->crt      = 0.40f;
        e->phosphor = PHOSPHOR_AMBER;
        e->bloom    = 0.35f;
        e->decay    = 0.25f;
        break;
    case EFX_PRESET_VHS_TAPE:
        /* Consumer-VHS warmth dialled to "watchable cassette" — the
           jitter / tape lines + light grain set the mood without
           wobbling glyphs into mush. No phosphor — keeps colours
           intact for the warm-cassette feel. */
        e->vhs    = 0.40f;
        e->glitch = 0.08f;
        e->grain  = 0.18f;
        e->bloom  = 0.20f;
        break;
    case EFX_PRESET_TRON:
        /* The light-cycle game grid: dark base, bright neon strokes,
           heavy bloom, light trails. No phosphor so coloured ANSI
           output keeps its hue. */
        e->crt    = 0.20f;
        e->bloom  = 0.85f;
        e->decay  = 0.40f;
        e->grain  = 0.10f;
        break;
    case EFX_PRESET_MATRIX:
        /* Operator console: green digital rain — heavy decay so glyphs
           streak, bloom for the warm-glow look, mild glitch. */
        e->crt      = 0.30f;
        e->phosphor = PHOSPHOR_GREEN;
        e->bloom    = 0.55f;
        e->decay    = 0.65f;
        e->glitch   = 0.10f;
        e->grain    = 0.10f;
        break;
    case EFX_PRESET_WARGAMES:
        /* WOPR / NORAD: stark green, heavy scanlines, almost no glow.
           1983-style "CRT through tinted plexi". */
        e->crt      = 0.55f;
        e->phosphor = PHOSPHOR_GREEN;
        e->bloom    = 0.20f;
        e->decay    = 0.10f;
        e->grain    = 0.05f;
        break;
    case EFX_PRESET_ROBOCOP:
        /* OCP HUD: scan-printed amber overlay. Halftone hints at the
           dot-screen aesthetic but stays low enough that the dots
           don't punch through glyphs — readability beats fidelity
           for a terminal. Bloom + decay carry most of the look. */
        e->crt      = 0.30f;
        e->phosphor = PHOSPHOR_AMBER;
        e->bloom    = 0.35f;
        e->halftone = 0.20f;
        e->grain    = 0.10f;
        e->decay    = 0.20f;
        break;
    case EFX_PRESET_BLACK_MIRROR:
        /* Title-card unease: light continuous glitch, medium CRT,
           subtle decay, tiny VHS so the picture never sits still. */
        e->crt      = 0.45f;
        e->bloom    = 0.25f;
        e->glitch   = 0.15f;
        e->decay    = 0.20f;
        e->grain    = 0.15f;
        e->vhs      = 0.05f;
        break;
    case EFX_PRESET_DUNE:
        /* Atreides ornithopter / display panels: warm amber with a
           hint of dot-screen and film grain — the printed-from-light
           feel without halftone shredding glyph strokes. Bloom takes
           a small bump to carry the warmth. */
        e->crt      = 0.20f;
        e->phosphor = PHOSPHOR_AMBER;
        e->bloom    = 0.30f;
        e->halftone = 0.25f;
        e->grain    = 0.20f;
        e->decay    = 0.15f;
        break;
    case EFX_PRESET_GAMEBOY:
        /* DMG-style monochrome dot-matrix LCD: zero CRT, very heavy
           halftone (the dots ARE the look), green tint, almost no
           glow. Trails are tiny — DMG had ghosting, but only just. */
        e->phosphor = PHOSPHOR_GREEN;
        e->bloom    = 0.10f;
        e->halftone = 0.85f;
        e->decay    = 0.05f;
        break;
    case EFX_PRESET_C64:
        /* Chunky 1985 home computer monitor: heavy CRT curvature, a
           generous blue tint, big phosphor bloom. The Commodore
           1702 / 1084 monitor look. */
        e->crt      = 0.70f;
        e->phosphor = PHOSPHOR_BLUE;
        e->bloom    = 0.65f;
        e->decay    = 0.30f;
        e->grain    = 0.15f;
        break;
    case EFX_PRESET_APOLLO_DSKY:
        /* DSKY 7-segment readout — minimal-everything except a
           strong amber phosphor glow. Reads like a stamped industrial
           display, not a CRT. */
        e->crt      = 0.10f;
        e->phosphor = PHOSPHOR_AMBER;
        e->bloom    = 0.50f;
        e->decay    = 0.30f;
        break;
    case EFX_PRESET_NEWSROOM:
        /* 1980s news-bulletin CRT: green phosphor, medium decay,
           visible grain, mild VHS to evoke "captured from broadcast". */
        e->crt      = 0.45f;
        e->phosphor = PHOSPHOR_GREEN;
        e->bloom    = 0.25f;
        e->decay    = 0.40f;
        e->grain    = 0.30f;
        e->vhs      = 0.10f;
        break;
    case EFX_PRESET_CYBERPUNK:
        /* Coloured-base cyberpunk: heavy bloom, heavy decay, occasional
           glitch + slight VHS. Phosphor stays off so neon palette
           survives. */
        e->crt    = 0.25f;
        e->bloom  = 0.75f;
        e->decay  = 0.65f;
        e->glitch = 0.15f;
        e->grain  = 0.10f;
        e->vhs    = 0.10f;
        break;
    case EFX_PRESET_SURVEILLANCE:
        /* Lo-fi security feed — toned again: VHS jitter still reads
           but no longer wobbles glyphs into illegibility, halftone is
           barely a hint, grain is sparse. The look is "VCR camera
           feed, just legible" rather than "tape damage". */
        e->vhs      = 0.30f;
        e->halftone = 0.05f;
        e->grain    = 0.15f;
        e->glitch   = 0.04f;
        e->crt      = 0.20f;
        e->bloom    = 0.10f;
        e->decay    = 0.10f;
        break;
    case EFX_PRESET_NEWSPRINT:
        /* Print look toned again — halftone is now a hint of dot-
           texture rather than the dominant feature, grain sparse.
           Terminal legibility wins over print fidelity. */
        e->halftone = 0.18f;
        e->grain    = 0.10f;
        e->bloom    = 0.05f;
        break;
    case EFX_PRESET_STATIC:
        /* TV-losing-signal vibe at a level you can still read.
           Glitch slice-scrambles are sparse enough to feel like
           occasional breakup; VHS + grain set the mood without
           torching glyphs. */
        e->vhs    = 0.25f;
        e->glitch = 0.08f;
        e->grain  = 0.18f;
        e->crt    = 0.30f;
        e->bloom  = 0.30f;
        break;
    default:
        break;
    }
}

bool rec_effects_is_default(const RecEffects *e) {
    if (!e) return true;
    return e->crt < 0.001f && e->bloom < 0.001f && e->vhs < 0.001f
        && e->glitch < 0.001f && e->grain < 0.001f && e->halftone < 0.001f
        && e->decay < 0.001f
        && e->phosphor == PHOSPHOR_NONE
        && e->speed > 0.99f && e->speed < 1.01f;
}
