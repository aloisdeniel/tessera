// memory.dart — Concentration / "Memory": a flat grid of face-down cards; flip
// two, and if their faces match they clear, otherwise they turn back. It is the
// card example that complements Blackjack: instead of fanned hands and a draw
// pile, it shows *freely placed* cards, per-card flips (toggling `hidden`
// crossfades a card from its concealing front to its face), and per-card picking
// (pick.hitCard). Same reducer shape as the other games (see game.dart).

import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter_tessera/flutter_tessera.dart';

import 'card_art.dart';
import 'game.dart';

const int _cols = 4;
const int _rows = 4;
const int _cells = _cols * _rows; // 16 cards, 8 pairs
const int _pairs = _cells ~/ 2;

// The eight distinct faces used for the pairs, as (rank, suit) — see card_art.
const List<(int, int)> _faces = [
  (1, 0), (13, 1), (12, 2), (11, 3), (10, 0), (9, 1), (8, 2), (7, 3),
];

const double _colSpacing = 1.7;
const double _rowSpacing = 2.2;

/// A deterministic Fisher–Yates shuffle of the paired ids [0,0,1,1,…], from
/// [seed] (keeps the reducer pure).
List<int> _shuffledBoard(int seed) {
  final ids = <int>[for (var p = 0; p < _pairs; p++) ...[p, p]];
  var s = seed & 0x7fffffff;
  if (s == 0) s = 1;
  for (var i = ids.length - 1; i > 0; i--) {
    s = (s * 1103515245 + 12345) & 0x7fffffff;
    final j = s % (i + 1);
    final t = ids[i];
    ids[i] = ids[j];
    ids[j] = t;
  }
  return ids;
}

int _nextSeed(int seed) => (seed * 1103515245 + 12345) & 0x7fffffff;

// ---- actions -------------------------------------------------------------
sealed class MemAction {
  const MemAction();
}

class MemFlip extends MemAction {
  const MemFlip(this.index);
  final int index;
}

/// Intrinsic: resolve the two face-up cards (clear a match, or turn them back).
class MemResolve extends MemAction {
  const MemResolve();
}

class MemNew extends MemAction {
  const MemNew();
}

// ---- state ---------------------------------------------------------------
sealed class MemState {
  const MemState({
    required this.board,
    required this.up,
    required this.matched,
    required this.moves,
    required this.seed,
  });

  final List<int> board; // pair id per cell
  final List<int> up; // 0..2 face-up, unmatched cell indices
  final Set<int> matched; // cleared cells
  final int moves; // attempts (pairs of flips)
  final int seed;

  int get pairsFound => matched.length ~/ 2;
}

/// Awaiting a pick (0 or 1 cards face-up).
class MemPlaying extends MemState {
  const MemPlaying({
    required super.board,
    required super.up,
    required super.matched,
    required super.moves,
    required super.seed,
  });
}

/// Two cards face-up; render shows both, then [MemResolve] settles it.
class MemEval extends MemState {
  const MemEval({
    required super.board,
    required super.up,
    required super.matched,
    required super.moves,
    required super.seed,
  });
  bool get isMatch => board[up[0]] == board[up[1]];
}

/// Every pair cleared.
class MemWon extends MemState {
  const MemWon({required super.board, required super.moves, required super.seed})
      : super(up: const [], matched: const {});
}

// ---- reducer -------------------------------------------------------------
MemState memUpdate(MemState s, MemAction a) {
  switch (a) {
    case MemNew():
      return MemPlaying(
          board: _shuffledBoard(_nextSeed(s.seed)),
          up: const [],
          matched: const {},
          moves: 0,
          seed: _nextSeed(s.seed));
    case MemFlip(:final index):
      return _flip(s, index);
    case MemResolve():
      return s is MemEval ? _resolve(s) : s;
  }
}

MemState _flip(MemState s, int i) {
  if (s is! MemPlaying) return s;
  if (i < 0 || i >= _cells || s.matched.contains(i) || s.up.contains(i)) return s;
  final up = [...s.up, i];
  if (up.length < 2) {
    return MemPlaying(
        board: s.board, up: up, matched: s.matched, moves: s.moves, seed: s.seed);
  }
  return MemEval(
      board: s.board, up: up, matched: s.matched, moves: s.moves + 1, seed: s.seed);
}

MemState _resolve(MemEval s) {
  if (s.isMatch) {
    final matched = {...s.matched, s.up[0], s.up[1]};
    if (matched.length >= _cells) {
      return MemWon(board: s.board, moves: s.moves, seed: s.seed);
    }
    return MemPlaying(
        board: s.board, up: const [], matched: matched, moves: s.moves, seed: s.seed);
  }
  return MemPlaying(
      board: s.board, up: const [], matched: s.matched, moves: s.moves, seed: s.seed);
}

// ---- controller (rendering + wiring) -------------------------------------
class MemoryController extends GameController<MemState, MemAction> {
  static const List<int> _fitCorners = [9001, 9002, 9003, 9004];

  int _felt = 0;
  int _hidden = 0;
  int _back = 0;
  final List<int> _faceDef = List<int>.filled(_pairs, 0);
  double _camDistance = 14.0;

  @override
  String get title => 'Memory';
  @override
  String get subtitle => 'Cards · flip two, find the matching pairs';
  @override
  IconData get icon => Icons.grid_on;

  @override
  TesseraLightData get light => const TesseraLightData(
        dir: [-0.3, -1.0, -0.35],
        color: [1.0, 0.98, 0.93],
        intensity: 1.1,
        ambient: [0.34, 0.37, 0.44],
      );

  @override
  TesseraTimingData get timing => const TesseraTimingData(
        addS: 0.35,
        removeS: 0.45,
        moveS: 0.35,
        tileS: 0.35,
        reflowS: 0.35,
        cameraS: 0.6,
      );

  @override
  Future<void> registerDefs(TesseraController c) async {
    _felt = c.registerTileType(
        const TesseraTileType(thickness: 0.2, tint: [0.12, 0.30, 0.42, 1.0]));
    _hidden = c.registerAtlas(await renderCardHidden());
    _back = c.registerAtlas(await renderCardBack());
    for (var p = 0; p < _pairs; p++) {
      final (rank, suit) = _faces[p];
      final face = c.registerAtlas(await renderCardFace(rank, suit));
      _faceDef[p] = c.registerCardType(TesseraCardType(
        visibleAtlas: face,
        hiddenAtlas: _hidden,
        backAtlas: _back,
        width: 1.4,
        height: 1.95,
        cornerRadius: 0.14,
      ));
    }
  }

  @override
  MemState initial() => MemPlaying(
      board: _shuffledBoard(0x1A2B3C), up: const [], matched: const {}, moves: 0, seed: 0x1A2B3C);

  @override
  MemState update(MemState state, MemAction action) => memUpdate(state, action);

  (double, double) _cellXZ(int index) {
    final col = index % _cols, row = index ~/ _cols;
    final x = (col - (_cols - 1) / 2) * _colSpacing;
    final z = (row - (_rows - 1) / 2) * _rowSpacing;
    return (x, z);
  }

  int _cornerId(int x, int z) {
    if (x == -4 && z == -5) return 9001;
    if (x == 4 && z == -5) return 9002;
    if (x == -4 && z == 5) return 9003;
    if (x == 4 && z == 5) return 9004;
    return 0;
  }

  TesseraScene _scene(MemState s) {
    final tiles = <TesseraTile>[];
    for (var z = -5; z <= 5; z++) {
      for (var x = -4; x <= 4; x++) {
        tiles.add(TesseraTile(x: x, y: z, def: _felt, id: _cornerId(x, z)));
      }
    }

    final cards = <TesseraCard>[];
    for (var i = 0; i < _cells; i++) {
      // Cleared cards fade out; at the win the whole board is cleared.
      if (s is MemWon || s.matched.contains(i)) continue;
      final (x, z) = _cellXZ(i);
      cards.add(TesseraCard(
        id: i + 1,
        def: _faceDef[s.board[i]],
        position: [x, 0.02, z],
        hidden: !s.up.contains(i), // face-up only while picked
      ));
    }

    return TesseraScene(
      tiles: tiles,
      cards: cards,
      camera: TesseraCameraPose(
        focusX: 0,
        focusY: 0,
        distance: _camDistance,
        yaw: 0,
        pitch: 1.15, // near top-down so the faces read
        fov: 0.7,
      ),
    );
  }

  @override
  List<TesseraScene> render(MemState s) => [_scene(s)];

  @override
  MemAction? autoAdvance(MemState state) =>
      state is MemEval ? const MemResolve() : null;

  @override
  MemAction? autoAction(MemState state, math.Random rng) {
    switch (state) {
      case MemWon():
        return const MemNew();
      case MemPlaying():
        final avail = [
          for (var i = 0; i < _cells; i++)
            if (!state.matched.contains(i) && !state.up.contains(i)) i
        ];
        if (avail.isEmpty) return null;
        return MemFlip(avail[rng.nextInt(avail.length)]); // a plausible guess
      case MemEval():
        return null;
    }
  }

  @override
  MemAction? onTap(MemState state, TesseraPickResult? pick,
      {Offset? local, Size? view}) {
    if (state is! MemPlaying || pick == null || !pick.hitCard) return null;
    final index = pick.card - 1;
    if (index < 0 || index >= _cells) return null;
    return MemFlip(index);
  }

  @override
  List<GameButton<MemAction>> buttons(MemState state) => switch (state) {
        MemWon() => const [
            GameButton('New game', MemNew(), icon: Icons.refresh, tone: GameButtonTone.primary)
          ],
        _ => const [
            GameButton('Restart', MemNew(), icon: Icons.refresh),
          ],
      };

  @override
  List<TesseraScene>? onResize(TesseraController c, MemState state, double w, double h) {
    final fit = c.cameraFitDistance(tileIds: _fitCorners, padding: 0.08);
    if (fit == null) return null;
    final dist = math.max(fit, 12.0);
    if (dist == _camDistance) return null;
    _camDistance = dist;
    return [_scene(state)];
  }

  @override
  String status(MemState state) {
    switch (state) {
      case MemWon():
        return 'Solved in ${state.moves} tries!  ·  Tap New game.';
      case MemPlaying():
      case MemEval():
        return 'Pairs ${state.pairsFound}/$_pairs  ·  tries ${state.moves}'
            '${state.up.length == 1 ? '  ·  pick its match' : ''}';
    }
  }
}
