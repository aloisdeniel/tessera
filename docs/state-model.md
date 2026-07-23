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
| entity id in both, different coord | `ENTITY_MOVE` — hop/arc between tiles (walks a `path`, if given, one tile at a time) |
| entity id in both, same coord, tile occupancy changed | `ENTITY_REFLOW` — slide to new slot/scale |
| entity id in target only | `ENTITY_ADD` — scale/fade in |
| entity id in current only | `ENTITY_REMOVE` — scale/fade out, then cull |
| effect id in target only | `EFFECT_ADD` — play `on_add` burst |
| effect id in current only | `EFFECT_REMOVE` — play `on_remove` burst |

A move also reflows both the source tile (n−1 occupants) and the destination
tile (n+1), for the entities that stayed.

**Multi-step moves.** A `TesseraEntityPlacement` may carry a `path` (a list of
`TesseraCoord` waypoints) with `path_count > 1`: the entity then *walks through*
those tiles in order — **arcing (hopping) from one to the next** — instead of
gliding straight to `coord`. The last waypoint must equal `coord` (still the
resting tile used by the layout solver and picking). The whole walk takes
**`2 × move_s`** (twice a single-tile move, so the individual hops stay legible),
split evenly across the steps. A `NULL` path or a count of `0`/`1` is a plain
single move at the normal `move_s`. `TesseraCardPlacement` has the same
`path`/`path_count` for free cards (see `definitions.md`).

## Multi-entity tiles & the layout solver

When several entities share a tile, a deterministic solver assigns each a
sub-slot within the tile footprint and a uniform shrink so they fit. The
contract: **slot `i` goes to the `i`-th entity in an id-sorted group.** Because
it's deterministic, an entity keeps its slot when its neighbours don't change —
which is what makes reflow animate smoothly instead of shuffling everything.

## Retargeting

If a new state arrives mid-animation, the engine re-diffs from the *current
interpolated transforms* (not the last target), so motion blends smoothly with
no snapping. `tessera_is_idle()` reports when *all* transitions have settled.

## Operation ids & completion events

`tessera_set_state` returns a monotonic, nonzero **operation id**. That id's
transition is "complete" once the engine has promoted the snapshot and every
resulting animation (entities, cards, dice, effects, camera) has settled — which
is more precise than `is_idle` when you need to wait on *one specific* push.
Track completion without polling:

- `tessera_operation_completed(e, id)` — has this id settled? (ids are monotonic,
  so it's just `id <= tessera_last_completed_operation(e)`; `id == 0` ⇒ true);
- `tessera_last_completed_operation(e)` — the highest id done so far;
- `tessera_set_operation_callback(e, fn, user)` — `fn(id, user)` fires once per
  operation as it completes, on the tick thread.

A superseded operation (a newer `set_state` replaced one that hadn't promoted
yet) completes no later than the operation that superseded it. The Dart/Flutter
binding turns this into an awaitable: `TesseraController.setScene` returns a
`Future<void>` that the completion callback resolves — letting a caller throw a
die, `await`, then move a piece, instead of pushing both at once.

## Timing

`tessera_set_timing` tunes per-transition-type durations plus a global
`speed_multiplier`. Defaults are tuned for a snappy board-game feel
(~0.3s moves).
