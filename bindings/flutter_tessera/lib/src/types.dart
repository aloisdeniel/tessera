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
class TesseraEntity {
  const TesseraEntity({
    required this.id,
    required this.def,
    required this.x,
    required this.y,
    this.facing = 0,
    this.anim = 0,
  });

  final int id;
  final int def;
  final int x;
  final int y;
  final int facing;
  final int anim;
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
    required this.tiles,
    required this.entities,
    required this.camera,
    this.epoch = 0,
  });

  final List<TesseraTile> tiles;
  final List<TesseraEntity> entities;
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
