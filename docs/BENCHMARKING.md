# Benchmarking rbterm against other terminals

`make bench` runs [vtebench](https://github.com/alacritty/vtebench)
inside whatever terminal launched it. The benchmark times how fast
the terminal drains its PTY through twelve escape-sequence streams
(dense cells, scrolling regions, cursor motion, sync, unicode).
Results land in `bench/<host>-<term>-<timestamp>.dat`.

To compare across terminals you run the **same** `make bench`
command from inside each terminal, then `make bench-plot` overlays
every `.dat` it finds.

> **Crucial caveat**: vtebench measures *one* axis — PTY drain rate.
> It says nothing about input latency, frame pacing, GPU utilisation,
> startup time, or memory. Don't use it as the only signal.

---

## One-time setup

```bash
# vtebench is a Rust binary. First run builds it (~4s with cargo cache):
make third_party/vtebench/target/release/vtebench

# Plotting needs gnuplot; only required for `make bench-plot`:
brew install gnuplot
```

> `make bench` clears any previous `target/release/vtebench` only if
> you run `make bench-clean` — the binary is reused across runs.

---

## How auto-tagging works

`TERM_TAG` defaults to lowercased `$TERM_PROGRAM` with spaces and
dots stripped to dashes. Most terminals set `$TERM_PROGRAM`
themselves, so for the typical run you don't need to touch it:

| Terminal     | What it sets `TERM_PROGRAM` to | Tag you'll get |
|--------------|--------------------------------|----------------|
| rbterm       | `rbterm`                       | `rbterm`       |
| iTerm2       | `iTerm.app`                    | `iterm-app`    |
| Apple Terminal | `Apple_Terminal`             | `apple_terminal` |
| WezTerm      | `WezTerm`                      | `wezterm`      |
| Tabby        | `Tabby`                        | `tabby`        |

Two terminals don't set it and need an explicit override:

| Terminal  | Set this              |
|-----------|-----------------------|
| Alacritty | `TERM_TAG=alacritty`  |
| Kitty     | `TERM_TAG=kitty`      |

If you're running inside tmux, `$TERM_PROGRAM` will be `tmux` — set
`TERM_TAG` explicitly to the host terminal name, or detach first.

---

## Running the suite in each terminal

**Open each terminal natively** (not via tmux, not via a shell
inside another terminal). For each, navigate to the rbterm repo
and run the command shown.

### rbterm

```bash
cd /path/to/rbterm
make bench           # auto-tags as "rbterm"
```

### iTerm2

```bash
cd /path/to/rbterm
make bench           # auto-tags as "iterm-app"
```

### Apple Terminal (Terminal.app)

```bash
cd /path/to/rbterm
make bench           # auto-tags as "apple_terminal"
```

> If you wanted "iTerm" classic (the unmaintained pre-2010 fork),
> note that `iTerm.app` here refers to **iTerm2**, the modern
> maintained build. The original "iTerm" hasn't shipped in 15
> years and isn't worth benching against.

### Kitty

```bash
cd /path/to/rbterm
TERM_TAG=kitty make bench
```

### Alacritty

```bash
cd /path/to/rbterm
TERM_TAG=alacritty make bench
```

Each run takes ~2 minutes (12 benchmarks × ~10s each) and writes
one `.dat` file under `bench/`.

---

## Plotting the results

```bash
make bench-plot         # → bench/summary.svg
open bench/summary.svg  # macOS
```

`bench-plot` calls vtebench's bundled `gnuplot/summary.sh` over
**every** `.dat` file in `bench/`, so colour-coded bars per
terminal-per-benchmark show up automatically. To remove a stale run
from the chart, delete its `.dat` file before running `bench-plot`.

If you want a per-benchmark detailed breakdown (sample distributions,
not just means), use vtebench's `detailed.sh`:

```bash
cd third_party/vtebench
./gnuplot/detailed.sh ../../bench/*.dat ../../bench/detailed/
```

This drops one SVG per benchmark into `bench/detailed/`.

---

## Quick text comparison without gnuplot

If you just want a side-by-side mean-times table without installing
gnuplot, this Python one-liner reads every `.dat` and prints a
table:

```bash
python3 - <<'PY'
import glob, os
runs = []
for fp in sorted(glob.glob("bench/*.dat")):
    with open(fp) as f:
        header = f.readline().split()
        rows = []
        for line in f:
            try: rows.append([float(x) for x in line.split()])
            except ValueError: pass
    if rows:
        runs.append((os.path.basename(fp), header,
                     [sum(c)/len(c) for c in zip(*rows)]))
w = max(len(b) for b in runs[0][1]) + 2
print(f"{'benchmark':<{w}}", *(f"{r[0][:24]:>26}" for r in runs))
for i, b in enumerate(runs[0][1]):
    print(f"{b:<{w}}", *(f"{r[2][i]:>23.2f}ms" for r in runs))
PY
```

---

## Cleaning up

```bash
make bench-clean        # rm bench/  &&  cargo clean in vtebench
```

This nukes both the `.dat` files and vtebench's build artefacts.
Run if you suspect a stale build is skewing numbers (vtebench
itself is part of what's being timed since it's the source of the
escape streams; an old build with different output would invalidate
older `.dat` files).

---

## Latency benchmarking with Typometer

vtebench measures PTY-drain throughput. The other axis users
actually *feel* is **keystroke-to-pixel latency** — how long
between hitting a key and seeing the character appear. A terminal
can have great drain throughput and still feel sluggish if its
render loop isn't tuned, and vice-versa.

[Pavel Fatin's Typometer](https://pavelfatin.com/typometer/) is
the standard tool for this. It paints a coloured square at the
moment of a synthetic keypress and watches the screen for the
rendered character to appear, sampling many times to give a
distribution.

```bash
make latency-bench
```

The first run downloads Typometer (~5 MB) into
`tools/typometer/`, checks for Java, and launches the GUI.
Subsequent runs reuse the cached download.

### Procedure (interactive — Typometer is a GUI)

1. **Open the terminal** you want to test (rbterm / alacritty /
   iTerm2 / …) at a comfortable size.
2. **Click into the terminal once** so its text cursor isn't
   blinking under the area Typometer will sample (a blinking
   cursor is just noise the diff has to filter out).
3. **Position the Typometer window** over a quiet patch of the
   terminal — anywhere that isn't actively being redrawn.
4. **Hit Typometer's "Measure" button**. Sit on your hands for
   ~30 seconds. Don't touch the keyboard, mouse, or any other
   window while it records.
5. **Read the result** — Typometer reports min, avg, median, and
   max latency in milliseconds. Lower = snappier.
6. **Repeat for each terminal** you want to compare. Screenshot
   or note the results manually; there's no automatic export.

### Permissions (macOS)

The first launch will prompt for two permissions:

- **Accessibility** — to send synthetic keystrokes (System
  Settings → Privacy & Security → Accessibility).
- **Screen Recording** — to capture the pixels under its window
  (System Settings → Privacy & Security → Screen Recording).

Grant both, then `make latency-bench` again. macOS may also
require a re-grant after every Typometer download (the binary
identity changes), so if it stops working after a `make
latency-bench-clean`, re-approve.

### Java

Typometer is a JVM app. Install Java once, system-wide:

```bash
# macOS
brew install --cask temurin

# Linux (Debian/Ubuntu)
sudo apt install default-jre
```

### Cleaning up

```bash
make latency-bench-clean   # rm tools/typometer/
```

### Realistic expectations

- A well-tuned native terminal lands around **10–25 ms** end-to-end
  on M-series macOS at 60 Hz. Below 16.7 ms means you're getting
  the keypress drawn within one display frame.
- Electron-based terminals (Hyper, etc.) typically sit at
  40–80 ms.
- Vsync gates the floor — at 60 Hz you can't be faster than
  ~8 ms (half a frame) without VRR / 120 Hz hardware.
- Run on AC power. Laptops on battery throttle the GPU/scheduler
  and add 5–15 ms of jitter.

---

## Notes on what the numbers mean

- vtebench writes 1 MiB of escape-sequence payload per sample and
  measures wall time from the first byte to the last byte being
  read off the PTY by the terminal. That's a **drain throughput**
  number, expressed as ms-per-MiB.
- Lower is better. A terminal that locks up briefly *will* show a
  longer time even if its rendering "looks" smooth.
- vtebench's writer keeps the PTY full; if a terminal has a small
  receive buffer or blocks on render, that backpressure shows up
  as inflation here.
- A terminal that drops bytes (does a `read()` and discards) would
  *win* this benchmark. None of the terminals listed here do that —
  they all process every byte through their VT parser.
- Standard deviations on the suite are typically small (<1ms);
  variance >2ms between runs in the same terminal usually means
  the host is doing something else heavy (e.g. CI builds, tmux
  redraws, browser tab loading a YouTube ad).

Run each terminal at the **same window size** — e.g. 80×24 or
120×40. The escape streams are sized off the terminal's reported
geometry, so different sizes produce different stream lengths and
the comparison is unfair.
