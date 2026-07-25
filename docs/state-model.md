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
- **Overlays** (tile decals) are keyed by `coord` — at most one per coord.
- **Labels** (3D text) are keyed by their stable `id`.
- **Highlights** (selection outline/glow) are keyed by (`kind`, `target_id`).

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
| overlay coord appears | fade in (`tile_s`) |
| overlay coord disappears | fade out (`tile_s`), then cull |
| overlay tint changes | crossfade from the *current interpolated* tint |
| label id appears | fade in (`add_s`) |
| label id disappears | fade out (`remove_s`), then cull |
| label text changes | crossfade the old string out under the new one (`tile_s`) |
| label color / position changes | retween from the *current interpolated* values (`tile_s`) |
| highlight (kind, id) appears | fade in (`tile_s`) |
| highlight (kind, id) disappears | fade out (`tile_s`), then cull |
| highlight color changes | crossfade from the *current interpolated* color (`tile_s`) |

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

## Tile overlays (ground decals)

`TesseraState.overlays` places flat decals on top of tiles — move-range fills,
threat rings, drop-target highlights. Each `TesseraOverlayPlacement` names a
`coord` (the diff key), a shape (`TESSERA_OVERLAY_SPRITE` samples an
`atlas`/`uv` rect like tiles do; `DISC` / `RING` are procedural fills), an RGBA
`tint` (all-zero ⇒ white) and an optional pulse: with `pulse_s > 0` the decal
breathes — alpha between `pulse_alpha_min..pulse_alpha_max` (when the max is
> 0) and/or footprint scale between `pulse_scale_min..pulse_scale_max`. The
pulse runs free and never keeps the engine from reporting idle.

Overlays render depth-tested against pieces (a piece standing on the tile
occludes its part of the decal) but never write depth, floating just above the
tile top like blob shadows do, so nothing z-fights.

## Text labels (world-anchored 3D text)

`TesseraState.labels` places text in the scene — scores, HP, dice totals,
board coordinates. Register a TrueType font once with
`tessera_register_font(engine, ttf_bytes_or_path, pixel_height)` (ASCII +
Latin-1 glyphs are baked into a GPU atlas at registration) and reference the
returned def id from each `TesseraLabelPlacement`. A placement names a stable
`id` (the diff key), the `font`, a bounded UTF-8 `text`
(`TESSERA_LABEL_TEXT_CAP` = 64 bytes incl. NUL), a `size` (line height in
world units), an RGBA `color` (all-zero ⇒ white) and a `billboard` flag
(face the camera vs. lie flat on the ground).

The `anchor` mode mirrors the camera's FOCUS modes: `WORLD` places the label
at `position` directly; `ENTITY` / `TILE` / `DICE` / `CARD` / `DRAW` glue it
to the live object named by `anchor_id`, with `position` as an offset from
that object's **live interpolated transform** — the label rides along through
moves, hops, throws and reflows, resolved fresh every frame at draw time. A
label anchored to an object that is not live is hidden until it appears.

Labels draw last, alpha-blended and depth-tested (never depth-writing), with
billboards nudged slightly toward the camera so a label hovering over its
piece stays readable instead of clipping into the mesh.

## Selection highlights (outline & glow post pass)

`TesseraState.highlights` flags live objects for a screen-space selection
treatment. Each `TesseraHighlightPlacement` names a `kind`
(`TESSERA_HIGHLIGHT_ENTITY` / `TILE` / `DICE` / `CARD` — mirroring the camera
FOCUS / label anchor object-reference conventions; tiles need the non-zero
instance `id` from their placement, `CARD` matches single cards, not piles)
plus the object's `target_id` — together the diff key — a `style`, an RGBA
`color` (all-zero ⇒ white), a `thickness` in pixels (<= 0 ⇒ default) and an
optional pulse: with `pulse_s > 0` the intensity breathes between
`pulse_min..pulse_max` (when the max is > 0). The pulse runs free in the
engine and never keeps it from reporting idle.

Rendering: each frame the flagged object's draws are re-rendered flat into an
offscreen single-channel silhouette mask **at their live interpolated
transforms** (skinned pose included), then a fullscreen pass composites the
mask over the lit scene — `TESSERA_HIGHLIGHT_OUTLINE` dilates it into a crisp
colored rim just outside the shape, `TESSERA_HIGHLIGHT_GLOW` blurs it into a
soft additive halo. The composite runs **after depth-of-field**, so a
selection stays sharp even when the focal blur softens the scene around it.
The silhouette ignores scene depth, so the outline reads even where the
object is partially occluded. A highlight whose target is not live simply
does not draw until the object appears.

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

## Engine event stream

Op completion tells you *when a push is done*; the typed **event stream**
tells you *what happened along the way*, at the exact animation moments a host
wants to sound/haptics/FX-sync against. As transitions play, the engine emits
`TesseraEvent`s (type, engine tick time, subject kind + id, board coord where
meaningful, small float payload):

| Event | Emitted when | payload (`value`) |
|---|---|---|
| `DICE_CONTACT` | a tumbling die touches the ground (per bounce; near-silent tail contacts are skipped) | impact speed |
| `DICE_SETTLED` | a die's tumble/slide comes to rest | face index |
| `ENTITY_HOP_LANDED` | a hop-arc touchdown (every hop of a move, incl. the last) | — |
| `ENTITY_WAYPOINT_REACHED` | a multi-step segment handoff (entities; also card paths, with a CARD/DRAW subject) | step number |
| `ENTITY_SPAWNED` / `ENTITY_REMOVED` | a spawn / removal transition finishes | — |
| `CARD_FLIPPED` | a hidden↔visible flip starts on a live card / pile top | 1 = now hidden |
| `CARD_DEALT` | a new card spawns off its `source_draw` pile | — |
| `CAMERA_ARRIVED` | a camera tween settles at its goal pose | — |
| `OP_COMPLETED` | an operation settles (`subject_id` = op id) — every transition event precedes its op's settle | — |

Events land in a fixed 256-slot ring drained with `tessera_poll_events`
(any-thread, oldest first; overflow drops the oldest and grows the
`tessera_events_dropped` counter), and are optionally delivered per-event via
`tessera_set_event_callback` on the tick thread — see `docs/ffi.md` for the
delivery contract and the Dart `Stream` / Lua poll bindings.

## Serialization, save / undo & replay

Because a `TesseraState` is *the whole scene*, saving, undo and replay all
reduce to keeping states around. `tessera_state_serialize(state, buf, cap)`
flattens one into a self-contained binary blob (two-call sizing: measure with
`buf = NULL`, allocate, call again); `tessera_state_deserialize(blob, len)`
reconstructs it as a single allocation freed with `tessera_state_free`, ready
to push straight back through `tessera_set_state` — the engine then animates
from wherever it currently is to the restored snapshot, exactly like any
other push.

The format is **little-endian on every platform** and starts with a versioned
header: magic (`"TSST"`), format version, total size, and the state's
`epoch`, which serves as the caller's sequence number (stamp your states if
you want ordering — the engine never interprets it). Every field of every
state array is carried — tiles, entities *including multi-step move paths*,
effects, cards (paths, hands, source piles), card draws, hands, dice,
overlays, labels (bounded text), highlights, and the full camera. Unknown
magic/versions and truncated or corrupt bytes are rejected with a NULL return,
never a crash, so blobs can come from untrusted saves. Round trips are exact:
`serialize(deserialize(blob))` is byte-identical to `blob`, and pushing the
reconstruction renders identically to pushing the original.

The wire walk lives in `src/serialize.c`, mirrors `ts_snapshot_copy`, and pins
`sizeof` of `TesseraState` and every placement struct with static asserts — a
new state field breaks the build until the serializer learns about it.

**Undo** is just a stack of blobs: serialize before each move, pop + `set_state`
to undo. **Replays** get a tiny container (`tessera_replay_*`): append
`(timestamp_ms, state)` records while playing, `tessera_replay_serialize` the
container to a file, later `tessera_replay_open` the bytes and feed each
`tessera_replay_get` result through `set_state` — the engine re-animates the
transitions between the recorded snapshots (`examples/replay/` shows the whole
loop, including headless captures of each replayed record).

## Timing

`tessera_set_timing` tunes per-transition-type durations plus a global
`speed_multiplier`. Defaults are tuned for a snappy board-game feel
(~0.3s moves).
