// ffi.dart — dart:ffi binding for the Tessera renderer.
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

import 'dart:ffi';

import 'package:ffi/ffi.dart';

import 'library.dart';

// ======================================================================
//  Handles & enums
// ======================================================================
final class TesseraEngine extends Opaque {}

// TesseraDefId is uint32 (0 = invalid/none); TesseraEntityId is uint64.
// Represented as plain Dart ints at the call sites.

/// enum TesseraLogLevel { TRACE=0, DEBUG=1, INFO=2, WARN=3, ERROR=4 }
abstract final class TesseraLogLevel {
  static const int trace = 0, debug = 1, info = 2, warn = 3, error = 4;
}

/// enum TesseraEmitMode { BURST=0, CONTINUOUS=1 }
abstract final class TesseraEmitMode {
  static const int burst = 0, continuous = 1;
}

/// enum TesseraBlendMode { ALPHA=0, ADD=1 }
abstract final class TesseraBlendMode {
  static const int alpha = 0, add = 1;
}

/// enum TesseraShadowMode { NONE=0, BLOB=1, MAP=2 }
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
  @Int32()
  external int width;
  @Int32()
  external int height;
  @Float()
  external double pixelDensity;
  @Bool()
  external bool engineDrivenLoop;
  @Bool()
  external bool debug;
  external Pointer<NativeFunction<TesseraLogFnC>> log; // optional; NULL => stderr
  external Pointer<Void> logUserdata;
}

// ======================================================================
//  Assets & definitions
// ======================================================================
final class TesseraBytes extends Struct {
  external Pointer<Void> data; // NULL => load from `path`
  @Size()
  external int size;
  external Pointer<Utf8> path;
  external Pointer<Utf8> debugName;
}

final class TesseraRect extends Struct {
  @Float()
  external double u0;
  @Float()
  external double v0;
  @Float()
  external double u1;
  @Float()
  external double v1;
}

final class TesseraTileDef extends Struct {
  @Uint32()
  external int atlas; // 0 = untextured / white
  external TesseraRect top;
  external TesseraRect side;
  external TesseraRect bottom;
  @Array(4)
  external Array<Float> tint; // RGBA multiply
  @Float()
  external double thickness; // relative tile height (default 0.25)
}

final class TesseraEntityDef extends Struct {
  external TesseraBytes gltf;
  @Uint32()
  external int atlas; // 0 = texture from gltf
  @Float()
  external double scale;
  @Array(3)
  external Array<Float> pivot; // feet anchor (model space)
  // Named clip roles resolved against clips in the gltf; -1 = unset.
  @Int32()
  external int defaultAnim;
  @Int32()
  external int moveAnim;
  @Int32()
  external int spawnAnim;
  @Int32()
  external int despawnAnim;
  // Linked effects played on spawn / despawn; 0 = none.
  @Uint32()
  external int onSpawnEffect;
  @Uint32()
  external int onDespawnEffect;
}

final class TesseraParticleSpec extends Struct {
  @Uint32()
  external int atlas; // 0 = white quad
  external TesseraRect sprite;
  @Int32()
  external int mode; // TesseraEmitMode
  @Uint32()
  external int count; // burst count, or rate/sec if continuous
  @Float()
  external double lifetimeS;
  @Float()
  external double lifetimeVar;
  @Float()
  external double speed;
  @Float()
  external double speedVar;
  @Float()
  external double spreadDeg;
  @Float()
  external double gravity;
  @Float()
  external double sizeStart;
  @Float()
  external double sizeEnd;
  @Array(4)
  external Array<Float> colorStart;
  @Array(4)
  external Array<Float> colorEnd;
  @Int32()
  external int blend; // TesseraBlendMode
  @Float()
  external double durationS; // 0 => one-shot burst
}

final class TesseraEffectDef extends Struct {
  external TesseraParticleSpec onAdd;
  external TesseraParticleSpec onRemove;
}

/// A card type. Each face references a registered atlas id (0 => white) plus the
/// sub-rect to use. [hiddenAtlas]/[backAtlas] are typically shared across a deck.
/// Dimensions are world units; <= 0 selects a default (a slab a little smaller
/// than 2x3 tiles). [tint] multiplies all faces.
final class TesseraCardDef extends Struct {
  @Uint32()
  external int visibleAtlas;
  external TesseraRect visibleUv;
  @Uint32()
  external int hiddenAtlas;
  external TesseraRect hiddenUv;
  @Uint32()
  external int backAtlas;
  external TesseraRect backUv;
  @Float()
  external double width;
  @Float()
  external double height;
  @Float()
  external double thickness;
  @Float()
  external double cornerRadius;
  @Array(4)
  external Array<Float> tint;
}

// ======================================================================
//  Dice
// ======================================================================
/// One face of a die: an encoded sprite image (shown centred on / filling it).
final class TesseraDiceFace extends Struct {
  external TesseraBytes sprite;
}

/// A die type. [faces] points at [faceCount] sprites (face index 0..count-1);
/// [faceCount] must be >= 2. [size] is the bounding diameter (<=0 => 1.0);
/// [tint] multiplies the sprites (all-zero => white).
final class TesseraDiceDef extends Struct {
  external Pointer<TesseraDiceFace> faces;
  @Size()
  external int faceCount;
  @Float()
  external double size;
  @Array(4)
  external Array<Float> tint;
}

// ======================================================================
//  Immutable state
// ======================================================================
final class TesseraCoord extends Struct {
  @Int32()
  external int x;
  @Int32()
  external int y;
}

/// Continuous board position: whole numbers land on tile centres, fractions
/// interpolate — (0.5, 0.5) is the corner shared by tiles (0,0) and (1,1).
final class TesseraCoordF extends Struct {
  @Float()
  external double x;
  @Float()
  external double y;
}

final class TesseraTilePlacement extends Struct {
  external TesseraCoord coord;
  @Uint32()
  external int tileDef; // 0 = no tile (hole)
  @Uint32()
  external int variant;
  @Uint64()
  external int id; // optional per-tile instance id (0 = none)
}

final class TesseraEntityPlacement extends Struct {
  @Uint64()
  external int id; // stable across states; diff key
  @Uint32()
  external int def;
  external TesseraCoord coord;
  @Uint16()
  external int facing; // 0..3 quadrant
  @Uint32()
  external int anim; // active clip index
  external Pointer<TesseraCoord> path; // multi-step waypoints (null => single move)
  @Uint32()
  external int pathCount;
}

final class TesseraEffectPlacement extends Struct {
  @Uint64()
  external int id;
  @Uint32()
  external int def;
  external TesseraCoord coord;
  @Uint64()
  external int attachEntityId; // 0 = anchored to tile coord
}

/// A die in the scene. A placement that newly appears (by id) is thrown; one
/// that vanishes fades out. Changing def/face/seed/position re-throws it.
final class TesseraDicePlacement extends Struct {
  @Uint64()
  external int id;
  @Uint32()
  external int def;
  @Uint32()
  external int face;
  @Array(3)
  external Array<Float> position;
  @Uint32()
  external int seed;
  @Float()
  external double throwS;
}

/// A card in the scene. [orientation] is a quaternion (xyzw; all-zero =>
/// identity, which lays it flat, front up). [hidden] shows the concealing front
/// (crossfades when toggled). If [hand] is non-zero the card is arranged by that
/// hand's fan ([position]/[orientation] ignored); [handSlot] orders it.
final class TesseraCardPlacement extends Struct {
  @Uint64()
  external int id;
  @Uint32()
  external int def;
  @Array(3)
  external Array<Float> position;
  @Array(4)
  external Array<Float> orientation;
  @Bool()
  external bool hidden;
  @Uint64()
  external int hand; // 0 => free placement
  @Uint32()
  external int handSlot;
  @Uint64()
  external int sourceDraw; // 0 => none; deal-from-pile spawn source
  external Pointer<Float> path; // multi-step waypoints, 3 floats each (null => single move)
  @Uint32()
  external int pathCount;
}

/// A pile of cards drawn as one slab (thickness tracks [count], tweens on
/// change). Top face shows the def's visible (or hidden when [topHidden]);
/// bottom always shows the def's hidden texture.
final class TesseraCardDrawPlacement extends Struct {
  @Uint64()
  external int id;
  @Uint32()
  external int def;
  @Array(3)
  external Array<Float> position;
  @Array(4)
  external Array<Float> orientation;
  @Uint32()
  external int count;
  @Bool()
  external bool topHidden;
}

/// A hand: fans out the cards whose [TesseraCardPlacement.hand] equals its id.
/// [orientation] is a quaternion; identity faces the fronts toward +Z and
/// spreads along +X. spread/radius/spacing default when <= 0.
final class TesseraHandPlacement extends Struct {
  @Uint64()
  external int id;
  @Array(3)
  external Array<Float> position;
  @Array(4)
  external Array<Float> orientation;
  @Float()
  external double spreadDeg;
  @Float()
  external double radius;
  @Float()
  external double cardSpacing;
}

final class TesseraCamera extends Struct {
  external TesseraCoordF focus;
  @Float()
  external double distance;
  @Float()
  external double yaw; // radians
  @Float()
  external double pitch; // radians
  @Float()
  external double fov; // vertical fov, radians
}

final class TesseraState extends Struct {
  external Pointer<TesseraTilePlacement> tiles;
  @Size()
  external int tileCount;
  external Pointer<TesseraEntityPlacement> entities;
  @Size()
  external int entityCount;
  external Pointer<TesseraEffectPlacement> effects;
  @Size()
  external int effectCount;
  external TesseraCamera camera;
  @Uint64()
  external int epoch; // optional caller sequence number
  // Appended after epoch so the offsets above stay stable.
  external Pointer<TesseraCardPlacement> cards;
  @Size()
  external int cardCount;
  external Pointer<TesseraCardDrawPlacement> cardDraws;
  @Size()
  external int cardDrawCount;
  external Pointer<TesseraHandPlacement> hands;
  @Size()
  external int handCount;
  external Pointer<TesseraDicePlacement> dice;
  @Size()
  external int diceCount;
}

// ======================================================================
//  Picking (screen ray -> scene)
// ======================================================================
final class TesseraPick extends Struct {
  @Bool()
  external bool hitTile;
  external TesseraCoord tile;
  @Float()
  external double tileDistance;
  @Bool()
  external bool hitEntity;
  @Uint64()
  external int entity;
  @Float()
  external double entityDistance;
  @Array(3)
  external Array<Float> rayOrigin;
  @Array(3)
  external Array<Float> rayDir;
  @Array(3)
  external Array<Float> point;
}

/// Inverse of picking: where a scene point lands on screen.
final class TesseraScreenPos extends Struct {
  @Bool()
  external bool onscreen;
  @Float()
  external double x; // logical window coords (top-left origin)
  @Float()
  external double y;
  @Float()
  external double depth; // NDC depth 0..1 (0 = near plane)
  @Array(3)
  external Array<Float> world; // world point that was projected
}

// ======================================================================
//  Timing / quality / lighting
// ======================================================================
final class TesseraTiming extends Struct {
  @Float()
  external double moveS;
  @Float()
  external double addS;
  @Float()
  external double removeS;
  @Float()
  external double tileS;
  @Float()
  external double reflowS;
  @Float()
  external double cameraS;
  @Float()
  external double speedMultiplier; // global; default 1.0
}

final class TesseraQuality extends Struct {
  @Int32()
  external int shadows; // TesseraShadowMode
  @Int32()
  external int msaa; // 1, 2, 4
  @Float()
  external double renderScale; // 1.0 = native
}

final class TesseraLight extends Struct {
  @Array(3)
  external Array<Float> dir; // points from light
  @Array(3)
  external Array<Float> color;
  @Float()
  external double intensity;
  @Array(3)
  external Array<Float> ambient;
}

/// enum TesseraProjection { PERSPECTIVE=0, ISOMETRIC=1 }
abstract final class TesseraProjection {
  static const int perspective = 0;
  static const int isometric = 1;
}

final class TesseraFocus extends Struct {
  @Bool()
  external bool enabled;
  @Float()
  external double focusDistance; // <=0 = auto (orbit focus)
  @Float()
  external double focusRange;
  @Float()
  external double blurStrength;
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

typedef _RegCardC = Uint32 Function(Pointer<TesseraEngine>, Pointer<TesseraCardDef>);
typedef _RegCardD = int Function(Pointer<TesseraEngine>, Pointer<TesseraCardDef>);
typedef _RegDiceC = Uint32 Function(Pointer<TesseraEngine>, Pointer<TesseraDiceDef>);
typedef _RegDiceD = int Function(Pointer<TesseraEngine>, Pointer<TesseraDiceDef>);
typedef _DiceFaceCountC = Uint32 Function(Pointer<TesseraEngine>, Uint32);
typedef _DiceFaceCountD = int Function(Pointer<TesseraEngine>, int);
typedef _DiceCountC = Uint32 Function(Pointer<TesseraEngine>);
typedef _DiceCountD = int Function(Pointer<TesseraEngine>);
typedef _DiceFaceC = Bool Function(Pointer<TesseraEngine>, Uint64, Pointer<Uint32>);
typedef _DiceFaceD = bool Function(Pointer<TesseraEngine>, int, Pointer<Uint32>);
typedef _DiceAllIdleC = Bool Function(Pointer<TesseraEngine>);
typedef _DiceAllIdleD = bool Function(Pointer<TesseraEngine>);

typedef _SetStateC = Uint64 Function(Pointer<TesseraEngine>, Pointer<TesseraState>);
typedef _SetStateD = int Function(Pointer<TesseraEngine>, Pointer<TesseraState>);
typedef _OpCompletedC = Bool Function(Pointer<TesseraEngine>, Uint64);
typedef _OpCompletedD = bool Function(Pointer<TesseraEngine>, int);
typedef _LastOpC = Uint64 Function(Pointer<TesseraEngine>);
typedef _LastOpD = int Function(Pointer<TesseraEngine>);
/// Native signature of the operation-completed callback: `void(uint64 op,
/// void* user)`. Wrap a Dart closure of this shape in a `NativeCallable` and
/// pass its `nativeFunction` to [Tessera.setOperationCallback].
typedef TesseraOpCompletedNative = Void Function(Uint64, Pointer<Void>);
typedef _SetOpCallbackC = Void Function(
    Pointer<TesseraEngine>, Pointer<NativeFunction<TesseraOpCompletedNative>>, Pointer<Void>);
typedef _SetOpCallbackD = void Function(
    Pointer<TesseraEngine>, Pointer<NativeFunction<TesseraOpCompletedNative>>, Pointer<Void>);
typedef _PickC = Bool Function(Pointer<TesseraEngine>, Float, Float, Pointer<TesseraPick>);
typedef _PickD = bool Function(Pointer<TesseraEngine>, double, double, Pointer<TesseraPick>);
typedef _WorldToScreenC =
    Bool Function(Pointer<TesseraEngine>, Pointer<Float>, Pointer<TesseraScreenPos>);
typedef _WorldToScreenD =
    bool Function(Pointer<TesseraEngine>, Pointer<Float>, Pointer<TesseraScreenPos>);
typedef _EntityScreenC =
    Bool Function(Pointer<TesseraEngine>, Uint64, Pointer<TesseraScreenPos>);
typedef _EntityScreenD =
    bool Function(Pointer<TesseraEngine>, int, Pointer<TesseraScreenPos>);
typedef _TileScreenC =
    Bool Function(Pointer<TesseraEngine>, Uint64, Pointer<TesseraScreenPos>);
typedef _TileScreenD =
    bool Function(Pointer<TesseraEngine>, int, Pointer<TesseraScreenPos>);
typedef _FitDistanceC = Bool Function(Pointer<TesseraEngine>, Pointer<Uint64>, Size,
    Pointer<Uint64>, Size, Float, Pointer<Float>);
typedef _FitDistanceD = bool Function(Pointer<TesseraEngine>, Pointer<Uint64>, int,
    Pointer<Uint64>, int, double, Pointer<Float>);
typedef _SetTimingC = Void Function(Pointer<TesseraEngine>, Pointer<TesseraTiming>);
typedef _SetTimingD = void Function(Pointer<TesseraEngine>, Pointer<TesseraTiming>);
typedef _IsIdleC = Bool Function(Pointer<TesseraEngine>);
typedef _IsIdleD = bool Function(Pointer<TesseraEngine>);
typedef _SetQualityC = Void Function(Pointer<TesseraEngine>, Pointer<TesseraQuality>);
typedef _SetQualityD = void Function(Pointer<TesseraEngine>, Pointer<TesseraQuality>);
typedef _SetLightC = Void Function(Pointer<TesseraEngine>, Pointer<TesseraLight>);
typedef _SetLightD = void Function(Pointer<TesseraEngine>, Pointer<TesseraLight>);
typedef _SetProjectionC = Void Function(Pointer<TesseraEngine>, Int32);
typedef _SetProjectionD = void Function(Pointer<TesseraEngine>, int);
typedef _SetFocusC = Void Function(Pointer<TesseraEngine>, Pointer<TesseraFocus>);
typedef _SetFocusD = void Function(Pointer<TesseraEngine>, Pointer<TesseraFocus>);

typedef _OrbitC = Void Function(Pointer<TesseraEngine>, Float, Float, Float);
typedef _OrbitD = void Function(Pointer<TesseraEngine>, double, double, double);
typedef _CaptureC = Bool Function(Pointer<TesseraEngine>, Int32, Int32, Pointer<Utf8>);
typedef _CaptureD = bool Function(Pointer<TesseraEngine>, int, int, Pointer<Utf8>);
typedef _RenderRgbaC =
    Bool Function(Pointer<TesseraEngine>, Double, Int32, Int32, Pointer<Void>, Size);
typedef _RenderRgbaD =
    bool Function(Pointer<TesseraEngine>, double, int, int, Pointer<Void>, int);

/// Override the directory that holds the engine's `shaders/` folder, read when
/// pipelines are built. Process-global — call BEFORE constructing a [Tessera]
/// (the engine loads shaders in its constructor). Pass "" to restore the
/// compile-time default. Needed on iOS/Android where assets are bundled rather
/// than at the build-time path; a no-op-friendly convenience on desktop.
void tesseraSetAssetDir(String dir, {DynamicLibrary? library, String? libraryPath}) {
  final lib = library ?? openTesseraLibrary(path: libraryPath);
  final fn = lib.lookupFunction<Void Function(Pointer<Utf8>),
      void Function(Pointer<Utf8>)>('tessera_set_asset_dir');
  final p = dir.toNativeUtf8();
  fn(p);
  calloc.free(p);
}

/// High-level Dart wrapper around the C engine. Exposes the FULL public API of
/// `include/tessera.h`.
///
/// Threading (see docs/platforms.md): GPU calls — create/destroy/tick/resize/
/// register*/capturePng — must run on the render (main) thread. setState,
/// setTiming, setLight, setQuality, isIdle and lastError are any-thread.
///
/// Construct once, drive frames with [tick], and always [dispose] when done.
/// The native library is located automatically (see [openTesseraLibrary]); pass
/// [libraryPath] or a pre-opened [library] to override discovery.
class Tessera {
  final DynamicLibrary _lib;
  late final Pointer<TesseraEngine> _engine;

  /// Whether this wrapper owns the engine (created it) and must destroy it on
  /// [dispose]. False for [Tessera.fromHandle], where a host (e.g. a native
  /// platform-view plugin) owns the engine lifecycle.
  final bool _ownsEngine;

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

  late final _RegCardD _registerCardDef =
      _lib.lookupFunction<_RegCardC, _RegCardD>('tessera_register_card_def');
  late final _RegDiceD _registerDiceDef =
      _lib.lookupFunction<_RegDiceC, _RegDiceD>('tessera_register_dice_def');
  late final _DiceFaceCountD _diceDefFaceCount = _lib
      .lookupFunction<_DiceFaceCountC, _DiceFaceCountD>('tessera_dice_def_face_count');
  late final _DiceCountD _diceCount =
      _lib.lookupFunction<_DiceCountC, _DiceCountD>('tessera_dice_count');
  late final _DiceFaceD _diceFace =
      _lib.lookupFunction<_DiceFaceC, _DiceFaceD>('tessera_dice_face');
  late final _DiceAllIdleD _diceAllIdle =
      _lib.lookupFunction<_DiceAllIdleC, _DiceAllIdleD>('tessera_dice_all_idle');

  late final _SetStateD _setState =
      _lib.lookupFunction<_SetStateC, _SetStateD>('tessera_set_state');
  late final _OpCompletedD _operationCompleted = _lib
      .lookupFunction<_OpCompletedC, _OpCompletedD>('tessera_operation_completed');
  late final _LastOpD _lastCompletedOperation = _lib
      .lookupFunction<_LastOpC, _LastOpD>('tessera_last_completed_operation');
  late final _SetOpCallbackD _setOperationCallback = _lib
      .lookupFunction<_SetOpCallbackC, _SetOpCallbackD>('tessera_set_operation_callback');
  late final _PickD _pick = _lib.lookupFunction<_PickC, _PickD>('tessera_pick');
  late final _WorldToScreenD _worldToScreen =
      _lib.lookupFunction<_WorldToScreenC, _WorldToScreenD>('tessera_world_to_screen');
  late final _EntityScreenD _entityScreenPosition = _lib
      .lookupFunction<_EntityScreenC, _EntityScreenD>('tessera_entity_screen_position');
  late final _TileScreenD _tileScreenPosition =
      _lib.lookupFunction<_TileScreenC, _TileScreenD>('tessera_tile_screen_position');
  late final _FitDistanceD _cameraFitDistance =
      _lib.lookupFunction<_FitDistanceC, _FitDistanceD>('tessera_camera_fit_distance');
  late final _SetTimingD _setTiming =
      _lib.lookupFunction<_SetTimingC, _SetTimingD>('tessera_set_timing');
  late final _IsIdleD _isIdle = _lib.lookupFunction<_IsIdleC, _IsIdleD>('tessera_is_idle');
  late final _SetQualityD _setQuality =
      _lib.lookupFunction<_SetQualityC, _SetQualityD>('tessera_set_quality');
  late final _SetLightD _setLight =
      _lib.lookupFunction<_SetLightC, _SetLightD>('tessera_set_light');
  late final _SetProjectionD _setProjection =
      _lib.lookupFunction<_SetProjectionC, _SetProjectionD>('tessera_set_projection');
  late final _SetFocusD _setFocus =
      _lib.lookupFunction<_SetFocusC, _SetFocusD>('tessera_set_focus');

  late final _OrbitD _debugOrbit =
      _lib.lookupFunction<_OrbitC, _OrbitD>('tessera__debug_orbit');
  late final _CaptureD _capturePng =
      _lib.lookupFunction<_CaptureC, _CaptureD>('tessera_capture_png');
  late final _RenderRgbaD _renderRgba =
      _lib.lookupFunction<_RenderRgbaC, _RenderRgbaD>('tessera_render_rgba');

  /// Create an engine and (when [nativeWindow] is null) its own window.
  ///
  /// [library] / [libraryPath] override native-library discovery. Throws
  /// [StateError] if `tessera_create` returns null.
  Tessera({
    int width = 1280,
    int height = 720,
    bool debug = false,
    double pixelDensity = 1.0,
    bool engineDrivenLoop = false,
    Pointer<Void>? nativeWindow,
    DynamicLibrary? library,
    String? libraryPath,
  })  : _lib = library ?? openTesseraLibrary(path: libraryPath),
        _ownsEngine = true {
    final cfg = calloc<TesseraConfig>();
    cfg.ref
      ..nativeWindow = nativeWindow ?? nullptr
      ..width = width
      ..height = height
      ..pixelDensity = pixelDensity
      ..engineDrivenLoop = engineDrivenLoop
      ..debug = debug;
    _engine = _create(cfg);
    calloc.free(cfg);
    if (_engine == nullptr) throw StateError('tessera_create failed');
  }

  /// Attach to an engine created and owned elsewhere in the same process — e.g.
  /// a native platform-view plugin that owns the GPU/render thread and hands the
  /// handle to Dart. This wrapper will NOT destroy the engine on [dispose].
  ///
  /// Only the any-thread calls (`setState`, `operationCompleted`,
  /// `lastCompletedOperation`, `isIdle`, `lastError`) are safe to use from
  /// Dart's UI isolate while the host ticks; GPU/render-thread calls
  /// (tick, resize, register*, pick, set light/quality/timing, capture, render)
  /// must be routed to the host's render thread — see docs/platforms.md.
  ///
  /// [handle] is the engine pointer as an integer address (as passed over a
  /// method channel from native, e.g. `reinterpret_cast<intptr_t>`).
  Tessera.fromHandle(
    int handle, {
    DynamicLibrary? library,
    String? libraryPath,
  })  : _lib = library ?? openTesseraLibrary(path: libraryPath),
        _ownsEngine = false {
    _engine = Pointer<TesseraEngine>.fromAddress(handle);
    if (_engine == nullptr) throw ArgumentError('null engine handle');
  }

  /// The raw engine handle, for calling any C entry point not wrapped here.
  Pointer<TesseraEngine> get handle => _engine;

  /// The underlying loaded native library.
  DynamicLibrary get library => _lib;

  // ---- lifecycle ----
  void resize(int w, int h, double density) => _resize(_engine, w, h, density);

  /// Advance animation + render one frame (host-driven mode). [dt] is seconds.
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

  // ---- cards ----
  /// Register a card def (visible/hidden/back atlas refs); returns a
  /// TesseraDefId (0 = fail). Place cards/piles/hands via [setState].
  int registerCardDef(Pointer<TesseraCardDef> def) => _registerCardDef(_engine, def);

  // ---- dice (state-driven: place dice via setState / TesseraDicePlacement) ----
  /// Register a dice def (per-face sprites); returns a TesseraDefId (0 = fail).
  int registerDiceDef(Pointer<TesseraDiceDef> def) => _registerDiceDef(_engine, def);

  /// Number of faces of a registered dice def (0 if not a dice def).
  int diceDefFaceCount(int def) => _diceDefFaceCount(_engine, def);

  /// Number of live dice (including those fading in/out).
  int get diceCount => _diceCount(_engine);

  /// The face targeted by a live die, or null if the id isn't present.
  int? diceFace(int id) {
    final out = calloc<Uint32>();
    try {
      return _diceFace(_engine, id, out) ? out.value : null;
    } finally {
      calloc.free(out);
    }
  }

  /// True when every live die has settled (no tumble or fade in progress).
  bool get diceAllIdle => _diceAllIdle(_engine);

  // ---- state / timing / quality / light ----
  /// Push a scene snapshot and return its *operation id* (monotonic, nonzero).
  /// The transition it triggers is complete once [operationCompleted] is true /
  /// [lastCompletedOperation] reaches it / the operation callback fires with it.
  int setState(Pointer<TesseraState> s) => _setState(_engine, s);

  /// True once operation [op] has fully animated. `op == 0` is always true.
  bool operationCompleted(int op) => _operationCompleted(_engine, op);

  /// Highest operation id whose transition has settled (0 if none yet).
  int get lastCompletedOperation => _lastCompletedOperation(_engine);

  /// Register a native callback fired once per operation as it completes (with
  /// the completed id). Fires on the engine tick thread; wrap it with a
  /// `NativeCallable.listener` when the listener lives on another isolate/thread.
  /// Pass nullptr to clear.
  void setOperationCallback(
    Pointer<NativeFunction<TesseraOpCompletedNative>> fn,
    Pointer<Void> user,
  ) =>
      _setOperationCallback(_engine, fn, user);

  /// Ray-pick the tile/entity under a logical window pixel (SDL input space).
  bool pick(double screenX, double screenY, Pointer<TesseraPick> out) =>
      _pick(_engine, screenX, screenY, out);

  /// Inverse of [pick]: project a world point (float[3]) to a logical window
  /// pixel. Returns false only on an invalid engine/viewport.
  bool worldToScreen(Pointer<Float> world, Pointer<TesseraScreenPos> out) =>
      _worldToScreen(_engine, world, out);

  /// Screen position of a live entity by its stable id. False if not present.
  bool entityScreenPosition(int id, Pointer<TesseraScreenPos> out) =>
      _entityScreenPosition(_engine, id, out);

  /// Screen position of a live tile by its instance id (0 = unqueryable).
  bool tileScreenPosition(int id, Pointer<TesseraScreenPos> out) =>
      _tileScreenPosition(_engine, id, out);

  /// Orbit distance (zoom) that keeps every listed tile/entity id on screen with
  /// a fractional [padding] margin. Pure query; feed the result into your
  /// camera's distance. Pass nullptr/0 for an unused id list.
  bool cameraFitDistance(Pointer<Uint64> tiles, int tileCount, Pointer<Uint64> entities,
          int entityCount, double padding, Pointer<Float> outDistance) =>
      _cameraFitDistance(_engine, tiles, tileCount, entities, entityCount, padding, outDistance);
  void setTiming(Pointer<TesseraTiming> t) => _setTiming(_engine, t);
  bool get isIdle => _isIdle(_engine);
  void setQuality(Pointer<TesseraQuality> q) => _setQuality(_engine, q);
  void setLight(Pointer<TesseraLight> l) => _setLight(_engine, l);

  /// Camera projection: TesseraProjection.perspective / .isometric.
  void setProjection(int mode) => _setProjection(_engine, mode);

  /// Depth-of-field; pass nullptr to disable.
  void setFocus(Pointer<TesseraFocus> focus) => _setFocus(_engine, focus);

  // ---- dev hooks ----
  void debugOrbit(double dyaw, double dpitch, double dzoom) =>
      _debugOrbit(_engine, dyaw, dpitch, dzoom);
  bool capturePng(int w, int h, String path) {
    final p = path.toNativeUtf8();
    final ok = _capturePng(_engine, w, h, p);
    calloc.free(p);
    return ok;
  }

  /// Advance animation by [dt] seconds and render one frame offscreen into
  /// [outRgba] (RGBA8, top-left origin, `w*h*4` bytes; [outSize] must be at
  /// least that). For embedders that present the frame themselves. Render-thread
  /// only. Returns false on failure.
  bool renderRgba(double dt, int w, int h, Pointer<Void> outRgba, int outSize) =>
      _renderRgba(_engine, dt, w, h, outRgba, outSize);

  /// Destroy the engine and release its window/GPU resources (owning wrappers
  /// only). A no-op for [Tessera.fromHandle], whose host owns the lifecycle.
  void dispose() {
    if (_ownsEngine) _destroy(_engine);
  }
}
