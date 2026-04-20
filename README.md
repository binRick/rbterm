# rbterm

<p align="center">
  <img src="assets/icon.png" width="96" alt="rbterm icon"><br>
  <em>A cross-platform terminal emulator written in C, rendered with <strong>raylib</strong>.</em>
</p>

<p align="center"><img src="docs/screenshot.png" alt="rbterm screenshot — two tabs, live CWD in the tab label, coloured ➜ prompt"></p>

## Why

Because building a terminal is one of those things that sounds small
and turns out to be a microcosm of everything: PTYs, Unicode, font
rendering, ANSI escape state machines, scrollback ring buffers,
reflow, colour palettes, mouse selection, IPC. It's a fun way to see
how the OS, the shell, and a graphics library all meet.

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
| Cmd+Shift+T | New tab over SSH (prompt for `user@host[:port]`) |
| Cmd+W | Close tab |
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
