#!/usr/bin/env bash
#
# publish_ios_sim.sh — build the iOS *Simulator* native slices with SDL_GPU's
# Metal hardware-family gate relaxed, then leave the SDL submodule pristine.
#
# Why this script exists
# ----------------------
# SDL_GPU's Metal backend requires MTLGPUFamilyApple3 (see the family check in
# third_party/SDL/src/gpu/metal/SDL_gpu_metal.m, METAL_CreateDevice). The iOS
# Simulator never advertises that family — even on Apple Silicon — so
# SDL_CreateGPUDevice fails there and the engine can't come up ("Device does not
# meet the hardware requirements for SDL_GPU Metal"). A *physical device* passes
# the check unmodified; only the Simulator needs help.
#
# This script makes the Simulator usable by TEMPORARILY relaxing that one check
# for TARGET_OS_SIMULATOR, building the simulator archives against the patched
# source, and then reverting the file so the vendored SDL tree stays byte-for-byte
# clean (git status clean). The patch is a throwaway local build shim: it is never
# committed to SDL and never contributed upstream — it exists only inside this
# build, between the patch and the revert. A dedicated EXIT trap restores the file
# even if the build fails or the script is interrupted.
#
# It writes the same three archives publish.sh produces for the simulator, into
# the same place the plugin's build manifests read by default:
#
#   bindings/flutter_tessera/native/ios-simulator/
#     libtessera.a  libtessera_thirdparty.a  libSDL3.a   (arm64 + x86_64)
#
# After running this, `flutter build ios --simulator` (in bindings/flutter_tessera/
# example) links these slices and the engine boots in the Simulator.
#
# Usage:
#   ./publish_ios_sim.sh            build the patched simulator slices
#   ./publish_ios_sim.sh --jobs 8   parallelism for the native builds
#   ./publish_ios_sim.sh --clean    wipe this script's build trees first
#
# NOTE: This does NOT touch the macOS or iOS *device* slices, and does NOT rebuild
# the xcframeworks. Run publish.sh for a full, unpatched distribution.
#
set -euo pipefail

# ---------------------------------------------------------------------------
# Config & argument parsing
# ---------------------------------------------------------------------------
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIST_ROOT="$REPO_ROOT/dist"
BUILD_ROOT="$DIST_ROOT/build"                 # shared with publish.sh (cache reuse)
SDL_SRC="$REPO_ROOT/third_party/SDL"
SDL_METAL="$SDL_SRC/src/gpu/metal/SDL_gpu_metal.m"
SDL_METAL_REL="src/gpu/metal/SDL_gpu_metal.m" # path relative to the SDL repo root
NATIVE_OUT="$REPO_ROOT/bindings/flutter_tessera/native/ios-simulator"

JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
DO_CLEAN=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --jobs)    JOBS="$2"; shift 2 ;;
    --clean)   DO_CLEAN=1; shift ;;
    -h|--help) sed -n '2,40p' "$0"; exit 0 ;;
    *) echo "publish_ios_sim.sh: unknown argument '$1' (see --help)" >&2; exit 2 ;;
  esac
done

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
log()  { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
step() { printf '\033[1;35m  ·\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31mpublish_ios_sim.sh: %s\033[0m\n' "$*" >&2; exit 1; }

require() { command -v "$1" >/dev/null 2>&1 || die "'$1' is required but not on PATH"; }

require cmake
require xcodebuild
require lipo
require perl
[[ "$(uname -s)" == "Darwin" ]] || die "must run on macOS (Metal/Apple toolchain required)"
[[ -f "$SDL_METAL" ]] || die "vendored SDL source not found at $SDL_METAL"
git -C "$SDL_SRC" rev-parse --is-inside-work-tree >/dev/null 2>&1 \
  || die "$SDL_SRC is not a git checkout; refusing to patch without a clean revert path"

if [[ "$DO_CLEAN" == 1 ]]; then
  log "Cleaning simulator build trees"
  rm -rf "$BUILD_ROOT/sdl-ios-simulator" "$BUILD_ROOT/tessera-ios-simulator"
fi
mkdir -p "$BUILD_ROOT" "$NATIVE_OUT"

# ---------------------------------------------------------------------------
# Patch SDL for the Simulator, guaranteeing a clean revert on any exit
# ---------------------------------------------------------------------------
# git is the source of truth for "clean": revert restores the file exactly to the
# committed version, so the submodule is left pristine regardless of what happened.
restore_sdl() {
  git -C "$SDL_SRC" checkout -- "$SDL_METAL_REL" 2>/dev/null || true
}
trap restore_sdl EXIT INT TERM

# Start from a pristine file (also recovers from a previously interrupted run).
restore_sdl

if ! git -C "$SDL_SRC" diff --quiet -- "$SDL_METAL_REL"; then
  die "$SDL_METAL_REL is dirty even after checkout; resolve the SDL tree first"
fi

log "Patching SDL_gpu_metal.m for the iOS Simulator (temporary)"
# Relax ONLY the simulator path: replace the single unique MTLGPUFamilyApple3 gate
# with a TARGET_OS_SIMULATOR-guarded branch. On a device TARGET_OS_SIMULATOR is 0,
# so the original family check is preserved verbatim; on the Simulator it is 1, so
# the engine is allowed to create its GPU device. <Metal/Metal.h> (already included
# by this file) pulls in <TargetConditionals.h>, so the macro is defined here.
perl -0777 -pi -e '
  my $n = s{^([ \t]+)hasHardwareSupport = \[device supportsFamily:MTLGPUFamilyApple3\];$}
           {#if TARGET_OS_SIMULATOR\n${1}hasHardwareSupport = true; // publish_ios_sim.sh: iOS Simulator lacks MTLGPUFamilyApple3\n#else\n${1}hasHardwareSupport = [device supportsFamily:MTLGPUFamilyApple3];\n#endif}m;
  die "target line (MTLGPUFamilyApple3 gate) not found\n" unless $n == 1;
' "$SDL_METAL" || die "failed to apply the Simulator patch (SDL source may have changed)"

grep -q '#if TARGET_OS_SIMULATOR' "$SDL_METAL" || die "patch verification failed"
step "patched (will be reverted on exit)"

# ---------------------------------------------------------------------------
# Build the simulator slices (arm64 + x86_64), mirroring publish.sh's iOS build
# ---------------------------------------------------------------------------
ARCHS="arm64;x86_64"
SYSROOT="iphonesimulator"
SDL_B="$BUILD_ROOT/sdl-ios-simulator"
TES_B="$BUILD_ROOT/tessera-ios-simulator"

log "Building iOS Simulator slices ($ARCHS)"

step "SDL3 (static, patched)"
cmake -S "$SDL_SRC" -B "$SDL_B" -DCMAKE_BUILD_TYPE=Release \
      -DSDL_TEST_LIBRARY=OFF -DSDL_EXAMPLES=OFF \
      -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT="$SYSROOT" \
      -DCMAKE_OSX_ARCHITECTURES="$ARCHS" \
      -DSDL_SHARED=OFF -DSDL_STATIC=ON >/dev/null
cmake --build "$SDL_B" --config Release -j "$JOBS" >/dev/null

step "libtessera (static)"
cmake -S "$REPO_ROOT" -B "$TES_B" -DCMAKE_BUILD_TYPE=Release \
      -DTESSERA_BUILD_EXAMPLES=OFF -DTESSERA_BUILD_TESTS=OFF \
      -DSDL3_DIR="$SDL_B" \
      -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT="$SYSROOT" \
      -DCMAKE_OSX_ARCHITECTURES="$ARCHS" \
      -DTESSERA_BUILD_SHARED=OFF >/dev/null
cmake --build "$TES_B" --config Release -j "$JOBS" --target tessera >/dev/null

# ---------------------------------------------------------------------------
# Install into the plugin's native/ios-simulator dir (the SPM/podspec default)
# ---------------------------------------------------------------------------
step "Installing archives into native/ios-simulator"
cp -f "$TES_B/Release-$SYSROOT/libtessera.a" "$NATIVE_OUT/"
cp -f "$TES_B/Release-$SYSROOT/libtessera_thirdparty.a" "$NATIVE_OUT/"
SDL_A="$(find "$SDL_B" -name 'libSDL3.a' -path '*Release*' | head -1)"
[[ -n "$SDL_A" ]] || die "SDL3 simulator archive not found under $SDL_B"
cp -f "$SDL_A" "$NATIVE_OUT/libSDL3.a"

step "ios-simulator: $(lipo -archs "$NATIVE_OUT/libtessera.a")"

# restore_sdl runs here via the EXIT trap.
log "Done — SDL submodule reverted; simulator slices in native/ios-simulator/"
printf '    %s\n' "$NATIVE_OUT"
