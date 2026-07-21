# Milestone 3 — Immutable visual state & static full-board rendering

**Goal:** Define the immutable `TesseraState`, implement `tessera_set_state` with
a thread-safe deep copy, and render a **complete static board** — all tiles, all
entities at their grid coords, effects ignored for now — including the
**multi-entity tile layout solver**. No transitions yet (state snaps instantly).

## Deliverables
- `TesseraState` flat struct + deep-copy into engine-owned storage.
- Grid → world coordinate layer (`grid.h`) with a square-grid implementation.
- Full-board render: iterate current state, draw every tile and entity.
- Multi-entity tile layout solver assigning sub-slots deterministically.
- Camera fed from `state.camera` (snap, no animation yet).

## The state
```c
typedef struct { int32_t x, y; } TesseraCoord;

typedef struct {
    TesseraCoord coord;
    TesseraDefId tile_def;      // 0 = no tile (hole)
    uint32_t     variant;       // optional per-instance variant/seed
} TesseraTilePlacement;

typedef struct {
    TesseraEntityId id;         // stable across states — key for diffing
    TesseraDefId    def;
    TesseraCoord    coord;
    uint16_t        facing;     // 0..3 (or degrees) — square grid
    uint32_t        anim;       // active animation index (played in M5)
} TesseraEntityPlacement;

typedef struct {
    TesseraEntityId id;
    TesseraDefId    def;
    TesseraCoord    coord;
} TesseraEffectPlacement;

typedef struct {
    TesseraCoord focus;         // (or world focus variant)
    float distance, yaw, pitch; // orbit params
    float fov;
} TesseraCamera;

typedef struct {
    const TesseraTilePlacement*   tiles;    size_t tile_count;
    const TesseraEntityPlacement* entities; size_t entity_count;
    const TesseraEffectPlacement* effects;  size_t effect_count;
    TesseraCamera camera;
    uint64_t      epoch;        // optional caller sequence number
} TesseraState;
```

## Tasks
1. **Deep copy** (`state.c`): allocate one contiguous block per snapshot (arena),
   copy all arrays, so a snapshot is a single freeable unit. Caller memory is not
   retained. Validate ids against the registry; skip/report invalid ones.
2. **Thread-safe handoff**: `set_state` writes to a `pending` slot under a mutex
   (or lock-free swap). The tick loop promotes `pending → target` at a safe point.
   Keep `current` (what's on screen). This milestone: promote and **snap** current
   = target immediately (transitions arrive in M4).
3. **Grid layer** (`scene/grid.c`): `grid_to_world(coord) → vec3`, tile size,
   neighbor iteration. Square implementation; interface allows hex later.
4. **Tile rendering**: build model matrices from coords; draw thin-box tiles with
   the TileDef atlas rects. Cull tiles with `tile_def == 0`.
5. **Layout solver** (`scene/layout.c`):
   - Group entities by tile. For a group of size `n`, produce `n` local offsets
     (within the tile footprint) + a uniform scale so they fit.
   - Patterns: `n=1` center; `n=2` side-by-side; `n≤4` 2×2; `n≤9` 3×3 grid;
     general ring/grid fallback. Scale shrinks as `n` grows (clamped).
   - **Deterministic**: sort group by `entity id`; slot index = sorted position.
     Guarantees the same entity keeps the same slot when others don't change,
     which M4 relies on for stable reflow.
6. **Entity rendering**: world transform = `grid_to_world(coord) + slot_offset`,
   `* slot_scale`, `* facing rotation`, `* def.scale`, pivot at feet.
7. **Frustum cull** (basic): skip off-screen tiles/entities for large boards.
8. **examples/board**: build a small board + several entities (some stacked on one
   tile) from code; verify layout + placement.

## Public API added
```c
TESSERA_API void tessera_set_state(TesseraEngine*, const TesseraState*);
```

## Acceptance criteria
- A full board with holes, multiple tile types, and entities renders correctly.
- Two/three entities on one tile are laid out without overlap and scaled to fit.
- Calling `set_state` repeatedly with different boards snaps to each correctly and
  leaks nothing (old snapshot freed when replaced).
- Caller can free its input arrays immediately after `set_state` returns.

## Risks / notes
- Lock contention: keep the mutex-held region to a pointer swap; do the deep copy
  outside the lock, publish under it.
- Layout determinism is a **contract** — document it; M4 correctness depends on it.
