#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

# Kill any currently-running rbterm (from .app or raw binary).
if pgrep -x rbterm >/dev/null 2>&1; then
  echo "run.sh: killing running rbterm…"
  pkill -x rbterm || true
  for _ in 1 2 3 4 5 6 7 8 9 10; do
    pgrep -x rbterm >/dev/null 2>&1 || break
    sleep 0.1
  done
  pkill -9 -x rbterm 2>/dev/null || true
fi

if [[ "$(uname -s)" == "Darwin" ]]; then
  make app
  # Launching the binary directly (rather than via `open`) keeps
  # stdout/stderr attached to this terminal so you can see the SSH
  # auth trace and any libssh warnings live. macOS still reads the
  # enclosing Info.plist for Cmd+Tab label and Dock icon since the
  # binary is inside rbterm.app. `exec` replaces the shell so Ctrl+C
  # in this terminal sends SIGINT directly to rbterm.
  exec ./rbterm.app/Contents/MacOS/rbterm "$@"
else
  make
  exec ./rbterm "$@"
fi
