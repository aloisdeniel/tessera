# Milestone 1 — Rendering foundations: math, camera, shaders, first tile

**Goal:** Render a single hardcoded, flat-shaded 3D tile (a thin box) with a
perspective orbit camera and a depth buffer, using a real SDL_GPU pipeline fed by
cross-compiled shaders. Establishes the rendering conventions everything reuses.

## Deliverables
- Math conventions locked (via cglm): right-handed, +Y up, column-major matrices.
- A **camera** with orbit controls (`focus`, `distance/zoom`, `yaw`, `pitch`) →
  view + projection matrices.
- Build-time **shader pipeline**: HLSL → SPIR-V/MSL via SDL_shadercross, blobs
  loaded at runtime and matched to backend.
- One graphics pipeline (vertex + fragment), a depth-stencil texture, uniform
  buffers for per-frame (camera) and per-object (model matrix) data.
- A rotating flat-shaded tile on screen.

## Tasks
1. **Math layer** (`core/math.h`): thin typedefs over cglm (`vec3`, `mat4`,
   `quat`), helpers for TRS compose, look-at, perspective, and grid→world.
2. **Camera** (`scene/camera.c`):
   - Inputs: `focus` (world pos), `distance`, `yaw`, `pitch`, `fov`, near/far.
   - Outputs cached `view`, `proj`, `viewProj`; recompute on change.
   - Clamp pitch; expose in the eventual `TesseraCamera` state struct.
3. **Vertex format**: `{ vec3 pos; vec3 normal; vec2 uv; }` interleaved. Define a
   canonical `TesseraVertex` used by all meshes.
4. **Shaders** (`shaders/mesh.hlsl`): vertex transforms by `mvp`; fragment does
   flat/Lambert shading with a single hardcoded directional light + ambient
   (placeholder for cel shading in M7). Sample base texture (white default now).
5. **Shader build**: CMake custom command runs `shadercross` per shader per
   target format; outputs to `assets/shaders/`. Runtime loads the format matching
   the device (`SDL_GetGPUShaderFormats`).
6. **Pipeline creation** (`gpu/pipeline.c`): vertex input state, rasterizer
   (back-face cull, CCW front), depth test (less, write on), one color target
   matching swapchain format.
7. **Uniforms**: per-frame UBO `{ mat4 viewProj; vec4 lightDir; vec4 ambient; }`
   pushed via `SDL_PushGPUVertexUniformData`; per-object `{ mat4 model; }`.
8. **Depth buffer**: create/resize a depth texture alongside the swapchain.
9. **Hardcoded tile mesh**: generate a thin box (top/side/bottom faces, correct
   normals + UVs) in code; upload to a GPU vertex/index buffer via a transfer
   buffer + copy pass.
10. **Render pass**: clear color + depth, bind pipeline, set camera UBO, draw the
    tile with a model matrix; add a debug key to orbit the camera.

## Public API added
- Internal only this milestone (camera exposed later via state). Optionally a dev
  hook `tessera__debug_orbit(engine, dyaw, dpitch, dzoom)` behind a debug macro.

## Acceptance criteria
- A correctly lit, depth-tested thin tile renders and can be orbited.
- Front-face culling/winding correct (no inverted faces).
- Shaders load on both Metal (macOS) and Vulkan (Linux) from prebuilt blobs.
- Resize keeps aspect ratio and depth buffer correct.

## Risks / notes
- **Coordinate/winding bugs** are the classic time sink — write down handedness,
  matrix order, and NDC depth range (SDL_GPU uses 0..1 depth) in `core/math.h`
  and assert on them.
- Keep uniform struct layout std140-compatible / 16-byte aligned to avoid
  cross-backend padding surprises.
