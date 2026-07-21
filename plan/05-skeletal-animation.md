# Milestone 5 — Skeletal & morph animation

**Goal:** Play the glTF animations authored into entity models — per-entity active
animation (idle/walk/attack…), driven by the `anim` field in the state and by
transition events (e.g. play "walk" while moving). Support skeletal skinning and
morph targets with smooth blending between clips.

## Deliverables
- glTF skin/joint import completed (from M2 parse) into a runtime skeleton.
- Animation sampler: keyframe interpolation of TRS channels + morph weights.
- GPU skinning (joint matrices via storage/uniform buffer, skinned vertex shader).
- Per-entity animation **state machine**: current clip, time, loop, speed, plus a
  crossfade blend to the next clip.
- Hooks so transitions can request a clip (walk during move, a spawn clip, etc.).

## Tasks
1. **Skeleton runtime** (`anim/skeleton.c`): joint hierarchy, inverse-bind
   matrices, per-instance local pose. Compute world joint matrices → skinning
   matrices each frame.
2. **Clip sampler** (`anim/clip.c`): for a clip + time, sample each channel
   (translation/rotation/scale via lerp/slerp, morph weights via lerp), write into
   the pose. Handle looping and clamped clips.
3. **Skinned pipeline**: a second graphics pipeline with skinning in the vertex
   shader. Vertex format gains `{ uvec4 joints; vec4 weights; }`. Joint matrices
   uploaded per instance (storage buffer, indexed by draw). Morph targets applied
   in-shader or via precomputed blend.
4. **Animation state** (`anim/entity_anim.c`): `{ clip, time, speed, loop,
   blend_from, blend_t }`. `set_state` `anim` changes → crossfade to the new clip
   over a short blend. Transition-driven clips (move→walk, add→spawn) layered on
   top or one-shot then return to base.
5. **Facing**: rotate the root by `facing`; during a move, optionally turn toward
   the destination (tween from M4).
6. **Instancing/perf**: batch skinned draws; cap joint count; fall back to static
   pipeline for non-skinned entity defs (mixed scene).
7. **Non-skinned defs**: entities without a rig still work (static mesh + M4
   transform tweens only) — the `anim` field is ignored for them.
8. **examples/anim**: an entity idling, then walking during a move, then a one-shot
   spawn clip on add.

## Public API added
```c
// Query which animation indices/names a def exposes (for host tooling)
TESSERA_API uint32_t    tessera_entity_def_anim_count(TesseraEngine*, TesseraDefId);
TESSERA_API const char* tessera_entity_def_anim_name(TesseraEngine*, TesseraDefId, uint32_t index);
// (state.entities[i].anim already selects the active clip)
```
Optional: `TesseraEntityDef` gains named clip roles — `default_anim`,
`move_anim`, `spawn_anim`, `despawn_anim` — so transitions auto-select clips.

## Acceptance criteria
- A skinned entity plays its idle clip and crossfades to walk during a move, back
  to idle on arrival.
- Morph-target animation (if present in the asset) plays correctly.
- Mixed scenes (skinned + static entities) render together without artifacts.
- Frame time stays within budget for a few dozen skinned entities on desktop.

## Risks / notes
- Joint-matrix upload strategy varies by backend; validate storage-buffer limits
  on mobile (defer heavy validation to M8 but keep joint counts modest).
- Keep a hard cap on joints per skeleton; document it as an asset constraint.
