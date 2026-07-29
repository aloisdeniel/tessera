# Tessera

A small, embeddable **3D renderer in C + SDL3** for turn-based, grid-based
games. It renders a board of tiles, entities placed on grid coordinates, and
transient visual effects — and animates the **transitions** between successive
game states (tiles added/removed, entities moving/reflowing, effects firing).

The engine is **state-driven**: the host registers immutable definitions and
pushes immutable state snapshots; the engine deep-copies each snapshot, diffs it
against the previous one, and owns all the animation logic. The FFI surface is a
single C header, `include/tessera.h`.

## Status

Built and verified on **macOS (Metal)**; structured cross-platform (Vulkan /
iOS / Android) per the plan. Backends other than Metal are not verifiable in the
current dev environment (no `SDL_shadercross`), so shaders ship as MSL and are
loaded directly at runtime; the HLSL→SPIR-V/DXIL build path is left for CI.

| Milestone | State |
|---|---|
| 0 Scaffolding, GPU device + window | ✅ builds & runs |
| 1 Math, camera, shaders, first tile, depth | ✅ verified |
| 2 glTF loading, textures, atlas, registry | ✅ implemented |
| 3 Immutable state, deep copy, grid, layout solver | ✅ implemented |
| 4 State diff, tween engine, transitions | ✅ implemented |
| 5–9 Skinning, particles, camera polish, FFI, hardening | 🚧 scaffolded / partial |

## Build

```sh
brew install sdl3                         # macOS (Linux: libsdl3-dev)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
./build/example_hello                     # windowed demo board
./build/example_hello --shot out.png      # headless PNG capture
./build/example_showcase                  # bigger board, cycle states + camera
```

### `example_showcase` controls

A larger board with a list of predefined states you step through and watch the
transitions animate live:

| Key | Action |
|-----|--------|
| **Space** | advance to the next predefined state (board grows/shrinks, units hop, stack, reflow, spawn/despawn) |
| **C** | glide the camera to focus the next entity (loops around) |
| **R** | return the camera to the overview pose |
| **↑ ↓ ← →** | orbit the camera |
| **Esc** | quit |

`./build/example_showcase --demo <dir>` runs the sequence headless and writes
PNGs of each stage.

## Documentation

- [Getting started](docs/getting-started.md)
- [State model & transitions](docs/state-model.md)
- [FFI & bindings (Dart + Lua)](docs/ffi.md)
- [Web binding (WebAssembly + WebGPU)](docs/web.md)

## Dependencies (vendored under `third_party/`)

SDL3 (system), cglm, cgltf, stb_image, stb_image_write — see
[`plan/`](plan/README.md) for the full design.
