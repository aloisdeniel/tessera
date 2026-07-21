# Milestone 7 — Camera transitions, lighting & visual polish

**Goal:** Make it look like a real board game. Animate the camera between focuses/
zooms/angles from the state, finalize the **stylized flat/cel** look with a single
directional light + ambient, and add simple shadows to ground entities and tiles.

## Deliverables
- Camera pose animated from `state.camera` (focus/zoom/yaw/pitch tween on change).
- Cel / flat shading pass with light ramp and optional rim light.
- Directional light + ambient; a **shadow** solution (blob shadows first, optional
  shadow map).
- Small polish: ambient occlusion-ish contact darkening, tile edge/outline,
  tonemap/gamma correctness.

## Tasks
1. **Camera transitions** (`scene/camera.c` + M4 tween): when `state.camera`
   changes, tween `focus`, `distance`, `yaw`, `pitch` with the M4 easing over
   `timing.camera_s`. Retarget smoothly if the camera changes mid-flight.
2. **Camera framing helpers (optional)**: convenience so a host can focus a coord
   or fit a set of coords; expose min/max zoom + pitch clamps.
3. **Cel/flat shader**: quantized diffuse (2–3 band ramp) or pure flat Lambert,
   ambient term, optional Fresnel rim. Keep it a variant of the M1 mesh shader so
   skinned + static + tile pipelines share the lighting.
4. **Shadows**:
   - **Phase A — blob shadows**: a soft dark disc projected under each entity onto
     `y=0`. Cheap, mobile-friendly, reads well for board games.
   - **Phase B (optional) — shadow map**: single directional shadow map for
     crisper contact shadows; gated behind a quality flag.
5. **Contact / grounding**: subtle darkening where entities meet tiles; tile
   outlines or beveled edges for readability.
6. **Color management**: render linear, output with gamma/tonemap so colors are
   consistent across Metal/Vulkan. sRGB swapchain or manual encode.
7. **Quality settings**: `TesseraQuality { shadows: none/blob/map; msaa; ... }` so
   mobile can dial down.
8. **examples/polish**: a scene that pans/zooms the camera between two focuses and
   shows the lit, shadowed board.

## Public API added
```c
typedef enum { TESSERA_SHADOW_NONE, TESSERA_SHADOW_BLOB, TESSERA_SHADOW_MAP } TesseraShadowMode;
typedef struct {
    TesseraShadowMode shadows;
    int   msaa;                 // 1,2,4
    float render_scale;         // resolution scale for mobile
} TesseraQuality;

TESSERA_API void tessera_set_quality(TesseraEngine*, const TesseraQuality*);

// Lighting is stylized/global; expose minimal controls:
typedef struct { float dir[3]; float color[3]; float intensity; float ambient[3]; } TesseraLight;
TESSERA_API void tessera_set_light(TesseraEngine*, const TesseraLight*);
```

## Acceptance criteria
- Changing `state.camera` glides the camera; mid-flight changes retarget smoothly.
- The board reads as a cohesive stylized scene with consistent color on both
  backends.
- Entities are grounded by shadows; toggling shadow mode works.
- Quality settings visibly change cost (measure frame time).

## Risks / notes
- Shadow maps add real complexity and cross-backend depth/bias tuning — keep them
  optional; blob shadows satisfy the stylized target on their own.
- Lock color pipeline (linear vs sRGB) early to avoid a washed-out look on one
  backend.
