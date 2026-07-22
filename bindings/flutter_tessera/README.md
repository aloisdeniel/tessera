# flutter_tessera

Embed the **[Tessera](https://github.com/aloisdeniel/tessera)** 3D board-game
renderer in a Flutter app as a native **platform view**.

A native platform view hosts the Metal-backed engine and its render loop on the
platform main thread; Dart drives it through a `TesseraController`. You register
tile / entity / effect definitions, push immutable scene snapshots, and the
engine diffs successive scenes and animates the transitions (pieces sliding,
capture puffs, the camera swinging).

```dart
TesseraView(
  onCreated: (c) async {
    final light = c.registerTileType(const TesseraTileType(thickness: 0.22));
    // ... register entities from GLB bytes, set light/quality/timing ...
    c.setScene(TesseraScene(tiles: [...], entities: [...],
        camera: const TesseraCameraPose()));
    await c.start();
  },
)
```

## Architecture & threading

Tessera's C API is render-**main-thread** for everything GPU-touching
(create / tick / resize / register / pick / lighting), and only `set_state` /
`is_idle` are any-thread (see `tessera/docs/platforms.md`). Flutter runs Dart on
the UI thread, so the split is:

| Concern | Runs where | How |
|---|---|---|
| Engine lifecycle + render loop | native, main thread | the platform view owns an `FTessera` bridge; a main-runloop timer calls `tessera_render_rgba` and blits into the view's `CAMetalLayer` |
| Def registration, light/quality/timing, camera fit | Dart UI thread, **before** `start()` | FFI (`Tessera.fromHandle`) — safe because the render loop is paused during setup |
| `setScene` (hot path) | Dart UI thread, anytime | FFI `tessera_set_state` (any-thread, mutex-guarded) |
| `pick`, `start` | native, main thread | per-view method channel `flutter_tessera/view_<id>` |

Because the native engine renders offscreen (into an RGBA buffer via the
`tessera_render_rgba` C API added for embedding) and the plugin presents those
pixels into the platform view's own `CAMetalLayer`, Tessera composits cleanly
inside the Flutter widget tree.

## Platform support

| Platform | Status |
|---|---|
| **macOS** (Metal) | Reference implementation (Swift + C bridge) |
| iOS / Android | Not yet implemented — the Dart API + native contract are documented; a `UIView` / `SurfaceView` platform view would follow the same shape |

## Building the native library

flutter_tessera links the prebuilt Tessera engine and SDL3; it does not build
them. Once, from the repo root:

```sh
brew install sdl3
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j          # -> build/libtessera.dylib
```

The macOS plugin's Swift Package manifest
(`macos/flutter_tessera/Package.swift`) links `libtessera` from `<repo>/build`
and `SDL3` from Homebrew by default; override with the `TESSERA_LIB_DIR`,
`SDL3_LIB_DIR`, and `TESSERA_INCLUDE_DIR` environment variables. A CocoaPods
podspec mirrors this for non-SPM projects. For a release app you must bundle
`libtessera.dylib` and `libSDL3.dylib` into the app's `Frameworks` and set the
rpath accordingly (and disable App Sandbox during development, since SDL opens a
GPU device / window).

## Status & what is verified

The **Dart layer and the C bridge are verified**:

- `flutter analyze` is clean for the plugin and the example.
- The C bridge (`macos/.../CTessera/tessera_bridge.c`) **compiles and links**
  against `libtessera` + SDL3, exercising the embedding API.
- The chess rules/model logic is the same code unit-tested in the `tessera`
  package (it reproduces the C reference game move-for-move).

The **Swift/Metal glue and the Xcode/Flutter native build** could not be
compiled or run in the authoring environment (no Flutter macOS toolchain there).
It is written to the standard AppKit/Metal APIs as a reference implementation;
expect to shake out minor issues on the first on-device build.

## Example

`example/` is a full game of chess — a live `TesseraView` board, a heuristic AI
playing both sides (autoplay), and tap-to-move via `TesseraController.pick`.
Ported from the C `examples/chess`.

```sh
cd example
flutter run -d macos
```

## License

MIT — see `LICENSE`.
