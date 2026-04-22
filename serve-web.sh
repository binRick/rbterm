#!/usr/bin/env bash
# Build the emscripten (wasm) version of rbterm, copy it into site/,
# and serve site/ over HTTP so you can open it in a browser.
#
#   ./serve-web.sh            # defaults: rebuild + serve on :8765
#   PORT=9000 ./serve-web.sh  # pick a different port
#   ./serve-web.sh --no-build # skip cmake --build, just reserve + serve
set -euo pipefail
cd "$(dirname "$0")"

PORT="${PORT:-8765}"
BUILD=1
for arg in "$@"; do
  case "$arg" in
    --no-build) BUILD=0 ;;
    -h|--help)
      sed -n '2,10p' "$0"; exit 0 ;;
  esac
done

# Homebrew's python3 (3.14) is what emscripten needs; system python3 is
# 3.9 and emcc.py refuses to run. Put /opt/homebrew/bin first so `emcc`
# finds a modern python, and our server below uses it too.
export PATH="/opt/homebrew/bin:$PATH"

if [[ ! -d build-web ]]; then
  echo "serve-web.sh: build-web/ missing — configuring emscripten build…"
  EMSDK_CMAKE="$(brew --prefix emscripten)/libexec/cmake/Modules/Platform/Emscripten.cmake"
  cmake -S . -B build-web -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$EMSDK_CMAKE" \
    -DCMAKE_BUILD_TYPE=Release
fi

if (( BUILD )); then
  echo "serve-web.sh: building wasm…"
  cmake --build build-web
fi

mkdir -p site
cp build-web/rbterm.js build-web/rbterm.wasm build-web/rbterm.data site/

# Free the port if a previous instance is still holding it (common when
# re-running this script). lsof is silent if nothing matches.
if lsof -ti tcp:"$PORT" >/dev/null 2>&1; then
  echo "serve-web.sh: freeing port $PORT…"
  lsof -ti tcp:"$PORT" | xargs kill -9 2>/dev/null || true
  sleep 0.2
fi

URL="http://localhost:$PORT/"
echo
echo "  rbterm web → $URL"
echo "  Ctrl-C to stop."
echo

# Try to open the browser automatically; no-op if that fails.
if command -v open >/dev/null 2>&1; then
  (sleep 0.5 && open "$URL") >/dev/null 2>&1 &
fi

cd site
exec python3 -m http.server "$PORT"
