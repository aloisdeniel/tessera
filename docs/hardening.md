# Hardening: error model, threading & limits

This document describes Tessera's robustness contract as it is actually
implemented: how errors are reported, what may be called from which thread, and
the fixed capacity limits a host must design around. Every statement below is
grounded in the source (`src/tessera.c`, `src/state.c`, `src/engine.c`,
`src/anim/skeleton.h`, `src/fx/fx.c`, `src/scene/scene.h`, `src/core/core.h`).

## Error model

Tessera never returns error codes as its primary channel. Instead it follows a
simple, uniform convention:

- **Constructors / registration return an identifier; `0` means failure.**
  `tessera_register_atlas`, `tessera_register_tile_def`,
  `tessera_register_entity_def`, and `tessera_register_effect_def` return a
  `TesseraDefId` (`0` = invalid). On failure they set the engine's last-error
  string and return `0`. A `NULL` definition pointer is rejected explicitly
  (e.g. `register_tile: null def`) rather than dereferenced.
- **`bool`-returning calls return `false` on bad input.**
  `tessera_capture_png` returns `false` for a `NULL` engine, a missing GPU
  device, a non-positive width/height, or a `NULL` path, and sets last-error on
  the device/size failures.
- **`void` calls are no-ops on bad input.** `tessera_set_state`,
  `tessera_tick`, `tessera_resize`, `tessera_set_timing`, `tessera_set_light`,
  `tessera_set_quality` all null-check their arguments and return quietly when
  the engine (or the payload) is `NULL`.

### `tessera_last_error` is always a valid string

`tessera_last_error(NULL)` returns the literal `"null engine"`; for a live
engine it returns the internal error buffer (never `NULL`). It is therefore safe
to log unconditionally. The string reflects the most recent failure and is not
cleared on success, so treat it as diagnostic context for a `0`/`false`/invalid
return, not as a live "is everything OK" flag.

### Bad FFI input is tolerated, not trusted

The snapshot copy (`ts_snapshot_copy` in `src/state.c`) is the hardening choke
point for `tessera_set_state`:

- **`NULL` array + non-zero count is treated as empty.** Each of
  `tiles`/`entities`/`effects` is only copied when its pointer is non-`NULL`;
  the reported count is ignored when the pointer is `NULL`. A hostile
  `{ .tiles = NULL, .tile_count = SIZE_MAX }` allocates nothing and copies
  nothing.
- **Each snapshot is one backing allocation.** The three arrays are packed into
  a single `malloc` (aligned per element type) and freed as a unit; an
  allocation failure is reported via last-error and drops the state without
  partial ownership leaks.
- **Unknown / zero / huge def ids are safe.** Placements that reference a
  `TesseraDefId` of `0`, an unregistered id, or `0xFFFFFFFF` are looked up
  through the registry (which returns `NULL` for anything unknown) and simply
  skipped at draw time. `NaN`/`Inf` in the camera or float fields propagate as
  finite-free math but do not corrupt memory.

The FFI robustness suite (`tests/test_fuzz.c`) drives all of the above —
`NULL` state, huge counts against `NULL` arrays, bad def ids, `NaN`/`Inf`
camera/light/quality/timing values, negative resize/capture sizes, empty
states, a single-tile board, a 500-entity stack on one tile, a 300-iteration
`set_state` retarget storm interleaved with `tick`, and `NULL`-def register
calls — asserting the engine stays alive and `tessera_last_error` stays
non-`NULL` throughout.

### Non-finite camera parameters

Non-finite (`NaN`/`Inf`) values in `camera.yaw/pitch/distance/fov` are sanitized
to finite fallbacks when a state is applied: `apply_camera` replaces non-finite
inputs with the current pose's values, and `advance_camera`'s shortest-arc yaw
normalization uses `remainderf` (O(1)) guarded by `isfinite` instead of an
unbounded `while` loop. (An earlier build could spin forever on `camera.yaw ==
Inf`; `tests/test_fuzz.c` now pushes `+/-Inf` yaw as a regression guard.)

## Threading contract

Tessera assumes a **single render/GPU thread** plus an optional **producer
thread** that pushes state.

- **`tessera_set_state` is thread-safe.** It deep-copies the caller's snapshot
  *off-lock*, then swaps it into the pending slot under the engine's state
  mutex (a short critical section). The caller may free its `tiles`/`entities`/
  `effects` arrays immediately after the call returns.
- **Everything that touches the GPU must run on the render thread.**
  `tessera_tick`, `tessera_resize`, `tessera_register_*`, and
  `tessera_capture_png` are **not** mutually thread-safe and must be called from
  the one thread that owns the GPU device. `tessera_tick` is where the pending
  snapshot is promoted (again under the state mutex) and diffed into the
  transition orchestrator.
- **`tessera_last_error` is callable from any thread.** It returns a pointer
  into the engine's error buffer; it performs no locking, so treat the returned
  string as a best-effort snapshot.

The lifecycle calls `tessera_create` / `tessera_destroy` must bracket all other
use; `tessera_destroy` waits for GPU idle before tearing down.

## Documented limits

| Limit | Value | Source | Meaning |
|-------|-------|--------|---------|
| `TS_MAX_JOINTS` | **64** | `src/anim/skeleton.h` | Hard cap on joints per skinned skeleton; matches the shader palette size. `ts_skeleton_skinning` clamps `joint_count` to this. Skinned assets must stay within it. |
| `TS_FX_GLOBAL_CAP` | **4096** | `src/fx/fx.c` | Maximum live particles across *all* emitters. Emission is budgeted against this cap, so a storm of effects degrades gracefully instead of growing unbounded. |
| `TS_MAX_SLOTS` | **32** | `src/scene/scene.h` | Deterministic per-tile sub-slot layout capacity for entities sharing one tile. Stacks larger than this still render (the layout solver fills up to the cap); occupancy beyond it reuses the solved footprint rather than crashing. |

### Memory model (arenas, pools, slot maps)

- **Arenas** (`TsArena`, `src/core/core.h`) are growable linked-block bump
  allocators, freed all-at-once. Each engine holds two: a **frame arena**
  (256 KiB initial block) that is `reset` at the start of every `tick` and every
  `capture` — bounding per-frame scratch to a rewind rather than a churn of
  `malloc`/`free` — and a **permanent arena** (256 KiB initial block) that backs
  registered definitions and their skeleton/clip data for the engine's lifetime.
  Arenas track `total_used` and `high_water` so peak scratch can be observed.
- **Pools** (`TsPool`) are fixed-size block allocators with a free list;
  `ts_pool_alloc` returns `NULL` when exhausted rather than growing.
- **Slot maps** (`TsSlotMap`) hand out generational handles; a handle to a freed
  or reallocated slot is rejected by `ts_slotmap_get`, so stale ids can't alias a
  new object.
- **State snapshots** are triple-buffered (`current` / `target` / `pending` in
  `struct TsStateStore`), each a single backing allocation, promoted on the tick
  thread and freed exactly once even when the slots alias before the first
  promotion.

### Redraw only when idle

`tessera_is_idle(e)` returns `true` only when **no** transition is in flight. It
returns `false` when any of the following hold (see `tessera_is_idle` in
`src/tessera.c`):

- the camera is mid-glide (`cam_active`),
- a pushed snapshot is still pending promotion,
- particles are still simulating (`ts_fx_is_idle` is false), or
- the transition orchestrator is still animating moves/adds/removes/reflows.

Guidance for hosts that want to save power:

- While `tessera_is_idle` is **false**, drive `tessera_tick` every frame —
  animations, particles, and camera motion all require continuous redraws.
- When `tessera_is_idle` is **true** and no user input has changed the scene,
  you may stop redrawing (or drop to a low idle rate). Resume immediately on the
  next `tessera_set_state`, camera nudge, or quality/light change.
- `tessera_is_idle` already accounts for a not-yet-promoted pending snapshot, so
  a host that pushes state from another thread can poll it to know when to wake
  the render loop. Note that promotion itself happens inside `tessera_tick`, so a
  paused loop must call `tick` at least once after a state push to make progress.

`tessera_is_idle(NULL)` returns `true`.
