# tessera (Dart)

Dart `dart:ffi` bindings for **[Tessera](https://github.com/aloisdeniel/tessera)**
— a small, embeddable **C + SDL3** 3D renderer for turn-based, grid-based games.

The host registers immutable *definitions* (tile / entity / effect types), then
pushes immutable *state* snapshots. The engine deep-copies each snapshot, diffs
it against the previous one, and animates the transitions (pieces sliding,
tiles appearing, capture puffs, the camera swinging to the side to move). The
whole surface is one frozen C header, `include/tessera.h`, mirrored here
one-to-one.

## Installing

Add the dependency (path or git, until published):

```yaml
dependencies:
  tessera:
    path: ../bindings/dart   # or: git: { url: ..., path: bindings/dart }
```

Then `dart pub get`.

## The native library

This package is a **binding only** — it does not ship the compiled engine. Build
`libtessera` once from the C sources at the repo root:

```sh
brew install sdl3                        # macOS (Linux: libsdl3-dev)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
# produces build/libtessera.dylib (macOS) / .so (Linux) / .dll (Windows)
```

At runtime the binding finds the library automatically (first hit wins):

1. `Tessera(libraryPath: '/path/to/libtessera.dylib')`
2. `$TESSERA_LIBRARY_PATH` (full path) or `$TESSERA_LIBRARY_DIR` (directory)
3. `./`, `./build/`, `../build/`, next to the Dart executable/script
4. the bare platform name, letting the OS loader search its own paths

Tessera links SDL3, so SDL must be loadable too. On macOS/Homebrew that means
running with `DYLD_LIBRARY_PATH=/opt/homebrew/lib`.

## Usage

```dart
import 'dart:ffi';
import 'package:ffi/ffi.dart';
import 'package:tessera/tessera.dart';

void main() {
  final engine = Tessera(width: 1280, height: 800);
  stdout.writeln('Tessera ${engine.versionString} · ${engine.backendName}');

  // Register a tile definition.
  final tile = calloc<TesseraTileDef>();
  tile.ref.thickness = 0.22;
  tile.ref.tint[0] = 0.82; tile.ref.tint[1] = 0.78;
  tile.ref.tint[2] = 0.68; tile.ref.tint[3] = 1.0;
  final tileDef = engine.registerTileDef(tile);
  calloc.free(tile);

  // Build a one-tile state and push it (see example/ for the full pattern).
  // ...

  engine.dispose();
}
```

The `Tessera` class wraps the entire C ABI. Struct arguments are plain
`dart:ffi` structs you allocate with `package:ffi`'s `calloc`; the field names
match the C header in `lowerCamelCase`. See `lib/src/ffi.dart` for the mapping
and `include/tessera.h` for the authoritative documentation of each call.

## Example: chess

`example/` is a complete game of chess, ported from the C `examples/chess`:

- a pure chess rules engine (full legal move generation — castling, en passant,
  promotion, check filtering),
- the six pieces × two armies generated at runtime as in-memory **GLB** models
  (a lathe of a hand-authored profile, tinted ivory vs. charcoal),
- a heuristic AI that plays both sides,
- persistent per-square entity ids so the engine animates a **slide** rather
  than a teleport, with a capture puff on despawn,
- the camera swinging behind the side to move.

Run the AI-vs-AI demo, writing one rendered PNG per ply:

```sh
cd bindings/dart
dart pub get
cd example && dart pub get
DYLD_LIBRARY_PATH=/opt/homebrew/lib \
  dart run bin/chess.dart --demo out --plies 40 --seed 0x1234abcd
```

## Layout

```
lib/tessera.dart        public API (barrel export)
lib/src/ffi.dart        struct + function bindings, the `Tessera` class
lib/src/library.dart    native-library discovery
example/                the chess port
```

## License

MIT — see `LICENSE`.
