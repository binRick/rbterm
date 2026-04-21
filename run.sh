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
  # -W waits for the app to exit; --stdout/--stderr pipe rbterm's output
  # back to this terminal so ssh-auth logs, font-load messages, and any
  # libssh warnings stream here while you're using the app.
  exec open -n -W \
       --stdout=/dev/stdout \
       --stderr=/dev/stderr \
       ./rbterm.app --args "$@"
else
  make
  exec ./rbterm "$@"
fi
