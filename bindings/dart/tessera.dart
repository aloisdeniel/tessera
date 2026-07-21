// tessera.dart — dart:ffi binding for the Tessera renderer.
//
// Struct layouts mirror include/tessera.h exactly. A self-test
// (bindings/dart/test/layout_test.dart) should assert sizeOf<> against the C
// side. Load the shared library with DynamicLibrary.open('libtessera.dylib'|
// 'libtessera.so').
//
// This is a thin, hand-written binding covering the lifecycle + definition +
// state surface. Regenerate with `ffigen` from include/tessera.h for a full,
// always-in-sync binding in production.

import 'dart:ffi';
import 'dart:io' show Platform;
import 'package:ffi/ffi.dart';

// ---- opaque handle ----
final class TesseraEngine extends Opaque {}

// ---- POD structs ----
final class TesseraConfig extends Struct {
  external Pointer<Void> nativeWindow;
  @Int32() external int width;
  @Int32() external int height;
  @Float() external double pixelDensity;
  @Bool() external bool engineDrivenLoop;
  @Bool() external bool debug;
  external Pointer<Void> log;
  external Pointer<Void> logUserdata;
}

final class TesseraCoord extends Struct {
  @Int32() external int x;
  @Int32() external int y;
}

final class TesseraTilePlacement extends Struct {
  external TesseraCoord coord;
  @Uint32() external int tileDef;
  @Uint32() external int variant;
}

final class TesseraEntityPlacement extends Struct {
  @Uint64() external int id;
  @Uint32() external int def;
  external TesseraCoord coord;
  @Uint16() external int facing;
  @Uint32() external int anim;
}

final class TesseraEffectPlacement extends Struct {
  @Uint64() external int id;
  @Uint32() external int def;
  external TesseraCoord coord;
  @Uint64() external int attachEntityId;
}

final class TesseraCamera extends Struct {
  external TesseraCoord focus;
  @Float() external double distance;
  @Float() external double yaw;
  @Float() external double pitch;
  @Float() external double fov;
}

final class TesseraState extends Struct {
  external Pointer<TesseraTilePlacement> tiles;
  @Size() external int tileCount;
  external Pointer<TesseraEntityPlacement> entities;
  @Size() external int entityCount;
  external Pointer<TesseraEffectPlacement> effects;
  @Size() external int effectCount;
  external TesseraCamera camera;
  @Uint64() external int epoch;
}

// ---- function typedefs ----
typedef _CreateC = Pointer<TesseraEngine> Function(Pointer<TesseraConfig>);
typedef _CreateD = Pointer<TesseraEngine> Function(Pointer<TesseraConfig>);
typedef _DestroyC = Void Function(Pointer<TesseraEngine>);
typedef _DestroyD = void Function(Pointer<TesseraEngine>);
typedef _TickC = Void Function(Pointer<TesseraEngine>, Double);
typedef _TickD = void Function(Pointer<TesseraEngine>, double);
typedef _SetStateC = Void Function(Pointer<TesseraEngine>, Pointer<TesseraState>);
typedef _SetStateD = void Function(Pointer<TesseraEngine>, Pointer<TesseraState>);
typedef _LastErrC = Pointer<Utf8> Function(Pointer<TesseraEngine>);
typedef _LastErrD = Pointer<Utf8> Function(Pointer<TesseraEngine>);

/// High-level Dart wrapper around the C engine.
class Tessera {
  final DynamicLibrary _lib;
  late final Pointer<TesseraEngine> _engine;

  late final _CreateD _create =
      _lib.lookupFunction<_CreateC, _CreateD>('tessera_create');
  late final _DestroyD _destroy =
      _lib.lookupFunction<_DestroyC, _DestroyD>('tessera_destroy');
  late final _TickD _tick =
      _lib.lookupFunction<_TickC, _TickD>('tessera_tick');
  late final _SetStateD _setState =
      _lib.lookupFunction<_SetStateC, _SetStateD>('tessera_set_state');
  late final _LastErrD _lastError =
      _lib.lookupFunction<_LastErrC, _LastErrD>('tessera_last_error');

  Tessera({int width = 1280, int height = 720, bool debug = false})
      : _lib = _open() {
    final cfg = calloc<TesseraConfig>();
    cfg.ref.width = width;
    cfg.ref.height = height;
    cfg.ref.pixelDensity = 1.0;
    cfg.ref.debug = debug;
    _engine = _create(cfg);
    calloc.free(cfg);
    if (_engine == nullptr) throw StateError('tessera_create failed');
  }

  static DynamicLibrary _open() {
    if (Platform.isMacOS) return DynamicLibrary.open('libtessera.dylib');
    if (Platform.isAndroid || Platform.isLinux) {
      return DynamicLibrary.open('libtessera.so');
    }
    throw UnsupportedError('platform not supported');
  }

  void tick(double dt) => _tick(_engine, dt);
  void setState(Pointer<TesseraState> s) => _setState(_engine, s);
  String get lastError => _lastError(_engine).toDartString();
  void dispose() => _destroy(_engine);
}
