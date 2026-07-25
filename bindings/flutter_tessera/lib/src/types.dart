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

/// enum TesseraOverlayShape { sprite=0, disc=1, ring=2 }
enum TesseraOverlayShape { sprite, disc, ring }

/// enum TesseraLabelAnchor { world=0, entity=1, tile=2, dice=3, card=4, draw=5 }
enum TesseraLabelAnchor { world, entity, tile, dice, card, draw }

/// enum TesseraHighlightKind { entity=0, tile=1, dice=2, card=3 }
enum TesseraHighlightKind { entity, tile, dice, card }

/// enum TesseraHighlightStyle { outline=0, glow=1 }
enum TesseraHighlightStyle { outline, glow }

/// enum TesseraEventType — the moments the engine surfaces on
/// [TesseraController.events] (see [TesseraEvent.value] for each payload).
enum TesseraEventType {
  none,
  diceContact, // die touched the ground; value = impact speed
  diceSettled, // tumble/slide finished; value = face index
  entityHopLanded, // hop-arc touchdown (each hop of a move)
  entityWaypointReached, // multi-step segment handoff; value = step number
  entitySpawned, // spawn transition completed
  entityRemoved, // removal transition completed
  cardFlipped, // hidden<->visible flip began; value = 1 when now hidden
  cardDealt, // card spawned off a source pile (deal began)
  cameraArrived, // camera tween settled at its goal pose
  opCompleted, // set_state operation settled; subjectId = op id
}

/// enum TesseraEventSubject — what [TesseraEvent.subjectId] refers to.
enum TesseraEventSubject { none, entity, dice, card, draw, camera, operation }

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

/// A flat decal rendered on top of the tile at ([x], [y]) — move-range fills,
/// threat rings, drop-target highlights. Keyed by coord when diffing: a coord
/// that newly appears fades in, one that vanishes fades out, and a tint change
/// crossfades from the currently displayed tint. At most one overlay per coord.
///
/// [shape] picks a textured sprite ([atlas]/[uv], atlas 0 = solid tint) or a
/// procedural filled disc / ring. When [pulseS] > 0 the overlay breathes:
/// alpha between [pulseAlphaMin]..[pulseAlphaMax] (when max > 0) and/or
/// footprint scale between [pulseScaleMin]..[pulseScaleMax] (when max > 0).
class TesseraOverlay {
  const TesseraOverlay({
    required this.x,
    required this.y,
    this.shape = TesseraOverlayShape.disc,
    this.atlas = 0,
    this.uv = const [0, 0, 0, 0],
    this.tint = const [1, 1, 1, 1],
    this.pulseS = 0,
    this.pulseAlphaMin = 0,
    this.pulseAlphaMax = 0,
    this.pulseScaleMin = 0,
    this.pulseScaleMax = 0,
  });

  final int x;
  final int y;
  final TesseraOverlayShape shape;
  final int atlas;
  final List<double> uv; // u0,v0,u1,v1 (all-zero => the whole atlas)
  final List<double> tint;
  final double pulseS;
  final double pulseAlphaMin;
  final double pulseAlphaMax;
  final double pulseScaleMin;
  final double pulseScaleMax;
}

/// A die placed in the world. A die that newly appears (by [id]) is thrown and
/// settles with [face] up at [position]; one that vanishes fades out. Changing
/// its def/face/seed/throwS re-throws it. Changing *only* the [position] (same
/// id/def/face/seed/throwS) instead slides the die from where it is to the new
/// spot with an easeInOut curve — no re-throw — so a settled die can be moved
/// without tumbling it again.
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

/// A world-anchored 3D text label (scores, HP, dice totals, coordinates),
/// rendered from a font registered with `TesseraController.registerFont`.
/// Keyed by [id] when diffing: a new id fades in, a vanished one fades out,
/// and a [text] or [color] change crossfades from what is currently shown.
///
/// [anchor] selects what the label is glued to: `world` places it at
/// [position] directly; the other modes anchor to the live object [anchorId]
/// and treat [position] as an offset from its LIVE animating transform (so
/// the label rides along with moves/hops/throws). [size] is the line height
/// in world units. With [billboard] the label always faces the camera;
/// otherwise it lies flat on the ground plane. Text longer than 63 UTF-8
/// bytes is truncated by the engine's bounded copy.
class TesseraLabel {
  const TesseraLabel({
    required this.id,
    required this.font,
    required this.text,
    this.anchor = TesseraLabelAnchor.world,
    this.anchorId = 0,
    this.position = const [0, 0, 0],
    this.size = 0.5,
    this.color = const [1, 1, 1, 1],
    this.billboard = true,
  });

  final int id;
  final int font;
  final String text;
  final TesseraLabelAnchor anchor;
  final int anchorId;
  final List<double> position; // world point (world) or anchor offset
  final double size;
  final List<double> color;
  final bool billboard;
}

/// A screen-space selection highlight on one live object ([kind] +
/// [targetId], mirroring the camera focus conventions). The object is
/// re-rendered into a silhouette mask at its live animating transform and
/// composited over the lit scene — after depth-of-field, so the selection
/// stays crisp — as a dilated colored outline or a blurred additive glow.
/// Keyed by (kind, targetId) when diffing: a new highlight fades in, a
/// vanished one fades out, and a color change crossfades. [thickness] is the
/// outline width / glow radius in pixels (<=0 => default). When [pulseS] > 0
/// the intensity breathes between [pulseMin]..[pulseMax] (used when max > 0);
/// the pulse animates in the engine and never blocks scene completion.
class TesseraHighlight {
  const TesseraHighlight({
    required this.targetId,
    this.kind = TesseraHighlightKind.entity,
    this.style = TesseraHighlightStyle.outline,
    this.color = const [1, 1, 1, 1],
    this.thickness = 0,
    this.pulseS = 0,
    this.pulseMin = 0,
    this.pulseMax = 0,
  });

  final int targetId;
  final TesseraHighlightKind kind;
  final TesseraHighlightStyle style;
  final List<double> color; // RGBA (all-zero => white)
  final double thickness; // px (<=0 => default)
  final double pulseS;
  final double pulseMin;
  final double pulseMax;
}

/// One typed engine event, delivered on [TesseraController.events] as
/// transitions animate — a die striking the felt ([TesseraEventType
/// .diceContact]), a piece landing from a hop, a card flipping over — for
/// sound / haptics / FX sync. [subjectId] names the object it happened to,
/// interpreted per [subject]; ([x], [y]) is the board tile nearest the subject
/// where that is meaningful, else (0, 0). [value] is a small per-type payload
/// (impact speed, face index, step number; see [TesseraEventType]). [time] is
/// the engine clock (accumulated tick seconds) at emission.
class TesseraEvent {
  const TesseraEvent({
    required this.type,
    required this.subject,
    required this.subjectId,
    required this.time,
    required this.x,
    required this.y,
    required this.value,
  });

  final TesseraEventType type;
  final TesseraEventSubject subject;
  final int subjectId;
  final double time;
  final int x;
  final int y;
  final double value;

  @override
  String toString() => 'TesseraEvent(${type.name}, ${subject.name}/$subjectId, '
      't: $time, coord: ($x,$y), value: $value)';
}

/// How the scene camera is positioned. Under the hood the engine tweens the
/// camera pose from its current pose to the resolved goal as state evolves, so
/// switching cases (or moving a followed object) glides rather than jumps.
sealed class TesseraCamera {
  const TesseraCamera();
}

/// The orbit camera (the classic board camera; the default). [focusX]/[focusY]
/// are continuous grid coordinates (may sit between tiles); [yaw]/[pitch]/[fov]
/// are radians.
class TesseraCameraPose extends TesseraCamera {
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

/// Fully manual eye pose. [orientation] is a quaternion xyzw (fwd = q·-Z,
/// up = q·+Y; all-zero => identity); [position] is the eye in world units.
class TesseraCameraManual extends TesseraCamera {
  const TesseraCameraManual({
    required this.position,
    required this.orientation,
    this.fov = 0.7,
  });

  final List<double> position; // len 3
  final List<double> orientation; // len 4 xyzw
  final double fov;
}

/// Eye at [source] looking at [target] (up = +Y). Both are world units.
class TesseraCameraTarget extends TesseraCamera {
  const TesseraCameraTarget({
    required this.source,
    required this.target,
    this.fov = 0.7,
  });

  final List<double> source; // len 3
  final List<double> target; // len 3
  final double fov;
}

/// Frame/follow a live tile from [distance] world units at [yaw]/[pitch].
class TesseraCameraFocusTile extends TesseraCamera {
  const TesseraCameraFocusTile(
    this.tileId, {
    this.distance = 12,
    this.yaw = 0,
    this.pitch = 0.9,
    this.fov = 0.7,
  });

  final int tileId;
  final double distance;
  final double yaw;
  final double pitch;
  final double fov;
}

/// Frame/follow a live entity from [distance] world units at [yaw]/[pitch].
class TesseraCameraFocusEntity extends TesseraCamera {
  const TesseraCameraFocusEntity(
    this.entityId, {
    this.distance = 12,
    this.yaw = 0,
    this.pitch = 0.9,
    this.fov = 0.7,
  });

  final int entityId;
  final double distance;
  final double yaw;
  final double pitch;
  final double fov;
}

/// Frame/follow a live die from [distance] world units at [yaw]/[pitch].
class TesseraCameraFocusDice extends TesseraCamera {
  const TesseraCameraFocusDice(
    this.diceId, {
    this.distance = 12,
    this.yaw = 0,
    this.pitch = 0.9,
    this.fov = 0.7,
  });

  final int diceId;
  final double distance;
  final double yaw;
  final double pitch;
  final double fov;
}

/// Frame/follow a live card pile from [distance] world units at [yaw]/[pitch].
class TesseraCameraFocusDraw extends TesseraCamera {
  const TesseraCameraFocusDraw(
    this.drawId, {
    this.distance = 12,
    this.yaw = 0,
    this.pitch = 0.9,
    this.fov = 0.7,
  });

  final int drawId;
  final double distance;
  final double yaw;
  final double pitch;
  final double fov;
}

/// Align in front of a live card (along its front normal) so it fills the frame
/// with a [padding] margin (fraction of the frame kept clear).
class TesseraCameraFocusCard extends TesseraCamera {
  const TesseraCameraFocusCard(
    this.cardId, {
    this.padding = 0.08,
    this.fov = 0.7,
  });

  final int cardId;
  final double padding;
  final double fov;
}

/// Frame a hand's cards so they all fit with a [padding] margin. When [cardId]
/// is set (and lives in this hand), that card comes fullscreen-centre and its
/// fan neighbours naturally fall to the screen edges (camera-only; cards are not
/// re-laid-out).
class TesseraCameraFocusHand extends TesseraCamera {
  const TesseraCameraFocusHand(
    this.handId, {
    this.cardId,
    this.padding = 0.08,
    this.fov = 0.7,
  });

  final int handId;
  final int? cardId;
  final double padding;
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
    this.overlays = const [],
    this.labels = const [],
    this.highlights = const [],
    required this.camera,
    this.epoch = 0,
  });

  final List<TesseraTile> tiles;
  final List<TesseraEntity> entities;
  final List<TesseraCard> cards;
  final List<TesseraCardDraw> cardDraws;
  final List<TesseraHand> hands;
  final List<TesseraDie> dice;
  final List<TesseraOverlay> overlays;
  final List<TesseraLabel> labels;
  final List<TesseraHighlight> highlights;
  final TesseraCamera camera;
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

/// The result of a screen-space [TesseraController.pick]. A ray is cast from the
/// camera through the tapped pixel and tested against the live scene; the nearest
/// tile, entity, die, and card are reported independently, so a caller can prefer
/// whichever is closest or use each kind for a different gesture.
class TesseraPickResult {
  const TesseraPickResult({
    required this.hitTile,
    required this.tileX,
    required this.tileY,
    required this.tileDistance,
    required this.hitEntity,
    required this.entity,
    required this.entityDistance,
    required this.hitDice,
    required this.dice,
    required this.diceDistance,
    required this.hitCard,
    required this.card,
    required this.cardDistance,
  });

  final bool hitTile;
  final int tileX;
  final int tileY;
  final double tileDistance;
  final bool hitEntity;
  final int entity;
  final double entityDistance;

  /// The nearest live die under the ray (see [dice] for its id).
  final bool hitDice;

  /// Id of the nearest hit die (0 when [hitDice] is false).
  final int dice;
  final double diceDistance;

  /// The nearest live single card under the ray (piles/draws are not picked).
  final bool hitCard;

  /// Id of the nearest hit card (0 when [hitCard] is false).
  final int card;
  final double cardDistance;

  factory TesseraPickResult.fromMap(Map<Object?, Object?> m) => TesseraPickResult(
        hitTile: (m['hitTile'] as bool?) ?? false,
        tileX: (m['tileX'] as num?)?.toInt() ?? 0,
        tileY: (m['tileY'] as num?)?.toInt() ?? 0,
        tileDistance: (m['tileDistance'] as num?)?.toDouble() ?? 0,
        hitEntity: (m['hitEntity'] as bool?) ?? false,
        entity: (m['entity'] as num?)?.toInt() ?? 0,
        entityDistance: (m['entityDistance'] as num?)?.toDouble() ?? 0,
        hitDice: (m['hitDice'] as bool?) ?? false,
        dice: (m['dice'] as num?)?.toInt() ?? 0,
        diceDistance: (m['diceDistance'] as num?)?.toDouble() ?? 0,
        hitCard: (m['hitCard'] as bool?) ?? false,
        card: (m['card'] as num?)?.toInt() ?? 0,
        cardDistance: (m['cardDistance'] as num?)?.toDouble() ?? 0,
      );
}
