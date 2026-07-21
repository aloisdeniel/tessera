# FFI & bindings

The public ABI is deliberately FFI-friendly (see `include/tessera.h`):

- Pure C, `extern "C"`, no C++/inline/macros in the surface.
- Only fixed-width integer types (`stdint.h`), `bool`, `float`, and pointers.
- Flat structs; arrays passed as `(pointer, count)` pairs of POD elements.
- Opaque handles (`TesseraEngine*`, `TesseraDefId`, `TesseraEntityId`) only; no
  engine ownership leaks across the boundary.
- No required callbacks for basic use (the logging callback is optional).
- Struct layouts evolve append-only.

## Threading contract

| Function | Thread |
|---|---|
| `tessera_create` / `tessera_destroy` | render thread |
| `tessera_tick` / `tessera_resize` | render thread (GPU) |
| `tessera_register_*` | render thread (uploads GPU resources) |
| `tessera_set_state` | **any thread** (deep-copies, mutex-guarded) |
| `tessera_set_timing` / `tessera_set_light` / `tessera_set_quality` | any thread |
| `tessera_last_error` | any thread |

## Dart (`dart:ffi`)

`bindings/dart/tessera.dart` is a hand-written binding covering the lifecycle,
definition, and state surface, plus a `Tessera` wrapper class. For a full,
always-in-sync binding, run [`ffigen`](https://pub.dev/packages/ffigen) against
`include/tessera.h`.

```dart
final t = Tessera(width: 1280, height: 720);
final state = calloc<TesseraState>();
// ... fill tiles/entities arrays ...
t.setState(state);
t.tick(0.016);
t.dispose();
```

## Lua (LuaJIT FFI)

`bindings/lua/tessera.lua` `ffi.cdef`s the header and `ffi.load`s `libtessera`.

```lua
local tessera = require("tessera")
local e = tessera.create{ width = 1280, height = 720 }
local C, ffi = tessera.C, tessera.ffi
-- build states with ffi.new("TesseraTilePlacement[?]", n) etc.
C.tessera_tick(e, 0.016)
C.tessera_destroy(e)
```

## Layout self-test

The classic FFI bug is struct-layout drift. Each binding should assert
`sizeof`/`offsetof` against the C side (a `tessera_abi_selftest` helper can be
added that returns the sizes for the binding to compare). Regenerate bindings
whenever `tessera.h` changes.
