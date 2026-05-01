/* Asciinema v2 (.cast) parser. Reads the JSON-line format rbterm
   writes during recording: header object on line 1 (width/height),
   then one event array per line `[<seconds>, "o"|"i", "<bytes>"]`.
   Both "o" (PTY output → screen) and "i" (user input → PTY) streams
   are collected; the kind field on each event tells callers which
   to act on. Renderers feed only "o" events through screen_feed
   and use "i" events for caption-style overlays. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    double   t;       /* seconds since start */
    char     kind;    /* 'o' = output, 'i' = input */
    uint8_t *data;    /* heap; un-escaped bytes */
    size_t   n;
} CastEvent;

typedef struct {
    int        cols, rows;
    CastEvent *events;
    size_t     count;
    size_t     cap;
    double     duration_s;  /* timestamp of the last event */
} CastFile;

/* Parse `path` into a CastFile. On failure writes a one-line reason
   into `err` and returns NULL. Caller owns the returned object and
   must call cast_free. */
CastFile *cast_load(const char *path, char *err, size_t errsz);

/* Free a CastFile and every event byte buffer it owns. NULL-safe. */
void      cast_free(CastFile *cf);
