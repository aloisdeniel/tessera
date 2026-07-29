#!/usr/bin/env bash
# Build the Tessera web binding: engine + WebGPU-patched SDL compiled to
# WebAssembly, packaged as a modularized tessera_web.js/.wasm pair with the
# WGSL shaders embedded in MEMFS.
#
#   ./tools/web/build_web.sh [--clean]
#
# Requires: emscripten (brew install emscripten), glslangValidator
# (brew install glslang), naga (cargo install naga-cli).
#
# Steps:
#   1. Stage a WebGPU-enabled SDL copy (tools/web/patch_sdl.sh; the
#      third_party/SDL checkout itself is never modified).
#   2. Build SDL for Emscripten with the WebGPU GPU backend.
#   3. Regenerate the WGSL shaders and stage them for --embed-file.
#   4. Generate the wasm export list from the public C API.
#   5. Build the engine + src/platform/web/tessera_web.c into tessera_web.js.
#   6. Copy the result into bindings/flutter_tessera/assets/web/.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
web="$root/build-web"

if ! command -v emcmake >/dev/null 2>&1; then
  for p in /opt/homebrew/opt/emscripten/bin /usr/local/opt/emscripten/bin; do
    [ -x "$p/emcmake" ] && export PATH="$p:$PATH" && break
  done
fi
command -v emcmake >/dev/null 2>&1 || { echo "error: emscripten not found (brew install emscripten)" >&2; exit 1; }
export PATH="$HOME/.cargo/bin:$PATH"

if [ "${1:-}" = "--clean" ]; then
  rm -rf "$web"
fi

# 1. Patched SDL copy.
"$here/patch_sdl.sh"

# 2. SDL for Emscripten with the WebGPU backend. Dawn is provided at final
#    link time by Emscripten's emdawnwebgpu port (SDL_WGPU_LIB_BYO).
if [ ! -f "$web/sdl-build/libSDL3.a" ]; then
  emcmake cmake -S "$web/SDL-src" -B "$web/sdl-build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DSDL_SHARED=OFF -DSDL_STATIC=ON \
    -DSDL_TEST_LIBRARY=OFF -DSDL_EXAMPLES=OFF \
    -DSDL_WGPU=ON -DSDL_WGPU_LIB=dawn -DSDL_WGPU_STATIC=ON -DSDL_WGPU_LIB_BYO=ON
  cmake --build "$web/sdl-build" -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)"
fi

# 3. WGSL shaders, staged alone so --embed-file only ships .wgsl.
python3 "$here/compile_wgsl.py"
rm -rf "$web/shaders-staged"
mkdir -p "$web/shaders-staged"
cp "$root/assets/shaders/"*.wgsl "$web/shaders-staged/"

# 4. Export list: every public tessera_* entry point plus the web shim and the
#    allocator (the host builds structs directly in wasm memory).
exports="$web/exports.json"
{
  printf '["_malloc","_free","_tessera_web_create"'
  grep -o 'TESSERA_API[^(;]*[ *]\(tessera_[a-z0-9_]*\)(' "$root/include/tessera.h" \
    | sed 's/.*[ *]\(tessera_[a-z0-9_]*\)($/\1/' | sort -u \
    | while read -r sym; do printf ',"_%s"' "$sym"; done
  printf ']\n'
} > "$exports"

# 5. Engine + web shim -> tessera_web.js/.wasm.
emcmake cmake -S "$root" -B "$web/tessera-build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DSDL3_DIR="$web/sdl-build" \
  -DTESSERA_BUILD_SHARED=OFF -DTESSERA_BUILD_EXAMPLES=OFF -DTESSERA_BUILD_TESTS=OFF \
  -DTESSERA_WEB_EXPORTS_FILE="$exports" \
  -DTESSERA_WEB_SHADER_DIR="$web/shaders-staged"
cmake --build "$web/tessera-build" --target tessera_web -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)"

# 6. Ship into the Flutter plugin.
out="$root/bindings/flutter_tessera/assets/web"
mkdir -p "$out"
cp "$web/tessera-build/tessera_web.js" "$web/tessera-build/tessera_web.wasm" "$out/"
echo
echo "Web binding built:"
ls -la "$out"
