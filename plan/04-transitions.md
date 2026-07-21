# Milestone 4 — State diff, transitions & the tween engine

**Goal:** When `set_state` delivers a new snapshot, **diff** it against the current
one, generate a list of **transitions**, and animate them over time: tiles fade/
rise in and out, entities move between tiles, entities added/removed, and entities
**reflow** when a tile's occupancy changes. This is the heart of the renderer.

## Deliverables
- Diff algorithm (current → target) producing typed transition records.
- A generic **tween engine** (value interpolation with easing, duration, delay).
- Per-instance animation state so the renderer draws interpolated transforms.
- Reflow: when entity count on a tile changes, moving/staying entities animate to
  new solver slots.
- Configurable timing (per-transition-type durations/easing).

## Diff → transitions
Key entities/effects by **id**, tiles by **coord**.

| Change | Transition |
|--------|-----------|
| tile coord in target not in current | `TILE_ADD` (rise + fade in) |
| tile coord in current not in target | `TILE_REMOVE` (sink + fade out, then cull) |
| same coord, different `tile_def`/variant | `TILE_UPDATE` (swap, small pop) |
| entity id in both, different coord | `ENTITY_MOVE` (arc/slide between tiles) |
| entity id in both, same coord, tile group size changed | `ENTITY_REFLOW` (slide to new slot/scale) |
| entity id in target only | `ENTITY_ADD` (spawn: scale/fade in, optional drop) |
| entity id in current only | `ENTITY_REMOVE` (despawn: scale/fade out, then cull) |
| entity `anim`/`facing` changed | forwarded to M5 (anim) / rotate tween |
| effect id in target only | `EFFECT_ADD` → play `on_add` (M6) |
| effect id in current only | `EFFECT_REMOVE` → play `on_remove` (M6) |

Note: a move **also** triggers reflow on both the source tile (n−1) and the
destination tile (n+1), for the entities that stayed on those tiles.

## Tasks
1. **Diff** (`orchestration/diff.c`): build lookup maps from `current` and
   `target`; emit a `TesseraTransition[]`. Compute affected tiles for reflow
   (union of tiles that changed occupancy). Recompute solver slots for old and new
   occupancy to know start/end offsets.
2. **Tween engine** (`anim/tween.c`):
   - `Tween { float t, duration, delay; Easing ease; }`; `tween_advance(dt)`;
     `tween_value01()`. Easings: linear, ease-in/out (cubic), back (overshoot),
     bounce.
   - Interpolate scalars, vec3 (position/scale), quats (slerp for rotation/arc).
3. **Instance animation state** (`anim/instances.c`): for each live entity/tile,
   hold `{ from_transform, to_transform, tween }`. The renderer reads the
   interpolated transform each frame instead of the static one from M3.
4. **Move motion**: position lerp + a vertical arc (parabola) so entities "hop"
   between tiles; optional face-toward-destination during travel.
5. **Add/remove**: spawn = scale 0→1 + fade; despawn = scale 1→0 + fade, then
   remove the instance when the tween completes. Removed tiles/entities persist as
   "ghost" instances until their out-transition finishes.
6. **Promotion model**: on promote `pending→target`, run diff vs `current`, spawn
   transitions, then treat `target` as the new logical `current` while animations
   interpolate toward it. If a **new** state arrives mid-animation, re-diff from
   the *in-flight* transforms (retarget) so motion stays smooth (no snap).
7. **Timing config**: `TesseraTiming { float move, add, remove, tile, reflow; ... }`
   set via API, with sane defaults. Global speed multiplier.
8. **Completion signal (optional)**: a callback / pollable flag
   `tessera_is_idle()` (all transitions done) so turn-based hosts can gate input.
9. **examples/transitions**: script a sequence of `set_state` calls (add tiles,
   move an entity onto a crowded tile, remove one) and watch it animate.

## Public API added
```c
typedef struct {
    float move_s, add_s, remove_s, tile_s, reflow_s, camera_s;
    float speed_multiplier;   // global
} TesseraTiming;

TESSERA_API void tessera_set_timing(TesseraEngine*, const TesseraTiming*);
TESSERA_API bool tessera_is_idle(TesseraEngine*);   // true when no transitions active
```

## Acceptance criteria
- Adding/removing tiles and entities animates in/out (no popping).
- An entity moving onto an occupied tile hops over and the tile's occupants reflow
  to fit; moving off reflows the source tile.
- Pushing a new state mid-animation retargets smoothly without snapping.
- `tessera_is_idle` correctly reports quiescence.

## Risks / notes
- **Retargeting** is the subtle part — keep per-instance current transform as the
  source of truth so re-diff always starts from what's on screen.
- Guard against id reuse: an entity removed and a new one added with the *same* id
  in the same frame should be treated as remove+add (or documented as illegal).
- Reflow correctness hinges on the M3 deterministic solver — reuse it verbatim.
