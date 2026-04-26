#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

# Fail early with a platform-specific hint if `make` isn't installed.
# On macOS this usually means Xcode Command Line Tools aren't set up;
# on Linux it's a missing build-essential / base-devel package.
if ! command -v make >/dev/null 2>&1; then
  echo "run.sh: 'make' not found."
  case "$(uname -s)" in
    Darwin)
      echo "  Install Apple's command line tools:"
      echo "    xcode-select --install"
      ;;
    Linux)
      if command -v apt-get >/dev/null 2>&1; then
        echo "  Debian/Ubuntu:  sudo apt-get install -y build-essential"
      elif command -v dnf >/dev/null 2>&1; then
        echo "  Fedora/RHEL:    sudo dnf groupinstall -y 'Development Tools'"
      elif command -v pacman >/dev/null 2>&1; then
        echo "  Arch:           sudo pacman -S --needed base-devel"
      elif command -v zypper >/dev/null 2>&1; then
        echo "  openSUSE:       sudo zypper install -t pattern devel_basis"
      else
        echo "  Install your distro's build tools meta-package (make, gcc/clang)."
      fi
      ;;
    *)
      echo "  Install 'make' using your platform's package manager."
      ;;
  esac
  exit 1
fi

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

# run.sh is the dev launcher — turn on the debug switches that
# shouldn't burden a normal `open -a rbterm` user. Today that's
# just RBTERM_DEBUG, which routes SFTP-upload tracing to
# ~/rbterm-upload.log; keep adding flags here as more debug-only
# instrumentation lands.
export RBTERM_DEBUG=1

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
