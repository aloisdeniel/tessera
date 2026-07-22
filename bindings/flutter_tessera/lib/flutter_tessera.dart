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
/// See `example/` for a full game of chess.
library;

export 'src/tessera_controller.dart';
export 'src/tessera_view.dart';
export 'src/types.dart';
