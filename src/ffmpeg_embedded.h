#pragma once
#include <stddef.h>
#include <stdbool.h>

/* Windows-only. On first call, extracts the ffmpeg.exe RCDATA blob
   baked into rbterm.exe to a stable path under %TEMP% and writes
   that path to `out`. Subsequent calls in the same user-session
   skip the rewrite (existing-file size match). Returns true when
   the temp path is ready to use, false on non-Windows builds or
   when the resource isn't present (e.g. dev builds without
   assets/ffmpeg/ffmpeg.exe). */
bool ffmpeg_embedded_extract(char *out, size_t cap);
