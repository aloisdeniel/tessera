# FFI & bindings

The public ABI is deliberately FFI-friendly (see `include/tessera.h`):

- Pure C, `extern "C"`, no C++/inline/macros in the surface.
- Only fixed-width integer types (`stdint.h`), `bool`, `float`, and pointers.
- Flat structs; arrays passed as `(pointer, count)` pairs of POD elements.
- Opaque handles (`TesseraEngine*`, `TesseraDefId`, `TesseraEntityId`) only; no
  engine ownership leaks across the boundary.
- No required callbacks for basic use (the logging callback is optional).
- Struct layouts evolve **append-only**.

## The ABI is frozen — and self-tested

`include/tessera.h` is the frozen source of truth. Evolution is append-only:
you may add new trailing struct fields or new functions, but never reorder or
resize existing fields. The classic FFI bug is struct-layout drift between C and
a binding, so the exact layout is pinned by a canonical self-test:

**`tests/test_ffi_layout.c`** locks the `sizeof` of every public struct and the
`offsetof` of every field with `_Static_assert` (fails to compile on drift) and
also prints the whole table at runtime for binding authors to cross-check:

```sh
cmake --build build-m8 --target test_ffi_layout
DYLD_LIBRARY_PATH=/opt/homebrew/lib ./build-m8/test_ffi_layout
```

The reference layout it produces (target LP64 ABI: pointer/`size_t` = 8, enum =
4, `bool` = 1, `float` = 4):

| Struct | size / align | Struct | size / align |
|---|---|---|---|
| `TesseraConfig` | 40 / 8 | `TesseraEntityPlacement` | 32 / 8 |
| `TesseraBytes` | 32 / 8 | `TesseraEffectPlacement` | 32 / 8 |
| `TesseraRect` | 16 / 4 | `TesseraCamera` | 24 / 4 |
| `TesseraTileDef` | 72 / 4 | `TesseraState` | 80 / 8 |
| `TesseraEntityDef` | 80 / 8 | `TesseraTiming` | 28 / 4 |
| `TesseraParticleSpec` | 100 / 4 | `TesseraQuality` | 12 / 4 |
| `TesseraEffectDef` | 200 / 4 | `TesseraLight` | 40 / 4 |
| `TesseraCoord` | 8 / 4 | `TesseraTilePlacement` | 16 / 4 |

Both bindings mirror this layout field-for-field:
`bindings/dart/lib/src/ffi.dart` (`dart:ffi`) and `bindings/lua/tessera.lua`
(LuaJIT FFI). Both carry a header comment pointing back at this self-test.

## Threading contract

See `docs/platforms.md` for the full table. In short: GPU calls
(`tessera_create`/`destroy`/`tick`/`resize`/`register_*`/`capture_png`) run on
the render (main) thread; `tessera_set_state`, `set_timing`, `set_light`,
`set_quality`, `is_idle`, and `last_error` are any-thread.

## Dart (`dart:ffi`)

`bindings/dart/` is a real pub package (`name: tessera`) — add it to a Dart
project with a `path`/`git` dependency and `import 'package:tessera/tessera.dart'`.
It is a hand-written binding covering the **full** public surface — every
struct/enum and every function — plus a `Tessera` wrapper class and automatic
native-library discovery (`openTesseraLibrary`). For an always-in-sync binding
you can instead run [`ffigen`](https://pub.dev/packages/ffigen) against
`include/tessera.h`.

The package ships a complete example under `bindings/dart/example/`: a game of
chess ported from `examples/chess` — pure rules engine, runtime GLB piece
models, a heuristic AI, and the visual projection onto `TesseraState`. Its logic
is verified against the C reference game with `dart test`. See the package
`README.md` for build/run instructions (and the note on driving the Cocoa/Metal
renderer, which requires the platform main thread).

## Flutter (`bindings/flutter_tessera/`)

`flutter_tessera` embeds Tessera in a Flutter app as a native **platform view**
(macOS/Metal reference impl). Because Tessera's GPU calls are render-main-thread
and Flutter's Dart runs on the UI thread, the native platform view owns the
engine and render loop; Dart drives it via a per-view method channel plus FFI
(the `tessera` package's `Tessera.fromHandle`) for the any-thread `set_state`.
On Apple platforms the plugin reparents the engine's SDL swapchain `CAMetalLayer`
into the platform view and presents to it directly (zero-copy) — see the Flutter
package README; Android renders offscreen through `tessera_render_rgba` (below)
and blits the pixels into its `Surface`. Its `example/` is the chess game as a
live, interactive Flutter widget.

## Embedding API — `tessera_render_rgba`

For hosts that present frames themselves (a platform view / texture compositor)
rather than to the engine's own window: `tessera_render_rgba(e, dt, w, h, out,
out_size)` advances animation by `dt` and renders one frame offscreen into an
RGBA8 buffer — `tessera_tick` for embedders. Create the engine with a hidden
host window as `native_window` so it never shows its own window. Render/main
thread.

A small scenario — register defs, push a one-tile board with a unit, tick, and
capture a PNG:

```dart
import 'dart:ffi';
import 'package:ffi/ffi.dart';
import 'package:tessera/tessera.dart';

void main() {
  final t = Tessera(width: 640, height: 480);
  print('backend=${t.backendName} version=${t.versionString}');

  // 1. definitions
  final tile = calloc<TesseraTileDef>();
  tile.ref.thickness = 0.25;
  tile.ref.tint[0] = 0.5; tile.ref.tint[1] = 0.8;
  tile.ref.tint[2] = 0.4; tile.ref.tint[3] = 1.0;
  final grass = t.registerTileDef(tile);

  final ent = calloc<TesseraEntityDef>();
  ent.ref.scale = 1.0;                 // empty gltf => cube fallback
  final unit = t.registerEntityDef(ent);

  // 2. push a state
  final tiles = calloc<TesseraTilePlacement>(1);
  tiles[0].coord.x = 0; tiles[0].coord.y = 0; tiles[0].tileDef = grass;
  final ents = calloc<TesseraEntityPlacement>(1);
  ents[0].id = 1; ents[0].def = unit; ents[0].coord.x = 0; ents[0].coord.y = 0;

  final st = calloc<TesseraState>();
  st.ref.tiles = tiles;       st.ref.tileCount = 1;
  st.ref.entities = ents;     st.ref.entityCount = 1;
  st.ref.camera
    ..distance = 8.0 ..yaw = 0.6 ..pitch = 0.7 ..fov = 0.9;
  t.setState(st);             // deep-copied; frees below are safe

  // 3. tick to settle, then capture
  for (var i = 0; i < 30 && !t.isIdle; i++) t.tick(1 / 60);
  t.capturePng(640, 480, 'board.png');

  calloc..free(tile)..free(ent)..free(tiles)..free(ents)..free(st);
  t.dispose();
}
```

## Lua (LuaJIT FFI)

`bindings/lua/tessera.lua` `ffi.cdef`s the whole header and `ffi.load`s
`libtessera`, exposing the raw C namespace as `tessera.C` and the FFI module as
`tessera.ffi`.

```lua
local tessera = require("tessera")
local C, ffi = tessera.C, tessera.ffi

local e = tessera.create{ width = 640, height = 480 }
print("backend", ffi.string(C.tessera_backend_name(e)))

-- 1. definitions
local tile = ffi.new("TesseraTileDef")
tile.thickness = 0.25
tile.tint = ffi.new("float[4]", 0.5, 0.8, 0.4, 1.0)
local grass = C.tessera_register_tile_def(e, tile)

local ent = ffi.new("TesseraEntityDef")
ent.scale = 1.0                              -- empty gltf => cube fallback
local unit = C.tessera_register_entity_def(e, ent)

-- 2. push a state (arrays as (pointer, count))
local tiles = ffi.new("TesseraTilePlacement[1]")
tiles[0].coord.x, tiles[0].coord.y, tiles[0].tile_def = 0, 0, grass
local ents = ffi.new("TesseraEntityPlacement[1]")
ents[0].id, ents[0].def = 1, unit
ents[0].coord.x, ents[0].coord.y = 0, 0

local st = ffi.new("TesseraState")
st.tiles, st.tile_count = tiles, 1
st.entities, st.entity_count = ents, 1
st.camera.distance, st.camera.yaw = 8.0, 0.6
st.camera.pitch, st.camera.fov = 0.7, 0.9
C.tessera_set_state(e, st)

-- 3. tick to settle, then capture
for _ = 1, 30 do
    if C.tessera_is_idle(e) then break end
    C.tessera_tick(e, 1/60)
end
C.tessera_capture_png(e, 640, 480, "board.png")

C.tessera_destroy(e)
```

Move the unit on a later turn by pushing a new state that keeps the same entity
`id` at a new `coord`; the engine animates the transition (see
`docs/state-model.md`). Whenever `tessera.h` changes, re-run
`test_ffi_layout` and reconcile both bindings.
