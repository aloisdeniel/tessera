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
import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter_tessera/flutter_tessera.dart';

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
  List<TesseraScene> render(S state);

  /// An *intrinsic* step that always advances [state] when the host goes idle,
  /// regardless of the auto-play toggle — for animations the game drives itself
  /// (dealing cards one at a time, a countdown, …). null = nothing to advance.
  A? autoAdvance(S state) => null;

  /// An action the auto-player takes on the human's behalf — only used while
  /// auto-play is on (AI move, dealer strategy, auto new round). null = wait.
  A? autoAction(S state, math.Random rng) => null;

  /// Map a board tap to an action, or null to ignore. [pick] is null on a miss.
  A? onTap(S state, TesseraPickResult? pick) => null;

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
  late S _state;
  bool _autoplay = false;
  bool _ready = false;
  bool _busy = true;

  GameController<S, A> get game => widget.game;

  @override
  void initState() {
    super.initState();
    _state = game.initial();
  }

  @override
  void dispose() {
    _loop?.cancel();
    _controller?.dispose();
    super.dispose();
  }

  Future<void> _onCreated(TesseraController c) async {
    _controller = c;
    await game.registerDefs(c);
    c.setLight(game.light);
    c.setQuality(game.quality);
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
    _loop = Timer.periodic(const Duration(milliseconds: 90), (_) => _pump());
    if (mounted) setState(() => _ready = true);
  }

  bool get _animating =>
      _controller == null || !_controller!.isIdle || _queue.isNotEmpty;

  /// Drive one step: play the next queued scene, or (idle + empty) auto-advance.
  void _pump() {
    final c = _controller;
    if (c == null) return;
    var changed = false;
    if (c.isIdle) {
      if (_queue.isNotEmpty) {
        c.setScene(_queue.removeAt(0));
        changed = true;
      } else {
        // Intrinsic steps always run; AI decisions only while auto-play is on.
        var a = game.autoAdvance(_state);
        a ??= _autoplay ? game.autoAction(_state, _rng) : null;
        if (a != null) {
          _state = game.update(_state, a);
          _queue.addAll(game.render(_state));
          if (_queue.isNotEmpty) c.setScene(_queue.removeAt(0));
          changed = true;
        }
      }
    }
    final busy = _animating;
    if (changed || busy != _busy) {
      _busy = busy;
      if (mounted) setState(() {});
    }
  }

  void _dispatch(A action) {
    _state = game.update(_state, action);
    _queue.addAll(game.render(_state));
    if (mounted) setState(() {});
    _pump();
  }

  Future<void> _onTapDown(TapDownDetails d) async {
    final c = _controller;
    if (c == null || _animating) return;
    final pick = await c.pick(d.localPosition.dx, d.localPosition.dy);
    final a = game.onTap(_state, pick);
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
      body: Column(
        children: [
          Expanded(
            child: GestureDetector(
              onTapDown: _onTapDown,
              child: TesseraView(onCreated: _onCreated),
            ),
          ),
          Container(
            width: double.infinity,
            color: Colors.black.withValues(alpha: 0.35),
            padding: const EdgeInsets.fromLTRB(16, 10, 16, 10),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              mainAxisSize: MainAxisSize.min,
              children: [
                Text(
                  _ready ? game.status(_state) : 'starting…',
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
