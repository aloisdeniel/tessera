# The state model & transitions

Tessera is **engine-driven**: the host never issues draw calls or animation
commands. It pushes an immutable snapshot of *what the board looks like now*, and
the engine figures out how to get there from the current on-screen state.

## Snapshots

`tessera_set_state(engine, state)` deep-copies the `TesseraState` into an
engine-owned buffer. The caller may free its arrays the moment the call returns.
Snapshots are triple-buffered internally: `pending` (just set) → `current`
(being animated from) → `target` (the goal). The diff runs when `pending` is
promoted on the next tick.

Keys used for diffing:
- **Tiles** are keyed by `coord` (`{x, y}`).
- **Entities / effects** are keyed by their stable `id`.

## Diff → transitions

| Change (current → target) | Transition |
|---|---|
| tile coord appears | `TILE_ADD` — rise + fade in |
| tile coord disappears | `TILE_REMOVE` — sink + fade out, then cull |
| tile def / variant changes | `TILE_UPDATE` — swap with a small pop |
| entity id in both, different coord | `ENTITY_MOVE` — hop/arc between tiles |
| entity id in both, same coord, tile occupancy changed | `ENTITY_REFLOW` — slide to new slot/scale |
| entity id in target only | `ENTITY_ADD` — scale/fade in |
| entity id in current only | `ENTITY_REMOVE` — scale/fade out, then cull |
| effect id in target only | `EFFECT_ADD` — play `on_add` burst |
| effect id in current only | `EFFECT_REMOVE` — play `on_remove` burst |

A move also reflows both the source tile (n−1 occupants) and the destination
tile (n+1), for the entities that stayed.

## Multi-entity tiles & the layout solver

When several entities share a tile, a deterministic solver assigns each a
sub-slot within the tile footprint and a uniform shrink so they fit. The
contract: **slot `i` goes to the `i`-th entity in an id-sorted group.** Because
it's deterministic, an entity keeps its slot when its neighbours don't change —
which is what makes reflow animate smoothly instead of shuffling everything.

## Retargeting

If a new state arrives mid-animation, the engine re-diffs from the *current
interpolated transforms* (not the last target), so motion blends smoothly with
no snapping. Poll `tessera_is_idle()` to know when all transitions have settled
(useful for gating turn input).

## Timing

`tessera_set_timing` tunes per-transition-type durations plus a global
`speed_multiplier`. Defaults are tuned for a snappy board-game feel
(~0.3s moves).
