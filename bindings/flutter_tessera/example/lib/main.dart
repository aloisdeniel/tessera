// A full game of chess rendered with Tessera, embedded in Flutter.
//
// The board is a live `TesseraView`. A heuristic AI plays both sides on a timer
// when autoplay is on; you can also tap a piece then a destination to move it
// (ray-picked through `TesseraController.pick`). This is the Flutter port of
// examples/chess.

import 'dart:async';

import 'package:flutter/material.dart' hide Action;
import 'package:flutter_tessera/flutter_tessera.dart';

import 'chess_game.dart';
import 'chess_scene.dart';

void main() => runApp(const ChessApp());

class ChessApp extends StatelessWidget {
  const ChessApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Tessera Chess',
      debugShowCheckedModeBanner: false,
      theme: ThemeData.dark(useMaterial3: true),
      home: const ChessPage(),
    );
  }
}

class ChessPage extends StatefulWidget {
  const ChessPage({super.key});

  @override
  State<ChessPage> createState() => _ChessPageState();
}

class _ChessPageState extends State<ChessPage> {
  final _game = Game();
  final _scene = ChessScene();
  final _rng = ChessRng(0x1234abcd);

  TesseraController? _controller;
  Timer? _loop;
  bool _autoplay = true;
  int _selected = -1; // selected source square, or -1
  String _status = 'starting…';

  @override
  void dispose() {
    _loop?.cancel();
    _controller?.dispose();
    super.dispose();
  }

  Future<void> _onCreated(TesseraController c) async {
    _controller = c;

    // Setup (runs while the native render loop is paused — safe on this thread).
    _scene.registerDefs(c);
    c.setLight(const TesseraLightData(
      dir: [-0.4, -1.0, -0.5],
      color: [1.0, 0.97, 0.9],
      intensity: 1.15,
      ambient: [0.30, 0.33, 0.4],
    ));
    c.setQuality(const TesseraQualityData(shadows: TesseraShadowMode.blob));
    c.setTiming(const TesseraTimingData(
      moveS: 0.5,
      addS: 0.35,
      removeS: 0.4,
      tileS: 0.4,
      reflowS: 0.4,
      cameraS: 0.9,
    ));

    // Fit the camera to the board on every (re)size. This is where the framing
    // happens: the fit needs live tiles (they exist only after the first tick
    // promotes the scene) and the real view aspect, so it can't run here in
    // setup — onResize fires after the first frame is presented, and again on
    // every orientation / window change.
    c.onResize = (_, _) {
      final fit = c.cameraFitDistance(tileIds: _scene.cornerTileIds, padding: 0.06);
      if (fit != null && fit != _scene.camDistance) {
        _scene.camDistance = fit;
        c.setScene(_scene.build(_game));
      }
    };

    // Push the opening position (framed by onResize once the view is sized).
    c.setScene(_scene.build(_game));

    await c.start();

    // Drive autoplay when the board is idle.
    _loop = Timer.periodic(const Duration(milliseconds: 200), (_) => _tickAutoplay());
    setState(() => _status = 'ready');
  }

  void _tickAutoplay() {
    final c = _controller;
    if (c == null || !_autoplay || _game.gameOver || !c.isIdle) return;
    final a = aiPickAction(_game.state, _rng);
    if (a == null) {
      setState(() => _status = 'game over');
      return;
    }
    _applyMove(a);
  }

  void _applyMove(Action a) {
    final side = _game.state.side == white ? 'white' : 'black';
    _game.apply(a);
    _controller?.setScene(_scene.build(_game));
    setState(() => _status = _game.gameOver
        ? 'game over'
        : '$side played ${moveStr(a)}  ·  ply ${_game.state.ply}');
  }

  Future<void> _onTapDown(TapDownDetails d) async {
    final c = _controller;
    if (c == null || _game.gameOver) return;
    final pick = await c.pick(d.localPosition.dx, d.localPosition.dy);
    if (pick == null || !pick.hitTile) {
      setState(() => _selected = -1);
      return;
    }
    final s = coordToSquare(pick.tileX, pick.tileY);
    if (s < 0) {
      setState(() => _selected = -1);
      return;
    }
    if (_selected < 0) {
      // select one of our own pieces
      if (_game.state.sq[s] != 0 &&
          pieceColor(_game.state.sq[s]) == _game.state.side) {
        setState(() => _selected = s);
      }
    } else {
      // try to play _selected -> s if legal
      final moves = genLegal(_game.state);
      Action? chosen;
      for (final m in moves) {
        if (m.from == _selected && m.to == s) {
          chosen = m;
          break;
        }
      }
      if (chosen != null) _applyMove(chosen);
      setState(() => _selected = -1);
    }
  }

  void _newGame() {
    _game.reset();
    _rng.seed = 0x1234abcd;
    _selected = -1;
    _controller?.setScene(_scene.build(_game));
    setState(() => _status = 'new game');
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Tessera Chess'),
        actions: [
          IconButton(
            tooltip: _autoplay ? 'Pause autoplay' : 'Autoplay',
            icon: Icon(_autoplay ? Icons.pause : Icons.play_arrow),
            onPressed: () => setState(() => _autoplay = !_autoplay),
          ),
          IconButton(
            tooltip: 'New game',
            icon: const Icon(Icons.refresh),
            onPressed: _newGame,
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
            padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 10),
            color: Colors.black26,
            child: Text(
              _selected >= 0
                  ? '$_status  ·  selected ${_squareName(_selected)}'
                  : _status,
              style: const TextStyle(fontSize: 13),
            ),
          ),
        ],
      ),
    );
  }

  String _squareName(int s) =>
      '${String.fromCharCode('a'.codeUnitAt(0) + fileOf(s))}${rankOf(s) + 1}';
}
