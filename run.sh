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

if [[ "$(uname -s)" == "Darwin" ]]; then
  # Multi-window needs raylib built with USE_EXTERNAL_GLFW so it
  # links against brew's libglfw rather than its own bundled copy
  # (the Makefile path uses brew's prebuilt raylib which has GLFW
  # bundled — two GLFW runtimes in one process produce
  # GLFWApplicationDelegate class-collision warnings and break
  # secondary windows). CMake fetches raylib from source and
  # passes the flag, so this is the build path that works.
  cmake -S . -B build >/dev/null
  cmake --build build -j

  # Hand-package the .app so macOS sees a Bundle ID + icon
  # (Cmd+Tab shows "rbterm" with the rbterm icon, not "exec").
  # We inline this rather than calling `make app` because the
  # Makefile target's deps trigger a recompile against brew raylib
  # which fails on multi-window symbols. Icon + Info.plist still
  # come from the build the Makefile produced earlier — if you
  # ever clean assets/, run `make app` once to regenerate, then
  # this script keeps using the existing assets.
  if [[ ! -f assets/rbterm.icns || ! -f Info.plist ]]; then
    echo "run.sh: assets/rbterm.icns or Info.plist missing — running 'make app' once to bootstrap…"
    cp build/rbterm rbterm
    make app
  else
    rm -rf rbterm.app
    mkdir -p rbterm.app/Contents/MacOS rbterm.app/Contents/Resources
    cp build/rbterm rbterm.app/Contents/MacOS/rbterm
    cp assets/rbterm.icns rbterm.app/Contents/Resources/rbterm.icns
    cp Info.plist rbterm.app/Contents/Info.plist
    touch rbterm.app
  fi
  exec ./rbterm.app/Contents/MacOS/rbterm "$@"
else
  cmake -S . -B build >/dev/null
  cmake --build build -j
  exec ./build/rbterm "$@"
fi
