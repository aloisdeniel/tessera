/// Flutter platform-view embedding of the
/// [Tessera](https://github.com/aloisdeniel/tessera) 3D board-game renderer.
///
/// A native platform view hosts the Metal-backed engine and its render loop on
/// the platform main thread; Dart drives it through [TesseraController] — a mix
/// of a method channel (native/GPU calls) and FFI (the any-thread state hot
/// path, via the `tessera` package's `Tessera.fromHandle`).
///
/// ```dart
/// TesseraView(
///   onCreated: (c) async {
///     final grass = c.registerTileType(const TesseraTileType(thickness: 0.22));
///     c.setLight(const TesseraLightData());
///     c.setScene(TesseraScene(tiles: [...], entities: [...],
///         camera: const TesseraCameraPose()));
///     await c.start();
///   },
/// )
/// ```
///
/// See `example/` for a menu of three games — Chess (entities), Blackjack
/// (cards) and Yahtzee (dice) — on a small shared reducer framework.
library;

// The controller/view pair is platform-conditional: the io files drive a
// native platform view over FFI + a method channel; the web files drive the
// Emscripten/WebGPU wasm build of the engine bound to a <canvas>. Both expose
// the exact same public API. The io files import dart:ffi and must never be
// reachable on the web (and vice versa).
export 'src/tessera_controller.dart'
    if (dart.library.js_interop) 'src/web/tessera_controller_web.dart';
export 'src/tessera_view.dart'
    if (dart.library.js_interop) 'src/web/tessera_view_web.dart';
export 'src/types.dart';
