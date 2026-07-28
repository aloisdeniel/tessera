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

import 'dart:async';
import 'dart:convert' show utf8;
import 'dart:ffi';
import 'dart:io' show Platform;

import 'package:ffi/ffi.dart';
import 'package:flutter/services.dart';
import 'package:tessera/tessera.dart' as t;

import 'types.dart';

/// Resolve the native library holding the tessera_* symbols for FFI. On Apple
/// platforms the plugin has already loaded them into the process image; on
/// Android the engine ships as `libtessera.so` alongside the plugin.
DynamicLibrary _defaultLibrary() {
  if (Platform.isAndroid) return DynamicLibrary.open('libtessera.so');
  return DynamicLibrary.process();
}

/// Controls a single [TesseraView]. Obtain one from `TesseraView`'s
/// `onCreated` callback; do the def/light/quality/timing setup, push an initial
/// scene, then call [start].
class TesseraController {
  TesseraController._(this._channel, this._engine) {
    _channel.setMethodCallHandler(_handleNativeCall);
    _installOperationCallback();
    _installEventCallback();
  }

  final MethodChannel _channel;
  final t.Tessera _engine;
  bool _started = false;
  bool _disposed = false;

  // Operation completion plumbing. `set_state` returns a monotonic op id; the
  // engine fires [_onOperationCompleted] (on its tick thread, marshalled to this
  // isolate by NativeCallable.listener) as each op's transition settles. We turn
  // that event stream into per-scene Futures instead of polling isIdle.
  NativeCallable<t.TesseraOpCompletedNative>? _opCallback;
  int _lastCompletedOp = 0;
  final _opWaiters = <int, List<Completer<void>>>{};

  void _installOperationCallback() {
    final cb = NativeCallable<t.TesseraOpCompletedNative>.listener(
      _onOperationCompleted,
    );
    _opCallback = cb;
    _engine.setOperationCallback(cb.nativeFunction, nullptr);
  }

  void _onOperationCompleted(int op, Pointer<Void> user) {
    if (op > _lastCompletedOp) _lastCompletedOp = op;
    // Complete every waiter whose op the engine has now reached (ids monotonic).
    _opWaiters.removeWhere((waited, completers) {
      if (waited > _lastCompletedOp) return false;
      for (final c in completers) {
        if (!c.isCompleted) c.complete();
      }
      return true;
    });
  }

  // Engine event plumbing. The native tick thread emits typed events into a
  // fixed ring; a NativeCallable.listener callback (registered at attach, before
  // the render loop starts) marshals a wake-up onto this isolate, where the ring
  // is drained and re-emitted on [events]. The callback's event pointer is only
  // a wake-up signal — delivery is asynchronous, so the slot may already be
  // recycled; the drain never loses anything between wake-ups.
  NativeCallable<t.TesseraEventNative>? _eventCallback;
  final _events = StreamController<TesseraEvent>.broadcast();

  void _installEventCallback() {
    final cb = NativeCallable<t.TesseraEventNative>.listener(_onEngineEvent);
    _eventCallback = cb;
    _engine.setEventCallback(cb.nativeFunction, nullptr);
  }

  void _onEngineEvent(Pointer<t.TesseraEvent> ev, Pointer<Void> user) {
    // Never dereference `ev` (recycled ring slot); drain the ring instead.
    if (_disposed || _events.isClosed) return;
    for (final e in _engine.drainEvents()) {
      _events.add(TesseraEvent(
        type: e.type >= 0 && e.type < TesseraEventType.values.length
            ? TesseraEventType.values[e.type]
            : TesseraEventType.none,
        subject: e.subject >= 0 && e.subject < TesseraEventSubject.values.length
            ? TesseraEventSubject.values[e.subject]
            : TesseraEventSubject.none,
        subjectId: e.subjectId,
        time: e.time,
        x: e.coordX,
        y: e.coordY,
        value: e.value,
      ));
    }
  }

  /// Broadcast stream of typed engine events — dice contacts and settles, hop
  /// landings, waypoint handoffs, spawns/removals, card flips and deals, camera
  /// arrival, operation completion — for sound, haptics and FX sync. Events fire
  /// on the engine's tick thread and are delivered asynchronously onto this
  /// isolate (the one the controller lives on), in emission order. Listen any
  /// time; events emitted while nobody listens are discarded (broadcast
  /// semantics). Closed by [dispose].
  Stream<TesseraEvent> get events => _events.stream;

  /// Total engine events dropped to ring overflow since engine creation (0 when
  /// delivery keeps up). Cumulative; any-thread.
  int get eventsDropped => _engine.eventsDropped;

  /// Called when the view's logical (point) size changes — on first layout and
  /// on every resize / orientation change. The engine has already been resized,
  /// so this is the moment to recompute [cameraFitDistance] and push a scene
  /// with the new camera `distance`. Arguments are the logical width/height.
  void Function(double width, double height)? onResize;

  Future<Object?> _handleNativeCall(MethodCall call) async {
    switch (call.method) {
      case 'resize':
        final args = call.arguments as Map<Object?, Object?>?;
        final w = (args?['width'] as num?)?.toDouble();
        final h = (args?['height'] as num?)?.toDouble();
        if (w != null && h != null && w > 0 && h > 0) onResize?.call(w, h);
    }
    return null;
  }

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
      library: library ?? _defaultLibrary(),
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

  /// Register an atlas (texture) from encoded image [image] bytes (PNG/JPG/…).
  /// Returns its def id (0 = failure). Referenced by card faces.
  int registerAtlas(Uint8List image) {
    final bytes = calloc<Uint8>(image.length);
    bytes.asTypedList(image.length).setAll(0, image);
    final def = calloc<t.TesseraBytes>();
    def.ref
      ..data = bytes.cast<Void>()
      ..size = image.length;
    final id = _engine.registerAtlas(def); // copies the bytes it needs
    calloc.free(def);
    calloc.free(bytes);
    return id;
  }

  /// Register a card definition (visible/hidden/back atlas ids). Returns its def
  /// id (0 = failure). Place cards/piles/hands via [setScene].
  int registerCardType(TesseraCardType type) {
    final def = calloc<t.TesseraCardDef>();
    def.ref
      ..visibleAtlas = type.visibleAtlas
      ..hiddenAtlas = type.hiddenAtlas
      ..backAtlas = type.backAtlas
      ..width = type.width
      ..height = type.height
      ..thickness = type.thickness
      ..cornerRadius = type.cornerRadius;
    for (var i = 0; i < 4; ++i) {
      def.ref.tint[i] = type.tint[i];
    }
    // visible/hidden/back UV rects are left zero => "use the whole texture".
    final id = _engine.registerCardDef(def);
    calloc.free(def);
    return id;
  }

  /// Register a dice definition from per-face sprite images. Returns its def id
  /// (0 = failure). Place dice via [setScene].
  int registerDiceType(TesseraDiceType type) {
    final n = type.faces.length;
    final faces = calloc<t.TesseraDiceFace>(n == 0 ? 1 : n);
    final buffers = <Pointer<Uint8>>[];
    for (var i = 0; i < n; ++i) {
      final img = type.faces[i];
      final b = calloc<Uint8>(img.length);
      b.asTypedList(img.length).setAll(0, img);
      buffers.add(b);
      faces[i].sprite
        ..data = b.cast<Void>()
        ..size = img.length;
    }
    final def = calloc<t.TesseraDiceDef>();
    def.ref
      ..faces = faces
      ..faceCount = n
      ..size = type.size;
    for (var i = 0; i < 4; ++i) {
      def.ref.tint[i] = type.tint[i];
    }
    final id = _engine.registerDiceDef(def); // copies/decodes the sprites
    calloc.free(def);
    for (final b in buffers) {
      calloc.free(b);
    }
    calloc.free(faces);
    return id;
  }

  /// Register a TrueType/OpenType font from raw file bytes; the engine bakes
  /// ASCII + Latin-1 glyphs at [pixelHeight] texels into a GPU atlas. Returns
  /// its def id (0 = failure). Referenced by [TesseraLabel.font]; place labels
  /// via [setScene].
  int registerFont(Uint8List ttf, {double pixelHeight = 48}) {
    final bytes = calloc<Uint8>(ttf.length);
    bytes.asTypedList(ttf.length).setAll(0, ttf);
    final def = calloc<t.TesseraBytes>();
    def.ref
      ..data = bytes.cast<Void>()
      ..size = ttf.length;
    final id = _engine.registerFont(def, pixelHeight); // copies the bytes
    calloc.free(def);
    calloc.free(bytes);
    return id;
  }

  /// Register a sound effect from WAV file bytes (PCM/float WAV — e.g. a
  /// Flutter asset loaded with `rootBundle.load`). Returns its sound id
  /// (0 = failure). Play it with [playSound]; typically triggered off the
  /// [events] stream so audio lands exactly on the engine's beats.
  int registerSound(Uint8List wav) {
    final bytes = calloc<Uint8>(wav.length);
    bytes.asTypedList(wav.length).setAll(0, wav);
    final def = calloc<t.TesseraBytes>();
    def.ref
      ..data = bytes.cast<Void>()
      ..size = wav.length;
    final id = _engine.registerSound(def); // copies/decodes the bytes
    calloc.free(def);
    calloc.free(bytes);
    return id;
  }

  /// (Re)start a registered sound at [gain] (1 = as authored; different sounds
  /// mix, re-triggering one restarts it). Any-thread; safe while the render
  /// loop runs. Returns false when [id] is unknown, playback is unavailable,
  /// or the controller is disposed.
  bool playSound(int id, {double gain = 1.0}) {
    if (_disposed) return false;
    return _engine.playSound(id, gain);
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

  /// Set render quality (shadows, MSAA, resolution scale). Unlike the other
  /// setup calls this one is any-thread on the engine side (mutex-guarded; the
  /// renderer takes one consistent copy per frame), so it may also be called
  /// live after [start] — e.g. toggling the shadow mode at runtime.
  void setQuality(TesseraQualityData quality) {
    if (_disposed) return;
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
  /// camera's `distance`. Uses the live viewport aspect, so call it during setup
  /// AND from [onResize] to re-fit after a layout/orientation change.
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

  /// Push a scene snapshot and return a Future that completes once the
  /// transition it triggers has fully played out — every entity/card/dice/
  /// effect/camera animation settled and the engine reports [isIdle] again.
  ///
  /// Thread-safe (deep-copied under the engine mutex); the engine diffs it
  /// against the current scene and animates the difference. `set_state` marks
  /// the snapshot pending immediately, so the engine reports *not* idle
  /// synchronously after this call — the Future can never resolve before the
  /// transition has started.
  ///
  /// Awaiting lets callers sequence dependent beats — e.g. throw a die, await
  /// it settling, *then* move a piece — instead of pushing both at once.
  /// Ignoring the Future keeps the old fire-and-forget behaviour.
  ///
  /// After [dispose] this is a no-op returning an already-completed Future —
  /// late pushes (e.g. an event handler racing a screen pop) must not reach an
  /// engine the native view may already be tearing down.
  Future<void> setScene(TesseraScene scene) {
    if (_disposed) return Future.value();
    final op = _withNativeScene(scene, (st) => _engine.setState(st));
    return _awaitOperation(op);
  }

  /// Marshal [scene] into a native `TesseraState`, run [body] with it, then
  /// free every allocation (the engine deep-copies whatever it keeps). The
  /// shared bridge under [setScene], [serializeScene] and replay recording.
  R _withNativeScene<R>(
      TesseraScene scene, R Function(Pointer<t.TesseraState> st) body) {
    final tiles = calloc<t.TesseraTilePlacement>(
        scene.tiles.isEmpty ? 1 : scene.tiles.length);
    final ents = calloc<t.TesseraEntityPlacement>(
        scene.entities.isEmpty ? 1 : scene.entities.length);
    final cards = calloc<t.TesseraCardPlacement>(
        scene.cards.isEmpty ? 1 : scene.cards.length);
    final draws = calloc<t.TesseraCardDrawPlacement>(
        scene.cardDraws.isEmpty ? 1 : scene.cardDraws.length);
    final hands = calloc<t.TesseraHandPlacement>(
        scene.hands.isEmpty ? 1 : scene.hands.length);
    final dice = calloc<t.TesseraDicePlacement>(
        scene.dice.isEmpty ? 1 : scene.dice.length);
    final overlays = calloc<t.TesseraOverlayPlacement>(
        scene.overlays.isEmpty ? 1 : scene.overlays.length);
    final labels = calloc<t.TesseraLabelPlacement>(
        scene.labels.isEmpty ? 1 : scene.labels.length);
    final highlights = calloc<t.TesseraHighlightPlacement>(
        scene.highlights.isEmpty ? 1 : scene.highlights.length);
    // Per-placement multi-step path buffers, freed after setState (which copies).
    final pathPtrs = <Pointer<NativeType>>[];

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
      if (s.path.length > 1) {
        final pp = calloc<t.TesseraCoord>(s.path.length);
        for (var k = 0; k < s.path.length; ++k) {
          pp[k]
            ..x = s.path[k].$1
            ..y = s.path[k].$2;
        }
        p
          ..path = pp
          ..pathCount = s.path.length;
        pathPtrs.add(pp);
      }
    }
    for (var i = 0; i < scene.cards.length; ++i) {
      final s = scene.cards[i];
      final p = cards[i];
      p
        ..id = s.id
        ..def = s.def
        ..hidden = s.hidden
        ..hand = s.hand
        ..handSlot = s.handSlot
        ..sourceDraw = s.sourceDraw;
      for (var k = 0; k < 3; ++k) {
        p.position[k] = s.position[k];
      }
      for (var k = 0; k < 4; ++k) {
        p.orientation[k] = s.orientation[k];
      }
      if (s.hand == 0 && s.path.length > 1) {
        final pp = calloc<Float>(s.path.length * 3);
        for (var k = 0; k < s.path.length; ++k) {
          pp[k * 3 + 0] = s.path[k][0];
          pp[k * 3 + 1] = s.path[k][1];
          pp[k * 3 + 2] = s.path[k][2];
        }
        p
          ..path = pp
          ..pathCount = s.path.length;
        pathPtrs.add(pp);
      }
    }
    for (var i = 0; i < scene.cardDraws.length; ++i) {
      final s = scene.cardDraws[i];
      final p = draws[i];
      p
        ..id = s.id
        ..def = s.def
        ..count = s.count
        ..topHidden = s.topHidden;
      for (var k = 0; k < 3; ++k) {
        p.position[k] = s.position[k];
      }
      for (var k = 0; k < 4; ++k) {
        p.orientation[k] = s.orientation[k];
      }
    }
    for (var i = 0; i < scene.hands.length; ++i) {
      final s = scene.hands[i];
      final p = hands[i];
      p
        ..id = s.id
        ..spreadDeg = s.spreadDeg
        ..radius = s.radius
        ..cardSpacing = s.cardSpacing;
      for (var k = 0; k < 3; ++k) {
        p.position[k] = s.position[k];
      }
      for (var k = 0; k < 4; ++k) {
        p.orientation[k] = s.orientation[k];
      }
    }
    for (var i = 0; i < scene.dice.length; ++i) {
      final s = scene.dice[i];
      final p = dice[i];
      p
        ..id = s.id
        ..def = s.def
        ..face = s.face
        ..seed = s.seed
        ..throwS = s.throwS;
      for (var k = 0; k < 3; ++k) {
        p.position[k] = s.position[k];
      }
    }
    for (var i = 0; i < scene.overlays.length; ++i) {
      final s = scene.overlays[i];
      final p = overlays[i];
      p.coord
        ..x = s.x
        ..y = s.y;
      p
        ..shape = s.shape.index
        ..atlas = s.atlas
        ..pulseS = s.pulseS
        ..pulseAlphaMin = s.pulseAlphaMin
        ..pulseAlphaMax = s.pulseAlphaMax
        ..pulseScaleMin = s.pulseScaleMin
        ..pulseScaleMax = s.pulseScaleMax;
      p.uv
        ..u0 = s.uv[0]
        ..v0 = s.uv[1]
        ..u1 = s.uv[2]
        ..v1 = s.uv[3];
      for (var k = 0; k < 4; ++k) {
        p.tint[k] = s.tint[k];
      }
    }
    for (var i = 0; i < scene.labels.length; ++i) {
      final s = scene.labels[i];
      final p = labels[i];
      p
        ..id = s.id
        ..font = s.font
        ..anchor = s.anchor.index
        ..anchorId = s.anchorId
        ..size = s.size
        ..billboard = s.billboard;
      final bytes = utf8.encode(s.text);
      final n = bytes.length < 63 ? bytes.length : 63;
      for (var k = 0; k < n; ++k) {
        p.text[k] = bytes[k];
      }
      p.text[n] = 0;
      for (var k = 0; k < 3; ++k) {
        p.position[k] = s.position[k];
      }
      for (var k = 0; k < 4; ++k) {
        p.color[k] = s.color[k];
      }
    }
    for (var i = 0; i < scene.highlights.length; ++i) {
      final s = scene.highlights[i];
      final p = highlights[i];
      p
        ..targetId = s.targetId
        ..kind = s.kind.index
        ..style = s.style.index
        ..thickness = s.thickness
        ..pulseS = s.pulseS
        ..pulseMin = s.pulseMin
        ..pulseMax = s.pulseMax;
      for (var k = 0; k < 4; ++k) {
        p.color[k] = s.color[k];
      }
    }

    final st = calloc<t.TesseraState>();
    st.ref
      ..tiles = tiles
      ..tileCount = scene.tiles.length
      ..entities = ents
      ..entityCount = scene.entities.length
      ..effects = nullptr
      ..effectCount = 0
      ..epoch = scene.epoch
      ..cards = cards
      ..cardCount = scene.cards.length
      ..cardDraws = draws
      ..cardDrawCount = scene.cardDraws.length
      ..hands = hands
      ..handCount = scene.hands.length
      ..dice = dice
      ..diceCount = scene.dice.length
      ..overlays = overlays
      ..overlayCount = scene.overlays.length
      ..labels = labels
      ..labelCount = scene.labels.length
      ..highlights = highlights
      ..highlightCount = scene.highlights.length;
    // `calloc` zeroed the whole TesseraState, so any camera field a case does
    // not touch stays 0 (mode 0 = ORBIT, ids/target/orientation all zero).
    final cam = st.ref.camera;
    switch (scene.camera) {
      case TesseraCameraPose c:
        cam
          ..mode = 0
          ..distance = c.distance
          ..yaw = c.yaw
          ..pitch = c.pitch
          ..fov = c.fov;
        cam.focus
          ..x = c.focusX
          ..y = c.focusY;
      case TesseraCameraManual c:
        cam
          ..mode = 1
          ..fov = c.fov;
        for (var k = 0; k < 3; ++k) {
          cam.position[k] = c.position[k];
        }
        for (var k = 0; k < 4; ++k) {
          cam.orientation[k] = c.orientation[k];
        }
      case TesseraCameraTarget c:
        cam
          ..mode = 2
          ..fov = c.fov;
        for (var k = 0; k < 3; ++k) {
          cam.position[k] = c.source[k];
          cam.target[k] = c.target[k];
        }
      case TesseraCameraFocusTile c:
        cam
          ..mode = 3
          ..targetId = c.tileId
          ..distance = c.distance
          ..yaw = c.yaw
          ..pitch = c.pitch
          ..fov = c.fov;
      case TesseraCameraFocusEntity c:
        cam
          ..mode = 4
          ..targetId = c.entityId
          ..distance = c.distance
          ..yaw = c.yaw
          ..pitch = c.pitch
          ..fov = c.fov;
      case TesseraCameraFocusDice c:
        cam
          ..mode = 5
          ..targetId = c.diceId
          ..distance = c.distance
          ..yaw = c.yaw
          ..pitch = c.pitch
          ..fov = c.fov;
      case TesseraCameraFocusDraw c:
        cam
          ..mode = 6
          ..targetId = c.drawId
          ..distance = c.distance
          ..yaw = c.yaw
          ..pitch = c.pitch
          ..fov = c.fov;
      case TesseraCameraFocusCard c:
        cam
          ..mode = 7
          ..targetId = c.cardId
          ..fitPadding = c.padding
          ..fov = c.fov;
      case TesseraCameraFocusHand c:
        cam
          ..mode = 8
          ..targetId = c.handId
          ..focusCardId = c.cardId ?? 0
          ..fitPadding = c.padding
          ..fov = c.fov;
    }

    // Whatever [body] does (set_state, serialize, replay-append) deep-copies
    // what it keeps, so everything is safe to free as soon as it returns.
    final result = body(st);
    calloc.free(st);
    calloc.free(tiles);
    calloc.free(ents);
    calloc.free(cards);
    calloc.free(draws);
    calloc.free(hands);
    calloc.free(dice);
    calloc.free(overlays);
    calloc.free(labels);
    calloc.free(highlights);
    for (final p in pathPtrs) {
      calloc.free(p);
    }
    return result;
  }

  // ---- state serialization, save / undo & replay ----

  /// Serialize [scene] into a self-contained, versioned binary blob (identical
  /// on every platform) — the building block for save games, undo stacks and
  /// replays. Pure data: no engine state is read and nothing is pushed; the
  /// bytes round-trip through [restoreScene]. Any-thread.
  Uint8List serializeScene(TesseraScene scene) =>
      _withNativeScene(scene, (st) => _engine.serializeState(st));

  /// Reconstruct the scene stored in [blob] (bytes produced by
  /// [serializeScene], or one record of a replay) and push it to the engine,
  /// returning the same completion Future as [setScene]. Throws
  /// [ArgumentError] on a malformed blob (bad magic/version/truncation). A
  /// no-op (already-completed Future) after [dispose], like [setScene].
  Future<void> restoreScene(Uint8List blob) {
    if (_disposed) return Future.value();
    final st = _engine.deserializeState(blob);
    if (st == nullptr) {
      throw ArgumentError('flutter_tessera: malformed scene blob');
    }
    final op = _engine.setState(st);
    _engine.freeState(st);
    return _awaitOperation(op);
  }

  /// New empty replay recorder. Append scenes as the game plays, then
  /// [TesseraReplayRecorder.serialize] the whole container to bytes. Dispose it
  /// when done recording.
  TesseraReplayRecorder createReplayRecorder() {
    final r = _engine.createReplay();
    if (r == nullptr) {
      throw StateError('flutter_tessera: replay allocation failed');
    }
    return TesseraReplayRecorder._(this, r);
  }

  /// Open a serialized replay container ([TesseraReplayRecorder.serialize]
  /// bytes) for playback through this controller. Throws [ArgumentError] on
  /// malformed input. Dispose the player when done.
  TesseraReplayPlayer openReplay(Uint8List data) {
    final r = _engine.openReplay(data);
    if (r == nullptr) {
      throw ArgumentError('flutter_tessera: malformed replay data');
    }
    return TesseraReplayPlayer._(this, r);
  }

  /// A Future that completes when operation [op]'s transition has fully settled,
  /// driven by the engine's completion event (no polling). Resolves synchronously
  /// if the operation is already done (or the controller is disposed).
  Future<void> _awaitOperation(int op) {
    if (op == 0 ||
        _disposed ||
        op <= _lastCompletedOp ||
        _engine.operationCompleted(op)) {
      return Future.value();
    }
    final completer = Completer<void>();
    (_opWaiters[op] ??= <Completer<void>>[]).add(completer);
    return completer.future;
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
    onResize = null;
    _channel.setMethodCallHandler(null);
    // Detach the native callbacks before closing their NativeCallables. The
    // engine holds its callback slots under a mutex ACROSS delivery, so each
    // clear below only returns once no tick-thread delivery is still using the
    // old function pointer — closing the NativeCallable right after is safe
    // even while the render loop is running. Then release any Futures still
    // waiting on an operation.
    _engine.setOperationCallback(nullptr, nullptr);
    _opCallback?.close();
    _opCallback = null;
    _engine.setEventCallback(nullptr, nullptr);
    _eventCallback?.close();
    _eventCallback = null;
    _events.close();
    for (final completers in _opWaiters.values) {
      for (final c in completers) {
        if (!c.isCompleted) c.complete();
      }
    }
    _opWaiters.clear();
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

/// Records a timestamped sequence of scenes into a replay container (see
/// `tessera_replay_*`). Obtain one from [TesseraController.createReplayRecorder],
/// [add] a scene per beat as the game plays (each is serialized immediately —
/// pure data, no engine state involved), [serialize] the whole container to
/// bytes (write them to a file, ship them, ...), and [dispose] when done. Play
/// the bytes back later with [TesseraController.openReplay].
class TesseraReplayRecorder {
  TesseraReplayRecorder._(this._controller, this._replay);

  final TesseraController _controller;
  Pointer<t.TesseraReplay> _replay;

  /// Number of recorded scenes (0 after [dispose]).
  int get length =>
      _replay == nullptr ? 0 : _controller._engine.replayCount(_replay);

  /// Append [scene] with a host-defined [timestampMs] (e.g. milliseconds since
  /// recording started; playback order is append order regardless). Returns
  /// false after [dispose] or on allocation failure.
  bool add(TesseraScene scene, {int timestampMs = 0}) {
    if (_replay == nullptr) return false;
    return _controller._withNativeScene(
        scene, (st) => _controller._engine.replayAppend(_replay, timestampMs, st));
  }

  /// Flatten the container to a self-contained byte blob (empty after
  /// [dispose]).
  Uint8List serialize() =>
      _replay == nullptr ? Uint8List(0) : _controller._engine.serializeReplay(_replay);

  /// Release the native container. Safe to call more than once.
  void dispose() {
    if (_replay != nullptr) {
      _controller._engine.freeReplay(_replay);
      _replay = nullptr;
    }
  }
}

/// Plays back a recorded replay through its controller. Obtain one from
/// [TesseraController.openReplay]; [play] pushes record [index]'s scene to the
/// engine (the engine animates the diff from whatever is currently shown, so
/// stepping records in order replays the game's beats). [dispose] when done.
class TesseraReplayPlayer {
  TesseraReplayPlayer._(this._controller, this._replay);

  final TesseraController _controller;
  Pointer<t.TesseraReplay> _replay;

  /// Number of records (0 after [dispose]).
  int get length =>
      _replay == nullptr ? 0 : _controller._engine.replayCount(_replay);

  /// The recorded timestamp (ms, as passed to [TesseraReplayRecorder.add]) of
  /// record [index], or null when out of range / after [dispose].
  int? timestampMs(int index) {
    if (_replay == nullptr) return null;
    final out = calloc<Uint64>();
    final st = _controller._engine.replayGet(_replay, index, out);
    final ts = st == nullptr ? null : out.value;
    if (st != nullptr) _controller._engine.freeState(st);
    calloc.free(out);
    return ts;
  }

  /// Push record [index]'s scene to the engine, returning the same completion
  /// Future as [TesseraController.setScene]. Throws [RangeError] on a bad
  /// index (or a corrupt record) and [StateError] after [dispose]. A no-op
  /// (already-completed Future) once the *controller* has been disposed — a
  /// playback loop racing a screen pop must not push into an engine the
  /// native view may already be tearing down.
  Future<void> play(int index) {
    if (_replay == nullptr) {
      throw StateError('flutter_tessera: replay player disposed');
    }
    if (_controller._disposed) return Future.value();
    final st = _controller._engine.replayGet(_replay, index, nullptr);
    if (st == nullptr) {
      throw RangeError('flutter_tessera: no replay record $index');
    }
    final op = _controller._engine.setState(st);
    _controller._engine.freeState(st);
    return _controller._awaitOperation(op);
  }

  /// Release the native container. Safe to call more than once.
  void dispose() {
    if (_replay != nullptr) {
      _controller._engine.freeReplay(_replay);
      _replay = nullptr;
    }
  }
}
