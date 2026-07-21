# Tessera — Table-Top 3D Renderer

A small, embeddable 3D renderer in **C + SDL3** for turn-based, grid-based games.
It renders a board of tiles, entities placed on grid coordinates, and transient
visual effects — plus the **transitions** between successive game states
(tiles added/removed, entities moving/reflowing, effects firing).

The engine exposes a **single C header** (`tessera.h`) designed for FFI binding
from Lua, Dart, and other languages.

---

## Locked design decisions

| Area | Decision | Rationale |
|------|----------|-----------|
| GPU backend | **SDL_GPU** (Metal on Apple, Vulkan on Linux/Android) | One codebase, no external GL deps, matches SDL3 direction |
| Shaders | Authored in HLSL, cross-compiled via **SDL_shadercross** to SPIR-V / MSL / DXIL | Single source, offline-compiled and shipped as blobs |
| Assets | **glTF 2.0** via **cgltf**; skeletal + morph animation | Blender/industry standard, matches "active animation" requirement |
| Loop | **Engine-driven** timing; caller only pushes immutable state; engine **diffs** old vs new and generates transitions | Tiny FFI surface, animation logic owned by C |
| Frame pacing | SDL3 **main-callbacks** (`SDL_AppIterate`) drives the frame; engine owns tick/animation clock | Works on iOS/Android where the OS controls the display link |
| Art direction | **Stylized flat/cel shading**, 1 directional light + ambient, simple shadows | Cheap on mobile, board-game look, fast to build |
| Grid | **Square grid**, integer `(x, y)` coords in v1; coordinate layer abstracted | Hex/iso can be added behind the same interface later |
| Math | Vendored **cglm** (header-only) | Avoids reinventing SIMD-friendly vec/mat/quat |

### Threading & memory model
- `tessera_create` optionally starts an internal render thread (desktop) **or**
  integrates with SDL main-callbacks (mobile). Both paths converge on one
  `engine_tick(dt)` that the rest of the code is written against.
- `tessera_set_state` is **thread-safe** and **deep-copies** the caller's
  snapshot into an engine-owned immutable buffer. The caller may free its memory
  immediately after the call returns. No pointers are shared across the FFI
  boundary except opaque handles and short-lived `const` inputs.
- Double/triple buffering of state: `pending` (just set) → `current` (being
  animated from) → `target`. The diff runs when a new state is promoted.

---

## Architecture (layers, bottom → top)

```
  Platform / FFI          tessera.h  (opaque TesseraEngine*, C ABI, flat structs)
  ─────────────────────────────────────────────────────────────────────────────
  Public API              lifecycle · definition registry · set_state · camera
  Orchestration           state diff → transition list → animation timeline
  Scene                   grid model · entity/effect instances · tile layout solver
  Animation               tween engine · skeletal/morph sampler · particle sim
  Renderer                SDL_GPU pipelines · passes (shadow, main) · materials
  Assets                  cgltf loader · mesh/texture upload · atlas · def registry
  Foundation             math (cglm) · memory arena/pool · logging · handles
```

### Core public objects
- **Definitions** (registered once, immutable): `TileDef`, `EntityDef`, `EffectDef`.
  A def owns GPU meshes/textures/animations and is referenced by id.
- **State** (pushed repeatedly, immutable snapshot): arrays of tile placements,
  entity placements, effect placements, and a camera pose.
- **Engine** (opaque handle): owns GPU device, def registry, current/target
  state, live animations, particle systems.

### Public API sketch (evolves per milestone — see each file)
```c
typedef struct TesseraEngine TesseraEngine;
typedef uint32_t TesseraDefId;   // 0 = invalid/none
typedef uint64_t TesseraEntityId;

// --- lifecycle ---
TesseraEngine* tessera_create(const TesseraConfig* cfg);   // native view + options
void           tessera_destroy(TesseraEngine*);
void           tessera_resize(TesseraEngine*, int w, int h, float pixel_density);
void           tessera_tick(TesseraEngine*, double dt_seconds); // manual mode only

// --- definitions ---
TesseraDefId tessera_register_tile_def(TesseraEngine*,   const TesseraTileDef*);
TesseraDefId tessera_register_entity_def(TesseraEngine*, const TesseraEntityDef*);
TesseraDefId tessera_register_effect_def(TesseraEngine*, const TesseraEffectDef*);

// --- state (deep-copied; diffed against previous) ---
void tessera_set_state(TesseraEngine*, const TesseraState*);

// --- diagnostics ---
const char* tessera_last_error(TesseraEngine*);
```

All input structs are **flat**: fixed-size fields plus `(pointer, count)` array
pairs of POD elements. No nested ownership, no callbacks required for basic use.

---

## Coordinate system
- Grid coords are integer `(x, y)`; the tile at `(0,0)` centers at world origin.
- World: right-handed, **+Y up**. Tile top surface sits at `y = 0`; tiles have a
  small thickness below. Entities stand on `y = 0`.
- `world = tile_size * (x, 0, y)` in v1 (square). The `grid.h` layer isolates this
  so hex/iso become alternate implementations.
- Camera pose = `{ focus (grid or world), zoom/distance, yaw, pitch }`, orbiting
  the focus point.

---

## Multi-entity tiles
When more than one entity occupies a tile, a **layout solver** assigns each a
sub-slot (e.g. ring / grid packing scaled to fit). When entity count on a tile
changes, remaining entities **reflow** (animated) to the new slots. Solver is
deterministic given `(tile, sorted entity ids, count)` so it's stable frame to
frame. See `03-scene-state.md`.

---

## Dependencies (all vendored under `third_party/`)
- **SDL3** (windowing, input, GPU, main-callbacks)
- **SDL_shadercross** (offline, build-time — compiles HLSL → target blobs)
- **cgltf** (glTF parsing, header-only)
- **cglm** (math, header-only)
- **stb_image** (texture decode, header-only)
- *(later, optional)* a tiny hash map / a small unit-test harness (e.g. `utest.h`)

## Build
- **CMake**, C11. Targets: `tessera` (shared + static lib), `examples/*`.
- Shaders compiled at build time by a CMake custom command → `assets/shaders/*.spv|*.msl`.
- Per-platform: macOS/iOS (Metal), Linux/Android (Vulkan). Android via
  Gradle+CMake NDK; iOS via Xcode/CMake framework.

---

## Milestones

| # | File | Outcome |
|---|------|---------|
| 0 | `00-scaffolding.md` | Repo, CMake, SDL_GPU device + window, clear frame, `create`/`destroy` |
| 1 | `01-render-foundations.md` | Math, camera, shader pipeline, first flat-shaded tile, depth |
| 2 | `02-assets-and-definitions.md` | cgltf loading, GPU upload, atlas, definition registry API |
| 3 | `03-scene-state.md` | Immutable state, deep copy, coord math, static full-board render, tile layout solver |
| 4 | `04-transitions.md` | State diff → transitions, tween engine, animated add/remove/move/reflow |
| 5 | `05-skeletal-animation.md` | Skinning + morph, per-entity animation state, blending, facing |
| 6 | `06-effects-particles.md` | Particle system, effect defs (on-add/on-remove), lifecycle |
| 7 | `07-camera-lighting-polish.md` | Camera focus transitions, cel shading, directional light, shadows |
| 8 | `08-platforms-and-ffi.md` | iOS/Android/macOS/Linux integration, threading, Dart & Lua bindings |
| 9 | `09-hardening.md` | Memory/leak audit, error model, perf budget, docs, sample game, tests |

Each milestone file lists: **Goal · Deliverables · Tasks · Public API added ·
Acceptance criteria · Risks**. Milestones are ordered so each ends with
something runnable/demoable.

---

## Definition of done (whole project)
- One header, stable C ABI, no leaks (ASan/valgrind clean on desktop).
- Runs on macOS, Linux, iOS, Android from the same core.
- A sample turn-based board demo driven from **both** Lua and Dart, showing
  tiles, moving/reflowing entities, effects, and camera transitions.
