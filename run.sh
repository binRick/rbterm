#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

# Kill any currently-running rbterm (from .app or raw binary).
if pgrep -x rbterm >/dev/null 2>&1; then
  echo "run.sh: killing running rbterm…"
  pkill -x rbterm || true
  # Wait briefly for it to exit so the new launch isn't fighting the old one.
  for _ in 1 2 3 4 5 6 7 8 9 10; do
    pgrep -x rbterm >/dev/null 2>&1 || break
    sleep 0.1
  done
  pkill -9 -x rbterm 2>/dev/null || true
fi

if [[ "$(uname -s)" == "Darwin" ]]; then
  make app
  # Launch the .app via LaunchServices so Cmd+Tab picks up the bundle name + icon.
  open -n ./rbterm.app --args "$@"
else
  make
  exec ./rbterm "$@"
fi
