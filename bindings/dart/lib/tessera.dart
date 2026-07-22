/// Dart FFI bindings for the [Tessera](https://github.com/aloisdeniel/tessera)
/// table-top 3D renderer.
///
/// Tessera is a small, embeddable C + SDL3 renderer for turn-based, grid-based
/// games. The host registers immutable *definitions* (tiles / entities /
/// effects), then pushes immutable *state* snapshots; the engine deep-copies
/// each snapshot, diffs it against the previous one, and animates the
/// transitions.
///
/// This package is a thin binding — it wraps the frozen C ABI in
/// `include/tessera.h` one-to-one and does NOT bundle the compiled engine. Build
/// `libtessera` from the C sources once and make it discoverable (see the README
/// and [openTesseraLibrary]).
///
/// Quick start:
/// ```dart
/// import 'package:tessera/tessera.dart';
///
/// void main() {
///   final engine = Tessera(width: 1280, height: 800);
///   print('Tessera ${engine.versionString} on ${engine.backendName}');
///   // ... register defs, build a TesseraState, engine.setState(...), tick ...
///   engine.dispose();
/// }
/// ```
library;

export 'src/ffi.dart';
export 'src/library.dart' show openTesseraLibrary, tesseraLibraryName, TesseraLibraryNotFound;
