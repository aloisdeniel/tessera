# tessera_chess

A full game of chess rendered with **[Tessera](../README.md)** from Dart — a
port of the C `examples/chess`.

What it exercises:

- **`lib/chess_game.dart`** — a pure chess rules engine: full legal move
  generation (castling, en passant, promotion, check filtering), a heuristic AI
  that plays both sides, and a controller that threads a stable entity id
  through each square so moves animate as **slides**, not teleports.
- **`lib/chess_gen.dart`** — the six pieces × two armies generated at runtime as
  in-memory **GLB** models (a lathe of a hand-authored profile + a couple of
  boxes), tinted ivory vs. charcoal via the glTF `baseColorFactor`.
- **`lib/chess_render.dart`** — projects the rules state into a `TesseraState`
  (tile + entity placements) and a camera that swings behind the side to move,
  then pushes it; the engine diffs successive states and animates them.
- **`bin/chess.dart`** — drives an AI-vs-AI game, ticking the engine and writing
  one rendered PNG per ply.

## Run

Build the native library first (see the package README), then:

```sh
dart pub get
DYLD_LIBRARY_PATH=/opt/homebrew/lib \
  dart run bin/chess.dart --demo out --plies 40 --seed 0x1234abcd
```

`out/chess_000.png` is the opening position; `out/chess_001.png …` follow one
per ply. Flags:

| flag | default | meaning |
|---|---|---|
| `--demo <dir>` | `out` | output directory for the PNG sequence |
| `--plies N` | `40` | maximum plies to play |
| `--seed 0xNNNN` | `0x1234abcd` | AI RNG seed (reproducible games) |

If the library is not found automatically, point Dart at it:

```sh
export TESSERA_LIBRARY_PATH=/absolute/path/to/libtessera.dylib
```

## Verify the logic without a GPU

The rules engine, legal-move generation, AI, controller, and GLB generation
need no renderer, and are checked against the C reference game (same seed → same
30 plies) plus GLB structure validation:

```sh
dart test
```

## A note on rendering from pure-Dart CLI on macOS

The capture path creates the engine, which brings up SDL's **Cocoa/Metal**
backend. On macOS that backend must run on the process's **main thread**, but a
standalone Dart program's main isolate runs on a VM-managed thread, *not* OS
thread 0. So `Tessera(...)` from a `dart run` / `dart compile exe` binary fails
at `SDL_Init(VIDEO)` ("No available video device") even though the identical C
binary renders fine in the same shell — the C `main()` *is* on the main thread.
(The `dummy` video driver initialises from Dart, proving it is the Cocoa
main-thread requirement, not a missing display.)

Practical paths to live rendering:

- **Embed via Flutter**, where the engine runs on the platform main thread and
  presents into a texture; feed pointer events to `Tessera.pick`. This is the
  intended integration for an app.
- On backends without the Cocoa main-thread rule (Vulkan on Linux), a pure-Dart
  CLI can drive the capture demo directly once that backend is built.

The C example also supports click-to-move interactivity via SDL events, which a
pure-Dart port would obtain from a windowing toolkit rather than binding SDL.
