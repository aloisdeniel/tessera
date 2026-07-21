# Definitions: tiles, entities, effects & atlases

Before pushing any state, the host registers immutable **definitions**. Each
registration returns a `TesseraDefId` (a `uint32`; `0` means invalid/failure —
check `tessera_last_error` on `0`). Definitions are referenced by id from the
placements inside a `TesseraState`. Registration uploads GPU resources, so it
must run on the render thread.

```c
TesseraDefId tessera_register_atlas     (TesseraEngine*, const TesseraBytes*  image);
TesseraDefId tessera_register_tile_def  (TesseraEngine*, const TesseraTileDef* def);
TesseraDefId tessera_register_entity_def(TesseraEngine*, const TesseraEntityDef* def);
TesseraDefId tessera_register_effect_def(TesseraEngine*, const TesseraEffectDef* def);
```

## `TesseraBytes` — a borrowed byte span

Every asset input (atlas image, glTF model) is a `TesseraBytes`:

| Field | Meaning |
|---|---|
| `data` | pointer to an in-memory buffer, or `NULL` to load from `path` |
| `size` | byte length of `data` |
| `path` | optional filesystem path, used when `data == NULL` |
| `debug_name` | optional label for diagnostics |

The span is **not retained** past the registration call — the engine copies or
decodes what it needs and returns, so the caller may free the bytes immediately.

## Atlases

`tessera_register_atlas` decodes an image (via `stb_image`) and uploads it as a
texture. Tile and particle definitions reference an atlas id and sample sub-
regions of it through normalized UV rectangles (`TesseraRect { u0,v0,u1,v1 }` in
`[0,1]` space). An atlas id of `0` in a def means "untextured / white".

## `TesseraTileDef`

A tile is an extruded quad with independent top / side / bottom UVs.

| Field | Meaning |
|---|---|
| `atlas` | atlas id to sample (`0` = untextured white) |
| `top`, `side`, `bottom` | `TesseraRect` UV regions for each face |
| `tint[4]` | RGBA multiply color |
| `thickness` | relative tile height (default `0.25`) |

Tiles are keyed by grid `coord` when diffing states (see `docs/state-model.md`).

## `TesseraEntityDef`

An entity is a 3D model placed on the board. If no glTF bytes/path are supplied
the engine substitutes a shared **unit-cube fallback mesh** (no error), so an
entity works out of the box for prototyping.

| Field | Meaning |
|---|---|
| `gltf` | `TesseraBytes` of the model (glTF/GLB); empty = cube fallback |
| `atlas` | optional texture override (`0` = use the material from the glTF) |
| `scale` | uniform scale (default `1.0`) |
| `pivot[3]` | feet-anchor offset in model space |
| `default_anim` / `move_anim` / `spawn_anim` / `despawn_anim` | clip roles; `-1` = unset |
| `on_spawn_effect` / `on_despawn_effect` | linked effect def ids played on add/remove; `0` = none |

### glTF entity path & animation clip roles

When `gltf` carries bytes or a path, the model is imported (geometry, and — for
skinned models — a skeleton and its animation clips). The clips discovered in
the file can be inspected after registration:

```c
uint32_t    tessera_entity_def_anim_count(TesseraEngine*, TesseraDefId def);
const char* tessera_entity_def_anim_name (TesseraEngine*, TesseraDefId def, uint32_t index);
```

The four **clip-role** fields (`default_anim`, `move_anim`, `spawn_anim`,
`despawn_anim`) are indices into that clip list (or `-1` for unset). The engine
uses them to auto-select which clip plays during a transition: the idle/default
pose, a movement hop, a spawn-in, and a despawn-out. A per-placement
`TesseraEntityPlacement.anim` index can override the active clip for a specific
entity in a state.

See `docs/assets.md` for the supported glTF subset and the 64-joint cap.

## `TesseraEffectDef` & `TesseraParticleSpec`

An effect def bundles two particle specs — one played when the effect is added
to a state, one when it is removed:

```c
typedef struct { TesseraParticleSpec on_add; TesseraParticleSpec on_remove; } TesseraEffectDef;
```

`TesseraParticleSpec` fields:

| Field | Meaning |
|---|---|
| `atlas` + `sprite` | particle sprite sheet id and UV region (`0` = white/dot) |
| `mode` | `TESSERA_EMIT_BURST` or `TESSERA_EMIT_CONTINUOUS` |
| `count` | burst count, or particles/second if continuous |
| `lifetime_s`, `lifetime_var` | particle lifetime and its random variance |
| `speed`, `speed_var` | initial speed and variance |
| `spread_deg` | emission-cone half-angle |
| `gravity` | world-space +Y acceleration |
| `size_start`, `size_end` | sprite size over life |
| `color_start[4]`, `color_end[4]` | RGBA over life |
| `blend` | `TESSERA_BLEND_ALPHA` or `TESSERA_BLEND_ADD` |
| `duration_s` | `0` = one-shot burst; `>0` = continuous emission window |

Effects appear in a state through `TesseraEffectPlacement`, anchored to a tile
`coord` or attached to an entity via `attach_entity_id` (`0` = tile-anchored).
An entity def's `on_spawn_effect` / `on_despawn_effect` fire the linked effect
automatically when that entity is added or removed.
