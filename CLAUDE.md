# rbterm — notes for Claude

Terminal emulator in C, rendered with raylib. Unix + Windows. Thin
abstraction (`pty.h`) keeps the rest of the code platform-agnostic.

## Keep the README in sync

The README cites a rough total line count for `src/`. After material
add/remove, re-run `wc -l src/*.c src/*.h src/*.m` and update if off
by more than ~1k lines (round to nearest thousand). Same for any
README figures describing code size, file counts, or feature lists —
new subsystems get a README update in the same commit.

## Architecture

```
   user input ─► raylib events ─► input_poll() ─► pty_write ─► shell
                                                                │
   glyph output ◄─ renderer_draw ◄─ screen_feed (VT) ◄─ pty_read ◄─┘
```

Each tab owns a `PaneNode *root` — recursive split tree. Leaves carry
a `Pane` (PTY + Screen + Selection + title + cwd). Internal nodes
carry SplitMode (V/H), ratio, two children. `tab_split` replaces the
active leaf with an internal node; `tab_close_leaf` collapses parent
into sibling. Main loop drains every leaf's PTY each frame.

## Files

| file | contents |
|------|----------|
| `src/main.c` | Event loop, tab bar UI, SSH prompt modal, clipboard, mouse/keyboard routing |
| `src/pty.h` / `pty_internal.h` / `pty_dispatch.c` | PTY interface + backend dispatch |
| `src/pty_unix.c` | Local PTY via `forkpty` (macOS/Linux) |
| `src/pty_win.c` | Local PTY via ConPTY + reader thread + ring buffer (Windows) |
| `src/pty_ssh.c` | SSH backend via libssh — key auth, trust-on-first-use |
| `src/screen.{c,h}` | ANSI/VT parser, grid buffer, scrollback, reflow |
| `src/render.{c,h}` | raylib rendering, glyph atlas, emoji cache, cursor |
| `src/input.{c,h}` | raylib event → PTY bytes (C0 chords, arrows, UTF-8) |
| `src/emoji_mac.m` / `emoji_stub.c` | CoreText rasterizer (mac) / no-op stub |
| `src/cast.{c,h}` | Asciinema v2 (.cast) parser for recording replay |
| `src/gif_encoder.{c,h}` / `webp_encoder.{c,h}` | Native encoders (no ffmpeg) |
| `src/hud.{c,h}` | System-info overlay |
| `tools/gen_icon.c` | Procedural app-icon generator (macOS .icns) |

## Platform notes

### macOS (primary target)
- Font fallback: Consolas → SFNSMono.ttf → Monaco.ttf → Menlo.ttc.
  Menlo is a TTC and raylib's stb_truetype can't index collections.
- Colour emoji via Core Text. `CTFontCreateForString` does font
  substitution itself (so `➜` falls through to Menlo).
- Cmd is the UI modifier; Ctrl is for C0 chords (SIGINT etc.).
- `make app` → `.app` bundle. `run.sh`: kill, build, `open -n`.

### Linux
- Same `pty_unix.c` backend. Emoji = stub (HarfBuzz/FreeType TODO).
- Font fallback: DejaVu Sans Mono → Liberation Mono → Noto.
- UI modifier is Ctrl.

### Windows
- ConPTY (`pty_win.c`). Requires Win10 1809+. Anonymous pipes don't
  support overlapped I/O — reader thread per PTY feeds a 256KB ring
  buffer under `CRITICAL_SECTION`.
- `pty_cwd()` returns false; tab labels fall back to OSC 0/2 title.
- Emoji = stub. Shell: `$SHELL` → powershell.exe → cmd.exe.

## Build

```bash
make           # macOS Makefile (uses brew raylib) → ./rbterm
make app       # → ./rbterm.app
cmake -S . -B build && cmake --build build   # cross-platform
```

## Tricky bits / gotchas

### VT parser (`screen.c`)
- Wide chars: `ATTR_WIDE` on head cell, `ATTR_WIDE_CONT` on next.
  Draw loop and selection copy skip `ATTR_WIDE_CONT`.
- Auto-wrap: `wrap_next && autowrap` triggers a line break and marks
  the leaving row in `main_wrap[]`. Scroll regions shift the flag
  array in parallel with cells.
- OSC 4 (palette) / OSC 104 (reset) are parsed; SGR 30-47 / 90-107 /
  38;5 / 48;5 all read through `pal(i)`, so `pal` CLI works.

### Reflow (`screen_resize`)
- Collects logical lines from `main_wrap[]` + `main[]`, rewraps at
  new `cols`, copies last `rows` rows into new main. Overflow into
  scrollback via `push_scrollback` (with temporary `s->cols` swap).
- Cursor lands at end of last row with content (so bash's SIGWINCH
  redraw lands in place). Scrollback is re-bucketed, not reflowed.
- Alt screen is *not* reflowed — full-screen apps redraw on SIGWINCH.

### Rendering (`render.c`)
- Missing-glyph set built once per font load. Draw loop uses it to
  decide emoji cache / Menlo fallback / `?` placeholder.
- **`GetFontDefault()` is ASCII-only.** Any non-ASCII codepoint
  (`↑↓←→ ⏎ ⌫ ⇥ …`) renders as `?`. Side-overlay code using the
  default font (recording captions, modal labels) MUST stick to
  printable ASCII or render via `g_renderer->font_data`. Has bitten
  multiple sessions — recording captions use `[Up] [Dn] [Ret]`.
- Glyph cache `colored` flag set by rasterizer (RGBA scan: any
  unequal channels = colour bitmap). Tint with WHITE for colour,
  cell `fg` for monochrome.
- `BeginMode2D` with y-offset camera puts terminal content below
  tab bar without touching every draw site.

### Input (`input.c`)
- macOS `GetCharPressed()` doesn't fire for `Ctrl+letter`. Pick up
  the C0 chord via `IsKeyPressed(KEY_A..KEY_Z)` and emit
  `k - KEY_A + 1`. Also `Ctrl+[ \ ]` and `Ctrl+Space`.
- Cmd vs Ctrl are *separate* — `ctrl_down()` is real Ctrl only.
  Otherwise Cmd+C eats Ctrl+C (no SIGINT).

### Intelligent double-click (`select_word` in main.c)
Naive "run of non-whitespace" then post-trim:
- Trailing sentence punctuation (`, ; : . ! ?`) stripped right
  unless clicked directly on it.
- Unmatched `) ] } > "` stripped right; unmatched `( [ { < "`
  stripped left.
- Interior `- _ / @ = . + #` and leading hyphens (`--flag`) kept.

When adding cases, extend the helpers or add another post-trim pass.
The `c2 != col` / `c1 != col` checks ensure clicking directly on
punctuation still selects it.

### Tab ordering
`Tab *g_tabs[MAX_TABS]`. `tab_close` shifts slots down. `Screen->io.user`
is the stable heap-allocated `Tab *`, so the shift doesn't invalidate
callbacks.

### Embedded fonts on Windows — `static` arrays in headers
`fonts_embedded.h` declares `k_embedded_fonts[]`. Mac/Linux: `static
const` with `.incbin` symbol pointers — every TU has its own copy
but all see the same data via extern symbols. **Windows: data
pointers must be filled at runtime** by `embedded_fonts_init()`
(FindResource/LoadResource). Array MUST NOT be `static` in the
header — `extern` decl in header + non-static defn in
`fonts_embedded_win.c`. If Windows slots show `data=NULL` after init,
check `gen_fonts.sh` hasn't regressed back to `static`.

## Recursive split tree

`Tab` owns `PaneNode *root` + `PaneNode *active`. Leaf:
`split == SPLIT_NONE`, `pane != NULL` (heap-allocated for stable
`io.user`). Internal: `split == SPLIT_VERTICAL/HORIZONTAL`,
`child[0/1]`, `ratio` (0.15..0.85), `splitter_drag` bool.

Helpers in `main.c`: `pane_node_new_leaf` / `_free_recursive` /
`pane_tree_count` / `_first_leaf` / `_next_leaf` / `_prev_leaf` /
`_cycle_leaf` / `_split_children` / `_node_rect_walk` / `leaf_rect`
/ `pane_tree_at` / `_splitter_at` / `pane_node_split_leaf` /
`_close_leaf` / `tab_split` / `tab_close_leaf` / `pane_close_active`.

Splitter dragging: each internal node has its own `splitter_drag`.
Mouse handler walks the tree to find dragging node, stashes outer
rect of press in a function-local static so drag math stays
consistent across frames as the tree mutates.

Canonical pane iteration:

```c
for (PaneNode *leaf = pane_tree_first_leaf(t->root); leaf;
     leaf = pane_tree_next_leaf(leaf)) {
    Pane *p = leaf->pane;
    if (!p) continue;
    /* ... */
}
```

Avoid hardcoding "pane 0/1" or `t->active_pane` — those don't exist.

## User preferences

- Terse, direct responses. No trailing summaries of what changed.
- "commit to github" = commit + push. Default visibility private;
  flip with `gh repo edit binRick/rbterm --visibility public`.
- GitHub: `binRick` (SSH auth, keyring token).
- Prefers real fixes over workarounds.
- "pal" = companion CLI at `third_party/pal/` (separate
  `binRick/pal` repo — see memory note, NOT a submodule). Palette
  applier emitting OSC 4/10/11. Run inside a tab to apply per-pane;
  rbterm keeps palette+fg/bg/cursor per-Screen.

## Settings modal (`Cmd+,`, `UI_SETTINGS`)

*The* place preferences live — new options go here, not as one-off
shortcuts. Layout computed once per frame in `settings_layout` so
draw + hit-test share rects. Modifier chords (`Cmd/Ctrl+A`) are
consumed so GetCharPressed doesn't leak the `a` into the field.
`g_settings_dir_focus` gates whether the directory field has input.

Persistence: `config_save` / `config_load_into_defaults` use
`~/.config/rbterm/config.ini`; **Save as Default** button triggers
save. Without that click, edits are process-local.

Logging tab: enabled toggle + log dir. Writes are **raw PTY bytes**
(ANSI, SGR included — `sed 's/\x1b\[[0-9;]*[a-zA-Z]//g'` for plain).
`fflush` after every write. Toggle/path edit calls `refresh_tab_logs`
which immediately opens/closes handles on every open tab. Filename
slot is the tab's open-time index (closing earlier tab doesn't
rename remaining files).

### Adding a new setting
1. Field in `AppSettings` (+ `app_settings_init` if non-zero default).
2. `Rect` in `SettingsLayout`, place in `settings_layout`.
3. Click + key in `settings_handle_mouse` / `_keys`.
4. Draw in `draw_settings`.
5. Wire side-effects (e.g. `refresh_tab_logs`).
6. Add to both `config_save` and `config_load_into_defaults` if it
   should survive restart.

## SSH

`pty_ssh.c` uses libssh. `PTY_SSH` Pty holds `SshPty *impl` with
`ssh_session` + `ssh_channel`. Session goes non-blocking after auth/
channel setup, so `pty_read` drains via `ssh_channel_read_nonblocking`.

- Connect/auth on main thread. UI stalls 1-2s during connect (TODO:
  threaded connect with spinner).
- Auth: `ssh_userauth_publickey_auto` (agent first, then `~/.ssh/id_*`).
  No interactive password yet (TODO: prompt through Screen).
- Host-key: trust-on-first-use into `~/.ssh/known_hosts`;
  `SSH_KNOWN_HOSTS_CHANGED` aborts.
- UI modal in `main.c`, `Cmd+Shift+T`. `ssh_prompt_handle_keys` calls
  `tab_open_ssh`; libssh errors render in red in the modal.

## Benchmark harness

```bash
# Manual (preferred — works for every terminal):
cd ~/Desktop/repos/rbterm
./tools/bench-here.sh             # 1000 samples; N=200 quick
./tools/bench-summary.sh          # side-by-side from any term

# Automated (flaky on focus/window timing):
N=1000 TIMEOUT_S=120 ./tools/run-benchmark.sh
```

`bench-here.sh` auto-detects host via `$TERM_PROGRAM` (rbterm sets
its own) and writes `bench/echo-<slug>-<stamp>.txt`. `echo_bench`
(`tools/echo_bench.c`) measures round-trip on `CSI 6n` DSR queries
— write query, parse + reply with `CSI <r>;<c>R`, read answer.

## Commit + release flow

**"commit"** / **"commit to github"**: `git add -A`, clear commit,
`git push`. That's it.

**"commit and release"** / **"publish a release"** / **"update the
release"**: above plus re-tag `v0.1.0`:

```bash
git tag -d v0.1.0
git push origin :refs/tags/v0.1.0
gh release delete v0.1.0 --repo binRick/rbterm --yes --cleanup-tag || true
git tag -a v0.1.0 -m "v0.1.0"
git push origin v0.1.0
```

Triggers `.github/workflows/release.yml` (~2 min) → builds
`rbterm-linux-x86_64` + `rbterm-windows-x86_64.exe`. `gh run watch
<run_id>` to block.

### "copy the exe to my windows vm"
Windows VM SSH alias: `win`. Windows Downloads is redirected to
`\\Mac\Home\Downloads` (Parallels/Fusion shared folder), so always
land the file on the **Mac side** — `scp`-ing into `win:Downloads/`
goes to a separate physical Windows folder the user doesn't browse.

**Always kill any running rbterm + delete the existing exe BEFORE
pushing a new one.** Otherwise the file lock blocks SCP silently or
the user double-clicks a stale binary (Explorer caches the icon).

```bash
# 1. Kill + remove stale exe.
ssh win 'powershell -NoProfile -Command "
    Get-Process rbterm-windows-x86_64 -ErrorAction SilentlyContinue | Stop-Process -Force;
    Remove-Item \"$env:USERPROFILE\Downloads\rbterm-windows-x86_64.exe\" -Force -ErrorAction SilentlyContinue
"'
# 2. Wipe local + fetch.
cd ~/Downloads && rm -f rbterm.exe rbterm-windows-x86_64.exe rbterm-windows-x86_64.zip
gh release download v0.1.0 --repo binRick/rbterm --pattern 'rbterm-windows-x86_64.zip' --clobber
unzip -o rbterm-windows-x86_64.zip
cp rbterm.exe rbterm-windows-x86_64.exe
# 3. SCP + Defender exclusion + verify mtime (the trust signal).
scp rbterm-windows-x86_64.exe win:Downloads/
ssh win 'powershell -NoProfile -Command "
    Add-MpPreference -ExclusionPath \"$env:USERPROFILE\Downloads\rbterm-windows-x86_64.exe\";
    ls \"$env:USERPROFILE\Downloads\rbterm-windows-x86_64.exe\" | Select-Object Name,LastWriteTime
"'
```

### "push to my mia vm"
`mia` = Linux x86_64 VM (RHEL-family/el10, currently 172.238.205.61).
Pre-release smoke-test box. SSH alias defaults `User root` but user
operates as **`rich`** — drop into `rich`'s home and chown:

```bash
scp rbterm-linux-x86_64.zip mia:/home/rich/Downloads/
ssh mia 'chown rich:rich /home/rich/Downloads/rbterm-linux-x86_64.zip'
```

GUI app needs `ssh -Y mia ./rbterm` or local desktop session.

### Smoke-test Linux release locally (OrbStack)

```bash
brew install --cask orbstack && open -a OrbStack
orb create --arch amd64 ubuntu rbterm-test   # x86 REQUIRED
orb -m rbterm-test bash -c 'sudo apt-get update -qq && \
    sudo apt-get install -y unzip libssh-4 libharfbuzz0b libwebp7 \
                            libwebpmux3 libfontconfig1 libxinerama1 \
                            libxcursor1 libxi6 libgl1 libxrandr2 libxkbcommon0'
gh release download v0.1.0 --repo binRick/rbterm \
    --pattern 'rbterm-linux-x86_64.zip' --dir ~/Downloads --clobber
orb -m rbterm-test bash -c \
    'mkdir -p ~/rbterm && cp /Users/$USER/Downloads/rbterm-linux-x86_64.zip ~/rbterm/ && \
     cd ~/rbterm && unzip -o rbterm-linux-x86_64.zip && chmod +x rbterm'
orb -m rbterm-test bash -c 'cd ~/rbterm && timeout 4 ./rbterm 2>&1 | head'
```

`--arch amd64` required (default Ubuntu image is arm64; cross-arch
deps sparse). For visible window: `brew install --cask xquartz`
(logout/login). Without XQuartz, GLFW init fails with "DISPLAY
missing" — still validates dep stack loaded.

## Graphics (sixel + kitty)

Primary-DA advertises sixel (`CSI ? 65;1;4;9 c`) so `img2sixel`,
`chafa -f sixel`, `ranger`, `gnuplot` auto-select.

Pipeline:
1. **Parse** (`screen.c`): DCS / APC payloads buffered. Terminator
   dispatches by first byte — DCS digits + `q` = sixel, APC `G` = kitty.
2. **Decode** (`sixel.c`, `kitty.c`): heap RGBA8. Sixel two-pass
   (extents, raster). Kitty v1: `f=100` PNG single-message, decoded
   via raylib's `LoadImageFromMemory`.
3. **Store** (`screen.c`): images attach with anchor row+col. Scrolls
   decrement anchor; row<0 drops. Cap 32 per screen, oldest evicted.
4. **Blit** (`render.c`): per-frame cache keyed by
   `(ScreenImage*, generation)`. Stale entries evicted. Cursor
   advances past the image so text doesn't overlap.

v1 gaps: sixel no P1=0 / P3 / custom registers; kitty no f=24/32, m=1,
animation, deletes, response protocol; no selection over images; no
reflow on resize.

## Recording

Rec button per pane → `g_rec.fp` mirrors PTY bytes as asciinema v2:
`[<sec>, "o", "<json-bytes>"]`. Starts with synthetic snapshot at
t=0 (clear + per-row cursor + cell codepoints + cursor restore) so
playback opens with what the user already saw.

Stop → `RecSave` → `UI_REC_SAVE` modal with seven format pills and
path field. Save dispatches into `rec_render_native(fmt, src, dst)`:

| fmt | path |
|--|--|
| cast | `rename` |
| txt | `strip_feed` — drops escapes; CR/BS/LF cursor model |
| gif | Native `gif_encoder.c` (LZW, 6×6×6 cube + 40-step gray) |
| webp | Native `webp_encoder.c` (libwebp + libwebpmux, q=75) |
| mp4 | Pipe rawvideo into `ffmpeg -pix_fmt yuv420p -movflags +faststart` |
| webm | Same but `-c:v libvpx -b:v 1M -pix_fmt yuv420p` |
| apng | Two-pass: temp gif → `ffmpeg -i tmp.gif -plays 1 dst.apng` |

Render loop is **chunked** (`CHUNK_SZ=6` frames) with a fresh modal
frame between chunks — without this the OS shows beachball during
long saves.

Render: `cast_load` → un-escaped `CastEvent[]`, hidden `Screen` with
no IO callbacks, `RenderTexture2D` of `cols*cell_w + 2*pad` ×
`rows*cell_h + 2*pad`, `LoadImageFromTexture` + Y-flip → encoder.
Fixed `fps=15`. Skip blank frames before first cast event.

Native encoders avoid ffmpeg-not-found failures for gif/webp.
mp4/webm/apng still need ffmpeg on PATH (release archive bundles
static build on Windows; macOS/Linux still need it externally — TODO).

Snapshot at t=0 preserves codepoints + cursor only (no SGR).
One recording globally (`g_rec.active`); switching tabs works but
only originally-selected pane is captured.

## HUD (system-info overlay)

Per-pane `hud_*` snapshot (host, IP, load, mem MB, disk %, CPU
ticks/history). Drawn from `draw_tab_contents` after panes — paints
on top. Translucent black (alpha 175) + thin border. Uses
`DrawText`/`MeasureText` (bundled font, 12pt/~14px), per-line width
so the slab hugs longest line.

### Data flow
- **Local**: 1 Hz via `hud_local_poll` — `gethostname` (strip
  `.local`), `getifaddrs` first non-loopback IPv4, `getloadavg`,
  `host_statistics64(HOST_VM_INFO64)` (mac) or `/proc/meminfo
  MemAvailable` (linux), `statfs("/")` or `statvfs("/")`.
- **SSH**: `SshPty` probe thread. Every 2s opens fresh exec channel
  on same session, runs portable POSIX shell echoing `KEY=VALUE`
  lines (HOST, IP, LOAD, MEM_MB, DISK_PCT, CPU_BUSY, CPU_TOTAL,
  END=1). Probe uses `pthread_mutex_trylock` against shared session
  lock so a busy shell doesn't stall the probe — skip cycle, show
  previous snap. Probe shell is mac+linux portable (tries `hostname
  -I`, then `-i`, then ifconfig en0/en1; `/proc/stat` else
  `sysctl -n kern.cp_time`; `free -m` else `vm_stat`+pagesize).

### Customisation (Settings → HUD)
- Master toggle (`show_hud`), position (TL/TR/BL/BR via `hud_pos`).
- Per-field grid (Host/IP/Load/Memory/Disk): visible toggle,
  colour swatch (8-entry `HUD_PALETTE`, indices stable across
  releases), size −/+ (10..18 pt).
- CPU graph toggle (`hud_show_cpu`).

### CPU sparkline
60-slot ring (`hud_cpu_pct`, head, init flag) + `hud_cpu_prev_busy/_total`.
1 Hz: `hud_read_cpu_ticks` returns cumulative busy + total; pct =
`(busy_delta * 100) / total_delta`, clamped 0..100. First call
primes; history seeded with `-1`, render skips negatives. Bars
oldest→newest, height = `gh * pct / 100`. Colour: green→yellow at
0..50, yellow→red at 50..100. Recent value as text tag.
- macOS: `host_statistics(HOST_CPU_LOAD_INFO)` USER+SYSTEM+NICE busy.
- Linux: `/proc/stat` cpu line, busy = user+nice+system+irq+softirq+steal.
- Windows/web: not implemented.

### Adding a field
1. Compute in `hud_local_poll` (+ SSH probe).
2. Storage on `Pane`.
3. Through `hud_format` + `hud_show_<field>` in `AppSettings`.
4. Toggle button in `SettingsLayout.hud_field_*`.
5. Default in `app_settings_init`.

## Multi-window (unfinished)

One raylib window per process today. Cmd+N `fork+exec`s — separate
Dock icons, Cmd+\` doesn't cross. Goal: same-process multi-window.

### Validated POC
- raylib `USE_EXTERNAL_GLFW=ON` + brew `libglfw` = one GLFW runtime.
  Without this, two GLFW runtimes register the same Objective-C
  classes (`GLFWApplicationDelegate`, etc.) and Cocoa warns about
  spurious casts / mysterious crashes.
- After `InitWindow`, `glfwGetCurrentContext()` returns raylib's
  `GLFWwindow*`. `glfwCreateWindow(..., share=<that>)` = additional
  windows sharing GL context.
- Per-frame extra window: `rlDrawRenderBatchActive` →
  `glfwMakeContextCurrent(extra)` → rlViewport/Mode/Identity/Ortho
  → rlClearColor/Buffers → draw → `rlDrawRenderBatchActive` →
  `glfwSwapBuffers(extra)` → make primary current.
- POC at `/tmp/multiwin_poc3` (`/tmp/multiwin_poc2.c`) confirmed
  (only with CMake-fetched raylib; brew bottle doesn't set the flag).

### Integration attempt was reverted
Naïve "add an extra GLFWwindow at startup" looked fine compiling
but was visually chaotic — context switching + raylib's rlgl state
+ mid-frame context swaps interacting badly. ~8-12h dedicated session
needed:

1. CMakeLists `USE_EXTERNAL_GLFW ON` + `find_package(glfw3)`. Makefile
   can't use brew raylib anymore (embedded GLFW).
2. Per-window state: extract globals into a `Window` struct.
3. Bootstrap: `glfwInit + glfwCreateWindow + rlglInit + rlLoadExtensions`
   per window, share context after first.
4. Input wrapper: per-window callbacks → `WindowInput`, retarget
   `IsKeyPressed`-style API to "focused window's" state. Don't
   rewrite call sites.
5. Main loop: iterate windows, current context, input+draw+swap.
6. Cmd+N → new Window. macOS Cmd+\` works once single-process. Win:
   `SetCurrentProcessExplicitAppUserModelID` for taskbar grouping.

Don't waste time on: separate processes + shared Dock icon (Cmd+\`
stays per-process, no hook for cross-process cycling); brew
GLFW alongside brew raylib (class name collisions).

## Search-in-scrollback

`Cmd/Ctrl+F` opens per-pane search bar. Substring, ASCII-fold
case-insensitive. Enter/Down/F3 next, Shift+ prev. Esc closes,
restores `view_offset`. Cmd/Ctrl+V pastes into query.

- `Search` on `Pane` (query, matches, current). Freed in `pane_free`.
- `screen_total_rows` + `screen_cell_abs` give absolute-row history
  for `search_recompute`. Wide-char cont skipped.
- Matches: `(abs_row, col_start, col_end)`. `search_next` wraps;
  `search_scroll_to_match` centres.
- `draw_tab_contents` paints translucent yellow (current brighter).
- `p->search.active` → main loop skips `input_poll`, calls
  `search_handle_input` so keys stay out of the shell.

v1 limits: ASCII fold only; match within single row (auto-wrapped
words missed — could use `screen_view_row_wrapped`); inside
tmux/vim/less = alt screen, no scrollback (use `<prefix>[`); no
regex / search-within-selection / case toggle.

## Dirty-flag gotcha

Main loop skips `BeginDrawing`/`EndDrawing` when `dirty == false`.
**Any path mutating UI state without producing PTY output must set
`dirty = true` itself**, or the screen freezes on old state until
something else triggers a redraw.

Symptom: "Cmd+1 is sluggish" when the tab actually switched — the
loop just isn't repainting.

Unified catch in `main.c`:

```c
if (g_active != prev_active) dirty = true;
if ((int)g_ui_mode != prev_ui_mode) dirty = true;
prev_active = g_active;
prev_ui_mode = (int)g_ui_mode;
```

Catches every tab-switch chord and modal toggle. New cross-frame UI
state: extend the snapshot/compare or set `dirty = true` at mutation
site. Don't rely on the next mouse twitch.

## Idle CPU floor (macOS)

**~3.2% idle CPU is the floor with brew raylib on macOS.**
alacritty/kitty sit at 0%.

How we got here:
1. Pre-opt: 60 fps full-grid → ~18%.
2. Dirty-flag gating in `main.c`. Triggers: PTY drain, input byte,
   focus, mouse move (focused only), button/wheel, resize, title.
   **Cursor blink is intentionally NOT a trigger** — biggest win.
3. Wake cadence: 250 Hz focused (cap=0.004), 20 Hz unfocused (cap=0.05).
   Counter-intuitive: high poll rate → empty NSApp queue, near-instant
   `PollInputEvents`; lower rate → queue accumulates, ~50 ms per call.
   4 ms cap also keeps chord shortcuts feeling instant. Don't try
   adaptive sleep.

**Remaining ~5% is `glfwPollEvents` itself.** macOS dispatches the
NSApp queue per call (~50 ms), so 1 Hz = 50/1000 = 5%. Verified with
minimal repro (no rbterm logic, no draw): 4.6-6.0% CPU.

**Only way past floor: `glfwWaitEventsTimeout`** (kernel-blocked).
Failed attempts:
- Link `-lglfw` alongside brew raylib: builds but two GLFW runtimes
  register same classes; wait fn resolves to brew GLFW which has no
  window → returns instantly, 3M empty iterations/sec.
- `extern void glfwWaitEventsTimeout` from brew raylib: symbols
  hidden, link fails.

**Real fix**: rebuild raylib with `USE_EXTERNAL_GLFW=ON` (CMake
FetchContent) — same architectural unblocker as multi-window arc,
deferred for the same reason (multi-hour change touching build/init/
per-window state).

Don't re-try: per-iteration timing, thread sampling, HUD bypass —
main loop body is ~0.16 ms at 1 Hz (0.016% main thread). CPU is
charged for `PollInputEvents` work synchronous inside the call.

## SFTP upload + download

SSH tabs have **↑** / **↓** buttons. Both ride existing libssh
session — no second auth, no scp/rsync.

**Upload** (`pty_upload_*` in `pty_ssh.c`): worker thread takes
`session_lock` cooperatively, `sftp_new` → `sftp_init` →
`sftp_open(O_WRONLY|O_CREAT|O_TRUNC, 0644)` → 32 KB chunked write.
`~/foo` strips `~/` (SFTP cwd is home, no tilde expansion). If
remote is dir, append local basename.

**Download** (`pty_download_*`): mirror. `sftp_stat` first; regular
file → chunked read; directory → `sftp_download_dir_recursive`
(snapshot listing per dir before recursing — libssh SFTP isn't
reentrant on same dir handle), local mkdir 0755, accumulate
`bytes_total` for toast %.

UI: `UI_SFTP_UPLOAD` / `UI_SFTP_DOWNLOAD`. Download modal does
`pty_listdir` (dirs-first alpha) on open + path change. Double-click
folder = navigate, single + Download = download (folder ⇒ pick local
**parent** via `mac_pick_open_directory`). Per-pane `Pane.upload` /
`.download`. Toasts bottom-left, stacked. `pane_free` cancels +
joins workers before tearing down PTY. Mac native pickers in
`emoji_mac.m`.

`run.sh` exports `RBTERM_DEBUG=1` → transfer steps log to
`~/rbterm-upload.log` with libssh + sftp error codes.

## Broadcast input

`Cmd+Shift+I` (or radio-tower tab-bar button) toggles
`g_broadcast_active`. Every keystroke / paste in active tab fans
out to **every leaf**. Output stays per-pane.

Visual cues (deliberately redundant): button glows red, `BROADCAST`
pill in right cluster, 3px red border on every leaf.

Safety: button hidden when `pane_tree_count < 2`. Auto-disarms on
tab switch (same `prev_active` snapshot block as dirty-flag).
**Mouse clicks / selection drags are NOT broadcast.** Only keystrokes
and pastes.

Implementation: input section replaces per-pane `pty_write` with
tree-walk when active. Each leaf checks own `screen_bracketed_paste`.
Per-leaf `screen_scroll_reset` + selection clear inside fan-out.

Future: per-leaf "broadcast member" toggle for curated subsets.

## Per-host SSH layout

Saved hosts can predefine recursive split layouts replayed on
connect. Authored via **Save Layout from Active Tab** on SSH form's
Connection tab — writes `# rbterm-*` lines into `~/.ssh/config`:

```
Host mia
    HostName 172.238.205.61
    User rich
    # rbterm-layout: V0.50(H0.50(0,1),2)
    # rbterm-pane-0-cwd: /var/log
    # rbterm-pane-2-cmd: htop
```

Grammar: `expr := DIGIT | ('V'|'H') ratio '(' expr ',' expr ')'`,
ratio `0.NN` (0.15..0.85), integers index pane_cwds[]/pane_cmds[].
DFS pre-order so serializer + parser agree without separate numbering.
Cap `SSH_LAYOUT_MAX_PANES = 8`.

- `layout_serialize` walks `t->root`, snapshots cwds (not cmds —
  unreliable to know what's running).
- `layout_parse` is recursive descent → `LayoutNode` tree. Malformed
  → NULL → fallback to single pane.
- `layout_replay_walk` does splits in DFS pre-order, sets
  `t->active = current_leaf` before each `tab_split`, sends
  `cd "<cwd>"; <cmd>\r` via `ssh_send_init_line`.
- `tab_open_ssh(layout, pane_cwds, pane_cmds)`. Non-empty parsing
  layout owns per-leaf init; otherwise legacy `init_cwd`/`init_cmd`
  for first pane.
- Single-pane Save = no-op (clears layout string) so users don't
  clobber `init_cwd`/`init_cmd` accidentally.
- Per-pane cmd preserved across saves (re-snapshot cwd, leave cmd
  array) so hand-edited cmds stick.

## Per-host SSH startup commands

`SshProfile` / `Tab` / `SshForm` carry `init_cwd` + `init_cmd`. After
`pane_open_ssh`, `tab_open_ssh` writes `cd "<cwd>"; <cmd>\r` as first
input. Either field can be empty. cwd quoted (spaces); cmd verbatim
(pipe/chain). Persisted as `# rbterm-init-cwd:` / `# rbterm-init-cmd:`
in ssh_config.

## Settings → Launch

`AppSettings.launch[]` (max 16) drives startup tabs. Entry: `kind`
(0=local, 1=ssh) + `host` (ssh_config alias for ssh; empty for
local). Persisted as `launch.<i>=local` / `launch.<i>=ssh:<alias>`.

UI: row per entry — `[kind pill] [host picker] [▲][▼][×]`. Host
picker dropdown of saved profiles, refreshed on open via
`ssh_profiles_load`. ▲/▼ swap with neighbour, zero-width at ends,
follow open dropdown anchor across swaps. SSH connect failures
log to stderr + skip — single typo can't lock user out. SSH entries
pick up matching `SshProfile` overrides.

## Settings → Window startup modes

`AppSettings.startup_window` enum, 8 values (back-compat: legacy
FULLSCREEN remapped to BORDERLESS at load).

| mode | what |
|--|--|
| DEFAULT | leave raylib's choice |
| SMALL/MEDIUM/LARGE | `SetWindowSize` + centre on monitor |
| FILL | `MaximizeWindow()` (work area) |
| BORDERLESS | `FLAG_WINDOW_UNDECORATED` + maximize. NOT native fullscreen so Cmd+Tab cycles |
| MAXIMIZED (Own Space) | `mac_enter_native_fullscreen` — `[NSWindow toggleFullScreen:]` |

**Critical gate**: renderer-init unconditionally calls
`SetWindowSize(win_w, win_h)` to fit default 100×30 grid. Without
`window_is_explicitly_sized`, that clobbers all size presets — Med/
Large rendered identical to Default until gated. New OS-/explicit-
sized modes must add to that test.

## Quake hotkey (Cmd+CapsLock)

`STARTUP_WINDOW_BORDERLESS` → `mac_install_quake_hotkey` registers a
system-wide summon/dismiss. Fights several macOS scheduling layers:

1. **Carbon `RegisterEventHotKey`** (not just NSEvent). NSEvent
   needs Input Monitoring permission and silently fails without it;
   Carbon doesn't. Both registered — Carbon as reliable fallback.
2. **App Nap throttling**. Lost focus → Cocoa events buffer 1-2s.
   Both required: `NSProcessInfo.beginActivityWithOptions:
   UserInitiated|LatencyCritical` + `IOPMAssertion` of type
   `kIOPMAssertionTypePreventUserIdleSystemSleep`. Either alone fails.
3. **`[NSApp hide:]` deep-suspends.** Even with activity tokens,
   events stop dispatching. Use `orderOut:` per window + explicit
   `activateWithOptions:` of previous frontmost (tracked via
   `NSWorkspaceDidActivateApplicationNotification`) → run loop hot.
4. **Toggle source = `[NSApp isActive]`**, not our `g_quake_was_hidden`
   — user might Cmd+Tab away (hide path didn't run) and chord still
   needs to summon.
5. **Caps Lock state reset** via `IOHIDSetModifierLockState(...,
   kIOHIDCapsLockState, false)` so keyboard doesn't drift to ALL CAPS.

Toggle runs inline from NSEvent/Carbon block, not via a polled flag
— raylib's main loop pauses while hidden and a flag handoff was the
original 1-2s queueing bug.

Need `-framework Carbon` (Makefile + CMakeLists have it).

## Cursor colour (Settings → Cursor + per-host)

`AppSettings.cursor_color[16]` `#rrggbb`; empty = cell's natural fg.
Persisted as `cursor_color=`. Applied at `pane_open_local` via
`screen_set_cursor_color`; live updates walk every open pane.
`SshProfile.cursor_color[16]` mirror, persisted as
`# rbterm-cursor-color:`, applied in `pane_apply_tab_appearance`,
takes precedence over app-wide.

Picker: 8-tile `SSH_COLOR_PRESETS` + 9th "default" tile that clears.

## SSH key manager (Settings → Keys)

Settings tab owning `~/.ssh` — list, generate, install, delete.
Pure libssh + POSIX, no `ssh-keygen`/`ssh-copy-id` subprocess.

### Discovery — `ssh_keys_rescan`
- `opendir(~/.ssh)` + scan `*.pub`. Each → `SshKeyEntry { name, algo,
  fingerprint, pubpath, privpath, has_private, mtime }`. Cap `SSH_KEYS_MAX = 32`.
- `algo` = first whitespace-token of pub line 1.
- `fingerprint` = SHA256 from `ssh-keygen -lf` (popen, optional —
  silently empty if not on PATH).
- Sort: descending mtime, case-insensitive name tiebreak — fresh
  key tops both this list and SSH form's Key file dropdown.

### Generation — `ssh_keys_generate_native`
- ed25519 → `ssh_pki_generate(SSH_KEYTYPE_ED25519, 0, &k)`; rsa-4096
  → `SSH_KEYTYPE_RSA, 4096`. Both deprecated but the only single-
  call generators that don't need OpenSSL directly.
- `ssh_pki_export_privkey_file` + `_pubkey_file`. Then `chmod 0600`
  / `0644`.
- Sub-modal at `SettingsLayout.keygen_*` with `g_keygen_form`.

### Install — `ssh_keys_install_native`
- Fresh `ssh_session` to chosen `SshProfile` with same options as
  SSH form (HostName/User/Port/IdentityFile), `ssh_connect`,
  `ssh_session_is_known_server` for TOFU, then
  `ssh_userauth_publickey_auto(s, NULL, "")` (empty-string
  passphrase load-bearing — see tty-prompt fix below).
- Exec channel runs:
  ```sh
  umask 077 && mkdir -p ~/.ssh && touch ~/.ssh/authorized_keys
  if ! grep -qF "$KEY" ~/.ssh/authorized_keys 2>/dev/null; then
      printf '%s\n' "$KEY" >> ~/.ssh/authorized_keys
  fi
  ```
  Pubkey via `ssh_channel_write` as stdin, exit via
  `ssh_channel_get_exit_status`. Dups are no-ops.
- Status in `g_keys_status` ("auth: …", "exec: …", "host key
  changed", "Installed id_rbterm on mia.").

### Delete
Per-row × → `g_keys_delete_idx`, modal at `L.keysdel_*` shows both
absolute paths + Cancel/Delete. `unlink()` each then rescan. errno
→ status. Both `g_keys_delete_idx` + `g_keys_install_dropdown`
reset in `settings_open`.

### tty-prompt fix (`pty_ssh.c` + `main.c`)
Bug to remember: `ssh_pki_import_privkey_file(path, NULL, NULL,
NULL, &k)` lets libssh/OpenSSL fall back to OpenSSL's `Enter PEM
pass phrase:` prompt on the controlling tty (the launching shell)
— modal hangs while invisible prompt sits on launching terminal.

Fix: `rbterm_passphrase_cb` in `pty_ssh.c` wired into every
`ssh_pki_import_privkey_file`. Non-empty userdata → return passphrase;
otherwise return -1 → libssh fails the load cleanly. Either way
short-circuits the OpenSSL tty fallback. Side effect: encrypted
private keys work in-app — SSH form's Password field passes through.

For `ssh_userauth_publickey_auto` (install path): pass `""` not
`NULL` as passphrase arg. Same root cause, same outcome.

### SSH form integration
Key file field has `▼` button → dropdown sourced from same
`ssh_keys_rescan`. Pick → `g_form.key` = absolute private path.
Esc collapses dropdown without dismissing modal — only Esc with
no dropdown open cancels the form.

## Things left for later

- SSH interactive password / kbd-interactive auth (prompt through
  Screen, capture chars).
- Async SSH connect (don't freeze UI on handshake).
- Saved SSH profiles / connect history.
- DirectWrite emoji for Windows.
- Ligatures (HarfBuzz shaping).
- Scrollback reflow on resize (currently re-bucketed only).
- Linux `.desktop` file + icon install.
- Bundle ffmpeg next to mac/Linux release (Windows already does).
- **Maximized startup mode is broken.** Wired to
  `mac_enter_native_fullscreen` / `[w toggleFullScreen:nil]` but
  result isn't right. Likely same class of fix as raylib's broken
  `ToggleFullscreen` — timing the mode change vs first frame /
  ensuring screens resize. Picker only offers Default + Maximized;
  config back-compat demotes `fullscreen` → default.

## Missing features (vs iTerm2 / Alacritty / kitty)

Shopping list, not roadmap.

**Window/layout**: per-tab/pane title override surviving shell
rewrites, tab bar styles (top/bottom/left, powerline), session
restore. **Scrollback**: regex search, vi mode, jump to prev
prompt (OSC 133 needed), URL detection + click, hints kitten,
OSC 8 hyperlinks, smart selection, triple/quad-click,
rectangular Alt+drag. **Shell integration**: OSC 133 prompt
marks shipped (parser + 3px gutter badge in pad — green exit 0,
red otherwise; helper scripts at `tools/rbterm-shell-integration.{zsh,
bash}`. Future: jump-prev/next-prompt, select-last-output,
long-cmd notification — all use already-stored marks). New-tab
cwd inheritance (OSC 7 parsed, not used), command status badges,
notify-on-cmd-finish, paste guards. **Graphics**: iTerm2
`OSC 1337 ; File=…` inline images, ligatures, per-local-tab font
size, bg image/blur/transparency, cursor trail, min-contrast,
dim inactive panes, gradients. **Config**: TOML/conf/Lua file +
live reload, per-local-tab profiles, per-cwd auto-switching.
**Input**: user keybindings, leader chords, Option-as-Meta
left/right distinct, compose key, password autofill.
**Extensibility**: remote control protocol (`kitty @`), regex
triggers, watchers/event hooks, status bar segments, ssh
kitten-equivalent (ship terminfo + dotfiles). **Bell/notify**:
visual bell, audible toggle, OSC 9 / OSC 777, per-tab
unread/activity/silence. **Performance**: damage-tracked
redraw, vsync/fps cap idle, headless daemon mode.
