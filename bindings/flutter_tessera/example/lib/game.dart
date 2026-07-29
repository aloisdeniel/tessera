// game.dart — a tiny reducer-style framework shared by every example game.
//
// Each game is a `GameController<S, A>`:
//
//     S              a sealed game-state (phases carry their data)
//     A              a sealed user/auto action
//     update(S, A)   the pure reducer  →  new S
//     render(S)      the new S projected to one *or a sequence of* TesseraScenes
//
// `GameScreen` hosts one controller: it owns the current state, drives a scene
// queue (playing multi-frame renders one step at a time, gated on the renderer
// going idle), routes taps and buttons to actions, and optionally auto-plays
// (AI / dealer / auto-advance) via `autoAction`.

import 'dart:async';
import 'dart:io';
import 'dart:math' as math;
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart' show rootBundle;
import 'package:flutter_tessera/flutter_tessera.dart';

import 'view_mode.dart';

/// The shared shadow mode applied to every game's engine. `GameScreen` merges
/// it over each game's own [GameController.quality] (which keeps its msaa /
/// renderScale) and re-applies it live when the app-bar toggle flips it.
/// Defaults to real shadow mapping so the feature is visible everywhere; the
/// toggle drops back to the cheap blob decals for comparison.
final ValueNotifier<TesseraShadowMode> sharedShadowMode =
    ValueNotifier<TesseraShadowMode>(TesseraShadowMode.map);

Uint8List? _gameFontBytes; // loaded once, shared by every game's engine

/// Register the example's shared label font on [c] and return its font def id
/// for [TesseraLabel.font] (0 when no font could be loaded — the engine then
/// simply skips those labels). Each `TesseraView` owns its own engine, so call
/// this once per controller during `registerDefs`:
///
///     _font = await registerGameFont(c);
///
/// Loads the bundled Roboto (Apache-2.0, `assets/fonts/`) and falls back to a
/// macOS system TTF if the asset is somehow unavailable. [pixelHeight] is the
/// baked glyph height in texels — the default suits typical label sizes.
Future<int> registerGameFont(TesseraController c,
    {double pixelHeight = 64}) async {
  var bytes = _gameFontBytes;
  if (bytes == null) {
    try {
      final data = await rootBundle.load('assets/fonts/Roboto-Regular.ttf');
      bytes = data.buffer.asUint8List(data.offsetInBytes, data.lengthInBytes);
    } catch (_) {
      for (final path in const [
        '/System/Library/Fonts/Supplemental/Arial.ttf',
        '/Library/Fonts/Arial.ttf',
      ]) {
        final f = File(path);
        if (f.existsSync()) {
          bytes = f.readAsBytesSync();
          break;
        }
      }
    }
    if (bytes == null) return 0;
    _gameFontBytes = bytes;
  }
  return c.registerFont(bytes, pixelHeight: pixelHeight);
}

/// A game the menu can launch and `GameScreen` can drive.
abstract class GameController<S, A> {
  String get title;
  String get subtitle => '';
  IconData get icon => Icons.videogame_asset;

  /// One-time GPU setup (register defs); may be async (e.g. generate textures).
  Future<void> registerDefs(TesseraController c);

  TesseraLightData get light => const TesseraLightData();
  TesseraQualityData get quality =>
      const TesseraQualityData(shadows: TesseraShadowMode.blob);
  TesseraTimingData get timing => const TesseraTimingData();

  /// The initial game state.
  S initial();

  /// The pure reducer: `(state, action) → state`.
  S update(S state, A action);

  /// Project [state] to one — or a sequence of — visual scenes. The host plays
  /// them in order, waiting for the renderer to go idle between frames, so a
  /// multi-element result animates step by step (e.g. a dealer revealing cards).
  /// Returning an `Iterable` lets a game express the sequence as a `sync*`
  /// generator, yielding one beat at a time straight from the state change
  /// (see Duel's lunge-then-settle attack); returning a plain `List` is fine.
  Iterable<TesseraScene> render(S state);

  /// An *intrinsic* step that always advances [state] when the host goes idle,
  /// regardless of the auto-play toggle — for animations the game drives itself
  /// (dealing cards one at a time, a countdown, …). null = nothing to advance.
  A? autoAdvance(S state) => null;

  /// An action the auto-player takes on the human's behalf — only used while
  /// auto-play is on (AI move, dealer strategy, auto new round). null = wait.
  A? autoAction(S state, math.Random rng) => null;

  /// How long to *dwell* on [state] once its animation settles before firing an
  /// intrinsic [autoAdvance] — a readable pause so the player can take in the
  /// beat (a flipped pair in memory, a revealed card) before it resolves. The
  /// host has no motion during this hold; it just delays the next auto step.
  /// Defaults to no pause. Only applies to intrinsic auto-advance, never to a
  /// human action or (auto-play) AI move.
  Duration holdFor(S state) => Duration.zero;

  /// Map a board tap to an action, or null to ignore. [pick] is null on a miss.
  /// [local] is the tap's position in the view and [view] the view's size, for
  /// games that care where the tap landed (e.g. left/right edge taps); games
  /// that don't just ignore the extras.
  A? onTap(S state, TesseraPickResult? pick, {Offset? local, Size? view}) => null;

  /// On-screen buttons offered for [state] (disabled while animating).
  List<GameButton<A>> buttons(S state) => const [];

  /// The status line for [state].
  String status(S state);

  /// Called on view (re)size; return scenes to push (e.g. a re-fitted camera),
  /// or null to leave the framing unchanged (fixed-camera games).
  List<TesseraScene>? onResize(TesseraController c, S state, double w, double h) =>
      null;
}

enum GameButtonTone { normal, primary, danger }

/// A labelled button that dispatches [action].
class GameButton<A> {
  const GameButton(this.label, this.action,
      {this.icon, this.tone = GameButtonTone.normal});
  final String label;
  final A action;
  final IconData? icon;
  final GameButtonTone tone;
}

/// Hosts a single [GameController], wiring its reducer to a live [TesseraView].
class GameScreen<S, A> extends StatefulWidget {
  const GameScreen({super.key, required this.game});
  final GameController<S, A> game;

  @override
  State<GameScreen<S, A>> createState() => _GameScreenState<S, A>();
}

class _GameScreenState<S, A> extends State<GameScreen<S, A>> {
  TesseraController? _controller;
  Timer? _loop;
  final _queue = <TesseraScene>[];
  final _rng = math.Random(0x51E5A);
  final _viewMode = ViewModeController(); // shared drag-to-orbit control
  TesseraScene? _lastScene; // the last scene pushed (view mode's home)
  late S _state;
  bool _autoplay = false;
  bool _ready = false;
  bool _busy = true;
  bool _playing = false; // a queued scene is mid-flight (awaiting setScene)
  Timer? _hold; // dwelling on a settled state before its intrinsic auto-advance
  Object? _dwelled; // the state instance whose hold has already elapsed
  Size? _viewSize; // latest view size, for taps that care where they landed

  GameController<S, A> get game => widget.game;

  @override
  void initState() {
    super.initState();
    _state = game.initial();
    sharedShadowMode.addListener(_onShadowModeChanged);
    _viewMode.addListener(_onViewModeChanged);
  }

  @override
  void dispose() {
    sharedShadowMode.removeListener(_onShadowModeChanged);
    _viewMode.dispose();
    _loop?.cancel();
    _hold?.cancel();
    _controller?.dispose();
    super.dispose();
  }

  void _onViewModeChanged() {
    if (mounted) setState(() {});
  }

  /// The game's quality knobs with the shared shadow mode merged over them.
  TesseraQualityData get _effectiveQuality {
    final q = game.quality;
    return TesseraQualityData(
      shadows: sharedShadowMode.value,
      msaa: q.msaa,
      renderScale: q.renderScale,
    );
  }

  void _onShadowModeChanged() {
    // Live re-apply while the render loop runs: tessera_set_quality is
    // any-thread (mutex-guarded engine-side; the renderer takes one consistent
    // copy per frame), so flipping the shadow mode mid-animation is safe.
    _controller?.setQuality(_effectiveQuality);
    if (mounted) setState(() {});
  }

  Future<void> _onCreated(TesseraController c) async {
    _controller = c;
    await game.registerDefs(c);
    c.setLight(game.light);
    c.setQuality(_effectiveQuality);
    c.setTiming(game.timing);

    c.onResize = (w, h) {
      final scenes = game.onResize(c, _state, w, h);
      if (scenes != null && scenes.isNotEmpty) {
        _queue
          ..clear()
          ..addAll(scenes);
        _pump();
      }
    };

    _queue.addAll(game.render(_state));
    _pump();
    await c.start();
    _loop = Timer.periodic(const Duration(milliseconds: 45), (_) => _pump());
    if (mounted) setState(() => _ready = true);
  }

  bool get _animating =>
      _controller == null ||
      _playing ||
      _hold != null ||
      _viewMode.active ||
      !_controller!.isIdle ||
      _queue.isNotEmpty;

  /// Drive one step. A queued scene is played through
  /// [TesseraController.setScene], whose Future resolves only once the
  /// transition it triggers has fully animated — so a multi-scene render (e.g.
  /// throw the die, *then* move the hero) plays one beat at a time, each
  /// awaiting the previous. When the queue drains and the renderer is idle, an
  /// intrinsic/auto step may enqueue the next batch.
  void _pump() {
    final c = _controller;
    if (c == null || _playing) return;
    if (_viewMode.active) {
      // The game is frozen while the player looks around; anything queued
      // (e.g. an onResize refit) plays once view mode is left.
      _syncBusy();
      return;
    }
    if (_queue.isNotEmpty) {
      _playNext(c);
      return;
    }
    if (c.isIdle) {
      // Intrinsic steps always run; AI decisions only while auto-play is on.
      var a = game.autoAdvance(_state);
      final intrinsic = a != null;
      a ??= _autoplay ? game.autoAction(_state, _rng) : null;
      if (a != null) {
        // Dwell on an intrinsic beat (a settled reveal) before resolving it, so
        // the player can read it. Held once per state instance; the timer's
        // firing marks this state dwelled and re-pumps to fall through below.
        if (intrinsic && !identical(_dwelled, _state)) {
          final hold = game.holdFor(_state);
          if (hold > Duration.zero) {
            if (_hold == null) {
              _hold = Timer(hold, () {
                _hold = null;
                _dwelled = _state;
                _pump();
              });
              _syncBusy();
            }
            return;
          }
        }
        final before = _state;
        _state = game.update(_state, a);
        _queue.addAll(game.render(_state));
        if (mounted) setState(() {}); // refresh status/buttons for the new state
        if (_queue.isNotEmpty) {
          _playNext(c);
          return;
        }
        // The step produced no scene to play (e.g. a purely logical advance):
        // if it actually advanced the state, re-pump on the next microtask so
        // the following beat isn't stalled up to a full poll interval. Guard on
        // a real change so a no-op action can't spin the microtask queue.
        if (!identical(before, _state)) {
          scheduleMicrotask(_pump);
          return;
        }
      }
    }
    _syncBusy();
  }

  /// Push the next queued scene and await its animation before pumping again.
  Future<void> _playNext(TesseraController c) async {
    _playing = true;
    _syncBusy();
    final scene = _queue.removeAt(0);
    _lastScene = scene; // view mode's home: the game's latest scene + camera
    await c.setScene(scene); // resolves when the transition is idle
    if (!mounted) return;
    _playing = false;
    _syncBusy();
    _pump();
  }

  void _syncBusy() {
    final busy = _animating;
    if (busy != _busy) {
      _busy = busy;
      if (mounted) setState(() {});
    }
  }

  void _dispatch(A action) {
    _viewMode.exit(); // any game action leaves view mode first
    _hold?.cancel();
    _hold = null;
    _state = game.update(_state, action);
    _queue.addAll(game.render(_state));
    if (mounted) setState(() {});
    _pump();
  }

  Future<void> _onTapDown(TapDownDetails d) async {
    final c = _controller;
    if (_viewMode.active) {
      // Any other interaction leaves view mode: the tap just brings the
      // camera home, it never reaches the game.
      _viewMode.exit();
      _pump();
      return;
    }
    if (c == null || _animating) return;
    final pick = await c.pick(d.localPosition.dx, d.localPosition.dy);
    final a = game.onTap(_state, pick, local: d.localPosition, view: _viewSize);
    if (a != null) _dispatch(a);
  }

  @override
  Widget build(BuildContext context) {
    final buttons = _ready ? game.buttons(_state) : const [];
    final interactive = _ready && !_animating;
    return Scaffold(
      appBar: AppBar(
        title: Text(game.title),
        actions: [
          IconButton(
            tooltip: _viewMode.active
                ? 'Exit view mode (the camera returns home)'
                : 'View mode — drag to orbit the board, pinch to zoom',
            icon: Icon(Icons.threed_rotation,
                color: _viewMode.active ? Colors.amberAccent : null),
            onPressed: !_ready || (!_viewMode.active && _animating)
                ? null
                : () {
                    final c = _controller;
                    final scene = _lastScene;
                    if (_viewMode.active) {
                      _viewMode.exit();
                      _pump();
                    } else if (c != null && scene != null) {
                      _viewMode.enter(tessera: c, scene: scene);
                    }
                  },
          ),
          IconButton(
            tooltip: sharedShadowMode.value == TesseraShadowMode.map
                ? 'Shadow-mapped (tap for blob shadows)'
                : 'Blob shadows (tap for shadow mapping)',
            icon: Icon(sharedShadowMode.value == TesseraShadowMode.map
                ? Icons.wb_sunny
                : Icons.wb_sunny_outlined),
            onPressed: !_ready
                ? null
                : () => sharedShadowMode.value =
                    sharedShadowMode.value == TesseraShadowMode.map
                        ? TesseraShadowMode.blob
                        : TesseraShadowMode.map,
          ),
          IconButton(
            tooltip: _autoplay ? 'Pause auto-play' : 'Auto-play',
            icon: Icon(_autoplay ? Icons.pause : Icons.smart_toy_outlined),
            onPressed: !_ready
                ? null
                : () {
                    setState(() => _autoplay = !_autoplay);
                    _pump();
                  },
          ),
        ],
      ),
      // The Tessera view fills the whole body; the status/buttons panel is
      // stacked on top of it (bottom-aligned). Keeping the view a fixed size —
      // rather than an Expanded sibling above a variable-height panel — means it
      // never resizes as buttons/status appear or grow, so the renderer isn't
      // churning through onResize/camera re-fits on every state change.
      body: Stack(
        children: [
          Positioned.fill(
            child: LayoutBuilder(
              builder: (context, constraints) {
                _viewSize = constraints.biggest;
                return GestureDetector(
                  onTapDown: _onTapDown,
                  // View-mode free look. A scale gesture subsumes a drag, so
                  // one recognizer covers both: the focal point's movement
                  // orbits the camera, the pinch factor zooms it. Inert
                  // while view mode is off.
                  onScaleStart: (_) => _viewMode.beginGesture(),
                  onScaleUpdate: (d) =>
                      _viewMode.gesture(d.focalPointDelta, d.scale),
                  child: TesseraView(onCreated: _onCreated),
                );
              },
            ),
          ),
          Positioned(
            left: 0,
            right: 0,
            bottom: 0,
            child: Container(
              width: double.infinity,
              // A soft top-to-bottom scrim keeps the overlaid text/buttons
              // legible against whatever the board renders behind them.
              decoration: BoxDecoration(
                gradient: LinearGradient(
                  begin: Alignment.topCenter,
                  end: Alignment.bottomCenter,
                  colors: [
                    Colors.black.withValues(alpha: 0.0),
                    Colors.black.withValues(alpha: 0.55),
                  ],
                ),
              ),
              padding: const EdgeInsets.fromLTRB(16, 24, 16, 16),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                mainAxisSize: MainAxisSize.min,
                children: [
                  Text(
                    !_ready
                        ? 'starting…'
                        : _viewMode.active
                            ? 'View mode — drag to orbit, pinch to zoom · '
                                'tap the board to return'
                            : game.status(_state),
                    style: const TextStyle(fontSize: 13),
                  ),
                  if (buttons.isNotEmpty) ...[
                    const SizedBox(height: 8),
                    ConstrainedBox(
                      constraints: const BoxConstraints(maxHeight: 168),
                      child: SingleChildScrollView(
                        child: Wrap(
                          spacing: 8,
                          runSpacing: 8,
                          children: [
                            for (final b in buttons.cast<GameButton<A>>())
                              _buildButton(b, interactive),
                          ],
                        ),
                      ),
                    ),
                  ],
                ],
              ),
            ),
          ),
        ],
      ),
    );
  }

  Widget _buildButton(GameButton<A> b, bool interactive) {
    final onPressed = interactive ? () => _dispatch(b.action) : null;
    final child = b.icon != null
        ? Row(mainAxisSize: MainAxisSize.min, children: [
            Icon(b.icon, size: 18),
            const SizedBox(width: 6),
            Text(b.label),
          ])
        : Text(b.label);
    switch (b.tone) {
      case GameButtonTone.primary:
        return FilledButton(onPressed: onPressed, child: child);
      case GameButtonTone.danger:
        return OutlinedButton(
          onPressed: onPressed,
          style: OutlinedButton.styleFrom(foregroundColor: Colors.redAccent),
          child: child,
        );
      case GameButtonTone.normal:
        return OutlinedButton(onPressed: onPressed, child: child);
    }
  }
}
