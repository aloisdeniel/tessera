# Embedded Lua game scripting

Tessera embeds a sandboxed **Lua 5.4** VM: a game's entire logic — rules,
animation staging, labels, sounds — can live in one Lua script run *inside the
engine*, with the host reduced to forwarding input events. The script pushes
the same immutable state snapshots a C/Dart host would (see `state-model.md`),
as plain Lua tables.

Three public functions drive it (appended to `include/tessera.h`):

```c
bool tessera_lua_load_bundle(TesseraEngine* e, const TesseraBytes* bundle);
bool tessera_lua_load_game  (TesseraEngine* e, const TesseraBytes* script);
bool tessera_lua_event      (TesseraEngine* e, const char* name,
                             const double* args, size_t arg_count);
```

- `tessera_lua_load_bundle` parses a **TSAB** asset container (below) and makes
  its entries available to scripts as `tessera.asset(name)`. Call it any number
  of times; later bundles add to / override earlier names. The bytes are copied.
- `tessera_lua_load_game` compiles and runs the script chunk. The chunk's
  top-level code executes immediately (typically registering defs from bundle
  assets) and must **return the game function**. Loading a new game replaces
  the previous one and drops its queued events/states. Returns `false` with
  `tessera_last_error` set on any compile/runtime error.
- `tessera_lua_event` queues one input event for the game function
  (any-thread, cheap). Returns `false` when no game is loaded.

Both load calls run on the setup/tick thread (not concurrent with a tick),
like the `tessera_register_*` family.

## The script protocol

A game script is a chunk that returns a single function:

```lua
-- top level: runs once at load — register defs from bundle assets
local atlas = tessera.register_atlas(tessera.asset("board.png"))
local tile  = tessera.register_tile_def{ atlas = atlas }

return function(event)
  -- called once per event, on the tick thread
  return { state_table, state_table, ... }  -- played strictly in order
end
```

- The engine invokes the function once with `{name = "start"}` on the first
  tick after `tessera_lua_load_game`, then once per `tessera_lua_event`, in
  queue order.
- A host event arrives as `{name = <string>, args = {n1, n2, ...}}` — `args`
  is an array of numbers and is **only present when the event carried
  arguments** (write `event.args or {}`).
- The returned value is an array of **state tables** (below). Each is converted
  to a `TesseraState` and pushed through `tessera_set_state` **sequentially**:
  every state waits for the previous operation to settle, so a returned
  sequence plays as ordered animation beats. Returning `nil` (or an empty
  array) means "nothing to show" — the event is consumed silently.
- One event is delivered per tick, and only once the previous event's states
  have all played, so sequences never interleave.
- A runtime error in the game function (or a bad state table) is logged, set
  as `tessera_last_error`, and that event/state is dropped — playback then
  continues with the next event.
- While a game is loaded, the script must be the **sole producer of states**:
  `tessera_set_state` (the host's `setScene`) shares the single pending-state
  slot with Lua playback — whichever snapshot is published last before the
  next tick wins, the other is silently dropped, and the superseded operation
  still reports completed without ever having been displayed. A host
  `setScene` can therefore silently swallow a queued Lua beat, and a Lua beat
  can swallow a host state whose awaitable Future still resolves. Load a
  replacement game (or don't run one) before driving the scene from the host,
  or funnel host-driven changes through `tessera_lua_event`.

### Sandbox

Scripts get the `base`, `table`, `string`, `math` and `utf8` libraries — no
`io`, `os`, `package` or `debug`, and `dofile`/`loadfile` are removed. `load`
is restricted to text chunks (mode `"t"`) — Lua does not verify precompiled
bytecode. `print` is redirected to the engine log (INFO), as is
`tessera.log(msg)`.

## The `tessera` module

Registration calls are legal at chunk top level (load thread) and inside the
game function (tick thread).

| Function | Meaning |
|---|---|
| `tessera.asset(name)` | bundle entry as a binary string (error on unknown name) |
| `tessera.asset_names()` | array of loaded entry names |
| `tessera.register_atlas(bytes)` | → atlas id (0 on failure); bytes = PNG/JPG string |
| `tessera.register_tile_def(t)` | → def id; `t` mirrors `TesseraTileDef` |
| `tessera.register_entity_def(t)` | → def id; mirrors `TesseraEntityDef` (`gltf` = binary string) |
| `tessera.register_effect_def(t)` | → def id; mirrors `TesseraEffectDef` (`on_add`/`on_remove` particle-spec tables) |
| `tessera.register_card_def(t)` | → def id; mirrors `TesseraCardDef` |
| `tessera.register_dice_def(t)` | → def id; mirrors `TesseraDiceDef` (`faces` = **required** array of sprite byte strings) |
| `tessera.register_font(ttf_bytes, size_px)` | → font id |
| `tessera.register_sound(wav_bytes)` | → sound id |
| `tessera.play_sound(id [, volume])` | fire-and-forget playback (any-thread safe) |
| `tessera.log(msg)` | INFO log line |

Def tables mirror the C structs **field-for-field in snake_case** (see
`definitions.md`): a `TesseraRect` is `{u0, v0, u1, v1}` (array or named
keys), `float[3]`/`float[4]` fields are arrays, `TesseraBytes` fields are
binary strings (typically `tessera.asset("x.png")`), and enum fields take an
integer — named constants are exported on the module (`tessera.EMIT_BURST`,
`tessera.BLEND_ADD`, `tessera.OVERLAY_RING`, `tessera.LABEL_ANCHOR_ENTITY`,
`tessera.HIGHLIGHT_CARD`, `tessera.HIGHLIGHT_GLOW`, `tessera.CAMERA_ORBIT`,
…). Omitted fields default to zero, with two exceptions: the entity def's
`default_anim`/`move_anim`/`spawn_anim`/`despawn_anim` default to `-1`
("unset", as in C), and the dice def's `faces` is required — omitting it (or
passing a non-table) raises a Lua error.

## State tables

A state table mirrors `TesseraState` (see `state-model.md` for what each
placement does and how diffs animate). Every array is optional (`nil` =
empty). Ids and defs are integers. Conventions:

- **Board coords** are integers, written `x = 1, y = 2` — or as a two-element
  array `{1, 2}` (the form path waypoints use). World **positions** are
  `{x, y, z}` arrays of numbers; **quaternions** are `{x, y, z, w}` (omitted /
  all-zero = identity).
- **Colors** are `{r, g, b, a}`, or `{r, g, b}` with alpha defaulting to 1.
- **Enum fields** take an integer or the lowercase suffix string of the C
  constant (`"orbit"`, `"ring"`, `"glow"`, `"card"`, …).
- Field names match the C struct fields (snake_case), with four shorthands:
  `x`/`y` for a `TesseraCoord` `coord`, `entity`/`card` for an effect's
  `attach_entity_id`/`attach_card_id`, `slot` for a card's `hand_slot`, and
  `target` for a highlight's `target_id`.
- Unknown keys are ignored silently; wrong types raise an error (the state is
  dropped and reported via `tessera_last_error`). Label `text` is clamped to
  63 bytes (`TESSERA_LABEL_TEXT_CAP - 1`).

```lua
{
  camera = { mode = <int or lowercase string of the TesseraCameraMode enum suffix,
                     e.g. "orbit">, focus = {x, y} -- floats,
             distance=, yaw=, pitch=, fov=, position={x,y,z}, orientation={x,y,z,w},
             target={x,y,z}, target_id=, focus_card_id=, fit_padding= },
  tiles      = { {x=, y=, def=, variant=, id=}, ... },
  entities   = { {id=, def=, x=, y=, facing=, anim=, path={{x,y}, ...}}, ... },
  effects    = { {id=, def=, x=, y=, entity=, card=}, ... },   -- entity/card = attach ids
  cards      = { {id=, def=, position={x,y,z}, orientation=, hidden=, hand=, slot=,
                  source_draw=, path={{x,y,z}, ...}}, ... },
  draws      = { {id=, def=, position=, orientation=, count=, top_hidden=}, ... },
  hands      = { {id=, position=, orientation=, spread_deg=, radius=, card_spacing=,
                  selected_card=}, ... },
  dice       = { {id=, def=, face=, position=, seed=, throw_s=}, ... },
  overlays   = { {x=, y=, shape= <int|"sprite"|"disc"|"ring">, atlas=, uv={u0,v0,u1,v1},
                  tint=, pulse_s=, pulse_alpha_min=, pulse_alpha_max=, pulse_scale_min=,
                  pulse_scale_max=}, ... },
  labels     = { {id=, font=, text=, anchor= <int|"world"|"entity"|"tile"|"dice"|"card"|"draw">,
                  anchor_id=, position=, size=, color=, billboard=}, ... },
  highlights = { {target=, kind= <int|"entity"|"tile"|"dice"|"card">,
                  style= <int|"outline"|"glow">, color=, thickness=, pulse_s=, pulse_min=,
                  pulse_max=}, ... },
  point_lights = { {id=, position=, color={r,g,b}, intensity=, radius=}, ... },
  world_models = { {id=, def=, position=, orientation=, scale=}, ... },
}
```

An `epoch = <int>` field is also honored (the caller sequence number carried
by serialized states).

## TSAB asset bundles

A bundle is a flat container of named blobs, little-endian throughout:

```
u32 magic   = 0x42415354  ("TSAB" as bytes 'T','S','A','B')
u32 version = 1
u32 count
count entries:
  u16 name_len,  name bytes (UTF-8, no NUL),
  u32 blob_len,  blob bytes
```

Parsing is strict — any truncation or overrun rejects the whole bundle (no
partial loads). Pack one with the stdlib-only CLI:

```
python3 tools/pack_bundle.py out.tsb file1 [file2 ...] [dir ...]
```

Files are stored under their basename; directories are walked recursively and
stored under their '/'-separated relative paths. A repeated name warns and
keeps the later entry, matching the loader's override semantics.

## A minimal game

```lua
local tile = tessera.register_tile_def{ tint = {0.3, 0.5, 0.3, 1} }

local function board(n)
  local st = { camera = { mode = "orbit", focus = {0, 0}, distance = 6 },
               tiles = {} }
  for i = 1, n do st.tiles[i] = { x = i - 1, y = 0, def = tile } end
  return st
end

return function(event)
  if event.name == "start" then
    return { board(1), board(2), board(3) }   -- three beats, played in order
  end
end
```

## Hosting from Dart / Flutter

The Flutter controller mirrors the C API: `loadLuaBundle(Uint8List)`,
`runLuaGame(String source)` (both `Future`s that throw on failure) and
`sendLuaEvent(name, [args])`.

On the **web**, the controller predicts def/sound ids to keep the `register*`
methods synchronous, and resyncs those predictors from the engine
(`tessera_def_count` / `tessera_sound_count`) after every `loadLuaBundle` /
`runLuaGame` completes — so Dart-side `register*` calls made *after* awaiting
`runLuaGame` stay correct even when the script's top level registered defs.
Registering defs from the game function **during event handling** (rather
than at chunk top level) does not compose with later Dart-side `register*`
calls on the web: those registrations happen asynchronously on the tick path
and cannot be predicted. The example app's **Lua Tic-Tac-Toe** screen
(`example/lib/lua_ttt.dart`) is the reference host: it loads
`example/assets/ttt/ttt.tsb` (generated by
`example/tool/generate_ttt_assets.py`) and `example/assets/ttt/game.lua`, then
just forwards picked tile taps as `tap` events — every rule and animation beat
comes from the script.
