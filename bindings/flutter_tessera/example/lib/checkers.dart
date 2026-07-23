// checkers.dart — Draughts (American checkers) on the shared reducer framework
// (see game.dart). It is the example that leans on the *multi-step* entity path:
// a chain of jumps is one move that hops the piece from square to square, with
// each captured piece dissolving in a particle poof as the jumper flies over it.
// Reaching the back rank swaps the piece's model to the crowned "king" mesh.
//
// Sealed action + sealed state, a pure reducer `ckUpdate`, greedy AI. The piece
// meshes come from checkers_gen.dart; the board is tiles, the pieces entities.

import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter_tessera/flutter_tessera.dart';

import 'checkers_gen.dart';
import 'game.dart';

const int _off = -4; // col/row 0..7 -> world -4..3

// Sides and piece codes (0 = empty square).
const int red = 0;
const int black = 1;
const int redMan = 1;
const int redKing = 2;
const int blkMan = 3;
const int blkKing = 4;

int _colorOf(int p) => (p == redMan || p == redKing) ? red : black;
bool _isKing(int p) => p == redKing || p == blkKing;
bool _inBoard(int r, int c) => r >= 0 && r < 8 && c >= 0 && c < 8;
bool _isDark(int r, int c) => ((r + c) & 1) == 1; // the playable squares

/// The four (or two) diagonal directions a piece may travel, as [dr, dc].
List<List<int>> _dirs(int p) {
  if (_isKing(p)) return const [[1, 1], [1, -1], [-1, 1], [-1, -1]];
  return _colorOf(p) == red ? const [[1, 1], [1, -1]] : const [[-1, 1], [-1, -1]];
}

// ---- a move: a run of landing squares + the squares it captures -----------
class CkMove {
  const CkMove(this.from, this.path, this.captures);
  final int from;
  final List<int> path; // landing squares; excludes [from]; last = destination
  final List<int> captures;
  int get to => path.last;
  bool get isJump => captures.isNotEmpty;
}

/// All legal moves for [side]. If any capture exists, only captures are legal
/// (forced-capture rule).
List<CkMove> ckMoves(List<int> b, int side) {
  final jumps = <CkMove>[];
  for (var s = 0; s < 64; s++) {
    final p = b[s];
    if (p != 0 && _colorOf(p) == side) {
      _collectJumps(b, s, p, s, const [], const [], jumps);
    }
  }
  if (jumps.isNotEmpty) return jumps;

  final steps = <CkMove>[];
  for (var s = 0; s < 64; s++) {
    final p = b[s];
    if (p == 0 || _colorOf(p) != side) continue;
    final r = s ~/ 8, c = s % 8;
    for (final d in _dirs(p)) {
      final nr = r + d[0], nc = c + d[1];
      if (_inBoard(nr, nc) && b[nr * 8 + nc] == 0) {
        steps.add(CkMove(s, [nr * 8 + nc], const []));
      }
    }
  }
  return steps;
}

void _collectJumps(List<int> b, int origin, int piece, int cur,
    List<int> captured, List<int> path, List<CkMove> out) {
  final r = cur ~/ 8, c = cur % 8;
  var extended = false;
  for (final d in _dirs(piece)) {
    final mr = r + d[0], mc = c + d[1]; // jumped-over
    final lr = r + 2 * d[0], lc = c + 2 * d[1]; // landing
    if (!_inBoard(lr, lc)) continue;
    final over = mr * 8 + mc, land = lr * 8 + lc;
    if (captured.contains(over)) continue;
    final op = b[over];
    if (op == 0 || _colorOf(op) == _colorOf(piece)) continue;
    if (b[land] != 0 && land != origin) continue; // landing must be clear
    extended = true;
    _collectJumps(b, origin, piece, land, [...captured, over], [...path, land], out);
  }
  if (!extended && path.isNotEmpty) {
    out.add(CkMove(origin, List.of(path), List.of(captured)));
  }
}

// ---- immutable board (rules snapshot + stable entity ids) -----------------
class CkBoard {
  const CkBoard({required this.sq, required this.side, required this.ids});

  final List<int> sq; // 64, piece code per square
  final int side; // side to move
  final List<int> ids; // 64, stable entity id per square (0 = none)

  factory CkBoard.initial() {
    final sq = List<int>.filled(64, 0);
    final ids = List<int>.filled(64, 0);
    var id = 1;
    for (var r = 0; r < 8; r++) {
      for (var c = 0; c < 8; c++) {
        if (!_isDark(r, c)) continue;
        if (r <= 2) {
          sq[r * 8 + c] = redMan;
          ids[r * 8 + c] = id++;
        } else if (r >= 5) {
          sq[r * 8 + c] = blkMan;
          ids[r * 8 + c] = id++;
        }
      }
    }
    return CkBoard(sq: sq, side: red, ids: ids);
  }

  CkBoard apply(CkMove m) {
    final s = List<int>.of(sq);
    final nids = List<int>.of(ids);
    var p = s[m.from];
    final id = nids[m.from];
    s[m.from] = 0;
    nids[m.from] = 0;
    for (final cap in m.captures) {
      s[cap] = 0;
      nids[cap] = 0;
    }
    final toRow = m.to ~/ 8;
    if (!_isKing(p) &&
        ((_colorOf(p) == red && toRow == 7) || (_colorOf(p) == black && toRow == 0))) {
      p = _colorOf(p) == red ? redKing : blkKing; // crowned on the back rank
    }
    s[m.to] = p;
    nids[m.to] = id;
    return CkBoard(sq: s, side: side ^ 1, ids: nids);
  }

  int count(int side) {
    var n = 0;
    for (final p in sq) {
      if (p != 0 && _colorOf(p) == side) n++;
    }
    return n;
  }
}

// ---- actions -------------------------------------------------------------
sealed class CkAction {
  const CkAction();
}

class CkTap extends CkAction {
  const CkTap(this.square);
  final int square;
}

class CkPlay extends CkAction {
  const CkPlay(this.move);
  final CkMove move;
}

class CkReset extends CkAction {
  const CkReset();
}

// ---- state ---------------------------------------------------------------
sealed class CkState {
  const CkState(this.board);
  final CkBoard board;
}

/// A game in progress. [selected] is the picked source square (or -1).
/// [lastFrom]/[lastPath] describe the move just played so `render` can hand the
/// engine the jump path for the moving piece (empty on the opening position).
class CkPlaying extends CkState {
  const CkPlaying(super.board,
      {this.selected = -1, this.lastFrom = -1, this.lastPath = const []});
  final int selected;
  final int lastFrom;
  final List<int> lastPath;
}

/// One side has no legal move (no pieces, or all blocked) — the other wins.
class CkOver extends CkState {
  const CkOver(super.board, this.winner);
  final int winner;
}

// ---- reducer -------------------------------------------------------------
CkState ckUpdate(CkState s, CkAction a) {
  switch (a) {
    case CkReset():
      return CkPlaying(CkBoard.initial());
    case CkPlay(:final move):
      return s is CkPlaying ? _applyMove(s.board, move) : s;
    case CkTap(:final square):
      return s is CkPlaying ? _tap(s, square) : s;
  }
}

CkState _applyMove(CkBoard b, CkMove m) {
  final nb = b.apply(m);
  if (ckMoves(nb.sq, nb.side).isEmpty) {
    return CkOver(nb, b.side); // the side that just moved wins
  }
  return CkPlaying(nb, lastFrom: m.from, lastPath: m.path);
}

CkState _tap(CkPlaying s, int sq) {
  final b = s.board;
  final own = b.sq[sq] != 0 && _colorOf(b.sq[sq]) == b.side;
  if (s.selected < 0) {
    return own ? CkPlaying(b, selected: sq) : s;
  }
  for (final m in ckMoves(b.sq, b.side)) {
    if (m.from == s.selected && m.to == sq) return _applyMove(b, m);
  }
  return CkPlaying(b, selected: own ? sq : -1);
}

// ---- controller (rendering + wiring) -------------------------------------
class CheckersController extends GameController<CkState, CkAction> {
  final math.Random _ai = math.Random(0x5AFE);
  double _camDistance = 12.0;
  int _tileLight = 0;
  int _tileDark = 0;
  int _tileSel = 0;
  int _tileTarget = 0;
  int _poof = 0;
  // [color][king?0/1] -> entity def id.
  final List<List<int>> _piece = List.generate(2, (_) => List<int>.filled(2, 0));

  @override
  String get title => 'Checkers';
  @override
  String get subtitle => 'Entities · chain jumps, crown a king, or watch the AI';
  @override
  IconData get icon => Icons.circle;

  @override
  TesseraLightData get light => const TesseraLightData(
        dir: [-0.4, -1.0, -0.5],
        color: [1.0, 0.97, 0.9],
        intensity: 1.15,
        ambient: [0.3, 0.33, 0.4],
      );

  @override
  TesseraTimingData get timing => const TesseraTimingData(
        moveS: 0.42,
        addS: 0.3,
        removeS: 0.35,
        tileS: 0.3,
        reflowS: 0.35,
        cameraS: 0.8,
      );

  @override
  Future<void> registerDefs(TesseraController c) async {
    _tileLight = c.registerTileType(
        const TesseraTileType(thickness: 0.22, tint: [0.86, 0.79, 0.63, 1.0]));
    _tileDark = c.registerTileType(
        const TesseraTileType(thickness: 0.22, tint: [0.40, 0.26, 0.19, 1.0]));
    _tileSel = c.registerTileType(
        const TesseraTileType(thickness: 0.24, tint: [0.26, 0.52, 0.86, 1.0]));
    _tileTarget = c.registerTileType(
        const TesseraTileType(thickness: 0.24, tint: [0.32, 0.66, 0.42, 1.0]));

    _poof = c.registerEffectType(const TesseraEffectType(
      onRemove: TesseraParticle(
        count: 40,
        lifetime: 0.6,
        lifetimeVar: 0.2,
        speed: 1.7,
        speedVar: 0.6,
        spreadDeg: 60,
        gravity: 0.6,
        sizeStart: 0.13,
        sizeEnd: 0.42,
        colorStart: [0.95, 0.85, 0.55, 0.85],
        colorEnd: [0.6, 0.4, 0.2, 0.0],
      ),
    ));

    const redCol = [0.80, 0.20, 0.17, 1.0];
    const blkCol = [0.14, 0.15, 0.19, 1.0];
    for (final king in [false, true]) {
      _piece[red][king ? 1 : 0] = c.registerEntityType(TesseraEntityType(
          glb: buildCheckerGlb(king, redCol), scale: 0.95, onDespawnEffect: _poof));
      _piece[black][king ? 1 : 0] = c.registerEntityType(TesseraEntityType(
          glb: buildCheckerGlb(king, blkCol), scale: 0.95, onDespawnEffect: _poof));
    }
  }

  @override
  CkState initial() => CkPlaying(CkBoard.initial());

  @override
  CkState update(CkState state, CkAction action) => ckUpdate(state, action);

  int _pieceDef(int p) => _piece[_colorOf(p)][_isKing(p) ? 1 : 0];

  List<int> get _cornerTileIds => [_tid(0, 0), _tid(0, 7), _tid(7, 0), _tid(7, 7)];
  int _tid(int r, int c) => r * 8 + c + 1;

  TesseraScene _scene(CkState s) {
    final b = s.board;
    final selected = s is CkPlaying ? s.selected : -1;

    // Destination squares reachable from the current selection (for highlights).
    final targets = <int>{};
    if (selected >= 0) {
      for (final m in ckMoves(b.sq, b.side)) {
        if (m.from == selected) targets.add(m.to);
      }
    }

    final tiles = <TesseraTile>[];
    for (var r = 0; r < 8; r++) {
      for (var c = 0; c < 8; c++) {
        final sq = r * 8 + c;
        int def;
        if (sq == selected) {
          def = _tileSel;
        } else if (targets.contains(sq)) {
          def = _tileTarget;
        } else {
          def = _isDark(r, c) ? _tileDark : _tileLight;
        }
        tiles.add(TesseraTile(x: c + _off, y: r + _off, def: def, id: _tid(r, c)));
      }
    }

    final lastPath = s is CkPlaying ? s.lastPath : const <int>[];
    final lastTo = lastPath.isNotEmpty ? lastPath.last : -1;

    final entities = <TesseraEntity>[];
    for (var sq = 0; sq < 64; sq++) {
      final p = b.sq[sq];
      if (p == 0 || b.ids[sq] == 0) continue;
      final path = sq == lastTo
          ? [for (final ls in lastPath) (ls % 8 + _off, ls ~/ 8 + _off)]
          : const <(int, int)>[];
      entities.add(TesseraEntity(
        id: b.ids[sq],
        def: _pieceDef(p),
        x: sq % 8 + _off,
        y: sq ~/ 8 + _off,
        path: path,
      ));
    }

    return TesseraScene(
      tiles: tiles,
      entities: entities,
      camera: TesseraCameraPose(
        focusX: _off + 3.5,
        focusY: _off + 3.5,
        distance: _camDistance,
        yaw: b.side == red ? 3.14159 : 0.0,
        pitch: 0.82,
        fov: 0.72,
      ),
    );
  }

  @override
  List<TesseraScene> render(CkState s) => [_scene(s)];

  @override
  CkAction? autoAction(CkState state, math.Random rng) {
    if (state is! CkPlaying) return null;
    final moves = ckMoves(state.board.sq, state.board.side);
    if (moves.isEmpty) return null;
    // Prefer the move that captures the most; break ties randomly.
    var best = moves.first;
    for (final m in moves) {
      if (m.captures.length > best.captures.length) best = m;
    }
    final top = moves.where((m) => m.captures.length == best.captures.length).toList();
    return CkPlay(top[_ai.nextInt(top.length)]);
  }

  @override
  CkAction? onTap(CkState state, TesseraPickResult? pick,
      {Offset? local, Size? view}) {
    if (state is! CkPlaying || pick == null || !pick.hitTile) return null;
    final r = pick.tileY - _off, c = pick.tileX - _off;
    if (!_inBoard(r, c) || !_isDark(r, c)) return null;
    return CkTap(r * 8 + c);
  }

  @override
  List<GameButton<CkAction>> buttons(CkState state) => const [
        GameButton('New game', CkReset(), icon: Icons.refresh),
      ];

  @override
  List<TesseraScene>? onResize(TesseraController c, CkState state, double w, double h) {
    final fit = c.cameraFitDistance(tileIds: _cornerTileIds, padding: 0.07);
    if (fit == null || fit == _camDistance) return null;
    _camDistance = fit;
    return [_scene(state)];
  }

  @override
  String status(CkState state) {
    final b = state.board;
    if (state is CkOver) {
      final who = state.winner == red ? 'Red' : 'Black';
      return 'Game over — $who wins.';
    }
    final side = b.side == red ? 'Red' : 'Black';
    final sel = state is CkPlaying && state.selected >= 0
        ? '  ·  selected ${_squareName(state.selected)}'
        : '';
    final forced = ckMoves(b.sq, b.side).any((m) => m.isJump) ? '  ·  must jump' : '';
    return '$side to move  ·  Red ${b.count(red)}  Black ${b.count(black)}$sel$forced';
  }

  String _squareName(int s) =>
      '${String.fromCharCode('a'.codeUnitAt(0) + s % 8)}${s ~/ 8 + 1}';
}
