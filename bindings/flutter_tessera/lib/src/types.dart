// types.dart — high-level, Flutter-idiomatic value types.
//
// These mirror the fields of the C structs in `include/tessera.h` but as plain
// Dart classes, so app code (and the chess example) never touches `dart:ffi`.
// The controller marshals them into native structs before calling the engine.

import 'dart:typed_data';

/// enum TesseraEmitMode { burst=0, continuous=1 }
enum TesseraEmitMode { burst, continuous }

/// enum TesseraBlendMode { alpha=0, add=1 }
enum TesseraBlendMode { alpha, add }

/// enum TesseraShadowMode { none=0, blob=1, map=2 }
enum TesseraShadowMode { none, blob, map }

/// enum TesseraProjection { perspective=0, isometric=1 }
enum TesseraProjection { perspective, isometric }

/// A tile definition: a flat prism tinted [tint] (RGBA, 0..1), [thickness]
/// relative to the tile (default 0.25).
class TesseraTileType {
  const TesseraTileType({
    this.thickness = 0.25,
    this.tint = const [1, 1, 1, 1],
  });

  final double thickness;
  final List<double> tint;
}

/// A particle spec for an effect (subset of the fields the engine accepts).
class TesseraParticle {
  const TesseraParticle({
    this.mode = TesseraEmitMode.burst,
    this.count = 32,
    this.lifetime = 0.7,
    this.lifetimeVar = 0.2,
    this.speed = 1.5,
    this.speedVar = 0.5,
    this.spreadDeg = 45,
    this.gravity = 0.0,
    this.sizeStart = 0.1,
    this.sizeEnd = 0.4,
    this.colorStart = const [1, 1, 1, 1],
    this.colorEnd = const [1, 1, 1, 0],
    this.blend = TesseraBlendMode.alpha,
    this.durationS = 0.0,
  });

  final TesseraEmitMode mode;
  final int count;
  final double lifetime;
  final double lifetimeVar;
  final double speed;
  final double speedVar;
  final double spreadDeg;
  final double gravity;
  final double sizeStart;
  final double sizeEnd;
  final List<double> colorStart;
  final List<double> colorEnd;
  final TesseraBlendMode blend;
  final double durationS;
}

/// An effect definition — particle bursts played when an entity is added and/or
/// removed. Either may be null (no particles for that transition).
class TesseraEffectType {
  const TesseraEffectType({this.onAdd, this.onRemove});
  final TesseraParticle? onAdd;
  final TesseraParticle? onRemove;
}

/// An entity definition backed by a glTF/GLB model ([glb] bytes), scaled by
/// [scale], with optional linked effects played on spawn / despawn (def ids).
class TesseraEntityType {
  const TesseraEntityType({
    required this.glb,
    this.scale = 1.0,
    this.onSpawnEffect = 0,
    this.onDespawnEffect = 0,
  });

  final Uint8List glb;
  final double scale;
  final int onSpawnEffect;
  final int onDespawnEffect;
}

/// A card definition: three textures referenced by *atlas def id* (register the
/// images with [TesseraController.registerAtlas] first). [hiddenAtlas] is the
/// concealing front; [backAtlas] the reverse. Dimensions are world units; <= 0
/// selects a default (a slab a little smaller than 2x3 tiles).
class TesseraCardType {
  const TesseraCardType({
    required this.visibleAtlas,
    required this.hiddenAtlas,
    required this.backAtlas,
    this.width = 0,
    this.height = 0,
    this.thickness = 0,
    this.cornerRadius = 0,
    this.tint = const [1, 1, 1, 1],
  });

  final int visibleAtlas;
  final int hiddenAtlas;
  final int backAtlas;
  final double width;
  final double height;
  final double thickness;
  final double cornerRadius;
  final List<double> tint;
}

/// A dice definition built from per-face sprite images ([faces], length >= 2 —
/// 2=coin, 6=cube, N=barrel). [size] is the bounding diameter.
class TesseraDiceType {
  const TesseraDiceType({
    required this.faces,
    this.size = 1.0,
    this.tint = const [1, 1, 1, 1],
  });

  final List<Uint8List> faces;
  final double size;
  final List<double> tint;
}

/// A tile placed on the grid at ([x], [y]) using tile def [def]. [id] is an
/// optional stable instance id (0 = unqueryable).
class TesseraTile {
  const TesseraTile({
    required this.x,
    required this.y,
    required this.def,
    this.id = 0,
    this.variant = 0,
  });

  final int x;
  final int y;
  final int def;
  final int id;
  final int variant;
}

/// An entity placed on the grid. [id] is stable across states (the diff key), so
/// a change in [x]/[y] for the same id animates as a move.
///
/// [path] is an optional multi-step move: when it holds more than one coord the
/// entity walks *through* them (each `(x, y)`, in order) — hopping from tile to
/// tile — on its way to [x]/[y] rather than gliding straight there. The last
/// entry must equal ([x], [y]). The whole walk takes `2 × moveS` (twice a single
/// move, so the hops stay legible), split across the steps. Empty or
/// single-element = a plain single move.
class TesseraEntity {
  const TesseraEntity({
    required this.id,
    required this.def,
    required this.x,
    required this.y,
    this.facing = 0,
    this.anim = 0,
    this.path = const [],
  });

  final int id;
  final int def;
  final int x;
  final int y;
  final int facing;
  final int anim;
  final List<(int x, int y)> path;
}

/// A card placed in the world. [orientation] is a quaternion (xyzw; all-zero =>
/// identity, which lays the card flat, front up). [hidden] shows the concealing
/// front (crossfades when toggled). If [hand] is non-zero the card is arranged
/// by that hand's fan ([position]/[orientation] ignored); [handSlot] orders it.
///
/// [sourceDraw] names a [TesseraCardDraw.id] this card is dealt from: when the
/// card first appears, if that pile is present it spawns resting on top of the
/// pile and slides/flips to its target instead of fading in from nowhere.
/// Ignored after the first frame and when the pile is absent.
class TesseraCard {
  const TesseraCard({
    required this.id,
    required this.def,
    this.position = const [0, 0, 0],
    this.orientation = const [0, 0, 0, 0],
    this.hidden = false,
    this.hand = 0,
    this.handSlot = 0,
    this.sourceDraw = 0,
    this.path = const [],
  });

  final int id;
  final int def;
  final List<double> position;
  final List<double> orientation;
  final bool hidden;
  final int hand;
  final int handSlot;
  final int sourceDraw;

  /// Optional multi-step move for a *free* card (ignored while [hand] != 0):
  /// when it holds more than one position (each `[x, y, z]`) the card tweens
  /// *through* them in order rather than sliding straight to [position]. The
  /// last entry must equal [position]. The whole move takes `2 × moveS` (twice a
  /// single move), split across the steps. Empty or single-element = a plain
  /// single move.
  final List<List<double>> path;
}

/// A pile of cards drawn as one slab (thickness tracks [count]). The top face
/// shows the def's visible (or hidden when [topHidden]) texture; the bottom the
/// def's hidden texture.
class TesseraCardDraw {
  const TesseraCardDraw({
    required this.id,
    required this.def,
    required this.count,
    this.position = const [0, 0, 0],
    this.orientation = const [0, 0, 0, 0],
    this.topHidden = false,
  });

  final int id;
  final int def;
  final int count;
  final List<double> position;
  final List<double> orientation;
  final bool topHidden;
}

/// A hand anchor that fans out the cards whose [TesseraCard.hand] equals its id.
/// [orientation] is a quaternion; identity faces the fronts toward +Z. All of
/// [spreadDeg]/[radius]/[cardSpacing] default when <= 0.
class TesseraHand {
  const TesseraHand({
    required this.id,
    this.position = const [0, 0, 0],
    this.orientation = const [0, 0, 0, 0],
    this.spreadDeg = 0,
    this.radius = 0,
    this.cardSpacing = 0,
  });

  final int id;
  final List<double> position;
  final List<double> orientation;
  final double spreadDeg;
  final double radius;
  final double cardSpacing;
}

/// A die placed in the world. A die that newly appears (by [id]) is thrown and
/// settles with [face] up at [position]; one that vanishes fades out. Changing
/// def/face/seed/position re-throws it.
class TesseraDie {
  const TesseraDie({
    required this.id,
    required this.def,
    this.face = 0,
    this.position = const [0, 0, 0],
    this.seed = 0,
    this.throwS = 0,
  });

  final int id;
  final int def;
  final int face;
  final List<double> position;
  final int seed;
  final double throwS;
}

/// The orbit camera. [focusX]/[focusY] are continuous grid coordinates (may sit
/// between tiles); [yaw]/[pitch]/[fov] are radians.
class TesseraCameraPose {
  const TesseraCameraPose({
    this.focusX = 0,
    this.focusY = 0,
    this.distance = 12,
    this.yaw = 3.14159,
    this.pitch = 0.8,
    this.fov = 0.7,
  });

  final double focusX;
  final double focusY;
  final double distance;
  final double yaw;
  final double pitch;
  final double fov;
}

/// A full immutable scene snapshot: the board, the entities on it, and the
/// camera. Pushed via [TesseraController.setScene]; the engine diffs successive
/// scenes and animates the transitions.
class TesseraScene {
  const TesseraScene({
    this.tiles = const [],
    this.entities = const [],
    this.cards = const [],
    this.cardDraws = const [],
    this.hands = const [],
    this.dice = const [],
    required this.camera,
    this.epoch = 0,
  });

  final List<TesseraTile> tiles;
  final List<TesseraEntity> entities;
  final List<TesseraCard> cards;
  final List<TesseraCardDraw> cardDraws;
  final List<TesseraHand> hands;
  final List<TesseraDie> dice;
  final TesseraCameraPose camera;
  final int epoch;
}

/// A single directional light plus ambient (all RGB 0..1; [dir] points *from*
/// the light).
class TesseraLightData {
  const TesseraLightData({
    this.dir = const [-0.4, -1.0, -0.3],
    this.color = const [1, 0.98, 0.92],
    this.intensity = 1.0,
    this.ambient = const [0.28, 0.30, 0.36],
  });

  final List<double> dir;
  final List<double> color;
  final double intensity;
  final List<double> ambient;
}

/// Render quality knobs.
class TesseraQualityData {
  const TesseraQualityData({
    this.shadows = TesseraShadowMode.blob,
    this.msaa = 1,
    this.renderScale = 1.0,
  });

  final TesseraShadowMode shadows;
  final int msaa;
  final double renderScale;
}

/// Transition durations (seconds) and a global [speedMultiplier].
class TesseraTimingData {
  const TesseraTimingData({
    this.moveS = 0.35,
    this.addS = 0.3,
    this.removeS = 0.25,
    this.tileS = 0.3,
    this.reflowS = 0.3,
    this.cameraS = 0.5,
    this.speedMultiplier = 1.0,
  });

  final double moveS;
  final double addS;
  final double removeS;
  final double tileS;
  final double reflowS;
  final double cameraS;
  final double speedMultiplier;
}

/// The result of a screen-space [TesseraController.pick].
class TesseraPickResult {
  const TesseraPickResult({
    required this.hitTile,
    required this.tileX,
    required this.tileY,
    required this.tileDistance,
    required this.hitEntity,
    required this.entity,
    required this.entityDistance,
  });

  final bool hitTile;
  final int tileX;
  final int tileY;
  final double tileDistance;
  final bool hitEntity;
  final int entity;
  final double entityDistance;

  factory TesseraPickResult.fromMap(Map<Object?, Object?> m) => TesseraPickResult(
        hitTile: (m['hitTile'] as bool?) ?? false,
        tileX: (m['tileX'] as num?)?.toInt() ?? 0,
        tileY: (m['tileY'] as num?)?.toInt() ?? 0,
        tileDistance: (m['tileDistance'] as num?)?.toDouble() ?? 0,
        hitEntity: (m['hitEntity'] as bool?) ?? false,
        entity: (m['entity'] as num?)?.toInt() ?? 0,
        entityDistance: (m['entityDistance'] as num?)?.toDouble() ?? 0,
      );
}
