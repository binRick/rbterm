# rbterm

<p align="center">
  <img src="assets/icon.png" width="96" alt="rbterm icon"><br>
  <em>A cross-platform terminal emulator written in <strong>pure C99</strong>, rendered with <strong>raylib</strong>. Single self-contained binary, ~250 themes and 16 monospace fonts baked in.</em>
</p>

<p align="center"><img src="docs/screenshot.png" alt="rbterm screenshot — two tabs, live CWD in the tab label, coloured ➜ prompt"></p>

## Why

Because building a terminal is one of those things that sounds small
and turns out to be a microcosm of everything: PTYs, Unicode, font
rendering, ANSI escape state machines, scrollback ring buffers,
reflow, colour palettes, mouse selection, IPC. It's a fun way to see
how the OS, the shell, and a graphics library all meet.

## Pure C99 — fast, lean, no runtime

The whole thing is a few thousand lines of straight C99 — no C++, no
garbage collector, no Electron, no JavaScript engine, no embedded
scripting language. Every keystroke goes from raylib's input → a
fixed-state-machine parser → a flat cell-grid → a single glyph atlas
in one frame, with no per-frame allocation in the hot path. The PTY
backend talks to `forkpty(3)` / ConPTY directly, the VT parser is a
hand-written DFA, and the GPU path is straight raylib `DrawTextEx` /
`DrawRectangle`. Cold-start to a usable shell is under 200 ms; idle
CPU is effectively zero (raylib's vsync gates the loop).

Everything you'd reach for at runtime is **embedded into the binary
at link time**:

- **252 colour palette themes** baked in as `static const` C arrays
  via a tiny generator that walks the [`pal`](https://github.com/binRick/pal)
  submodule.
- **16 monospace fonts** (JetBrains Mono, Fira Code, Hack, Monaspace
  Argon/Krypton/Neon/Radon/Xenon ± Nerd Font icons, etc.) pulled in
  with `.incbin` so the executable carries them as raw bytes — no
  `fonts/` folder beside the binary, no runtime download. `make app`
  on macOS produces a single self-contained `.app` you can drag and
  drop; the Linux/Windows release zips are likewise standalone.

The result is a 13 MB single-file terminal that opens instantly,
runs with one process per window, has zero non-system dependencies
when packaged, and feels closer in spirit to a 1990s native app than
to a modern web stack.

## Themes + fonts ship inside the binary

There's no `~/.config/rbterm/themes/`, no separate font folder, no
network roundtrip — open Settings on a fresh install and you have
**252 colour palette themes** and **16 monospace fonts** to pick
from immediately, all baked into the executable.

**Themes** come from the [`pal`](https://github.com/binRick/pal)
companion CLI (vendored as a submodule). At build time
`tools/gen_themes.sh` walks `third_party/pal/palettes/kfc/dark/*` and
emits `src/themes_embedded.h` — a `static const` array of name +
key=value bodies. At startup `themes_load_builtins()` parses each
into a `Theme { fg, bg, cursor, palette[16] }`. Click one in
Settings → it applies to the *active pane only* (palette + default
fg/bg/cursor are per-screen, so a theme change in one tab can't
leak into another). Per-host SSH stanzas can pin a theme via a
`# rbterm-theme:` comment that survives plain ssh round-trips.

**Fonts** are pulled in even more directly: `tools/gen_fonts.sh`
emits a tiny `.S` file with one `.incbin` directive per `.ttf`/`.otf`
in `assets/fonts/`. The assembler folds the raw bytes into the
binary's read-only data segment, exposing start labels that C
references via `extern const unsigned char rbterm_font_FOO_start[]`.
At runtime `LoadFontFromMemory` rasterises straight from the
embedded blob — no disk I/O, no `--preload-file`. Re-sizing the font
re-rasterises from the same in-memory buffer.

Result: a single self-contained binary you can `scp` anywhere and
run, no install steps, no missing-font warnings. A stripped-down
machine with no `~/Library/Fonts` and no `Resources/fonts/` next to
the exe still boots straight into a usable shell because rbterm
falls through to the embedded set.

The bundled fonts (Apache-2.0 / OFL-1.1):
- JetBrains Mono, Fira Code, Hack, Source Code Pro, Inconsolata,
  IBM Plex Mono.
- Monaspace family — Argon / Krypton / Neon / Radon / Xenon, in both
  Static and Nerd Font Regulars (10 variants).

Only ~13 MB total — the entire bundled font payload is smaller than
a single Electron framework dylib.

## GPU-accelerated rendering

Every pixel goes through OpenGL — there's no software rasterizer in
the path. Raylib's `rlgl` batcher coalesces the per-cell draw calls
each frame into a handful of textured-quad batches:

- **macOS**: OpenGL 3.3 (Apple's framework translates to Metal).
- **Linux**: native OpenGL 3.3 over GLX / EGL.
- **Windows**: WGL OpenGL 3.3.
- **Web**: WebGL 1 / OpenGL ES 2.0 via emscripten.

A single glyph atlas (one `Texture2D`) holds every codepoint the
current font covers, rasterised at 2× the display size and downsampled
with a bilinear filter so strokes stay crisp on Retina without doing
any per-frame work. Cell backgrounds, the cursor, the scrollback
indicator, the splitter bar, and modal panels all compile down to
`DrawRectangle` / `DrawTextEx` calls that the GPU draws as batched
quads — no CPU compositing.

Idle-frame cost is effectively zero: vsync gates the main loop and
raylib emits no draw calls when nothing has changed. The CPU's only
job each frame is walking the cell grid, parsing PTY input through a
hand-written DFA, and handing rlgl the vertex data; the rest is
pixels on the GPU.

## Features

- **Shell in a PTY** via `forkpty` on macOS/Linux and **ConPTY** on
  Windows 10+.
- **VT500-style parser** — SGR (16 / 256 / truecolor), cursor movement,
  scroll regions, erase-in-display / -line, alt screen, save/restore
  cursor, OSC 0/2 window title, OSC 4 / 104 palette, UTF-8 text,
  wide-char support via `ATTR_WIDE`/`ATTR_WIDE_CONT`.
- **Tabs** — up to 16 concurrent shells, each with its own PTY,
  scrollback, selection and title. Background tabs stay live.
- **Embedded SSH** — Cmd+Shift+T opens a modal, type `user@host[:port]`,
  Enter and a new tab is an SSH session via libssh (no local shell in
  the middle). Key auth via ssh-agent or `~/.ssh/id_*`; host keys
  trust-on-first-use into `~/.ssh/known_hosts`.
- **Tab label tracks `cd`** via `proc_pidinfo` (macOS) /
  `/proc/<pid>/cwd` (Linux). `$HOME` shortens to `~`.
- **Reflow on resize** — widen the window and wrapped prompts
  un-wrap; narrow it and long lines re-wrap. Overflow goes to
  scrollback.
- **Colour emoji** on macOS via Core Text + SBIX bitmap fonts.
  `CTFontCreateForString` handles font substitution, so glyphs the
  primary font lacks (e.g. `➜` with SF Mono) still render — the
  rasterizer inspects the output to tell colour bitmaps from white
  vector masks and tints with the cell's `fg` for the latter.
- **Mouse selection** — click-drag, double-click word, triple-click
  row. Translucent overlay. Cmd+C copies, Cmd+V pastes.
- **Scrollback** — 5000 lines per tab, Shift+PgUp/PgDn or mouse wheel.
  Right-hand indicator shows position.
- **Live font resize** — Cmd + `+` / `-` / `0`. Reflows every tab.
- **OSC palette** works with the
  [`pal`](https://github.com/binRick/pal) CLI.

## Keybindings

(Cmd on macOS; Ctrl on Linux/Windows.)

| Shortcut | Action |
|----------|--------|
| Cmd+T | New tab (local shell) |
| Cmd+Shift+T | New tab over SSH (PuTTY-style form) |
| Cmd+W | Close tab |
| Cmd+, | Settings (font size, session logging) |
| Cmd+1..9 | Jump to tab N |
| Cmd+[ / Cmd+] | Prev / next tab |
| Cmd++ / Cmd+- / Cmd+0 | Grow / shrink / reset font |
| Cmd+C / Cmd+V | Copy selection / paste |
| Ctrl+(letter) | C0 control byte (SIGINT, etc.) |
| Shift+PgUp / PgDn | Scroll history |
| Mouse wheel | Scroll history |
| Double-click | Select word |
| Triple-click | Select row |

## Build

### macOS (fastest path)

```bash
brew install raylib libssh
make            # ./rbterm
make app        # ./rbterm.app with icon + Info.plist
./run.sh        # kills any running rbterm, rebuilds, launches
```

### macOS / Linux / Windows (CMake; no raylib install needed)

```bash
cmake -S . -B build   # fetches raylib 5.5
cmake --build build
./build/rbterm        # (./build/Release/rbterm.exe on Windows)
```

Windows needs:
- Windows 10 version 1809+ (ConPTY)
- Visual Studio or MinGW-w64
- CMake 3.15+

Linux needs:
- GLFW's X11/Wayland deps (raylib builds them via its submodules)
- `libutil` (for `forkpty`)
- `libssh` (either installed — `apt install libssh-dev` — or let
  CMake FetchContent pull it in with mbedTLS)

## Usage

```
rbterm [--font PATH] [--size N] [--cols N] [--rows N]
```

Bring your own font if you want (e.g. JetBrains Mono, Fira Code). On
macOS rbterm searches, in order: Consolas → SFNSMono → Monaco → Menlo.
On Linux: DejaVu Sans Mono → Liberation Mono → Noto Sans Mono.

## Limitations

- No shaping, so ligatures and ZWJ sequences render as components.
- Windows has no colour emoji (DirectWrite port would be a separate
  piece of work — the stub fails gracefully and monochrome glyphs
  still work via the main font).
- Windows has no CWD-in-tab-label tracking (falls back to OSC title).
- No sixel / kitty graphics protocol.
- No bracketed paste (accepted, not yet acted on).
- No mouse reporting — clicks don't reach the shell.

## Layout

See [CLAUDE.md](CLAUDE.md) for an architecture tour and the tricky
bits of the VT parser, reflow, glyph pipeline, and input translation.
