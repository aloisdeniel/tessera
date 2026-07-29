# Web binding (WebAssembly + WebGPU)

The web binding compiles the engine to WebAssembly with Emscripten and renders
through SDL_GPU's experimental **WebGPU** backend. The Flutter plugin
(`bindings/flutter_tessera`) embeds it behind the same `TesseraView` /
`TesseraController` API as the native platforms.

```
Dart (flutter_tessera web)               wasm module (tessera_web.js/.wasm)
  TesseraController ──ccall (ASYNCIFY)──▶ tessera_* C ABI
  scene → wire blob ────────────────────▶ tessera_state_deserialize → set_state
  requestAnimationFrame ────────────────▶ tessera_tick → SDL_GPU → WebGPU → <canvas>
```

## Building

```sh
./tools/web/build_web.sh          # full build
./tools/web/build_web.sh --clean  # from scratch
```

Prerequisites: `brew install emscripten glslang`, `cargo install naga-cli`.

The script:

1. **Stages a patched SDL** — `tools/web/patch_sdl.sh` copies `third_party/SDL`
   to `build-web/SDL-src` and applies `tools/web/sdl_webgpu.patch`. The vendored
   SDL checkout is never modified. The patch is SDL PR #16020 (experimental
   WebGPU SDL_GPU backend, zlib license) rebased onto the vendored SDL 3.4.12,
   plus fixes described in the patch header (a `//!nofilter` reflection marker
   for depth-texture bindings, and corrected pushed-uniform tracking).
2. **Builds SDL** for Emscripten with `-DSDL_WGPU=ON -DSDL_WGPU_LIB=dawn
   -DSDL_WGPU_STATIC=ON -DSDL_WGPU_LIB_BYO=ON`; the WebGPU implementation
   itself is provided at link time by Emscripten's `emdawnwebgpu` port.
3. **Generates WGSL shaders** — `tools/web/compile_wgsl.py` compiles the GLSL
   sources through glslang → SPIR-V → naga → WGSL into
   `assets/shaders/*.wgsl` (see "Shaders" below).
4. **Links `tessera_web.js` + `tessera_web.wasm`** — the engine, the patched
   SDL, and `src/platform/web/tessera_web.c` (canvas + MEMFS asset-dir shim),
   with the WGSL shaders embedded in the module's virtual filesystem. Built
   with `-sMODULARIZE` (global factory `createTesseraModule`), `-sASYNCIFY`
   (the WebGPU backend suspends during device creation and swapchain waits)
   and `-sALLOW_MEMORY_GROWTH`.
5. Copies the pair into `bindings/flutter_tessera/assets/web/`, where the
   Flutter plugin serves them as package assets.

## Shaders

WebGPU consumes WGSL. `tools/web/compile_wgsl.py` derives the WGSL set from the
same Vulkan GLSL sources used for SPIR-V, with two mechanical transforms:

- Combined `sampler2D` uniforms are split into `texture2D` + `sampler` pairs —
  SDL's WebGPU backend binds sampler slot *i* as texture `@binding(2i)` /
  sampler `@binding(2i+1)` (vertex resources in `@group(0)`, vertex uniforms
  `@group(1)`, fragment resources `@group(2)`, fragment uniforms `@group(3)`).
- `texture()` becomes `textureLod(..., 0.0)` — every engine texture is
  single-level, and explicit-LOD sampling is exempt from WGSL's
  uniform-control-flow analysis (the mesh shader samples the shadow map inside
  varying-dependent branches).

naga runs with `--keep-coordinate-space`: by default it negates clip-space Y
to translate raw-Vulkan NDC to WebGPU, but SDL_GPU shaders already use the
Metal/D3D convention (SDL flips the viewport on its Vulkan backend instead),
so the "adjustment" would mirror every pass vertically.

Depth textures (the shadow map, the DoF depth input) are tagged with a
`//!nofilter` comment; the patched backend reflects those bindings as
unfilterable-float textures with non-filtering samplers, which WebGPU requires
for depth formats. The engine already samples them with nearest samplers.

## Calling convention (JS/Dart hosts)

The module is built with ASYNCIFY: **any export may suspend, and only one call
may be in flight at a time**. Call exports with
`Module.ccall(name, ret, argTypes, args, { async: true })` and serialize all
calls behind a promise chain (the Dart wrapper
`lib/src/web/tessera_wasm.dart` does this). `uint64_t` parameters (ids,
operation ids) must be passed as JS `BigInt`.

Entry points:

- `tessera_web_create(canvas_selector, width, height, pixel_density, debug)` —
  binds the engine to an existing `<canvas>` (CSS selector), points the shader
  loader at the embedded assets, and routes engine logs to the console.
  Returns the engine pointer used by every other `tessera_*` call.
- The rest is the regular frozen C ABI from `include/tessera.h`. The host
  drives frames with `tessera_tick` from `requestAnimationFrame`, and polls
  `tessera_poll_events` / `tessera_operation_completed` instead of installing
  function-pointer callbacks.
- Scene state crosses the boundary as the engine's own serialized blob format
  (`tessera_state_deserialize` → `tessera_set_state`), so hosts never build
  in-memory `TesseraState` structs in wasm memory.

## Flutter usage

`TesseraView` works unchanged on web (one live view per page — SDL binds one
canvas). The plugin loads `tessera_web.js` on first use; apps need no manual
setup beyond depending on `flutter_tessera` and building with a WebGPU-capable
browser (Chrome/Edge 113+, Safari/Firefox behind flags as of mid-2026).

## Known limitations

- **Experimental backend**: SDL PR #16020 is a draft; mipmap generation,
  stencil, and GPU→CPU downloads are unimplemented or unreliable upstream.
  Consequently `tessera_capture_png` / `tessera_render_rgba` (synchronous
  readback) are not supported on web — present-to-canvas is the only path.
- One engine / one canvas per page.
- Audio starts after the first user gesture (browser autoplay policy).
