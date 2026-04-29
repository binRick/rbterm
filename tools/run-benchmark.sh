#!/usr/bin/env bash
# tools/run-benchmark.sh
#
# Drive each installed terminal emulator through the echo_bench
# round-trip-latency test and tabulate the results.
#
# Each terminal is launched with a one-shot command that runs
# `echo_bench <N>` and writes its summary line to a per-terminal
# result file. The driver waits for the file to appear, parses the
# summary line, and prints a side-by-side table at the end.
#
# Defaults: N=1000 samples, all five competitors below. Override
# either via env (N=200 ./tools/run-benchmark.sh) or by editing
# the TERMS list at the bottom.
#
# Terminals covered:
#   - rbterm     (built locally, ~/Desktop/repos/rbterm/rbterm.app)
#   - kitty      (https://sw.kovidgoyal.net/kitty/)
#   - alacritty  (https://alacritty.org/)
#   - iterm2     (driven via AppleScript)
#   - terminal   (Apple Terminal.app, via AppleScript)
#
# Adding a terminal: write a `run_<label>` function that takes one
# arg (the absolute path of the result file the terminal must write
# echo_bench's stdout to) and arrange for the terminal to launch a
# shell that runs `$ECHO_BENCH $N > $1; exit`. Append `<label>` to
# TERMS at the bottom.

set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO"

N="${N:-1000}"
TIMEOUT_S="${TIMEOUT_S:-90}"
STAMP="$(date +%Y%m%d-%H%M%S)"
RESULT_DIR="$REPO/bench"
mkdir -p "$RESULT_DIR"

# Build echo_bench if missing or stale relative to its source.
if [[ ! -x "$REPO/echo_bench" || \
      "$REPO/tools/echo_bench.c" -nt "$REPO/echo_bench" ]]; then
    echo "+ building echo_bench"
    cc -O2 -o "$REPO/echo_bench" "$REPO/tools/echo_bench.c"
fi
ECHO_BENCH="$REPO/echo_bench"

# Common shell wrapper: each terminal runs this with $1 set to
# the per-terminal result path. The wrapper redirects stdout to
# the result file and exits cleanly so the terminal closes itself
# (Apple Terminal / iTerm2 leave the window around with a "process
# completed" banner — we live with that to avoid having to script
# their close-on-exit prefs).
WRAPPER="$(mktemp /tmp/rbterm-bench-wrapper.XXXXXX.sh)"
trap 'rm -f "$WRAPPER"' EXIT
cat > "$WRAPPER" <<EOF
#!/usr/bin/env bash
"$ECHO_BENCH" "$N" > "\$1" 2>&1
exit 0
EOF
chmod +x "$WRAPPER"

# ---------- per-terminal launchers ----------

run_rbterm() {
    local result="$1"
    # rbterm has no -e flag; spawn it with SHELL pointing at the
    # wrapper. The wrapper takes its result path as $1 — pass it via
    # env (RESULT) since SHELL only gets exec'd, not passed args.
    local shim="$(mktemp /tmp/rbterm-bench-shim.XXXXXX.sh)"
    cat > "$shim" <<EOS
#!/usr/bin/env bash
"$WRAPPER" "$result"
EOS
    chmod +x "$shim"
    SHELL="$shim" open -na "$REPO/rbterm.app" --args --cols 100 --rows 30
    # Clean up the shim once the result is written.
    (
        local waited=0
        while [[ ! -s "$result" && $waited -lt $TIMEOUT_S ]]; do
            sleep 1; waited=$((waited+1))
        done
        # Kill the rbterm we spawned (rbterm exits when shell exits,
        # but the .app may stick around in some launch states).
        pkill -f "$REPO/rbterm.app/Contents/MacOS/rbterm" 2>/dev/null || true
        rm -f "$shim"
    ) &
}

run_alacritty() {
    local result="$1"
    alacritty --hold=false --title rbterm-bench-alacritty \
              -e "$WRAPPER" "$result" &
}

run_kitty() {
    local result="$1"
    # `kitty <cmd...>` runs a one-shot. --hold would keep the
    # window open after exit; default closes it.
    kitty --override "close_on_child_death=yes" \
          "$WRAPPER" "$result" &
}

run_iterm2() {
    local result="$1"
    # AppleScript opens a fresh tab and types the command. The
    # exit at the end closes the session.
    /usr/bin/osascript <<APPLESCRIPT
tell application "iTerm"
    activate
    if (count of windows) = 0 then
        create window with default profile
    end if
    tell current window
        create tab with default profile
        tell current session
            write text "$WRAPPER $result; exit"
        end tell
    end tell
end tell
APPLESCRIPT
}

run_terminal() {
    local result="$1"
    /usr/bin/osascript <<APPLESCRIPT
tell application "Terminal"
    activate
    do script "$WRAPPER $result; exit"
end tell
APPLESCRIPT
}

# ---------- driver loop ----------

# Order: pick an order that doesn't leave the user staring at one
# terminal for too long. Native ones first (rbterm, kitty,
# alacritty close-on-exit cleanly), then the GUI-heavy ones that
# leave windows around.
TERMS=(rbterm kitty alacritty iterm2 terminal)

# macOS ships bash 3.2 which lacks associative arrays. Track summaries
# in two parallel indexed arrays keyed by the position in TERMS.
SUMMARIES=()
RESULTS=()

for term in "${TERMS[@]}"; do
    result="$RESULT_DIR/echo-${term}-${STAMP}.txt"
    rm -f "$result"
    RESULTS+=("$result")
    echo
    echo "===> driving $term ($(date +%H:%M:%S))"
    case "$term" in
        rbterm)    run_rbterm    "$result" ;;
        kitty)     run_kitty     "$result" ;;
        alacritty) run_alacritty "$result" ;;
        iterm2)    run_iterm2    "$result" ;;
        terminal)  run_terminal  "$result" ;;
        *) echo "  (unknown terminal $term — skipped)"; SUMMARIES+=("(skipped)"); continue ;;
    esac
    waited=0
    while [[ ! -s "$result" && $waited -lt $TIMEOUT_S ]]; do
        sleep 1; waited=$((waited+1))
        printf '.'
    done
    echo
    if [[ -s "$result" ]]; then
        line="$(head -1 "$result" | tr -s ' ')"
        SUMMARIES+=("$line")
        echo "    $term → $line"
    else
        SUMMARIES+=("(no result after ${TIMEOUT_S}s)")
        echo "    $term → (no result after ${TIMEOUT_S}s)"
    fi
    sleep 1
done

echo
echo "================== summary =================="
i=0
for term in "${TERMS[@]}"; do
    printf "%-12s  %s\n" "$term" "${SUMMARIES[$i]:-(missing)}"
    i=$((i+1))
done
echo
echo "(per-run result files in $RESULT_DIR/echo-<term>-${STAMP}.txt)"
