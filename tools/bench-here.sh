#!/usr/bin/env bash
# tools/bench-here.sh
#
# Run *inside* each terminal you want to benchmark. Auto-detects
# which terminal is hosting (via $TERM_PROGRAM), runs echo_bench,
# and writes the summary line to bench/echo-<terminal>-<stamp>.txt.
#
# Typical workflow:
#   1. Open kitty / alacritty / iTerm2 / Terminal / rbterm
#      (whichever you want to compare).
#   2. cd into this repo.
#   3. Paste:    ./tools/bench-here.sh
#   4. Repeat in the next terminal.
#   5. Once you've covered the field, run
#         ./tools/bench-summary.sh
#      to get a side-by-side table.
#
# Override the sample count with N=200 ./tools/bench-here.sh.

set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO"

N="${N:-1000}"

# Build echo_bench if missing or stale.
if [[ ! -x "$REPO/echo_bench" || \
      "$REPO/tools/echo_bench.c" -nt "$REPO/echo_bench" ]]; then
    cc -O2 -o "$REPO/echo_bench" "$REPO/tools/echo_bench.c"
fi

# Identify the host terminal. macOS-friendly mappings to readable
# slugs so the result filenames stay tidy.
detect_term() {
    local p="${TERM_PROGRAM:-}"
    case "$p" in
        iTerm.app)         echo iterm2 ;;
        Apple_Terminal)    echo terminal ;;
        rbterm)            echo rbterm ;;
        kitty|kitty.app)   echo kitty ;;
        Alacritty)         echo alacritty ;;
        WezTerm)           echo wezterm ;;
        ghostty)           echo ghostty ;;
        "")
            # Some terminals don't set TERM_PROGRAM. Probe a few env
            # vars. Last resort: ask.
            if [[ -n "${ALACRITTY_LOG:-}" ]]; then echo alacritty
            elif [[ -n "${KITTY_WINDOW_ID:-}" ]]; then echo kitty
            elif [[ -n "${WEZTERM_PANE:-}" ]]; then echo wezterm
            else echo unknown
            fi
            ;;
        *) echo "${p,,}" 2>/dev/null || echo "$p" ;;
    esac
}

term_slug="$(detect_term)"
mkdir -p "$REPO/bench"
stamp="$(date +%Y%m%d-%H%M%S)"
out="$REPO/bench/echo-${term_slug}-${stamp}.txt"

echo "Detected terminal: $term_slug"
echo "Sampling $N round-trips (echo_bench has a 5s hard cap)..."
"$REPO/echo_bench" "$N" | tee "$out"
echo
echo "Saved to $out"
