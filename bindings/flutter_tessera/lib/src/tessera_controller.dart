// tessera_controller.dart — drives one embedded Tessera engine.
//
// Threading model (see tessera/docs/platforms.md): the native platform view
// owns the engine and its render loop on the platform main thread. Dart reaches
// the same engine two ways:
//
//   • Method channel  → native, main thread — for GPU/render-thread work that
//     runs concurrently with the loop: start(), pick(), resize, dispose.
//   • FFI (Tessera.fromHandle) → the any-thread `set_state` hot path, plus the
//     one-time setup calls (register*, light/quality/timing) which run BEFORE
//     the native loop starts, so nothing races the tick.
//
// The lifecycle is therefore: attach → (FFI setup: register defs, light,
// quality, timing, first scene) → start() → (runtime: setScene via FFI, pick
// via channel) → dispose.

import 'dart:ffi';

import 'package:ffi/ffi.dart';
import 'package:flutter/services.dart';
import 'package:tessera/tessera.dart' as t;

import 'types.dart';

/// Controls a single [TesseraView]. Obtain one from `TesseraView`'s
/// `onCreated` callback; do the def/light/quality/timing setup, push an initial
/// scene, then call [start].
class TesseraController {
  TesseraController._(this._channel, this._engine);

  final MethodChannel _channel;
  final t.Tessera _engine;
  bool _started = false;
  bool _disposed = false;

  /// Whether the native render loop has been started.
  bool get started => _started;

  /// The low-level FFI wrapper around the native-owned engine, for calls not
  /// surfaced here. Only the any-thread calls are safe once [start] has run.
  t.Tessera get engine => _engine;

  /// Attach to the native platform view [viewId]. Retrieves the engine handle
  /// the native side created and wraps it for FFI. [library] overrides how the
  /// native library symbols are resolved (defaults to the current process, where
  /// the plugin has already loaded them).
  static Future<TesseraController> attach(
    int viewId, {
    DynamicLibrary? library,
  }) async {
    final channel = MethodChannel('flutter_tessera/view_$viewId');
    final handle = await channel.invokeMethod<int>('handle');
    if (handle == null || handle == 0) {
      throw StateError('flutter_tessera: native engine handle unavailable');
    }
    final engine = t.Tessera.fromHandle(
      handle,
      library: library ?? DynamicLibrary.process(),
    );
    return TesseraController._(channel, engine);
  }

  // ---- setup (call before start(); run on the UI thread while the native
  //      render loop is paused, so these GPU calls do not race the tick) ----

  /// Register a tile definition. Returns its def id (0 = failure).
  int registerTileType(TesseraTileType type) {
    final def = calloc<t.TesseraTileDef>();
    def.ref.thickness = type.thickness;
    for (var i = 0; i < 4; ++i) {
      def.ref.tint[i] = type.tint[i];
    }
    final id = _engine.registerTileDef(def);
    calloc.free(def);
    return id;
  }

  /// Register an effect definition (particle bursts on add/remove). Returns its
  /// def id (0 = failure).
  int registerEffectType(TesseraEffectType type) {
    final def = calloc<t.TesseraEffectDef>();
    if (type.onAdd != null) _fillParticle(def.ref.onAdd, type.onAdd!);
    if (type.onRemove != null) _fillParticle(def.ref.onRemove, type.onRemove!);
    final id = _engine.registerEffectDef(def);
    calloc.free(def);
    return id;
  }

  /// Register an entity definition from GLB [type.glb] bytes. Returns its def id
  /// (0 = failure).
  int registerEntityType(TesseraEntityType type) {
    final bytes = calloc<Uint8>(type.glb.length);
    bytes.asTypedList(type.glb.length).setAll(0, type.glb);
    final name = 'entity'.toNativeUtf8();
    final def = calloc<t.TesseraEntityDef>();
    def.ref.gltf
      ..data = bytes.cast<Void>()
      ..size = type.glb.length
      ..debugName = name;
    def.ref
      ..scale = type.scale
      ..defaultAnim = -1
      ..moveAnim = -1
      ..spawnAnim = -1
      ..despawnAnim = -1
      ..onSpawnEffect = type.onSpawnEffect
      ..onDespawnEffect = type.onDespawnEffect;
    final id = _engine.registerEntityDef(def); // copies the bytes it needs
    calloc.free(def);
    calloc.free(bytes);
    calloc.free(name);
    return id;
  }

  /// Set the directional light + ambient.
  void setLight(TesseraLightData light) {
    final l = calloc<t.TesseraLight>();
    for (var i = 0; i < 3; ++i) {
      l.ref.dir[i] = light.dir[i];
      l.ref.color[i] = light.color[i];
      l.ref.ambient[i] = light.ambient[i];
    }
    l.ref.intensity = light.intensity;
    _engine.setLight(l);
    calloc.free(l);
  }

  /// Set render quality (shadows, MSAA, resolution scale).
  void setQuality(TesseraQualityData quality) {
    final q = calloc<t.TesseraQuality>();
    q.ref
      ..shadows = quality.shadows.index
      ..msaa = quality.msaa
      ..renderScale = quality.renderScale;
    _engine.setQuality(q);
    calloc.free(q);
  }

  /// Set the transition timings.
  void setTiming(TesseraTimingData timing) {
    final tm = calloc<t.TesseraTiming>();
    tm.ref
      ..moveS = timing.moveS
      ..addS = timing.addS
      ..removeS = timing.removeS
      ..tileS = timing.tileS
      ..reflowS = timing.reflowS
      ..cameraS = timing.cameraS
      ..speedMultiplier = timing.speedMultiplier;
    _engine.setTiming(tm);
    calloc.free(tm);
  }

  /// Choose the camera projection.
  void setProjection(TesseraProjection projection) =>
      _engine.setProjection(projection.index);

  /// Smallest orbit distance keeping every listed tile/entity id on screen with
  /// a fractional [padding] margin. Pure query; feed the result into your
  /// camera's `distance`. Call during setup (before [start]).
  double? cameraFitDistance({
    List<int> tileIds = const [],
    List<int> entityIds = const [],
    double padding = 0.06,
  }) {
    final tiles = calloc<Uint64>(tileIds.isEmpty ? 1 : tileIds.length);
    final ents = calloc<Uint64>(entityIds.isEmpty ? 1 : entityIds.length);
    for (var i = 0; i < tileIds.length; ++i) {
      tiles[i] = tileIds[i];
    }
    for (var i = 0; i < entityIds.length; ++i) {
      ents[i] = entityIds[i];
    }
    final out = calloc<Float>();
    final ok = _engine.cameraFitDistance(
      tiles, tileIds.length, ents, entityIds.length, padding, out);
    final result = ok ? out.value : null;
    calloc.free(tiles);
    calloc.free(ents);
    calloc.free(out);
    return result;
  }

  // ---- runtime ----

  /// Push a scene snapshot. Thread-safe (deep-copied under the engine mutex);
  /// the engine diffs it against the current scene and animates the transition.
  void setScene(TesseraScene scene) {
    final tiles = calloc<t.TesseraTilePlacement>(
        scene.tiles.isEmpty ? 1 : scene.tiles.length);
    final ents = calloc<t.TesseraEntityPlacement>(
        scene.entities.isEmpty ? 1 : scene.entities.length);

    for (var i = 0; i < scene.tiles.length; ++i) {
      final s = scene.tiles[i];
      final p = tiles[i];
      p.coord
        ..x = s.x
        ..y = s.y;
      p
        ..tileDef = s.def
        ..variant = s.variant
        ..id = s.id;
    }
    for (var i = 0; i < scene.entities.length; ++i) {
      final s = scene.entities[i];
      final p = ents[i];
      p
        ..id = s.id
        ..def = s.def
        ..facing = s.facing
        ..anim = s.anim;
      p.coord
        ..x = s.x
        ..y = s.y;
    }

    final st = calloc<t.TesseraState>();
    st.ref
      ..tiles = tiles
      ..tileCount = scene.tiles.length
      ..entities = ents
      ..entityCount = scene.entities.length
      ..effects = nullptr
      ..effectCount = 0
      ..epoch = scene.epoch;
    st.ref.camera
      ..distance = scene.camera.distance
      ..yaw = scene.camera.yaw
      ..pitch = scene.camera.pitch
      ..fov = scene.camera.fov;
    st.ref.camera.focus
      ..x = scene.camera.focusX
      ..y = scene.camera.focusY;

    _engine.setState(st); // deep-copies; safe to free immediately after
    calloc.free(st);
    calloc.free(tiles);
    calloc.free(ents);
  }

  /// True when no transitions are active (any-thread).
  bool get isIdle => _engine.isIdle;

  /// Start the native render loop. Call once, after setup + the first scene.
  Future<void> start() async {
    if (_started || _disposed) return;
    await _channel.invokeMethod<void>('start');
    _started = true;
  }

  /// Ray-pick the tile/entity under a logical view pixel ([x],[y] — top-left
  /// origin, the same space as Flutter pointer events). Runs on the native main
  /// thread, serialized with the render loop.
  Future<TesseraPickResult?> pick(double x, double y) async {
    if (_disposed) return null;
    final map = await _channel.invokeMethod<Map<Object?, Object?>>(
      'pick',
      {'x': x, 'y': y},
    );
    return map == null ? null : TesseraPickResult.fromMap(map);
  }

  /// Release the controller. The native platform view owns the engine and tears
  /// it down when the view is disposed; this just detaches the Dart side.
  Future<void> dispose() async {
    if (_disposed) return;
    _disposed = true;
    _engine.dispose(); // no-op for a fromHandle wrapper (host owns lifecycle)
  }

  void _fillParticle(t.TesseraParticleSpec p, TesseraParticle spec) {
    p
      ..mode = spec.mode.index
      ..count = spec.count
      ..lifetimeS = spec.lifetime
      ..lifetimeVar = spec.lifetimeVar
      ..speed = spec.speed
      ..speedVar = spec.speedVar
      ..spreadDeg = spec.spreadDeg
      ..gravity = spec.gravity
      ..sizeStart = spec.sizeStart
      ..sizeEnd = spec.sizeEnd
      ..blend = spec.blend.index
      ..durationS = spec.durationS;
    for (var i = 0; i < 4; ++i) {
      p.colorStart[i] = spec.colorStart[i];
      p.colorEnd[i] = spec.colorEnd[i];
    }
  }
}
