#!/usr/bin/env bash
# Stage a WebGPU-enabled copy of SDL for the web build.
#
# The vendored checkout in third_party/SDL is never modified: this script
# copies it to build-web/SDL-src and applies tools/web/sdl_webgpu.patch
# (the SDL PR #16020 WebGPU backend, rebased onto SDL 3.4.12) to the copy.
#
#   ./tools/web/patch_sdl.sh [--force]
#
# --force re-creates the staged copy even if it is already patched.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
src="$root/third_party/SDL"
dst="$root/build-web/SDL-src"
patch_file="$here/sdl_webgpu.patch"

[ -f "$src/include/SDL3/SDL_gpu.h" ] || { echo "error: $src is not an SDL checkout" >&2; exit 1; }
[ -f "$patch_file" ] || { echo "error: missing $patch_file" >&2; exit 1; }

if [ -d "$dst" ] && [ "${1:-}" != "--force" ]; then
  if [ -f "$dst/src/gpu/webgpu/SDL_gpu_webgpu.c" ]; then
    echo "SDL already staged and patched at $dst (use --force to redo)"
    exit 0
  fi
fi

rm -rf "$dst"
mkdir -p "$(dirname "$dst")"
cp -R "$src" "$dst"
git -C "$dst" apply "$patch_file"
echo "Patched SDL staged at $dst (third_party/SDL untouched)"
