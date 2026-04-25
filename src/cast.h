/* Asciinema v2 (.cast) parser. Reads the JSON-line format rbterm
   writes during recording: header object on line 1 (width/height),
   then one event array per line `[<seconds>, "o", "<bytes>"]`.
   Only the "o" output stream is collected; "i" (input) lines are
   skipped if they appear. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    double   t;       /* seconds since start */
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
