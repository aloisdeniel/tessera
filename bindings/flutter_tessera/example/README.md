# flutter_tessera example — Chess

A full game of chess rendered with Tessera inside a Flutter app — the Flutter
port of the C `examples/chess`.

- The board is a live `TesseraView` (a native platform view).
- A heuristic AI plays both sides while **autoplay** is on (toolbar ▶/⏸).
- **Tap** a piece of the side to move, then a destination square, to move it —
  the tap is ray-picked through `TesseraController.pick`.
- **↻** starts a new game.

The chess rules engine (`chess_game.dart`) and the runtime GLB piece models
(`chess_gen.dart`) are the same pure-Dart code unit-tested in the `tessera`
package; `chess_scene.dart` projects the game into a `TesseraScene` using
flutter_tessera's value types.

## Run

Build the native library first (see the plugin README), then:

```sh
flutter run -d macos
```

> The Metal renderer needs the GPU/window subsystem; during development run an
> unsandboxed debug build and make sure `libtessera.dylib` + `libSDL3.dylib` are
> reachable at runtime.
