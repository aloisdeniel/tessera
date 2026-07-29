// tessera_controller_web.dart — the web implementation of TesseraController.
//
// Mirrors lib/src/tessera_controller.dart (the io/FFI implementation) with the
// exact same public API, driving the Emscripten/WebGPU engine build
// (assets/web/tessera_web.js) through the serialized [TesseraWasm] call chain:
//
//   • The controller OWNS the render loop: a requestAnimationFrame-driven
//     `tessera_tick`, with event polling, operation-completion polling and
//     canvas-size tracking after each tick (there are no native callbacks on
//     the web — everything is polled between ticks).
//   • Scenes cross into wasm as the engine's own serialized blob format
//     (tessera_scene_wire.dart -> tessera_state_deserialize -> tessera_set_state),
//     so no in-memory TesseraState struct is mirrored here.
//   • Registration/config structs are built directly in wasm32 heap memory,
//     with layouts derived from include/tessera.h under the wasm32 ABI
//     (pointers/size_t 4 bytes, u64/double 8-byte aligned).
//
// Registration ids are returned synchronously by PREDICTING the engine's
// deterministic id assignment (see _predictDefId / _predictSoundId below and
// the notes on src/registry.c + src/core/core.c); each prediction is verified
// when the underlying async call resolves.

import 'dart:async';
import 'dart:js_interop';
import 'dart:js_interop_unsafe';
import 'dart:math' as math;
import 'dart:typed_data';

import 'package:flutter/foundation.dart' show debugPrint;
import 'package:web/web.dart' as web;

import '../types.dart';
import 'tessera_scene_wire.dart';
import 'tessera_wasm.dart';

const Endian _le = Endian.little;

/// Controls a single [TesseraView]. Obtain one from `TesseraView`'s
/// `onCreated` callback; do the def/light/quality/timing setup, push an initial
/// scene, then call [start]. Web counterpart of the io controller — same API,
/// same semantics.
class TesseraController {
  TesseraController._(this._wasm, this._engine, this._canvas) {
    _evBuf = _wasm.malloc(_evCap * _evSize);
  }

  // On the web the wasm module hosts ONE SDL canvas binding at a time, so at
  // most one controller may be live (see TesseraView's web docs).
  static TesseraController? _live;

  final TesseraWasm _wasm;
  final int _engine; // TesseraEngine* (wasm32 address)
  final web.HTMLCanvasElement _canvas;
  bool _started = false;
  bool _disposed = false;

  // ---- engine event polling (TesseraEvent is 40 bytes on every target) ----
  static const _evSize = 40;
  static const _evCap = 16;
  late final int _evBuf; // TesseraEvent[16] in wasm memory
  final _events = StreamController<TesseraEvent>.broadcast();
  int _eventsDropped = 0;

  // ---- operation completion (polled off tessera_last_completed_operation) --
  int _lastCompletedOp = 0;
  final _opWaiters = <int, List<Completer<void>>>{};
  int _busyScenes = 0; // scene pushes issued and not yet settled (drives isIdle)

  // ---- render loop ----
  int? _rafId;
  bool _ticking = false;
  double _lastFrameMs = -1;
  int _tickCount = 0;
  final _firstTick = Completer<void>();
  int _physW = 0, _physH = 0; // last physical drawable size pushed to the engine

  // ---- deterministic id prediction (see registerTileType docs) ----
  int _defCount = 0;
  int _soundCount = 0;

  // ---- cameraFitDistance async cache ----
  final _fitCache = <String, double>{};
  final _fitInFlight = <String>{};

  /// Broadcast stream of typed engine events — dice contacts and settles, hop
  /// landings, waypoint handoffs, spawns/removals, card flips and deals, camera
  /// arrival, operation completion — for sound, haptics and FX sync. On the web
  /// the engine's event ring is drained after every tick and re-emitted here in
  /// emission order. Listen any time; events emitted while nobody listens are
  /// discarded (broadcast semantics). Closed by [dispose].
  Stream<TesseraEvent> get events => _events.stream;

  /// Total engine events dropped to ring overflow since engine creation (0 when
  /// delivery keeps up). Refreshed periodically from the engine while the
  /// render loop runs.
  int get eventsDropped => _eventsDropped;

  /// Called when the view's logical (CSS pixel) size changes — on first layout
  /// and on every resize / orientation change. The engine has already been
  /// resized, so this is the moment to recompute [cameraFitDistance] and push a
  /// scene with the new camera `distance`. Arguments are the logical
  /// width/height.
  void Function(double width, double height)? onResize;

  /// Whether the render loop has been started.
  bool get started => _started;

  /// The low-level wasm module wrapper, for exported `tessera_*` calls not
  /// surfaced here (the web analogue of the io controller's FFI `engine`
  /// getter). Pass [engineHandle] as the first argument of engine calls.
  TesseraWasm get engine => _wasm;

  /// The raw `TesseraEngine*` (a wasm32 address) for use with [engine].
  int get engineHandle => _engine;

  /// Attach to the platform view [viewId]: loads the wasm module (once per
  /// page) and creates an engine bound to this view's canvas. [library] is
  /// accepted for io API compatibility and ignored on the web.
  static Future<TesseraController> attach(
    int viewId, {
    Object? library,
  }) async {
    if (_live != null && !_live!._disposed) {
      throw StateError(
          'flutter_tessera: only one TesseraView may be live at a time on the '
          'web (the SDL/WebGPU engine binds a single canvas). Dispose the '
          'previous view\'s controller first.');
    }
    final wasm = await TesseraWasm.load();
    // The platform-view element is slotted into the DOM during the frame
    // AFTER onPlatformViewCreated fires. The first view never notices (the
    // wasm module load above spans many frames), but on a warm module —
    // every view after the first — attach gets here before the canvas is
    // inserted, so poll briefly instead of failing outright.
    web.Element? canvas;
    for (var waitedMs = 0; waitedMs <= 5000; waitedMs += 16) {
      canvas = web.document.getElementById('tessera-canvas-$viewId');
      if (canvas != null) break;
      await Future<void>.delayed(const Duration(milliseconds: 16));
    }
    if (canvas == null || !canvas.isA<web.HTMLCanvasElement>()) {
      throw StateError(
          'flutter_tessera: canvas for view $viewId not found in the DOM');
    }
    final el = canvas as web.HTMLCanvasElement;
    // The canvas itself is pointer-transparent, but the engine slots it into
    // an <flt-platform-view> wrapper with default pointer-events — which would
    // swallow taps before Flutter's gesture system sees them (Flutter ignores
    // pointer events targeted at platform views). Make the wrapper chain
    // transparent so hits fall through to the Flutter scene underneath.
    for (web.Element? p = el.parentElement;
        p != null && p.tagName != 'FLT-GLASS-PANE';
        p = p.parentElement) {
      if (p.isA<web.HTMLElement>()) {
        (p as web.HTMLElement).style.pointerEvents = 'none';
      }
    }
    final dpr = web.window.devicePixelRatio;
    final cw = el.clientWidth > 0 ? el.clientWidth : 1280;
    final ch = el.clientHeight > 0 ? el.clientHeight : 720;
    final pw = (cw * dpr).round();
    final ph = (ch * dpr).round();
    final ptr = await wasm.call('tessera_web_create', 'number',
        ['#tessera-canvas-$viewId', pw, ph, dpr, 0]) as int;
    if (ptr == 0) {
      // tessera_last_error tolerates a NULL engine ("null engine"), but with a
      // failed create there is no engine to hold an error — report generically.
      throw StateError(
          'flutter_tessera: engine creation failed (WebGPU unavailable?)');
    }
    final c = TesseraController._(wasm, ptr, el);
    c._physW = pw;
    c._physH = ph;
    _live = c;
    // Debug hook next to __tesseraModule: the live engine pointer.
    web.window.setProperty('__tesseraEngine'.toJS, ptr.toJS);
    return c;
  }

  /// Serialized call into the module; see [TesseraWasm.call].
  Future<Object?> _call(String name, String? returns, List<Object?> args) =>
      _wasm.call(name, returns, args);

  // ---- setup (call before start(), like the io controller) ----------------

  // Definition ids are handles from the engine's def slot map
  // (src/registry.c -> ts_slotmap_alloc in src/core/core.c): handle =
  // slot_index | generation << 24, generations start at 1, and a fresh
  // registry hands out slot indices 0, 1, 2, ... in registration order (defs
  // are never freed while an engine lives — there is no unregister API), so
  // the n-th successful registration of ANY def kind gets id 0x01000000 + n.
  // Sound ids (src/audio/audio.c) are simply 1, 2, 3, ... in order. Both are
  // therefore predictable, which lets the web register* methods keep the io
  // API's synchronous `int` returns: the id is predicted and returned
  // immediately, the marshalling happens behind the serialized call chain,
  // and the prediction is verified (and the counter resynced + an error
  // logged) when the real id arrives.
  int _predictDefId() => 0x01000000 + _defCount++;

  int _predictSoundId() => ++_soundCount;

  void _verifyDefId(String what, int predicted, int actual) {
    if (actual == predicted) return;
    if (actual == 0) {
      // Registration failed natively: the engine consumed no slot, so give the
      // predicted id back to the next registration.
      _defCount--;
      _lastError('$what: registration failed');
    } else {
      debugPrint('flutter_tessera: $what returned id $actual, predicted '
          '$predicted — resyncing');
      _defCount = (actual - 0x01000000) + 1;
    }
  }

  void _verifySoundId(String what, int predicted, int actual) {
    if (actual == predicted) return;
    if (actual == 0) {
      _soundCount--;
      _lastError('$what: registration failed');
    } else {
      debugPrint('flutter_tessera: $what returned id $actual, predicted '
          '$predicted — resyncing');
      _soundCount = actual;
    }
  }

  void _lastError(String what) {
    _call('tessera_last_error', 'string', [_engine]).then((msg) {
      debugPrint('flutter_tessera: $what — engine says: $msg');
    }).catchError((_) {
      debugPrint('flutter_tessera: $what');
    });
  }

  /// Allocate a NUL-terminated C string in wasm memory.
  int _allocCString(String s) {
    final b = Uint8List.fromList([...s.codeUnits.map((c) => c & 0x7f), 0]);
    return _wasm.allocBytes(b);
  }

  static void _setU64(ByteData d, int off, int v) {
    final hi = v ~/ 0x100000000;
    d.setUint32(off, v - hi * 0x100000000, _le);
    d.setUint32(off + 4, hi, _le);
  }

  static int _getU64(ByteData d, int off) =>
      d.getUint32(off, _le) + d.getUint32(off + 4, _le) * 0x100000000;

  /// A packed little-endian `uint64_t[]` for wasm memory.
  static Uint8List _u64Array(List<int> v) {
    final b = Uint8List(v.length * 8);
    final d = ByteData.sublistView(b);
    for (var i = 0; i < v.length; ++i) {
      _setU64(d, i * 8, v[i]);
    }
    return b;
  }

  /// Register a tile definition. Returns its def id (0 = failure — on the web
  /// a native failure is reported asynchronously via debugPrint; the returned
  /// id is the predicted one).
  int registerTileType(TesseraTileType type) {
    if (_disposed) return 0;
    final predicted = _predictDefId();
    // TesseraTileDef, wasm32: atlas u32@0, top/side/bottom TesseraRect
    // (4 f32) @4/@20/@36, tint f32[4]@52, thickness f32@68 — 72 bytes.
    final b = Uint8List(72);
    final d = ByteData.sublistView(b);
    for (var i = 0; i < 4; ++i) {
      d.setFloat32(52 + 4 * i, type.tint[i], _le);
    }
    d.setFloat32(68, type.thickness, _le);
    _registerCall('registerTileType', 'tessera_register_tile_def', b, predicted);
    return predicted;
  }

  void _registerCall(
      String what, String symbol, Uint8List defBytes, int predicted,
      {List<int> extraPtrs = const [], List<Object?> extraArgs = const []}) {
    final def = _wasm.allocBytes(defBytes);
    () async {
      try {
        final id =
            await _call(symbol, 'number', [_engine, def, ...extraArgs]) as int;
        _verifyDefId(what, predicted, id);
      } finally {
        _wasm.free(def);
        for (final p in extraPtrs) {
          _wasm.free(p);
        }
      }
    }();
  }

  /// Register an effect definition (particle bursts on add/remove). Returns
  /// its def id (0 = failure).
  int registerEffectType(TesseraEffectType type) {
    if (_disposed) return 0;
    final predicted = _predictDefId();
    // TesseraEffectDef = two TesseraParticleSpec (100 bytes each on wasm32):
    // atlas u32@0, sprite rect@4, mode u32@20, count u32@24, lifetime@28,
    // lifetime_var@32, speed@36, speed_var@40, spread_deg@44, gravity@48,
    // size_start@52, size_end@56, color_start[4]@60, color_end[4]@76,
    // blend u32@92, duration_s@96. on_add@0, on_remove@100 — 200 bytes.
    final b = Uint8List(200);
    final d = ByteData.sublistView(b);
    void fill(int base, TesseraParticle spec) {
      d.setUint32(base + 20, spec.mode.index, _le);
      d.setUint32(base + 24, spec.count, _le);
      d.setFloat32(base + 28, spec.lifetime, _le);
      d.setFloat32(base + 32, spec.lifetimeVar, _le);
      d.setFloat32(base + 36, spec.speed, _le);
      d.setFloat32(base + 40, spec.speedVar, _le);
      d.setFloat32(base + 44, spec.spreadDeg, _le);
      d.setFloat32(base + 48, spec.gravity, _le);
      d.setFloat32(base + 52, spec.sizeStart, _le);
      d.setFloat32(base + 56, spec.sizeEnd, _le);
      for (var i = 0; i < 4; ++i) {
        d.setFloat32(base + 60 + 4 * i, spec.colorStart[i], _le);
        d.setFloat32(base + 76 + 4 * i, spec.colorEnd[i], _le);
      }
      d.setUint32(base + 92, spec.blend.index, _le);
      d.setFloat32(base + 96, spec.durationS, _le);
    }

    if (type.onAdd != null) fill(0, type.onAdd!);
    if (type.onRemove != null) fill(100, type.onRemove!);
    _registerCall(
        'registerEffectType', 'tessera_register_effect_def', b, predicted);
    return predicted;
  }

  /// Register an entity definition from GLB [type.glb] bytes. Returns its def
  /// id (0 = failure).
  int registerEntityType(TesseraEntityType type) {
    if (_disposed) return 0;
    final predicted = _predictDefId();
    final bytes = _wasm.allocBytes(type.glb);
    final name = _allocCString('entity');
    // TesseraEntityDef, wasm32: gltf TesseraBytes{data u32@0, size u32@4,
    // path u32@8, debug_name u32@12}, atlas u32@16, scale f32@20,
    // pivot f32[3]@24, default/move/spawn/despawn_anim i32@36/40/44/48,
    // on_spawn_effect u32@52, on_despawn_effect u32@56 — 60 bytes.
    final b = Uint8List(60);
    final d = ByteData.sublistView(b);
    d.setUint32(0, bytes, _le);
    d.setUint32(4, type.glb.length, _le);
    d.setUint32(12, name, _le);
    d.setFloat32(20, type.scale, _le);
    for (final off in const [36, 40, 44, 48]) {
      d.setInt32(off, -1, _le);
    }
    d.setUint32(52, type.onSpawnEffect, _le);
    d.setUint32(56, type.onDespawnEffect, _le);
    _registerCall('registerEntityType', 'tessera_register_entity_def', b,
        predicted,
        extraPtrs: [bytes, name]);
    return predicted;
  }

  /// A TesseraBytes struct (16 bytes on wasm32) pointing at [ptr]/[len].
  static Uint8List _bytesStruct(int ptr, int len) {
    final b = Uint8List(16);
    final d = ByteData.sublistView(b);
    d.setUint32(0, ptr, _le);
    d.setUint32(4, len, _le);
    return b;
  }

  /// Register an atlas (texture) from encoded image [image] bytes (PNG/JPG/…).
  /// Returns its def id (0 = failure). Referenced by card faces.
  int registerAtlas(Uint8List image) {
    if (_disposed) return 0;
    final predicted = _predictDefId();
    final bytes = _wasm.allocBytes(image);
    _registerCall('registerAtlas', 'tessera_register_atlas',
        _bytesStruct(bytes, image.length), predicted,
        extraPtrs: [bytes]);
    return predicted;
  }

  /// Register a card definition (visible/hidden/back atlas ids). Returns its
  /// def id (0 = failure). Place cards/piles/hands via [setScene].
  int registerCardType(TesseraCardType type) {
    if (_disposed) return 0;
    final predicted = _predictDefId();
    // TesseraCardDef, wasm32: visible_atlas u32@0 + uv rect@4,
    // hidden_atlas@20 + uv@24, back_atlas@40 + uv@44, width@60, height@64,
    // thickness@68, corner_radius@72, tint f32[4]@76 — 92 bytes. UV rects are
    // left zero => "use the whole texture" (same as io).
    final b = Uint8List(92);
    final d = ByteData.sublistView(b);
    d.setUint32(0, type.visibleAtlas, _le);
    d.setUint32(20, type.hiddenAtlas, _le);
    d.setUint32(40, type.backAtlas, _le);
    d.setFloat32(60, type.width, _le);
    d.setFloat32(64, type.height, _le);
    d.setFloat32(68, type.thickness, _le);
    d.setFloat32(72, type.cornerRadius, _le);
    for (var i = 0; i < 4; ++i) {
      d.setFloat32(76 + 4 * i, type.tint[i], _le);
    }
    _registerCall('registerCardType', 'tessera_register_card_def', b, predicted);
    return predicted;
  }

  /// Register a dice definition from per-face sprite images. Returns its def
  /// id (0 = failure). Place dice via [setScene].
  int registerDiceType(TesseraDiceType type) {
    if (_disposed) return 0;
    final predicted = _predictDefId();
    final n = type.faces.length;
    final buffers = <int>[];
    // TesseraDiceFace[] — each face is one TesseraBytes (16 bytes on wasm32).
    final faces = Uint8List(n == 0 ? 16 : n * 16);
    final fd = ByteData.sublistView(faces);
    for (var i = 0; i < n; ++i) {
      final img = type.faces[i];
      final p = _wasm.allocBytes(img);
      buffers.add(p);
      fd.setUint32(i * 16, p, _le);
      fd.setUint32(i * 16 + 4, img.length, _le);
    }
    final facesPtr = _wasm.allocBytes(faces);
    // TesseraDiceDef, wasm32: faces ptr u32@0, face_count size_t u32@4,
    // size f32@8, tint f32[4]@12 — 28 bytes.
    final b = Uint8List(28);
    final d = ByteData.sublistView(b);
    d.setUint32(0, facesPtr, _le);
    d.setUint32(4, n, _le);
    d.setFloat32(8, type.size, _le);
    for (var i = 0; i < 4; ++i) {
      d.setFloat32(12 + 4 * i, type.tint[i], _le);
    }
    _registerCall('registerDiceType', 'tessera_register_dice_def', b, predicted,
        extraPtrs: [...buffers, facesPtr]);
    return predicted;
  }

  /// Register a TrueType/OpenType font from raw file bytes; the engine bakes
  /// ASCII + Latin-1 glyphs at [pixelHeight] texels into a GPU atlas. Returns
  /// its def id (0 = failure). Referenced by [TesseraLabel.font]; place labels
  /// via [setScene].
  int registerFont(Uint8List ttf, {double pixelHeight = 48}) {
    if (_disposed) return 0;
    final predicted = _predictDefId();
    final bytes = _wasm.allocBytes(ttf);
    _registerCall('registerFont', 'tessera_register_font',
        _bytesStruct(bytes, ttf.length), predicted,
        extraPtrs: [bytes], extraArgs: [pixelHeight]);
    return predicted;
  }

  /// Register a sound effect from WAV file bytes (PCM/float WAV — e.g. a
  /// Flutter asset loaded with `rootBundle.load`). Returns its sound id
  /// (0 = failure). Play it with [playSound]; typically triggered off the
  /// [events] stream so audio lands exactly on the engine's beats.
  int registerSound(Uint8List wav) {
    if (_disposed) return 0;
    final predicted = _predictSoundId();
    final bytes = _wasm.allocBytes(wav);
    final def = _wasm.allocBytes(_bytesStruct(bytes, wav.length));
    () async {
      try {
        final id =
            await _call('tessera_register_sound', 'number', [_engine, def])
                as int;
        _verifySoundId('registerSound', predicted, id);
      } finally {
        _wasm.free(def);
        _wasm.free(bytes);
      }
    }();
    return predicted;
  }

  /// (Re)start a registered sound at [gain] (1 = as authored; different sounds
  /// mix, re-triggering one restarts it). Safe while the render loop runs.
  /// Returns false when the controller is disposed. On the web the underlying
  /// call is fire-and-forget behind the serialized call chain, so an unknown
  /// [id] cannot be reported synchronously — the return value is optimistic.
  bool playSound(int id, {double gain = 1.0}) {
    if (_disposed) return false;
    _call('tessera_play_sound', 'number', [_engine, id, gain])
        .catchError((_) => null);
    return true;
  }

  /// Set the directional light + ambient.
  void setLight(TesseraLightData light) {
    if (_disposed) return;
    // TesseraLight: dir f32[3]@0, color f32[3]@12, intensity f32@24,
    // ambient f32[3]@28 — 40 bytes.
    final b = Uint8List(40);
    final d = ByteData.sublistView(b);
    for (var i = 0; i < 3; ++i) {
      d.setFloat32(0 + 4 * i, light.dir[i], _le);
      d.setFloat32(12 + 4 * i, light.color[i], _le);
      d.setFloat32(28 + 4 * i, light.ambient[i], _le);
    }
    d.setFloat32(24, light.intensity, _le);
    _structCall('tessera_set_light', b);
  }

  /// Set render quality (shadows, MSAA, resolution scale). Like io, this is
  /// any-thread on the engine side, so it may also be called live after
  /// [start] — e.g. toggling the shadow mode at runtime.
  void setQuality(TesseraQualityData quality) {
    if (_disposed) return;
    // TesseraQuality: shadows i32@0, msaa i32@4, render_scale f32@8 — 12 bytes.
    final b = Uint8List(12);
    final d = ByteData.sublistView(b);
    d.setInt32(0, quality.shadows.index, _le);
    d.setInt32(4, quality.msaa, _le);
    d.setFloat32(8, quality.renderScale, _le);
    _structCall('tessera_set_quality', b);
  }

  /// Set the transition timings.
  void setTiming(TesseraTimingData timing) {
    if (_disposed) return;
    // TesseraTiming: 7 f32 — 28 bytes.
    final b = Uint8List(28);
    final d = ByteData.sublistView(b);
    final v = [
      timing.moveS,
      timing.addS,
      timing.removeS,
      timing.tileS,
      timing.reflowS,
      timing.cameraS,
      timing.speedMultiplier,
    ];
    for (var i = 0; i < v.length; ++i) {
      d.setFloat32(4 * i, v[i], _le);
    }
    _structCall('tessera_set_timing', b);
  }

  /// Choose the camera projection.
  void setProjection(TesseraProjection projection) {
    if (_disposed) return;
    _call('tessera_set_projection', null, [_engine, projection.index])
        .catchError((_) => null);
  }

  /// Fire-and-forget `symbol(engine, &struct)` (struct copied by the engine).
  void _structCall(String symbol, Uint8List structBytes) {
    final ptr = _wasm.allocBytes(structBytes);
    () async {
      try {
        await _call(symbol, null, [_engine, ptr]);
      } finally {
        _wasm.free(ptr);
      }
    }();
  }

  /// Smallest orbit distance keeping every listed tile/entity id on screen
  /// with a fractional [padding] margin. Pure query; feed the result into your
  /// camera's `distance`.
  ///
  /// Web semantics: the engine can only answer asynchronously, so this returns
  /// the most recent value computed for the same arguments (null until one
  /// exists) and kicks off a refresh in the background. When the refreshed
  /// value differs, [onResize] is re-fired with the current logical size so
  /// callers re-query — the standard "call it during setup AND from onResize"
  /// pattern therefore converges to the correct fit within a frame or two.
  double? cameraFitDistance({
    List<int> tileIds = const [],
    List<int> entityIds = const [],
    double padding = 0.06,
  }) {
    if (_disposed) return null;
    final key = '${tileIds.join(',')}|${entityIds.join(',')}|$padding';
    _refreshFitDistance(key, List.of(tileIds), List.of(entityIds), padding);
    return _fitCache[key];
  }

  void _refreshFitDistance(
      String key, List<int> tileIds, List<int> entityIds, double padding) {
    if (_fitInFlight.contains(key)) return;
    _fitInFlight.add(key);
    () async {
      final tiles = _wasm.allocBytes(_u64Array(tileIds));
      final ents = _wasm.allocBytes(_u64Array(entityIds));
      final out = _wasm.allocBytes(Uint8List(4));
      try {
        final ok = await _call('tessera_camera_fit_distance', 'number', [
          _engine,
          tiles,
          tileIds.length,
          ents,
          entityIds.length,
          padding,
          out,
        ]) as int;
        if (ok != 0) {
          final v = _wasm.readData(out, 4).getFloat32(0, _le);
          final prev = _fitCache[key];
          _fitCache[key] = v;
          if (prev != v) _notifyFitChanged();
        }
      } finally {
        _wasm.free(tiles);
        _wasm.free(ents);
        _wasm.free(out);
        _fitInFlight.remove(key);
      }
    }();
  }

  void _notifyFitChanged() {
    if (_disposed) return;
    final cb = onResize;
    if (cb == null) return;
    final w = _canvas.clientWidth, h = _canvas.clientHeight;
    if (w > 0 && h > 0) cb(w.toDouble(), h.toDouble());
  }

  // ---- runtime -------------------------------------------------------------

  /// Push a scene snapshot and return a Future that completes once the
  /// transition it triggers has fully played out — every entity/card/dice/
  /// effect/camera animation settled and the engine reports [isIdle] again.
  /// Same contract as the io controller (see its docs); on the web the scene
  /// crosses the boundary as the engine's own serialized state blob.
  Future<void> setScene(TesseraScene scene) {
    if (_disposed) return Future.value();
    return _pushBlob(encodeSceneBlob(scene), _malformedScene);
  }

  static Never _malformedScene() =>
      throw ArgumentError('flutter_tessera: malformed scene blob');

  /// blob -> tessera_state_deserialize -> tessera_set_state -> completion.
  Future<void> _pushBlob(Uint8List blob, Never Function() onMalformed) {
    _busyScenes++;
    final done = () async {
      final ptr = _wasm.allocBytes(blob);
      int st = 0;
      try {
        st = await _call(
            'tessera_state_deserialize', 'number', [ptr, blob.length]) as int;
        if (st == 0) onMalformed();
        final op = await _call('tessera_set_state', 'number', [_engine, st])
            as int;
        await _awaitOperation(op);
      } finally {
        if (st != 0) {
          await _call('tessera_state_free', null, [st]).catchError((_) => null);
        }
        _wasm.free(ptr);
      }
    }();
    return done.whenComplete(() => _busyScenes--);
  }

  /// Imperatively retarget the camera WITHOUT pushing a new scene: the camera
  /// SNAPS straight to the goal resolved from [camera] — no tween — and the
  /// scene itself is untouched. Applied on the next tick. The goal holds until
  /// the next [setCamera] or [setScene]. No-op after [dispose].
  void setCamera(TesseraCamera camera) {
    if (_disposed) return;
    _structCall('tessera_set_camera', _cameraStruct(camera));
  }

  /// Marshal a high-level [TesseraCamera] case into the in-memory
  /// TesseraCamera struct (96 bytes, both ABIs): mode u32@0, focus f32x2@4,
  /// distance@12, yaw@16, pitch@20, fov@24, position[3]@28, orientation[4]@40,
  /// target[3]@56, (pad), target_id u64@72, focus_card_id u64@80,
  /// fit_padding f32@88. Field mapping mirrors the io `_fillCamera`.
  static Uint8List _cameraStruct(TesseraCamera camera) {
    final b = Uint8List(96);
    final d = ByteData.sublistView(b);
    void pose(int mode, int targetId, double distance, double yaw, double pitch,
        double fov) {
      d.setUint32(0, mode, _le);
      _setU64(d, 72, targetId);
      d.setFloat32(12, distance, _le);
      d.setFloat32(16, yaw, _le);
      d.setFloat32(20, pitch, _le);
      d.setFloat32(24, fov, _le);
    }

    switch (camera) {
      case TesseraCameraPose c:
        pose(0, 0, c.distance, c.yaw, c.pitch, c.fov);
        d.setFloat32(4, c.focusX, _le);
        d.setFloat32(8, c.focusY, _le);
      case TesseraCameraManual c:
        d.setUint32(0, 1, _le);
        d.setFloat32(24, c.fov, _le);
        for (var k = 0; k < 3; ++k) {
          d.setFloat32(28 + 4 * k, c.position[k], _le);
        }
        for (var k = 0; k < 4; ++k) {
          d.setFloat32(40 + 4 * k, c.orientation[k], _le);
        }
      case TesseraCameraTarget c:
        d.setUint32(0, 2, _le);
        d.setFloat32(24, c.fov, _le);
        for (var k = 0; k < 3; ++k) {
          d.setFloat32(28 + 4 * k, c.source[k], _le);
          d.setFloat32(56 + 4 * k, c.target[k], _le);
        }
      case TesseraCameraFocusTile c:
        pose(3, c.tileId, c.distance, c.yaw, c.pitch, c.fov);
      case TesseraCameraFocusEntity c:
        pose(4, c.entityId, c.distance, c.yaw, c.pitch, c.fov);
      case TesseraCameraFocusDice c:
        pose(5, c.diceId, c.distance, c.yaw, c.pitch, c.fov);
      case TesseraCameraFocusDraw c:
        pose(6, c.drawId, c.distance, c.yaw, c.pitch, c.fov);
      case TesseraCameraFocusCard c:
        d.setUint32(0, 7, _le);
        _setU64(d, 72, c.cardId);
        d.setFloat32(88, c.padding, _le);
        d.setFloat32(24, c.fov, _le);
      case TesseraCameraFocusHand c:
        d.setUint32(0, 8, _le);
        _setU64(d, 72, c.handId);
        _setU64(d, 80, c.cardId ?? 0);
        d.setFloat32(88, c.padding, _le);
        d.setFloat32(24, c.fov, _le);
    }
    return b;
  }

  // ---- state serialization, save / undo & replay ---------------------------

  /// Serialize [scene] into a self-contained, versioned binary blob (identical
  /// on every platform — byte-for-byte the same as the io path's output) — the
  /// building block for save games, undo stacks and replays. Pure data.
  Uint8List serializeScene(TesseraScene scene) => encodeSceneBlob(scene);

  /// Reconstruct the scene stored in [blob] (bytes produced by
  /// [serializeScene], or one record of a replay) and push it to the engine,
  /// returning the same completion Future as [setScene]. Throws
  /// [ArgumentError] on a malformed blob. A no-op (already-completed Future)
  /// after [dispose], like [setScene].
  Future<void> restoreScene(Uint8List blob) {
    if (_disposed) return Future.value();
    if (!sceneBlobLooksValid(blob)) _malformedScene();
    return _pushBlob(blob, _malformedScene);
  }

  /// New empty replay recorder. Append scenes as the game plays, then
  /// [TesseraReplayRecorder.serialize] the whole container to bytes. Dispose
  /// it when done recording.
  TesseraReplayRecorder createReplayRecorder() => TesseraReplayRecorder._();

  /// Open a serialized replay container ([TesseraReplayRecorder.serialize]
  /// bytes) for playback through this controller. Throws [ArgumentError] on
  /// malformed input. Dispose the player when done.
  TesseraReplayPlayer openReplay(Uint8List data) {
    final recs = decodeReplayContainer(data);
    if (recs == null) {
      throw ArgumentError('flutter_tessera: malformed replay data');
    }
    return TesseraReplayPlayer._(this, recs);
  }

  /// A Future that completes when operation [op]'s transition has fully
  /// settled, driven by polling `tessera_last_completed_operation` after each
  /// tick. Resolves synchronously if the operation is already done (or the
  /// controller is disposed).
  Future<void> _awaitOperation(int op) {
    if (op == 0 || _disposed || op <= _lastCompletedOp) {
      return Future.value();
    }
    final completer = Completer<void>();
    (_opWaiters[op] ??= <Completer<void>>[]).add(completer);
    return completer.future;
  }

  /// True when no scene transition issued through this controller is still
  /// animating. (The web approximation of the engine's `tessera_is_idle`,
  /// tracked through operation completion — equivalent for state-driven
  /// hosts, which is the only way this API mutates the scene.)
  bool get isIdle => _busyScenes == 0;

  /// Start the render loop (requestAnimationFrame-driven `tessera_tick`).
  /// Call once, after setup + the first scene. Resolves after the first
  /// successful tick.
  Future<void> start() async {
    if (_started || _disposed) return;
    _started = true;
    _scheduleFrame();
    await _firstTick.future;
  }

  late final JSFunction _frameCallback = _onFrame.toJS;

  void _scheduleFrame() {
    if (_disposed || !_started) return;
    _rafId = web.window.requestAnimationFrame(_frameCallback);
  }

  void _onFrame(double nowMs) {
    if (_disposed || !_started) return;
    _scheduleFrame(); // next frame; if this tick is still pending, it skips
    if (_ticking) return;
    _ticking = true;
    _tick(nowMs).catchError((Object e) {
      debugPrint('flutter_tessera: tick failed: $e');
    }).whenComplete(() => _ticking = false);
  }

  Future<void> _tick(double nowMs) async {
    final dt = _lastFrameMs < 0
        ? 1 / 60.0
        : math.min(0.1, (nowMs - _lastFrameMs) / 1000.0);
    _lastFrameMs = nowMs;
    if (_disposed) return;
    await _call('tessera_tick', null, [_engine, dt]);
    if (_disposed) return;
    await _drainEvents();
    if (_disposed) return;
    await _checkOperations();
    if (_disposed) return;
    await _checkSize();
    if ((_tickCount++ % 120) == 0 && !_disposed) {
      _eventsDropped =
          await _call('tessera_events_dropped', 'number', [_engine]) as int;
    }
    if (!_firstTick.isCompleted) _firstTick.complete();
  }

  Future<void> _drainEvents() async {
    while (!_disposed) {
      final n = await _call(
          'tessera_poll_events', 'number', [_engine, _evBuf, _evCap]) as int;
      if (n <= 0) return;
      final d = _wasm.readData(_evBuf, n * _evSize);
      for (var i = 0; i < n; ++i) {
        final off = i * _evSize;
        // TesseraEvent: time f64@0, subject_id u64@8, type u32@16,
        // subject u32@20, coord i32@24/i32@28, value f32@32, reserved@36.
        final type = d.getUint32(off + 16, _le);
        final subject = d.getUint32(off + 20, _le);
        if (!_events.isClosed) {
          _events.add(TesseraEvent(
            type: type < TesseraEventType.values.length
                ? TesseraEventType.values[type]
                : TesseraEventType.none,
            subject: subject < TesseraEventSubject.values.length
                ? TesseraEventSubject.values[subject]
                : TesseraEventSubject.none,
            subjectId: _getU64(d, off + 8),
            time: d.getFloat64(off, _le),
            x: d.getInt32(off + 24, _le),
            y: d.getInt32(off + 28, _le),
            value: d.getFloat32(off + 32, _le),
          ));
        }
      }
      if (n < _evCap) return;
    }
  }

  Future<void> _checkOperations() async {
    if (_opWaiters.isEmpty && _busyScenes == 0) return;
    final last = await _call(
        'tessera_last_completed_operation', 'number', [_engine]) as int;
    if (last > _lastCompletedOp) _lastCompletedOp = last;
    _opWaiters.removeWhere((waited, completers) {
      if (waited > _lastCompletedOp) return false;
      for (final c in completers) {
        if (!c.isCompleted) c.complete();
      }
      return true;
    });
  }

  Future<void> _checkSize() async {
    final dpr = web.window.devicePixelRatio;
    final cw = _canvas.clientWidth, ch = _canvas.clientHeight;
    if (cw <= 0 || ch <= 0) return;
    final pw = (cw * dpr).round(), ph = (ch * dpr).round();
    final first = !_resized;
    if (!first && pw == _physW && ph == _physH) return;
    _physW = pw;
    _physH = ph;
    _resized = true;
    // The engine has no SDL event pump here (mirrors the macOS embedder):
    // drive the drawable ourselves — canvas backing store + engine resize.
    _canvas.width = pw;
    _canvas.height = ph;
    await _call('tessera_resize', null, [_engine, pw, ph, dpr]);
    // Notify AFTER the tick + resize so pending scenes have been promoted —
    // cameraFitDistance fits to live tiles (same ordering as the io hosts).
    // Fired on first layout too, like the native views.
    onResize?.call(cw.toDouble(), ch.toDouble());
  }

  bool _resized = false;

  /// Ray-pick the tile/entity under a logical view pixel ([x],[y] — top-left
  /// origin, the same space as Flutter pointer events). Serialized with the
  /// render loop through the module call chain.
  Future<TesseraPickResult?> pick(double x, double y) async {
    if (_disposed) return null;
    // TesseraPick, wasm32: hit_tile u8@0, tile coord i32@4/i32@8, tile_distance
    // f32@12, hit_entity u8@16, entity u64@24, entity_distance f32@32,
    // ray_origin f32[3]@36, ray_dir f32[3]@48, point f32[3]@60, hit_dice u8@72,
    // dice u64@80, dice_distance f32@88, hit_card u8@92, card u64@96,
    // card_distance f32@104 — 112 bytes.
    final out = _wasm.allocBytes(Uint8List(112));
    try {
      await _call('tessera_pick', 'number', [_engine, x, y, out]);
      if (_disposed) return null;
      final d = _wasm.readData(out, 112);
      return TesseraPickResult(
        hitTile: d.getUint8(0) != 0,
        tileX: d.getInt32(4, _le),
        tileY: d.getInt32(8, _le),
        tileDistance: d.getFloat32(12, _le),
        hitEntity: d.getUint8(16) != 0,
        entity: _getU64(d, 24),
        entityDistance: d.getFloat32(32, _le),
        hitDice: d.getUint8(72) != 0,
        dice: _getU64(d, 80),
        diceDistance: d.getFloat32(88, _le),
        hitCard: d.getUint8(92) != 0,
        card: _getU64(d, 96),
        cardDistance: d.getFloat32(104, _le),
      );
    } finally {
      _wasm.free(out);
    }
  }

  /// Release the controller: stops the render loop, destroys the engine and
  /// frees the wasm-side buffers. The canvas element itself is owned by the
  /// platform view and torn down with the widget.
  Future<void> dispose() async {
    if (_disposed) return;
    _disposed = true;
    onResize = null;
    final raf = _rafId;
    if (raf != null) web.window.cancelAnimationFrame(raf);
    if (!_firstTick.isCompleted) _firstTick.complete();
    _events.close();
    for (final completers in _opWaiters.values) {
      for (final c in completers) {
        if (!c.isCompleted) c.complete();
      }
    }
    _opWaiters.clear();
    _busyScenes = 0;
    if (identical(_live, this)) _live = null;
    // Queued behind any in-flight tick on the serialized chain, so the engine
    // is never destroyed mid-call.
    try {
      await _call('tessera_destroy', null, [_engine]);
    } finally {
      _wasm.free(_evBuf);
    }
  }
}

/// Records a timestamped sequence of scenes into a replay container — the web
/// counterpart of the io recorder, holding the records in Dart (each scene is
/// serialized immediately to the engine's blob format; pure data, no engine
/// state involved). [serialize] flattens to the same `tessera_replay_*`
/// container bytes as every other platform.
class TesseraReplayRecorder {
  TesseraReplayRecorder._();

  List<ReplayRecord>? _records = <ReplayRecord>[];

  /// Number of recorded scenes (0 after [dispose]).
  int get length => _records?.length ?? 0;

  /// Append [scene] with a host-defined [timestampMs] (e.g. milliseconds since
  /// recording started; playback order is append order regardless). Returns
  /// false after [dispose].
  bool add(TesseraScene scene, {int timestampMs = 0}) {
    final recs = _records;
    if (recs == null) return false;
    recs.add((timestampMs, encodeSceneBlob(scene)));
    return true;
  }

  /// Flatten the container to a self-contained byte blob (empty after
  /// [dispose]).
  Uint8List serialize() {
    final recs = _records;
    return recs == null ? Uint8List(0) : encodeReplayContainer(recs);
  }

  /// Release the recorder. Safe to call more than once.
  void dispose() => _records = null;
}

/// Plays back a recorded replay through its controller. Obtain one from
/// [TesseraController.openReplay]; [play] pushes record [index]'s scene to the
/// engine (the engine animates the diff from whatever is currently shown, so
/// stepping records in order replays the game's beats). [dispose] when done.
class TesseraReplayPlayer {
  TesseraReplayPlayer._(this._controller, this._records);

  final TesseraController _controller;
  List<ReplayRecord>? _records;

  /// Number of records (0 after [dispose]).
  int get length => _records?.length ?? 0;

  /// The recorded timestamp (ms, as passed to [TesseraReplayRecorder.add]) of
  /// record [index], or null when out of range / after [dispose].
  int? timestampMs(int index) {
    final recs = _records;
    if (recs == null || index < 0 || index >= recs.length) return null;
    return recs[index].$1;
  }

  /// Push record [index]'s scene to the engine, returning the same completion
  /// Future as [TesseraController.setScene]. Throws [RangeError] on a bad
  /// index (or a corrupt record) and [StateError] after [dispose]. A no-op
  /// (already-completed Future) once the *controller* has been disposed.
  Future<void> play(int index) {
    final recs = _records;
    if (recs == null) {
      throw StateError('flutter_tessera: replay player disposed');
    }
    if (_controller._disposed) return Future.value();
    if (index < 0 || index >= recs.length) {
      throw RangeError('flutter_tessera: no replay record $index');
    }
    final blob = recs[index].$2;
    if (!sceneBlobLooksValid(blob)) {
      throw RangeError('flutter_tessera: no replay record $index');
    }
    return _controller._pushBlob(
        blob,
        () =>
            throw RangeError('flutter_tessera: no replay record $index'));
  }

  /// Release the records. Safe to call more than once.
  void dispose() => _records = null;
}
