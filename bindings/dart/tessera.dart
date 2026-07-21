// tessera.dart — dart:ffi binding for the Tessera renderer.
//
// Struct layouts mirror include/tessera.h EXACTLY. The canonical layout
// reference (sizeof of every struct + offsetof of every field) is the C
// self-test tests/test_ffi_layout.c; run it and cross-check if you touch
// anything here. The ABI in include/tessera.h is FROZEN — evolution is
// append-only, so appending new trailing fields is safe but reordering /
// resizing existing ones is not.
//
// dart:ffi lays structs out with the platform C ABI (natural alignment, LP64:
// pointers/IntPtr/Size = 8 bytes, enums = Int32, bool = 1 byte, float = 4),
// so declaring the fields in header order reproduces the C layout without
// manual padding.
//
// Load the shared library with DynamicLibrary.open('libtessera.dylib' on
// macOS / 'libtessera.so' on Linux+Android). For a full always-in-sync
// binding in production, generate with `ffigen` from include/tessera.h.

import 'dart:ffi';
import 'dart:io' show Platform;
import 'package:ffi/ffi.dart';

// ======================================================================
//  Handles & enums
// ======================================================================
final class TesseraEngine extends Opaque {}

// TesseraDefId is uint32 (0 = invalid/none); TesseraEntityId is uint64.
// Represented as plain Dart ints at the call sites.

// enum TesseraLogLevel { TRACE=0, DEBUG=1, INFO=2, WARN=3, ERROR=4 }
abstract final class TesseraLogLevel {
  static const int trace = 0, debug = 1, info = 2, warn = 3, error = 4;
}

// enum TesseraEmitMode { BURST=0, CONTINUOUS=1 }
abstract final class TesseraEmitMode {
  static const int burst = 0, continuous = 1;
}

// enum TesseraBlendMode { ALPHA=0, ADD=1 }
abstract final class TesseraBlendMode {
  static const int alpha = 0, add = 1;
}

// enum TesseraShadowMode { NONE=0, BLOB=1, MAP=2 }
abstract final class TesseraShadowMode {
  static const int none = 0, blob = 1, map = 2;
}

// void (*TesseraLogFn)(void* userdata, int level, const char* msg)
typedef TesseraLogFnC = Void Function(Pointer<Void>, Int32, Pointer<Utf8>);

// ======================================================================
//  Lifecycle / config
// ======================================================================
final class TesseraConfig extends Struct {
  external Pointer<Void> nativeWindow; // NULL => engine creates its own window
  @Int32() external int width;
  @Int32() external int height;
  @Float() external double pixelDensity;
  @Bool() external bool engineDrivenLoop;
  @Bool() external bool debug;
  external Pointer<NativeFunction<TesseraLogFnC>> log; // optional; NULL => stderr
  external Pointer<Void> logUserdata;
}

// ======================================================================
//  Assets & definitions
// ======================================================================
final class TesseraBytes extends Struct {
  external Pointer<Void> data; // NULL => load from `path`
  @Size() external int size;
  external Pointer<Utf8> path;
  external Pointer<Utf8> debugName;
}

final class TesseraRect extends Struct {
  @Float() external double u0;
  @Float() external double v0;
  @Float() external double u1;
  @Float() external double v1;
}

final class TesseraTileDef extends Struct {
  @Uint32() external int atlas; // 0 = untextured / white
  external TesseraRect top;
  external TesseraRect side;
  external TesseraRect bottom;
  @Array(4) external Array<Float> tint; // RGBA multiply
  @Float() external double thickness; // relative tile height (default 0.25)
}

final class TesseraEntityDef extends Struct {
  external TesseraBytes gltf;
  @Uint32() external int atlas; // 0 = texture from gltf
  @Float() external double scale;
  @Array(3) external Array<Float> pivot; // feet anchor (model space)
  // Named clip roles resolved against clips in the gltf; -1 = unset.
  @Int32() external int defaultAnim;
  @Int32() external int moveAnim;
  @Int32() external int spawnAnim;
  @Int32() external int despawnAnim;
  // Linked effects played on spawn / despawn; 0 = none.
  @Uint32() external int onSpawnEffect;
  @Uint32() external int onDespawnEffect;
}

final class TesseraParticleSpec extends Struct {
  @Uint32() external int atlas; // 0 = white quad
  external TesseraRect sprite;
  @Int32() external int mode; // TesseraEmitMode
  @Uint32() external int count; // burst count, or rate/sec if continuous
  @Float() external double lifetimeS;
  @Float() external double lifetimeVar;
  @Float() external double speed;
  @Float() external double speedVar;
  @Float() external double spreadDeg;
  @Float() external double gravity;
  @Float() external double sizeStart;
  @Float() external double sizeEnd;
  @Array(4) external Array<Float> colorStart;
  @Array(4) external Array<Float> colorEnd;
  @Int32() external int blend; // TesseraBlendMode
  @Float() external double durationS; // 0 => one-shot burst
}

final class TesseraEffectDef extends Struct {
  external TesseraParticleSpec onAdd;
  external TesseraParticleSpec onRemove;
}

// ======================================================================
//  Immutable state
// ======================================================================
final class TesseraCoord extends Struct {
  @Int32() external int x;
  @Int32() external int y;
}

final class TesseraTilePlacement extends Struct {
  external TesseraCoord coord;
  @Uint32() external int tileDef; // 0 = no tile (hole)
  @Uint32() external int variant;
}

final class TesseraEntityPlacement extends Struct {
  @Uint64() external int id; // stable across states; diff key
  @Uint32() external int def;
  external TesseraCoord coord;
  @Uint16() external int facing; // 0..3 quadrant
  @Uint32() external int anim; // active clip index
}

final class TesseraEffectPlacement extends Struct {
  @Uint64() external int id;
  @Uint32() external int def;
  external TesseraCoord coord;
  @Uint64() external int attachEntityId; // 0 = anchored to tile coord
}

final class TesseraCamera extends Struct {
  external TesseraCoord focus;
  @Float() external double distance;
  @Float() external double yaw; // radians
  @Float() external double pitch; // radians
  @Float() external double fov; // vertical fov, radians
}

final class TesseraState extends Struct {
  external Pointer<TesseraTilePlacement> tiles;
  @Size() external int tileCount;
  external Pointer<TesseraEntityPlacement> entities;
  @Size() external int entityCount;
  external Pointer<TesseraEffectPlacement> effects;
  @Size() external int effectCount;
  external TesseraCamera camera;
  @Uint64() external int epoch; // optional caller sequence number
}

// ======================================================================
//  Picking (screen ray -> scene)
// ======================================================================
final class TesseraPick extends Struct {
  @Bool() external bool hitTile;
  external TesseraCoord tile;
  @Float() external double tileDistance;
  @Bool() external bool hitEntity;
  @Uint64() external int entity;
  @Float() external double entityDistance;
  @Array(3) external Array<Float> rayOrigin;
  @Array(3) external Array<Float> rayDir;
  @Array(3) external Array<Float> point;
}

// ======================================================================
//  Timing / quality / lighting
// ======================================================================
final class TesseraTiming extends Struct {
  @Float() external double moveS;
  @Float() external double addS;
  @Float() external double removeS;
  @Float() external double tileS;
  @Float() external double reflowS;
  @Float() external double cameraS;
  @Float() external double speedMultiplier; // global; default 1.0
}

final class TesseraQuality extends Struct {
  @Int32() external int shadows; // TesseraShadowMode
  @Int32() external int msaa; // 1, 2, 4
  @Float() external double renderScale; // 1.0 = native
}

final class TesseraLight extends Struct {
  @Array(3) external Array<Float> dir; // points from light
  @Array(3) external Array<Float> color;
  @Float() external double intensity;
  @Array(3) external Array<Float> ambient;
}

// ======================================================================
//  Function typedefs (C signature / Dart signature pairs)
// ======================================================================
typedef _CreateC = Pointer<TesseraEngine> Function(Pointer<TesseraConfig>);
typedef _CreateD = Pointer<TesseraEngine> Function(Pointer<TesseraConfig>);
typedef _DestroyC = Void Function(Pointer<TesseraEngine>);
typedef _DestroyD = void Function(Pointer<TesseraEngine>);
typedef _ResizeC = Void Function(Pointer<TesseraEngine>, Int32, Int32, Float);
typedef _ResizeD = void Function(Pointer<TesseraEngine>, int, int, double);
typedef _TickC = Void Function(Pointer<TesseraEngine>, Double);
typedef _TickD = void Function(Pointer<TesseraEngine>, double);
typedef _EngStrC = Pointer<Utf8> Function(Pointer<TesseraEngine>);
typedef _EngStrD = Pointer<Utf8> Function(Pointer<TesseraEngine>);
typedef _VersionC = Uint32 Function();
typedef _VersionD = int Function();
typedef _VersionStrC = Pointer<Utf8> Function();
typedef _VersionStrD = Pointer<Utf8> Function();

typedef _RegAtlasC = Uint32 Function(Pointer<TesseraEngine>, Pointer<TesseraBytes>);
typedef _RegAtlasD = int Function(Pointer<TesseraEngine>, Pointer<TesseraBytes>);
typedef _RegTileC = Uint32 Function(Pointer<TesseraEngine>, Pointer<TesseraTileDef>);
typedef _RegTileD = int Function(Pointer<TesseraEngine>, Pointer<TesseraTileDef>);
typedef _RegEntityC = Uint32 Function(Pointer<TesseraEngine>, Pointer<TesseraEntityDef>);
typedef _RegEntityD = int Function(Pointer<TesseraEngine>, Pointer<TesseraEntityDef>);
typedef _RegEffectC = Uint32 Function(Pointer<TesseraEngine>, Pointer<TesseraEffectDef>);
typedef _RegEffectD = int Function(Pointer<TesseraEngine>, Pointer<TesseraEffectDef>);
typedef _AnimCountC = Uint32 Function(Pointer<TesseraEngine>, Uint32);
typedef _AnimCountD = int Function(Pointer<TesseraEngine>, int);
typedef _AnimNameC = Pointer<Utf8> Function(Pointer<TesseraEngine>, Uint32, Uint32);
typedef _AnimNameD = Pointer<Utf8> Function(Pointer<TesseraEngine>, int, int);

typedef _SetStateC = Void Function(Pointer<TesseraEngine>, Pointer<TesseraState>);
typedef _SetStateD = void Function(Pointer<TesseraEngine>, Pointer<TesseraState>);
typedef _PickC = Bool Function(Pointer<TesseraEngine>, Float, Float, Pointer<TesseraPick>);
typedef _PickD = bool Function(Pointer<TesseraEngine>, double, double, Pointer<TesseraPick>);
typedef _SetTimingC = Void Function(Pointer<TesseraEngine>, Pointer<TesseraTiming>);
typedef _SetTimingD = void Function(Pointer<TesseraEngine>, Pointer<TesseraTiming>);
typedef _IsIdleC = Bool Function(Pointer<TesseraEngine>);
typedef _IsIdleD = bool Function(Pointer<TesseraEngine>);
typedef _SetQualityC = Void Function(Pointer<TesseraEngine>, Pointer<TesseraQuality>);
typedef _SetQualityD = void Function(Pointer<TesseraEngine>, Pointer<TesseraQuality>);
typedef _SetLightC = Void Function(Pointer<TesseraEngine>, Pointer<TesseraLight>);
typedef _SetLightD = void Function(Pointer<TesseraEngine>, Pointer<TesseraLight>);

typedef _OrbitC = Void Function(Pointer<TesseraEngine>, Float, Float, Float);
typedef _OrbitD = void Function(Pointer<TesseraEngine>, double, double, double);
typedef _CaptureC = Bool Function(Pointer<TesseraEngine>, Int32, Int32, Pointer<Utf8>);
typedef _CaptureD = bool Function(Pointer<TesseraEngine>, int, int, Pointer<Utf8>);

/// High-level Dart wrapper around the C engine. Exposes the FULL public API.
///
/// Threading (see docs/platforms.md): GPU calls — create/destroy/tick/resize/
/// register*/capturePng — must run on the render (main) thread. setState,
/// setTiming, setLight, setQuality, isIdle and lastError are any-thread.
class Tessera {
  final DynamicLibrary _lib;
  late final Pointer<TesseraEngine> _engine;

  late final _CreateD _create = _lib.lookupFunction<_CreateC, _CreateD>('tessera_create');
  late final _DestroyD _destroy = _lib.lookupFunction<_DestroyC, _DestroyD>('tessera_destroy');
  late final _ResizeD _resize = _lib.lookupFunction<_ResizeC, _ResizeD>('tessera_resize');
  late final _TickD _tick = _lib.lookupFunction<_TickC, _TickD>('tessera_tick');
  late final _EngStrD _lastError = _lib.lookupFunction<_EngStrC, _EngStrD>('tessera_last_error');
  late final _EngStrD _backendName =
      _lib.lookupFunction<_EngStrC, _EngStrD>('tessera_backend_name');
  late final _VersionD _version = _lib.lookupFunction<_VersionC, _VersionD>('tessera_version');
  late final _VersionStrD _versionString =
      _lib.lookupFunction<_VersionStrC, _VersionStrD>('tessera_version_string');

  late final _RegAtlasD _registerAtlas =
      _lib.lookupFunction<_RegAtlasC, _RegAtlasD>('tessera_register_atlas');
  late final _RegTileD _registerTileDef =
      _lib.lookupFunction<_RegTileC, _RegTileD>('tessera_register_tile_def');
  late final _RegEntityD _registerEntityDef =
      _lib.lookupFunction<_RegEntityC, _RegEntityD>('tessera_register_entity_def');
  late final _RegEffectD _registerEffectDef =
      _lib.lookupFunction<_RegEffectC, _RegEffectD>('tessera_register_effect_def');
  late final _AnimCountD _animCount =
      _lib.lookupFunction<_AnimCountC, _AnimCountD>('tessera_entity_def_anim_count');
  late final _AnimNameD _animName =
      _lib.lookupFunction<_AnimNameC, _AnimNameD>('tessera_entity_def_anim_name');

  late final _SetStateD _setState =
      _lib.lookupFunction<_SetStateC, _SetStateD>('tessera_set_state');
  late final _PickD _pick =
      _lib.lookupFunction<_PickC, _PickD>('tessera_pick');
  late final _SetTimingD _setTiming =
      _lib.lookupFunction<_SetTimingC, _SetTimingD>('tessera_set_timing');
  late final _IsIdleD _isIdle = _lib.lookupFunction<_IsIdleC, _IsIdleD>('tessera_is_idle');
  late final _SetQualityD _setQuality =
      _lib.lookupFunction<_SetQualityC, _SetQualityD>('tessera_set_quality');
  late final _SetLightD _setLight =
      _lib.lookupFunction<_SetLightC, _SetLightD>('tessera_set_light');

  late final _OrbitD _debugOrbit =
      _lib.lookupFunction<_OrbitC, _OrbitD>('tessera__debug_orbit');
  late final _CaptureD _capturePng =
      _lib.lookupFunction<_CaptureC, _CaptureD>('tessera_capture_png');

  Tessera({int width = 1280, int height = 720, bool debug = false, double pixelDensity = 1.0})
      : _lib = _open() {
    final cfg = calloc<TesseraConfig>();
    cfg.ref.width = width;
    cfg.ref.height = height;
    cfg.ref.pixelDensity = pixelDensity;
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

  Pointer<TesseraEngine> get handle => _engine;

  // ---- lifecycle ----
  void resize(int w, int h, double density) => _resize(_engine, w, h, density);
  void tick(double dt) => _tick(_engine, dt);
  String get lastError => _lastError(_engine).toDartString();
  String get backendName => _backendName(_engine).toDartString();
  int get version => _version();
  String get versionString => _versionString().toDartString();

  // ---- definitions (return a TesseraDefId; 0 = failure) ----
  int registerAtlas(Pointer<TesseraBytes> image) => _registerAtlas(_engine, image);
  int registerTileDef(Pointer<TesseraTileDef> def) => _registerTileDef(_engine, def);
  int registerEntityDef(Pointer<TesseraEntityDef> def) => _registerEntityDef(_engine, def);
  int registerEffectDef(Pointer<TesseraEffectDef> def) => _registerEffectDef(_engine, def);
  int entityDefAnimCount(int def) => _animCount(_engine, def);
  String entityDefAnimName(int def, int index) =>
      _animName(_engine, def, index).toDartString();

  // ---- state / timing / quality / light ----
  void setState(Pointer<TesseraState> s) => _setState(_engine, s);

  /// Ray-pick the tile/entity under a logical window pixel (SDL input space).
  bool pick(double screenX, double screenY, Pointer<TesseraPick> out) =>
      _pick(_engine, screenX, screenY, out);
  void setTiming(Pointer<TesseraTiming> t) => _setTiming(_engine, t);
  bool get isIdle => _isIdle(_engine);
  void setQuality(Pointer<TesseraQuality> q) => _setQuality(_engine, q);
  void setLight(Pointer<TesseraLight> l) => _setLight(_engine, l);

  // ---- dev hooks ----
  void debugOrbit(double dyaw, double dpitch, double dzoom) =>
      _debugOrbit(_engine, dyaw, dpitch, dzoom);
  bool capturePng(int w, int h, String path) {
    final p = path.toNativeUtf8();
    final ok = _capturePng(_engine, w, h, p);
    calloc.free(p);
    return ok;
  }

  void dispose() => _destroy(_engine);
}
