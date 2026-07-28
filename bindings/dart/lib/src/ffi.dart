// ffi.dart — dart:ffi binding for the Tessera renderer.
//
// Struct layouts mirror include/tessera.h EXACTLY. The canonical layout
// reference (sizeof of every struct + offsetof of every field) is the C
// self-test tests/test_ffi_layout.c (`--dump` for the machine-readable table);
// the ctest gate `ffi_binding_drift` (tools/check_ffi_bindings.py) diffs this
// file against it and hard-fails on any drift. The ABI in include/tessera.h is
// FROZEN — evolution is append-only, so appending new trailing fields is safe
// but reordering / resizing existing ones is not.
//
// dart:ffi lays structs out with the platform C ABI (natural alignment, LP64:
// pointers/IntPtr/Size = 8 bytes, enums = Int32, bool = 1 byte, float = 4),
// so declaring the fields in header order reproduces the C layout without
// manual padding.

import 'dart:async';
import 'dart:ffi';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';

import 'library.dart';

// ======================================================================
//  Handles & enums
// ======================================================================
final class TesseraEngine extends Opaque {}

/// Opaque replay handle (a timestamped sequence of serialized state blobs);
/// see `tessera_replay_*` in include/tessera.h.
final class TesseraReplay extends Opaque {}

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
  @Uint64()
  external int selectedCard;
}

/// enum TesseraOverlayShape: SPRITE samples atlas/uv across the tile (atlas 0
/// => solid tinted quad); DISC and RING are procedural fills.
abstract final class TesseraOverlayShape {
  static const int sprite = 0;
  static const int disc = 1;
  static const int ring = 2;
}

/// A flat decal rendered on top of the tile at [coord] (move-range fills,
/// threat rings, drop-target highlights). Keyed by coord when diffing: a coord
/// that newly appears fades in, one that vanishes fades out, and a tint change
/// crossfades from the currently displayed tint. At most one overlay per coord.
/// When [pulseS] > 0 the overlay breathes: alpha between
/// [pulseAlphaMin]..[pulseAlphaMax] (when max > 0) and/or footprint scale
/// between [pulseScaleMin]..[pulseScaleMax] (when max > 0).
final class TesseraOverlayPlacement extends Struct {
  external TesseraCoord coord;
  @Uint32()
  external int shape; // TesseraOverlayShape
  @Uint32()
  external int atlas; // SPRITE source atlas (0 = untextured/white)
  external TesseraRect uv; // SPRITE sub-rect of atlas
  @Array(4)
  external Array<Float> tint; // RGBA multiply (all-zero => white)
  @Float()
  external double pulseS; // breathe period seconds (<= 0 => steady)
  @Float()
  external double pulseAlphaMin;
  @Float()
  external double pulseAlphaMax;
  @Float()
  external double pulseScaleMin;
  @Float()
  external double pulseScaleMax;
}

/// enum TesseraLabelAnchor: what a text label is glued to. WORLD anchors at
/// `position` directly; the other modes anchor to a live object by id and
/// treat `position` as an offset from its LIVE animating transform.
abstract final class TesseraLabelAnchor {
  static const int world = 0;
  static const int entity = 1;
  static const int tile = 2;
  static const int dice = 3;
  static const int card = 4;
  static const int draw = 5;
}

/// A 3D text label (scores, HP, dice totals, board coordinates). Keyed by
/// [id] when diffing: a new id fades in, a vanished one fades out, and a text
/// or color change crossfades from what is currently displayed. [text] is a
/// bounded inline UTF-8 array (64 bytes incl. NUL); [size] is the line height
/// in world units (<=0 => 0.5); [color] multiplies the glyphs (all-zero =>
/// white). With [billboard] the label always faces the camera; otherwise it
/// lies flat on the ground plane.
final class TesseraLabelPlacement extends Struct {
  @Uint64()
  external int id;
  @Uint32()
  external int font; // registered font def id (0 => label skipped)
  @Array(64)
  external Array<Uint8> text; // UTF-8, NUL-terminated
  @Uint32()
  external int anchor; // TesseraLabelAnchor
  @Uint64()
  external int anchorId; // live object id (anchor != world)
  @Array(3)
  external Array<Float> position; // world point (world) or anchor offset
  @Float()
  external double size; // line height, world units (<=0 => 0.5)
  @Array(4)
  external Array<Float> color; // RGBA multiply (all-zero => white)
  @Bool()
  external bool billboard; // face the camera each frame
}

/// enum TesseraHighlightKind: what a selection highlight is attached to —
/// a live object kind plus its instance id, mirroring the camera FOCUS_* /
/// label anchor conventions.
abstract final class TesseraHighlightKind {
  static const int entity = 0;
  static const int tile = 1;
  static const int dice = 2;
  static const int card = 3;
}

/// enum TesseraHighlightStyle: crisp colored outline or soft additive glow.
abstract final class TesseraHighlightStyle {
  static const int outline = 0;
  static const int glow = 1;
}

/// A screen-space selection highlight on one live object, keyed by
/// ([kind], [targetId]) when diffing: a new one fades in, a vanished one
/// fades out, and a color change crossfades. The object is re-rendered into
/// a silhouette mask at its LIVE animating transform and composited over the
/// lit scene (after depth-of-field) as an outline or glow. [thickness] is in
/// pixels (<=0 => default); when [pulseS] > 0 the intensity breathes between
/// [pulseMin]..[pulseMax] (used when max > 0).
final class TesseraHighlightPlacement extends Struct {
  @Uint64()
  external int targetId; // live object id, interpreted per kind (diff key)
  @Uint32()
  external int kind; // TesseraHighlightKind
  @Uint32()
  external int style; // TesseraHighlightStyle
  @Array(4)
  external Array<Float> color; // RGBA (all-zero => white)
  @Float()
  external double thickness; // outline width / glow radius, px (<=0 => default)
  @Float()
  external double pulseS; // breathe period seconds (<=0 => steady)
  @Float()
  external double pulseMin;
  @Float()
  external double pulseMax; // intensity range (used when max > 0)
}

/// enum TesseraCameraMode: how the camera is positioned. 0 = ORBIT (default,
/// reproduces the classic board camera when the whole struct is left zeroed).
abstract final class TesseraCameraMode {
  static const int orbit = 0;
  static const int manual = 1;
  static const int target = 2;
  static const int focusTile = 3;
  static const int focusEntity = 4;
  static const int focusDice = 5;
  static const int focusDraw = 6;
  static const int focusCard = 7;
  static const int focusHand = 8;
}

/// Tagged camera. [mode] (TesseraCameraMode) selects which fields matter; the
/// engine tweens the camera pose from its current pose to the resolved goal as
/// state evolves. ORBIT uses focus/distance/yaw/pitch; MANUAL uses position/
/// orientation; TARGET uses position/target; the FOCUS_* modes follow a live
/// object by [targetId]. [fov] (vertical radians; <=0 => keep current) applies
/// to every mode.
final class TesseraCamera extends Struct {
  @Uint32()
  external int mode; // TesseraCameraMode
  external TesseraCoordF focus; // ORBIT grid focus
  @Float()
  external double distance; // ORBIT + FOCUS_* : dst from the framed point
  @Float()
  external double yaw; // ORBIT + FOCUS_* : orbit angle, radians
  @Float()
  external double pitch; // ORBIT + FOCUS_* : orbit angle, radians
  @Float()
  external double fov; // all modes; vertical fov radians (<=0 => keep)
  @Array(3)
  external Array<Float> position; // MANUAL/TARGET eye
  @Array(4)
  external Array<Float> orientation; // MANUAL quaternion xyzw (all-zero => identity)
  @Array(3)
  external Array<Float> target; // TARGET look-at point
  @Uint64()
  external int targetId; // FOCUS_* object id (tile/entity/dice/draw/card/hand)
  @Uint64()
  external int focusCardId; // FOCUS_HAND: optional card to bring fullscreen (0=none)
  @Float()
  external double fitPadding; // FOCUS_CARD/FOCUS_HAND: frame margin (<=0 => 0.08)
}

/// A positional light with spherical falloff, adding to the global
/// directional + ambient light. Diff-keyed by id: appears fade in, vanished
/// fade out, parameter changes tween.
final class TesseraPointLightPlacement extends Struct {
  @Uint64()
  external int id;
  @Array(3)
  external Array<Float> position;
  @Array(3)
  external Array<Float> color;
  @Float()
  external double intensity;
  @Float()
  external double radius; // falloff range, world units (<=0 => default 6)
}

/// A static decoration model in continuous world coordinates; its origin
/// plane (position y = 0) sits just below the tiles. Uses an entity def.
final class TesseraWorldModelPlacement extends Struct {
  @Uint64()
  external int id;
  @Uint32()
  external int def;
  @Array(3)
  external Array<Float> position;
  @Array(4)
  external Array<Float> orientation; // quat xyzw (all-zero => identity)
  @Float()
  external double scale; // extra multiplier (<=0 => 1)
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
  external Pointer<TesseraOverlayPlacement> overlays;
  @Size()
  external int overlayCount;
  external Pointer<TesseraLabelPlacement> labels;
  @Size()
  external int labelCount;
  external Pointer<TesseraHighlightPlacement> highlights;
  @Size()
  external int highlightCount;
  external Pointer<TesseraPointLightPlacement> pointLights;
  @Size()
  external int pointLightCount;
  external Pointer<TesseraWorldModelPlacement> worldModels;
  @Size()
  external int worldModelCount;
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
  @Bool()
  external bool hitDice;
  @Uint64()
  external int dice;
  @Float()
  external double diceDistance;
  @Bool()
  external bool hitCard;
  @Uint64()
  external int card;
  @Float()
  external double cardDistance;
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
//  Engine event stream (sound / haptics / FX sync)
// ======================================================================

/// enum TesseraEventType — the moments the engine surfaces as events.
abstract final class TesseraEventType {
  static const int none = 0;
  static const int diceContact = 1; // value = impact speed
  static const int diceSettled = 2; // value = face index
  static const int entityHopLanded = 3;
  static const int entityWaypointReached = 4; // value = step number
  static const int entitySpawned = 5;
  static const int entityRemoved = 6;
  static const int cardFlipped = 7; // value = 1 when now hidden
  static const int cardDealt = 8;
  static const int cameraArrived = 9;
  static const int opCompleted = 10; // subjectId = the operation id
}

/// enum TesseraEventSubject — what `subjectId` refers to.
abstract final class TesseraEventSubject {
  static const int none = 0;
  static const int entity = 1;
  static const int dice = 2;
  static const int card = 3;
  static const int draw = 4;
  static const int camera = 5;
  static const int operation = 6;
}

/// One typed engine event (mirrors the C `TesseraEvent`, 40 bytes).
final class TesseraEvent extends Struct {
  @Double()
  external double time; // engine tick time (s)
  @Uint64()
  external int subjectId; // object id, interpreted per `subject`
  @Uint32()
  external int type; // TesseraEventType
  @Uint32()
  external int subject; // TesseraEventSubject
  external TesseraCoord coord; // board coord where meaningful, else (0,0)
  @Float()
  external double value; // small payload (impact speed, face, step...)
  @Uint32()
  external int reserved; // always 0
}

/// A materialized (pure-Dart) copy of a [TesseraEvent], safe to hold after the
/// native ring slot has been reused. Emitted by [Tessera.events] /
/// [Tessera.drainEvents].
class TesseraEngineEvent {
  final int type; // TesseraEventType
  final int subject; // TesseraEventSubject
  final int subjectId;
  final double time; // engine tick time (s)
  final int coordX, coordY; // board coord where meaningful, else (0,0)
  final double value; // small payload (impact speed, face, step...)
  const TesseraEngineEvent({
    required this.type,
    required this.subject,
    required this.subjectId,
    required this.time,
    required this.coordX,
    required this.coordY,
    required this.value,
  });

  @override
  String toString() =>
      'TesseraEngineEvent(type: $type, subject: $subject/$subjectId, '
      't: $time, coord: ($coordX,$coordY), value: $value)';
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
typedef _RegFontC = Uint32 Function(Pointer<TesseraEngine>, Pointer<TesseraBytes>, Float);
typedef _RegFontD = int Function(Pointer<TesseraEngine>, Pointer<TesseraBytes>, double);
typedef _RegDiceC = Uint32 Function(Pointer<TesseraEngine>, Pointer<TesseraDiceDef>);
typedef _RegDiceD = int Function(Pointer<TesseraEngine>, Pointer<TesseraDiceDef>);
typedef _RegSoundC = Uint32 Function(Pointer<TesseraEngine>, Pointer<TesseraBytes>);
typedef _RegSoundD = int Function(Pointer<TesseraEngine>, Pointer<TesseraBytes>);
typedef _PlaySoundC = Bool Function(Pointer<TesseraEngine>, Uint32, Float);
typedef _PlaySoundD = bool Function(Pointer<TesseraEngine>, int, double);
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
typedef _StateSerializeC = Size Function(Pointer<TesseraState>, Pointer<Void>, Size);
typedef _StateSerializeD = int Function(Pointer<TesseraState>, Pointer<Void>, int);
typedef _StateDeserializeC = Pointer<TesseraState> Function(Pointer<Void>, Size);
typedef _StateDeserializeD = Pointer<TesseraState> Function(Pointer<Void>, int);
typedef _StateFreeC = Void Function(Pointer<TesseraState>);
typedef _StateFreeD = void Function(Pointer<TesseraState>);
typedef _ReplayCreateC = Pointer<TesseraReplay> Function();
typedef _ReplayCreateD = Pointer<TesseraReplay> Function();
typedef _ReplayOpenC = Pointer<TesseraReplay> Function(Pointer<Void>, Size);
typedef _ReplayOpenD = Pointer<TesseraReplay> Function(Pointer<Void>, int);
typedef _ReplayFreeC = Void Function(Pointer<TesseraReplay>);
typedef _ReplayFreeD = void Function(Pointer<TesseraReplay>);
typedef _ReplayAppendC = Bool Function(Pointer<TesseraReplay>, Uint64, Pointer<TesseraState>);
typedef _ReplayAppendD = bool Function(Pointer<TesseraReplay>, int, Pointer<TesseraState>);
typedef _ReplayCountC = Uint32 Function(Pointer<TesseraReplay>);
typedef _ReplayCountD = int Function(Pointer<TesseraReplay>);
typedef _ReplayGetC =
    Pointer<TesseraState> Function(Pointer<TesseraReplay>, Uint32, Pointer<Uint64>);
typedef _ReplayGetD =
    Pointer<TesseraState> Function(Pointer<TesseraReplay>, int, Pointer<Uint64>);
typedef _ReplaySerializeC = Size Function(Pointer<TesseraReplay>, Pointer<Void>, Size);
typedef _ReplaySerializeD = int Function(Pointer<TesseraReplay>, Pointer<Void>, int);
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
typedef _PollEventsC = Uint32 Function(
    Pointer<TesseraEngine>, Pointer<TesseraEvent>, Uint32);
typedef _PollEventsD = int Function(
    Pointer<TesseraEngine>, Pointer<TesseraEvent>, int);
typedef _EventsDroppedC = Uint32 Function(Pointer<TesseraEngine>);
typedef _EventsDroppedD = int Function(Pointer<TesseraEngine>);
/// Native signature of the engine-event callback: `void(const TesseraEvent*,
/// void* user)`. Fired on the tick thread; with `NativeCallable.listener` the
/// invocation is delivered asynchronously, so the pointer must be treated as a
/// wake-up signal only (drain via [Tessera.pollEvents], never dereference it).
typedef TesseraEventNative = Void Function(Pointer<TesseraEvent>, Pointer<Void>);
typedef _SetEventCallbackC = Void Function(
    Pointer<TesseraEngine>, Pointer<NativeFunction<TesseraEventNative>>, Pointer<Void>);
typedef _SetEventCallbackD = void Function(
    Pointer<TesseraEngine>, Pointer<NativeFunction<TesseraEventNative>>, Pointer<Void>);
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
  late final _RegFontD _registerFont =
      _lib.lookupFunction<_RegFontC, _RegFontD>('tessera_register_font');
  late final _RegDiceD _registerDiceDef =
      _lib.lookupFunction<_RegDiceC, _RegDiceD>('tessera_register_dice_def');
  late final _RegSoundD _registerSound =
      _lib.lookupFunction<_RegSoundC, _RegSoundD>('tessera_register_sound');
  late final _PlaySoundD _playSound =
      _lib.lookupFunction<_PlaySoundC, _PlaySoundD>('tessera_play_sound');
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
  late final _StateSerializeD _stateSerialize = _lib
      .lookupFunction<_StateSerializeC, _StateSerializeD>('tessera_state_serialize');
  late final _StateDeserializeD _stateDeserialize = _lib
      .lookupFunction<_StateDeserializeC, _StateDeserializeD>('tessera_state_deserialize');
  late final _StateFreeD _stateFree =
      _lib.lookupFunction<_StateFreeC, _StateFreeD>('tessera_state_free');
  late final _ReplayCreateD _replayCreate =
      _lib.lookupFunction<_ReplayCreateC, _ReplayCreateD>('tessera_replay_create');
  late final _ReplayOpenD _replayOpen =
      _lib.lookupFunction<_ReplayOpenC, _ReplayOpenD>('tessera_replay_open');
  late final _ReplayFreeD _replayFree =
      _lib.lookupFunction<_ReplayFreeC, _ReplayFreeD>('tessera_replay_free');
  late final _ReplayAppendD _replayAppend =
      _lib.lookupFunction<_ReplayAppendC, _ReplayAppendD>('tessera_replay_append');
  late final _ReplayCountD _replayCount =
      _lib.lookupFunction<_ReplayCountC, _ReplayCountD>('tessera_replay_count');
  late final _ReplayGetD _replayGet =
      _lib.lookupFunction<_ReplayGetC, _ReplayGetD>('tessera_replay_get');
  late final _ReplaySerializeD _replaySerialize = _lib
      .lookupFunction<_ReplaySerializeC, _ReplaySerializeD>('tessera_replay_serialize');
  late final _OpCompletedD _operationCompleted = _lib
      .lookupFunction<_OpCompletedC, _OpCompletedD>('tessera_operation_completed');
  late final _LastOpD _lastCompletedOperation = _lib
      .lookupFunction<_LastOpC, _LastOpD>('tessera_last_completed_operation');
  late final _SetOpCallbackD _setOperationCallback = _lib
      .lookupFunction<_SetOpCallbackC, _SetOpCallbackD>('tessera_set_operation_callback');
  late final _PollEventsD _pollEvents =
      _lib.lookupFunction<_PollEventsC, _PollEventsD>('tessera_poll_events');
  late final _EventsDroppedD _eventsDropped = _lib
      .lookupFunction<_EventsDroppedC, _EventsDroppedD>('tessera_events_dropped');
  late final _SetEventCallbackD _setEventCallback = _lib
      .lookupFunction<_SetEventCallbackC, _SetEventCallbackD>('tessera_set_event_callback');
  late final _PickD _pick = _lib.lookupFunction<_PickC, _PickD>('tessera_pick');
  late final _WorldToScreenD _worldToScreen =
      _lib.lookupFunction<_WorldToScreenC, _WorldToScreenD>('tessera_world_to_screen');
  late final _EntityScreenD _entityScreenPosition = _lib
      .lookupFunction<_EntityScreenC, _EntityScreenD>('tessera_entity_screen_position');
  late final _TileScreenD _tileScreenPosition =
      _lib.lookupFunction<_TileScreenC, _TileScreenD>('tessera_tile_screen_position');
  late final _EntityScreenD _diceScreenPosition = _lib
      .lookupFunction<_EntityScreenC, _EntityScreenD>('tessera_dice_screen_position');
  late final _EntityScreenD _cardScreenPosition = _lib
      .lookupFunction<_EntityScreenC, _EntityScreenD>('tessera_card_screen_position');
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

  // ---- fonts ----
  /// Register a TrueType/OpenType font (file bytes or a path, like an atlas);
  /// bakes ASCII + Latin-1 glyphs at [pixelHeight] texels into a GPU atlas.
  /// Returns a TesseraDefId (0 = fail). Labels reference it via
  /// [TesseraLabelPlacement.font] in [setState].
  int registerFont(Pointer<TesseraBytes> ttf, double pixelHeight) =>
      _registerFont(_engine, ttf, pixelHeight);

  // ---- sounds ----
  /// Register a sound effect from WAV file bytes or a path (like an atlas).
  /// Returns a TesseraSoundId (0 = fail). Play it with [playSound].
  int registerSound(Pointer<TesseraBytes> wav) => _registerSound(_engine, wav);

  /// (Re)start a registered sound at [gain] (1 = as authored). Any-thread.
  /// False when the id is unknown or no playback device is available.
  bool playSound(int id, double gain) => _playSound(_engine, id, gain);

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

  // ---- state serialization / save / undo / replay ----
  // Pure-data calls (no engine involved, any-thread); they live here because
  // the class already owns the loaded native library.

  /// Serialize a native state into a self-contained, versioned little-endian
  /// blob (`tessera_state_serialize`, two-call sizing) — the building block
  /// for saves, undo stacks and replays. Returns an empty list for nullptr.
  Uint8List serializeState(Pointer<TesseraState> s) {
    final n = _stateSerialize(s, nullptr, 0);
    if (n == 0) return Uint8List(0);
    final buf = calloc<Uint8>(n);
    try {
      _stateSerialize(s, buf.cast(), n);
      return Uint8List.fromList(buf.asTypedList(n));
    } finally {
      calloc.free(buf);
    }
  }

  /// Reconstruct a state from a [serializeState] blob. The state and all its
  /// arrays are ONE native allocation — free it with [freeState] (only).
  /// Returns nullptr on malformed input (bad magic/version/truncation).
  /// Push it straight through [setState].
  Pointer<TesseraState> deserializeState(Uint8List blob) {
    if (blob.isEmpty) return nullptr;
    final buf = calloc<Uint8>(blob.length);
    try {
      buf.asTypedList(blob.length).setAll(0, blob);
      return _stateDeserialize(buf.cast(), blob.length);
    } finally {
      calloc.free(buf);
    }
  }

  /// Free a state returned by [deserializeState] / [replayGet].
  void freeState(Pointer<TesseraState> s) => _stateFree(s);

  /// New empty replay recorder (`tessera_replay_create`); free with
  /// [freeReplay].
  Pointer<TesseraReplay> createReplay() => _replayCreate();

  /// Parse a serialized replay container ([serializeReplay] bytes). Returns
  /// nullptr on malformed input. Free with [freeReplay].
  Pointer<TesseraReplay> openReplay(Uint8List data) {
    if (data.isEmpty) return nullptr;
    final buf = calloc<Uint8>(data.length);
    try {
      buf.asTypedList(data.length).setAll(0, data);
      return _replayOpen(buf.cast(), data.length);
    } finally {
      calloc.free(buf);
    }
  }

  void freeReplay(Pointer<TesseraReplay> r) => _replayFree(r);

  /// Append one (timestampMs, state) record; the state is serialized
  /// immediately, so the caller keeps ownership of [s].
  bool replayAppend(Pointer<TesseraReplay> r, int timestampMs, Pointer<TesseraState> s) =>
      _replayAppend(r, timestampMs, s);

  /// Number of records in a replay.
  int replayCount(Pointer<TesseraReplay> r) => _replayCount(r);

  /// Reconstruct record [index] (0-based, append order); the record's
  /// timestamp is returned through [outTimestampMs] when non-null. Free the
  /// state with [freeState]; nullptr on a bad index / corrupt record.
  Pointer<TesseraState> replayGet(Pointer<TesseraReplay> r, int index,
          [Pointer<Uint64>? outTimestampMs]) =>
      _replayGet(r, index, outTimestampMs ?? nullptr);

  /// Flatten a replay container to bytes (write them to a file, ship them...).
  Uint8List serializeReplay(Pointer<TesseraReplay> r) {
    final n = _replaySerialize(r, nullptr, 0);
    if (n == 0) return Uint8List(0);
    final buf = calloc<Uint8>(n);
    try {
      _replaySerialize(r, buf.cast(), n);
      return Uint8List.fromList(buf.asTypedList(n));
    } finally {
      calloc.free(buf);
    }
  }

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

  // ---- typed engine event stream ----
  /// Drain up to [cap] pending engine events into [out] (a
  /// `calloc<TesseraEvent>(cap)` buffer), oldest first; returns how many were
  /// written. Consumes what it returns. Any-thread. Prefer [drainEvents] or
  /// the broadcast [events] stream unless you need zero-copy access.
  int pollEvents(Pointer<TesseraEvent> out, int cap) =>
      _pollEvents(_engine, out, cap);

  /// Total events dropped to ring overflow since engine creation (0 when the
  /// host keeps up — poll or listen at least once a frame-ish).
  int get eventsDropped => _eventsDropped(_engine);

  /// Register a raw native event callback fired once per event on the engine
  /// tick thread (see `tessera_set_event_callback`). Pass nullptr to clear.
  ///
  /// The engine has ONE callback slot, and the broadcast [events] stream is
  /// built on it too: while that stream has listeners the slot belongs to it,
  /// and this method throws [StateError] instead of silently clobbering it
  /// (and vice versa — see [events]). A non-null [fn] claims the slot for the
  /// external owner; passing nullptr releases it.
  void setEventCallback(
    Pointer<NativeFunction<TesseraEventNative>> fn,
    Pointer<Void> user,
  ) {
    if (_eventCallable != null) {
      throw StateError(
          'tessera: the events stream owns the engine event callback; cancel '
          'its listeners before installing an external callback');
    }
    _externalEventCallback = fn != nullptr;
    _setEventCallback(_engine, fn, user);
  }

  bool _externalEventCallback = false;
  NativeCallable<TesseraEventNative>? _eventCallable;
  StreamController<TesseraEngineEvent>? _eventController;
  Pointer<TesseraEvent> _eventBuf = nullptr;
  static const int _eventBufCap = 64;

  /// Drain every pending engine event into materialized Dart objects (oldest
  /// first). Complements the [events] stream for poll-style hosts; do not mix
  /// the two (both consume the same ring).
  List<TesseraEngineEvent> drainEvents() {
    if (_eventBuf == nullptr) _eventBuf = calloc<TesseraEvent>(_eventBufCap);
    final out = <TesseraEngineEvent>[];
    for (;;) {
      final n = _pollEvents(_engine, _eventBuf, _eventBufCap);
      for (var i = 0; i < n; i++) {
        final ev = (_eventBuf + i).ref;
        out.add(TesseraEngineEvent(
          type: ev.type,
          subject: ev.subject,
          subjectId: ev.subjectId,
          time: ev.time,
          coordX: ev.coord.x,
          coordY: ev.coord.y,
          value: ev.value,
        ));
      }
      if (n < _eventBufCap) break;
    }
    return out;
  }

  /// Broadcast stream of engine events (dice contacts, hop landings, card
  /// flips, camera arrival, op completion, ...) for sound/haptics/FX sync.
  ///
  /// Built on `NativeCallable.listener`: the engine's tick-thread callback is
  /// used purely as a wake-up marshalled onto this isolate, and the ring is
  /// then drained with `tessera_poll_events` — so no native pointer outlives
  /// its validity and nothing is lost between wake-ups. The native callback is
  /// installed on first listen and removed when the last listener cancels.
  /// Don't combine with manual [pollEvents]/[drainEvents] while listening
  /// (both consume the same ring).
  ///
  /// The engine has ONE callback slot: while an external callback installed
  /// via [setEventCallback] is registered (e.g. a higher-level host such as
  /// flutter_tessera's TesseraController — use its own events stream instead),
  /// listening here throws [StateError] rather than silently stealing the
  /// slot and the ring from that owner.
  Stream<TesseraEngineEvent> get events {
    final ctl = _eventController ??= StreamController<TesseraEngineEvent>.broadcast(
      onListen: _startEventStream,
      onCancel: _stopEventStream,
    );
    return ctl.stream;
  }

  void _startEventStream() {
    if (_eventCallable != null) return;
    if (_externalEventCallback) {
      throw StateError(
          'tessera: an external event callback owns the engine callback slot '
          '(setEventCallback); use that owner\'s event surface instead of '
          'Tessera.events, or clear it first');
    }
    final cb = NativeCallable<TesseraEventNative>.listener(_onEventWakeup);
    _eventCallable = cb;
    _setEventCallback(_engine, cb.nativeFunction, nullptr);
    _pumpEventStream(); // catch up on anything already buffered
  }

  void _onEventWakeup(Pointer<TesseraEvent> ev, Pointer<Void> user) {
    // Delivered asynchronously: `ev` may already point at a recycled slot.
    // Never dereference it — drain the ring instead.
    _pumpEventStream();
  }

  void _pumpEventStream() {
    final ctl = _eventController;
    if (ctl == null || ctl.isClosed) return;
    for (final ev in drainEvents()) {
      ctl.add(ev);
    }
  }

  void _stopEventStream() {
    final cb = _eventCallable;
    _eventCallable = null;
    if (cb != null) {
      _setEventCallback(_engine, nullptr, nullptr);
      cb.close();
    }
  }

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

  /// Screen position of a live die by its placement id. False if not present.
  bool diceScreenPosition(int id, Pointer<TesseraScreenPos> out) =>
      _diceScreenPosition(_engine, id, out);

  /// Screen position of a live (single) card by its placement id. False if not
  /// present or the id names a pile/draw.
  bool cardScreenPosition(int id, Pointer<TesseraScreenPos> out) =>
      _cardScreenPosition(_engine, id, out);

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
  /// only). For [Tessera.fromHandle] the host owns the engine lifecycle, but
  /// this still tears down the Dart-side event stream plumbing.
  void dispose() {
    _stopEventStream();
    _eventController?.close();
    _eventController = null;
    if (_eventBuf != nullptr) {
      calloc.free(_eventBuf);
      _eventBuf = nullptr;
    }
    if (_ownsEngine) _destroy(_engine);
  }
}
