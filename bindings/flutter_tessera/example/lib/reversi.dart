// reversi.dart — Reversi (Othello) on the shared reducer framework (see
// game.dart). It is the example that plays a board game out of *cards*: each
// disc is a square card rounded into a circle (cornerRadius = width/2) whose
// visible face is ivory and whose back/hidden faces are black — so flipping a
// captured disc is the engine's real card turn-over, arcing up off the board
// and landing the other color. A move renders as two beats: the placed disc
// drops in, then every flanked disc physically flips.
//
// Sealed action + sealed state, a pure reducer `rvUpdate`, a positional-weight
// AI. Legal squares are ground overlays tinted for the side to move, the last
// move wears a gold ring, and a 3D label at the board edge keeps the count.

import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter_tessera/flutter_tessera.dart';

import 'game.dart';
import 'reversi_art.dart';

const int _off = -4; // col/row 0..7 -> world -4..3

const int rvBlack = 1;
const int rvWhite = 2;

// The eight compass directions as (row step, col step); walking by row/col
// keeps a run from wrapping around a board edge.
const List<(int, int)> _dirs = [
  (-1, -1), (-1, 0), (-1, 1), (0, -1), (0, 1), (1, -1), (1, 0), (1, 1),
];

bool _inBoard(int r, int c) => r >= 0 && r < 8 && c >= 0 && c < 8;

/// The discs a move by [side] on empty square [sq] would flip — empty when the
/// move is illegal (occupied square or no flanked run in any direction).
List<int> rvFlips(List<int> b, int side, int sq) {
  if (sq < 0 || sq >= 64 || b[sq] != 0) return const [];
  final opp = 3 - side;
  final flips = <int>[];
  final r0 = sq ~/ 8, c0 = sq % 8;
  for (final (stepR, stepC) in _dirs) {
    var r = r0 + stepR, c = c0 + stepC;
    final run = <int>[];
    while (_inBoard(r, c) && b[r * 8 + c] == opp) {
      run.add(r * 8 + c);
      r += stepR;
      c += stepC;
    }
    if (run.isNotEmpty && _inBoard(r, c) && b[r * 8 + c] == side) {
      flips.addAll(run);
    }
  }
  return flips;
}

/// Every square where [side] has a legal move.
List<int> rvLegalMoves(List<int> b, int side) => [
      for (var sq = 0; sq < 64; sq++)
        if (b[sq] == 0 && rvFlips(b, side, sq).isNotEmpty) sq
    ];

List<int> rvInitialBoard() {
  final b = List<int>.filled(64, 0);
  b[3 * 8 + 3] = rvWhite;
  b[3 * 8 + 4] = rvBlack;
  b[4 * 8 + 3] = rvBlack;
  b[4 * 8 + 4] = rvWhite;
  return b;
}

// ---- actions -------------------------------------------------------------
sealed class RvAction {
  const RvAction();
}

class RvPlace extends RvAction {
  const RvPlace(this.square);
  final int square;
}

class RvReset extends RvAction {
  const RvReset();
}

// ---- state ---------------------------------------------------------------
/// [lastMove]/[flipped] describe the move just played so `render` can stage
/// the drop-then-flip beats (empty on the opening position / after a reset).
sealed class RvState {
  const RvState(this.board, {this.lastMove = -1, this.flipped = const []});
  final List<int> board;
  final int lastMove;
  final List<int> flipped;

  int count(int side) => board.where((p) => p == side).length;
}

/// A game in progress. [passed] marks that the *other* side had no legal move,
/// so [side] plays again (the mandatory Reversi pass).
class RvPlaying extends RvState {
  const RvPlaying(super.board,
      {required this.side, this.passed = false, super.lastMove, super.flipped});
  final int side;
  final bool passed;
}

/// Neither side can move — most discs wins.
class RvOver extends RvState {
  const RvOver(super.board, {super.lastMove, super.flipped});
}

// ---- reducer -------------------------------------------------------------
RvState rvUpdate(RvState s, RvAction a) {
  switch (a) {
    case RvReset():
      return RvPlaying(rvInitialBoard(), side: rvBlack);
    case RvPlace(:final square):
      if (s is! RvPlaying) return s;
      final flips = rvFlips(s.board, s.side, square);
      if (flips.isEmpty) return s;
      final b = List<int>.of(s.board);
      b[square] = s.side;
      for (final f in flips) {
        b[f] = s.side;
      }
      final opp = 3 - s.side;
      if (rvLegalMoves(b, opp).isNotEmpty) {
        return RvPlaying(b, side: opp, lastMove: square, flipped: flips);
      }
      if (rvLegalMoves(b, s.side).isNotEmpty) {
        return RvPlaying(b,
            side: s.side, passed: true, lastMove: square, flipped: flips);
      }
      return RvOver(b, lastMove: square, flipped: flips);
  }
}

// ---- controller (rendering + wiring) -------------------------------------
class ReversiController extends GameController<RvState, RvAction> {
  static const int _cardIdBase = 100; // card id = base + square + 1
  static const int _scoreLabelId = 9201;
  static const List<int> _fitCorners = [1, 8, 57, 64]; // tile ids

  // Corner-greedy positional weights (corners rule, X-squares poison).
  static const List<int> _weights = [
    120, -20, 20, 5, 5, 20, -20, 120, //
    -20, -40, -5, -5, -5, -5, -40, -20, //
    20, -5, 15, 3, 3, 15, -5, 20, //
    5, -5, 3, 3, 3, 3, -5, 5, //
    5, -5, 3, 3, 3, 3, -5, 5, //
    20, -5, 15, 3, 3, 15, -5, 20, //
    -20, -40, -5, -5, -5, -5, -40, -20, //
    120, -20, 20, 5, 5, 20, -20, 120,
  ];

  int _feltLight = 0;
  int _feltDark = 0;
  int _disc = 0;
  int _font = 0;
  double _camDistance = 12.5;

  @override
  String get title => 'Reversi';
  @override
  String get subtitle => 'Cards as discs · flank a run to flip it';
  @override
  IconData get icon => Icons.contrast;

  @override
  TesseraLightData get light => const TesseraLightData(
        dir: [-0.35, -1.0, -0.45],
        color: [1.0, 0.98, 0.92],
        intensity: 1.1,
        ambient: [0.33, 0.36, 0.42],
      );

  @override
  TesseraTimingData get timing => const TesseraTimingData(
        addS: 0.28,
        removeS: 0.35,
        moveS: 0.35,
        tileS: 0.3,
        reflowS: 0.3,
        cameraS: 0.8,
      );

  @override
  Future<void> registerDefs(TesseraController c) async {
    _font = await registerGameFont(c);
    _feltLight = c.registerTileType(
        const TesseraTileType(thickness: 0.22, tint: [0.16, 0.42, 0.26, 1.0]));
    _feltDark = c.registerTileType(
        const TesseraTileType(thickness: 0.22, tint: [0.13, 0.36, 0.22, 1.0]));

    final ivory = await renderDiscFace(dark: false);
    final coal = await renderDiscFace(dark: true);
    final ivoryAtlas = c.registerAtlas(ivory);
    final coalAtlas = c.registerAtlas(coal);
    // visible = ivory, back + concealing hidden face = coal: face-up reads
    // white, face-down reads black, and the flip is a genuine turn-over.
    _disc = c.registerCardType(TesseraCardType(
      visibleAtlas: ivoryAtlas,
      hiddenAtlas: coalAtlas,
      backAtlas: coalAtlas,
      width: 0.82,
      height: 0.82,
      cornerRadius: 0.41, // width/2 -> a circle
    ));
  }

  @override
  RvState initial() => RvPlaying(rvInitialBoard(), side: rvBlack);

  @override
  RvState update(RvState state, RvAction action) => rvUpdate(state, action);

  /// The board as one scene. With [preFlip] the discs the last move captured
  /// still show their *old* color, so pushing the final scene right after
  /// animates each of them turning over.
  TesseraScene _scene(RvState s, {bool preFlip = false}) {
    final tiles = <TesseraTile>[];
    for (var r = 0; r < 8; r++) {
      for (var c = 0; c < 8; c++) {
        tiles.add(TesseraTile(
          x: c + _off,
          y: r + _off,
          def: ((r + c) & 1) == 0 ? _feltLight : _feltDark,
          id: r * 8 + c + 1,
        ));
      }
    }

    final cards = <TesseraCard>[];
    for (var sq = 0; sq < 64; sq++) {
      final p = s.board[sq];
      if (p == 0) continue;
      var black = p == rvBlack;
      if (preFlip && s.flipped.contains(sq)) black = !black;
      cards.add(TesseraCard(
        id: _cardIdBase + sq + 1,
        def: _disc,
        position: [sq % 8 + _off + 0.0, 0.02, sq ~/ 8 + _off + 0.0],
        hidden: black,
      ));
    }

    return TesseraScene(
      tiles: tiles,
      cards: cards,
      overlays: _overlays(s),
      labels: [_scoreLabel(s)],
      camera: TesseraCameraPose(
        focusX: _off + 3.5,
        focusY: _off + 3.5,
        distance: _camDistance,
        yaw: 3.14159, // black plays from the bottom; the view never swings
        pitch: 0.95,
        fov: 0.72,
      ),
    );
  }

  @override
  Iterable<TesseraScene> render(RvState s) sync* {
    if (s.flipped.isNotEmpty) {
      yield _scene(s, preFlip: true); // the placed disc drops in
      yield _scene(s); // then the flanked discs turn over
    } else {
      yield _scene(s);
    }
  }

  /// Ground decals: every legal square for the side to move pulses in that
  /// side's color, and the last move keeps a gold ring under its disc.
  List<TesseraOverlay> _overlays(RvState s) {
    final out = <TesseraOverlay>[];
    if (s is RvPlaying) {
      final dark = s.side == rvBlack;
      for (final sq in rvLegalMoves(s.board, s.side)) {
        out.add(TesseraOverlay(
          x: sq % 8 + _off,
          y: sq ~/ 8 + _off,
          tint: dark
              ? const [0.10, 0.12, 0.16, 0.55]
              : const [0.95, 0.93, 0.85, 0.50],
          pulseS: 1.6,
          pulseAlphaMin: 0.30,
          pulseAlphaMax: 0.55,
          pulseScaleMin: 0.45,
          pulseScaleMax: 0.55,
        ));
      }
    }
    if (s.lastMove >= 0) {
      out.add(TesseraOverlay(
        x: s.lastMove % 8 + _off,
        y: s.lastMove ~/ 8 + _off,
        shape: TesseraOverlayShape.ring,
        tint: const [1.0, 0.82, 0.25, 0.8],
        pulseS: 1.2,
        pulseAlphaMin: 0.45,
        pulseAlphaMax: 0.80,
      ));
    }
    return out;
  }

  TesseraLabel _scoreLabel(RvState s) {
    final b = s.count(rvBlack), w = s.count(rvWhite);
    // ASCII only: the baked label font atlas has no em-dash glyph.
    final text = switch (s) {
      RvOver() when b != w => '${b > w ? 'Black' : 'White'} wins  $b - $w',
      RvOver() => 'Draw  $b - $w',
      _ => 'Black $b - $w White',
    };
    return TesseraLabel(
      id: _scoreLabelId,
      font: _font,
      text: text,
      position: const [0, 0.4, 4.9], // past the black-side edge, camera-near
      size: 0.5,
      color: const [1, 1, 1, 0.92],
    );
  }

  @override
  RvAction? autoAction(RvState state, math.Random rng) {
    if (state is RvOver) return const RvReset();
    if (state is! RvPlaying) return null;
    final moves = rvLegalMoves(state.board, state.side);
    if (moves.isEmpty) return null;
    // Positional weight + a little mobility; random among the near-best.
    int score(int sq) =>
        _weights[sq] + 2 * rvFlips(state.board, state.side, sq).length;
    final best = moves.map(score).reduce(math.max);
    final top = [
      for (final sq in moves)
        if (score(sq) >= best - 2) sq
    ];
    return RvPlace(top[rng.nextInt(top.length)]);
  }

  @override
  Duration holdFor(RvState state) => Duration.zero;

  @override
  RvAction? onTap(RvState state, TesseraPickResult? pick,
      {Offset? local, Size? view}) {
    if (state is! RvPlaying || pick == null || !pick.hitTile) return null;
    final r = pick.tileY - _off, c = pick.tileX - _off;
    if (!_inBoard(r, c)) return null;
    final sq = r * 8 + c;
    return rvFlips(state.board, state.side, sq).isEmpty ? null : RvPlace(sq);
  }

  @override
  List<GameButton<RvAction>> buttons(RvState state) => const [
        GameButton('New game', RvReset(), icon: Icons.refresh),
      ];

  @override
  List<TesseraScene>? onResize(
      TesseraController c, RvState state, double w, double h) {
    final fit = c.cameraFitDistance(tileIds: _fitCorners, padding: 0.08);
    if (fit == null || fit == _camDistance) return null;
    _camDistance = fit;
    return [_scene(state)];
  }

  @override
  String status(RvState state) {
    final b = state.count(rvBlack), w = state.count(rvWhite);
    if (state is RvOver) {
      if (b == w) return 'Game over — a draw, $b all.';
      final who = b > w ? 'Black' : 'White';
      return 'Game over — $who wins $b to $w.';
    }
    final p = state as RvPlaying;
    final side = p.side == rvBlack ? 'Black' : 'White';
    final passNote =
        p.passed ? '  ·  ${p.side == rvBlack ? 'White' : 'Black'} passed' : '';
    return '$side to move  ·  Black $b  White $w$passNote';
  }
}
