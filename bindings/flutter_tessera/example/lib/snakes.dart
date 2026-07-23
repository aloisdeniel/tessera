// snakes.dart — Snakes & Ladders: a two-player race up a 10x10 boustrophedon
// track. It is the example that pairs the Dice APIs with a *following* camera:
// each turn throws a d6, the token then walks the rolled number of cells one hop
// at a time (a multi-step entity path) while the camera rides along with it
// (FOCUS_ENTITY), and a ladder or snake whisks it up or down with a single glide.
//
// You are the red token; the blue token auto-rolls on its turn. Same reducer
// shape as the other games (see game.dart). Tokens reuse the chess pawn mesh.

import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter_tessera/flutter_tessera.dart';

import 'chess_gen.dart';
import 'dice_art.dart';
import 'game.dart';

const int _off = -5; // col/row 0..9 -> world -5..4
const int _goal = 100;

// Ladders lift you up (base -> top); snakes drop you down (head -> tail).
const Map<int, int> _ladders = {
  4: 14, 9: 31, 21: 42, 28: 84, 36: 44, 51: 67, 71: 91, 80: 100,
};
const Map<int, int> _snakes = {
  16: 6, 47: 26, 49: 11, 56: 53, 62: 19, 64: 60, 87: 24, 93: 73, 95: 75, 98: 78,
};

/// Where cell [cell] (1..100) sits on the grid, as (x, y). The track snakes:
/// odd rows run right-to-left, so the path is continuous.
(int, int) _cellXY(int cell) {
  final i = cell - 1;
  final row = i ~/ 10;
  final col = row.isEven ? i % 10 : 9 - (i % 10);
  return (col + _off, row + _off);
}

int _jump(int cell) => _ladders[cell] ?? _snakes[cell] ?? cell;

int _lcg(int seed) => (seed * 1103515245 + 12345) & 0x7fffffff;

// ---- the outcome of one roll (drives the render sequence) -----------------
class SlOutcome {
  const SlOutcome({
    required this.mover,
    required this.rolled,
    required this.from,
    required this.walked,
    required this.dest,
    required this.dieSeed,
  });

  final int mover; // which player rolled (0/1)
  final int rolled; // 1..6
  final int from; // cell before the roll
  final int walked; // cell after the dice walk (before snake/ladder)
  final int dest; // cell after the snake/ladder
  final int dieSeed;

  bool get moved => walked != from;
  bool get jumped => dest != walked;
}

// ---- actions -------------------------------------------------------------
sealed class SlAction {
  const SlAction();
}

/// Throw the current player's die and resolve the move.
class SlRoll extends SlAction {
  const SlRoll();
}

/// Intrinsic: commit the resolved move and pass the turn (or end the game).
class SlCommit extends SlAction {
  const SlCommit();
}

class SlNewGame extends SlAction {
  const SlNewGame();
}

// ---- state ---------------------------------------------------------------
sealed class SlState {
  const SlState({required this.pos, required this.turn, required this.seed});
  final List<int> pos; // cell per player (starts at 1)
  final int turn; // whose turn (0/1)
  final int seed;
}

/// Awaiting a roll from [turn] (0 = you, 1 = CPU which auto-rolls).
class SlReady extends SlState {
  const SlReady({required super.pos, required super.turn, required super.seed});
}

/// A roll just landed; `render` plays it out, then [SlCommit] advances.
class SlResolving extends SlState {
  const SlResolving({
    required super.pos, // already committed to the outcome's destination
    required super.turn,
    required super.seed,
    required this.outcome,
  });
  final SlOutcome outcome;
}

/// [winner] reached 100.
class SlWon extends SlState {
  const SlWon({required super.pos, required super.seed, required this.winner})
      : super(turn: 0);
  final int winner;
}

// ---- reducer -------------------------------------------------------------
SlState slUpdate(SlState s, SlAction a) {
  switch (a) {
    case SlNewGame():
      return SlReady(pos: const [1, 1], turn: 0, seed: _lcg(s.seed));
    case SlRoll():
      return s is SlReady ? _roll(s) : s;
    case SlCommit():
      return s is SlResolving ? _commit(s) : s;
  }
}

SlState _roll(SlReady s) {
  final seed = _lcg(s.seed);
  final rolled = 1 + seed % 6;
  final from = s.pos[s.turn];
  final walked = from + rolled <= _goal ? from + rolled : from; // must land exactly
  final dest = _jump(walked);
  final pos = List<int>.of(s.pos)..[s.turn] = dest;
  return SlResolving(
    pos: pos,
    turn: s.turn,
    seed: seed,
    outcome: SlOutcome(
      mover: s.turn,
      rolled: rolled,
      from: from,
      walked: walked,
      dest: dest,
      dieSeed: seed == 0 ? 1 : seed,
    ),
  );
}

SlState _commit(SlResolving s) {
  if (s.outcome.dest == _goal) {
    return SlWon(pos: s.pos, seed: s.seed, winner: s.outcome.mover);
  }
  return SlReady(pos: s.pos, turn: s.turn ^ 1, seed: s.seed);
}

// ---- controller (rendering + wiring) -------------------------------------
class SnakesController extends GameController<SlState, SlAction> {
  static const int _dieId = 10;
  static const List<int> _fitCorners = [1, 10, 91, 100];

  int _die = 0;
  int _tileLight = 0;
  int _tileDark = 0;
  int _tileLadder = 0;
  int _tileSnake = 0;
  int _tileGoal = 0;
  final List<int> _token = [0, 0];
  double _camDistance = 15.0;

  int _tokenId(int player) => player + 1;

  @override
  String get title => 'Snakes & Ladders';
  @override
  String get subtitle => 'Dice · roll and race up the board — camera rides along';
  @override
  IconData get icon => Icons.stairs;

  @override
  TesseraLightData get light => const TesseraLightData(
        dir: [-0.35, -1.0, -0.4],
        color: [1.0, 0.98, 0.92],
        intensity: 1.12,
        ambient: [0.32, 0.35, 0.42],
      );

  @override
  TesseraTimingData get timing => const TesseraTimingData(
        moveS: 0.5,
        addS: 0.3,
        removeS: 0.3,
        tileS: 0.3,
        cameraS: 0.7,
      );

  @override
  Future<void> registerDefs(TesseraController c) async {
    _tileLight = c.registerTileType(
        const TesseraTileType(thickness: 0.2, tint: [0.84, 0.80, 0.70, 1.0]));
    _tileDark = c.registerTileType(
        const TesseraTileType(thickness: 0.2, tint: [0.62, 0.58, 0.50, 1.0]));
    _tileLadder = c.registerTileType(
        const TesseraTileType(thickness: 0.24, tint: [0.30, 0.62, 0.40, 1.0]));
    _tileSnake = c.registerTileType(
        const TesseraTileType(thickness: 0.24, tint: [0.78, 0.32, 0.30, 1.0]));
    _tileGoal = c.registerTileType(
        const TesseraTileType(thickness: 0.28, tint: [0.90, 0.74, 0.28, 1.0]));

    _die = c.registerDiceType(TesseraDiceType(faces: await renderDiceFaces(), size: 1.3));

    const redCol = [0.85, 0.30, 0.24, 1.0];
    const blueCol = [0.30, 0.48, 0.90, 1.0];
    _token[0] = c.registerEntityType(
        TesseraEntityType(glb: buildPieceGlb(chessPawn, 1, redCol), scale: 1.15));
    _token[1] = c.registerEntityType(
        TesseraEntityType(glb: buildPieceGlb(chessPawn, 1, blueCol), scale: 1.15));
  }

  @override
  SlState initial() => const SlReady(pos: [1, 1], turn: 0, seed: 0xC0FFEE);

  @override
  SlState update(SlState state, SlAction action) => slUpdate(state, action);

  int _tileDef(int cell) {
    if (cell == _goal) return _tileGoal;
    if (_ladders.containsKey(cell)) return _tileLadder;
    if (_snakes.containsKey(cell)) return _tileSnake;
    final (x, y) = _cellXY(cell);
    return ((x + y) & 1) == 0 ? _tileLight : _tileDark;
  }

  List<TesseraTile> _boardTiles() {
    final tiles = <TesseraTile>[];
    for (var cell = 1; cell <= _goal; cell++) {
      final (x, y) = _cellXY(cell);
      tiles.add(TesseraTile(x: x, y: y, def: _tileDef(cell), id: cell));
    }
    return tiles;
  }

  /// The tokens for a settled board (both at [pos]); the mover may carry a path.
  List<TesseraEntity> _tokens(List<int> pos, {int? moverAtCell, List<int>? walk}) {
    final out = <TesseraEntity>[];
    for (var p = 0; p < 2; p++) {
      final cell = (p == _movingPlayer && moverAtCell != null) ? moverAtCell : pos[p];
      final (x, y) = _cellXY(cell);
      final path = (p == _movingPlayer && walk != null)
          ? [for (final ce in walk) _cellXY(ce)]
          : const <(int, int)>[];
      out.add(TesseraEntity(id: _tokenId(p), def: _token[p], x: x, y: y, path: path));
    }
    return out;
  }

  int _movingPlayer = -1;

  TesseraCamera _boardCamera() => TesseraCameraPose(
        focusX: _off + 4.5,
        focusY: _off + 4.5,
        distance: _camDistance,
        yaw: 0,
        pitch: 0.86,
        fov: 0.72,
      );

  @override
  List<TesseraScene> render(SlState s) {
    switch (s) {
      case SlReady():
      case SlWon():
        _movingPlayer = -1;
        return [
          TesseraScene(
            tiles: _boardTiles(),
            entities: _tokens(s.pos),
            camera: _boardCamera(),
          )
        ];
      case SlResolving():
        return _resolveSequence(s);
    }
  }

  List<TesseraScene> _resolveSequence(SlResolving s) {
    final o = s.outcome;
    _movingPlayer = o.mover;
    final (dfx, dfy) = _cellXY(o.from);
    final die = TesseraDie(
      id: _dieId,
      def: _die,
      face: o.rolled - 1,
      position: [dfx + 0.9, 0.6, dfy.toDouble()],
      seed: o.dieSeed,
      throwS: 0.9,
    );
    final follow = TesseraCameraFocusEntity(_tokenId(o.mover),
        distance: 8.5, yaw: 0, pitch: 0.9, fov: 0.72);

    final out = <TesseraScene>[];
    // Beat A: throw the die and walk the token, riding along with it.
    final walk = o.moved
        ? [for (var ce = o.from + 1; ce <= o.walked; ce++) ce]
        : const <int>[];
    out.add(TesseraScene(
      tiles: _boardTiles(),
      entities: _tokens(s.pos, moverAtCell: o.walked, walk: walk),
      dice: [die],
      camera: o.moved ? follow : _boardCamera(),
    ));
    // Beat B: a ladder or snake glides the token to its final cell.
    if (o.jumped) {
      out.add(TesseraScene(
        tiles: _boardTiles(),
        entities: _tokens(s.pos, moverAtCell: o.dest),
        dice: [die],
        camera: follow,
      ));
    }
    return out;
  }

  @override
  SlAction? autoAdvance(SlState state) => switch (state) {
        SlResolving() => const SlCommit(),
        SlReady(:final turn) => turn == 1 ? const SlRoll() : null, // CPU rolls itself
        _ => null,
      };

  @override
  SlAction? autoAction(SlState state, math.Random rng) => switch (state) {
        SlReady() => const SlRoll(), // with auto-play on, roll for you too
        SlWon() => const SlNewGame(),
        _ => null,
      };

  @override
  List<GameButton<SlAction>> buttons(SlState state) => switch (state) {
        SlReady(:final turn) when turn == 0 => const [
            GameButton('Roll', SlRoll(), icon: Icons.casino, tone: GameButtonTone.primary)
          ],
        SlWon() => const [
            GameButton('New game', SlNewGame(), icon: Icons.refresh, tone: GameButtonTone.primary)
          ],
        _ => const [],
      };

  @override
  List<TesseraScene>? onResize(TesseraController c, SlState state, double w, double h) {
    final fit = c.cameraFitDistance(tileIds: _fitCorners, padding: 0.08);
    if (fit == null) return null;
    final dist = math.max(fit, 15.0);
    if (dist == _camDistance) return null;
    _camDistance = dist;
    // Only re-fit the settled framings; a mid-move follow shot re-fits itself.
    return state is SlResolving ? null : render(state);
  }

  @override
  String status(SlState state) {
    switch (state) {
      case SlWon():
        return '${state.winner == 0 ? 'You' : 'CPU'} reached 100 — game over!';
      case SlReady():
        final who = state.turn == 0 ? 'Your' : "CPU's";
        return "$who turn  ·  You ${state.pos[0]}  ·  CPU ${state.pos[1]}";
      case SlResolving():
        final o = state.outcome;
        final who = o.mover == 0 ? 'You' : 'CPU';
        final tail = !o.moved
            ? ' — need exactly ${_goal - o.from}, stay on ${o.from}'
            : o.jumped
                ? (o.dest > o.walked ? ' — ladder up to ${o.dest}!' : ' — snake down to ${o.dest}!')
                : ' — to ${o.dest}';
        return '$who rolled ${o.rolled}$tail';
    }
  }
}
