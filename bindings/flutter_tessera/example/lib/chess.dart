// chess.dart — the Chess example on the shared reducer framework (see game.dart).
//
// Sealed action + sealed state, a pure reducer `chessUpdate`, and a controller
// that projects the board into a scene (tiles + piece entities). The rules /
// AI live in chess_rules.dart; the piece meshes in chess_gen.dart.

import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter_tessera/flutter_tessera.dart';

import 'chess_gen.dart';
import 'chess_rules.dart';
import 'game.dart';

const int boardOff = -4; // file/rank 0..7 -> world -4..3

int tileId(int square) => square + 1;

int coordToSquare(int x, int y) {
  final f = x - boardOff, r = y - boardOff;
  if (f < 0 || f > 7 || r < 0 || r > 7) return -1;
  return sqOf(f, r);
}

// ---- immutable board (rules snapshot + stable entity ids) ----------------
class ChessBoard {
  const ChessBoard({
    required this.sq,
    required this.side,
    required this.castle,
    required this.ep,
    required this.ply,
    required this.ids,
  });

  final List<int> sq;
  final int side;
  final int castle;
  final int ep;
  final int ply;
  final List<int> ids; // entity id per square (0 = none)

  factory ChessBoard.initial() {
    final g = GameState()..reset();
    final ids = List<int>.filled(64, 0);
    var id = 1;
    for (var s = 0; s < 64; s++) {
      if (g.sq[s] != 0) ids[s] = id++;
    }
    return ChessBoard(
        sq: List<int>.of(g.sq), side: g.side, castle: g.castle, ep: g.ep, ply: g.ply, ids: ids);
  }

  GameState toGameState() {
    final g = GameState();
    g.sq.setAll(0, sq);
    g
      ..side = side
      ..castle = castle
      ..ep = ep
      ..ply = ply;
    return g;
  }

  /// Apply [m] to the rules AND thread each square's stable id so the renderer
  /// animates a slide (and a capture fades the victim out).
  ChessBoard applyMove(Move m) {
    final nids = List<int>.of(ids);
    final color = pieceColor(sq[m.from]);
    if (m.isEp != 0) {
      final capR = rankOf(m.to) - (color == white ? 1 : -1);
      nids[sqOf(fileOf(m.to), capR)] = 0;
    }
    nids[m.to] = nids[m.from];
    nids[m.from] = 0;
    if (m.isCastle != 0) {
      final r = rankOf(m.from);
      if (m.isCastle == 1) {
        nids[sqOf(5, r)] = nids[sqOf(7, r)];
        nids[sqOf(7, r)] = 0;
      } else {
        nids[sqOf(3, r)] = nids[sqOf(0, r)];
        nids[sqOf(0, r)] = 0;
      }
    }
    final nx = gameApply(toGameState(), m);
    return ChessBoard(
        sq: List<int>.of(nx.sq), side: nx.side, castle: nx.castle, ep: nx.ep, ply: nx.ply, ids: nids);
  }
}

// ---- actions -------------------------------------------------------------
sealed class ChessAction {
  const ChessAction();
}

/// The human tapped board square [square] (select, or move the selection here).
class ChessTap extends ChessAction {
  const ChessTap(this.square);
  final int square;
}

/// Play a fully-resolved [move] (chosen by the AI in autoAction).
class ChessPlayMove extends ChessAction {
  const ChessPlayMove(this.move);
  final Move move;
}

/// Start a new game.
class ChessReset extends ChessAction {
  const ChessReset();
}

// ---- state ---------------------------------------------------------------
sealed class ChessState {
  const ChessState(this.board);
  final ChessBoard board;
}

/// A game in progress; [selected] is the picked source square (or -1).
/// [lastMove] (if any) drives the knight's L-shaped glide in `render`.
class ChessPlaying extends ChessState {
  const ChessPlaying(super.board, {this.selected = -1, this.lastMove});
  final int selected;
  final Move? lastMove;
}

/// Checkmate or stalemate — the side to move has no legal reply.
class ChessOver extends ChessState {
  const ChessOver(super.board, {this.lastMove});
  final Move? lastMove;
}

// ---- reducer -------------------------------------------------------------
ChessState chessUpdate(ChessState s, ChessAction a) {
  switch (a) {
    case ChessReset():
      return ChessPlaying(ChessBoard.initial());
    case ChessPlayMove(:final move):
      return s is ChessPlaying ? _apply(s.board, move) : s;
    case ChessTap(:final square):
      return s is ChessPlaying ? _tap(s, square) : s;
  }
}

ChessState _apply(ChessBoard board, Move m) {
  final nb = board.applyMove(m);
  return genLegal(nb.toGameState()).isEmpty
      ? ChessOver(nb, lastMove: m)
      : ChessPlaying(nb, lastMove: m);
}

ChessState _tap(ChessPlaying s, int sq) {
  final b = s.board;
  final ownPiece = b.sq[sq] != 0 && pieceColor(b.sq[sq]) == b.side;
  if (s.selected < 0) {
    return ownPiece ? ChessPlaying(b, selected: sq) : s;
  }
  for (final m in genLegal(b.toGameState())) {
    if (m.from == s.selected && m.to == sq) return _apply(b, m);
  }
  return ChessPlaying(b, selected: ownPiece ? sq : -1);
}

// ---- controller (rendering + wiring) -------------------------------------
class ChessController extends GameController<ChessState, ChessAction> {
  final ChessRng _rng = ChessRng(0x1234abcd);
  double _camDistance = 11.5;
  int _tileLight = 0;
  int _tileDark = 0;
  final List<List<int>> _piece = List.generate(2, (_) => List<int>.filled(7, 0));

  @override
  String get title => 'Chess';
  @override
  String get subtitle => 'Entities · tap a piece then a square, or watch the AI';
  @override
  IconData get icon => Icons.grid_view;

  @override
  TesseraLightData get light => const TesseraLightData(
        dir: [-0.4, -1.0, -0.5],
        color: [1.0, 0.97, 0.9],
        intensity: 1.15,
        ambient: [0.3, 0.33, 0.4],
      );

  @override
  TesseraTimingData get timing => const TesseraTimingData(
        moveS: 0.5,
        addS: 0.35,
        removeS: 0.4,
        tileS: 0.4,
        reflowS: 0.4,
        cameraS: 0.9,
      );

  @override
  Future<void> registerDefs(TesseraController c) async {
    _tileLight = c.registerTileType(
        const TesseraTileType(thickness: 0.22, tint: [0.82, 0.78, 0.68, 1.0]));
    _tileDark = c.registerTileType(
        const TesseraTileType(thickness: 0.22, tint: [0.32, 0.36, 0.42, 1.0]));

    const whiteCol = [0.9, 0.87, 0.8, 1.0];
    const blackCol = [0.16, 0.17, 0.2, 1.0];

    final poof = c.registerEffectType(const TesseraEffectType(
      onRemove: TesseraParticle(
        count: 44,
        lifetime: 0.7,
        lifetimeVar: 0.25,
        speed: 1.6,
        speedVar: 0.6,
        spreadDeg: 55,
        gravity: 0.8,
        sizeStart: 0.14,
        sizeEnd: 0.5,
        colorStart: [0.85, 0.85, 0.9, 0.75],
        colorEnd: [0.5, 0.5, 0.55, 0.0],
      ),
    ));

    for (var color = 0; color < 2; color++) {
      final col = (color == white) ? whiteCol : blackCol;
      final face = (color == white) ? 1 : -1;
      for (var kind = chessPawn; kind <= chessKing; kind++) {
        _piece[color][kind] = c.registerEntityType(TesseraEntityType(
          glb: buildPieceGlb(kind, face, col),
          scale: 1.1,
          onDespawnEffect: poof,
        ));
      }
    }
  }

  @override
  ChessState initial() => ChessPlaying(ChessBoard.initial());

  @override
  ChessState update(ChessState state, ChessAction action) => chessUpdate(state, action);

  List<int> get _cornerTileIds =>
      [tileId(sqOf(0, 0)), tileId(sqOf(7, 0)), tileId(sqOf(0, 7)), tileId(sqOf(7, 7))];

  /// An L-shaped waypoint list for a knight hop: travel the long (2-square) axis
  /// first, then the short one. The final waypoint is the destination itself
  /// (the engine snaps the last step to the piece's layout target).
  List<(int, int)> _knightPath(int from, int to) {
    final f0 = fileOf(from), r0 = rankOf(from);
    final f1 = fileOf(to), r1 = rankOf(to);
    final (cf, cr) = ((f1 - f0).abs() == 2) ? (f1, r0) : (f0, r1);
    return [(cf + boardOff, cr + boardOff), (f1 + boardOff, r1 + boardOff)];
  }

  // The camera faces the side to move: white plays from yaw π (white at the
  // bottom), black from yaw 0. The board sweeps 180° to the mover's side on each
  // ply, so whoever is up always sees the board from their own perspective.
  TesseraCameraPose _cameraFor(int side) => TesseraCameraPose(
        focusX: boardOff + 3.5,
        focusY: boardOff + 3.5,
        distance: _camDistance,
        yaw: side == white ? 3.14159 : 0.0,
        pitch: 0.82,
        fov: 0.72,
      );

  /// Ground markers, one per square (the engine keys overlays by coord, so a
  /// legal-move marker simply replaces the last-move tint when they collide):
  /// the last move keeps a subtle steady tint on its from/to squares, and while
  /// a piece is selected its legal destinations show as green discs (quiet
  /// moves) or pulsing red rings (captures, en passant included).
  List<TesseraOverlay> _overlays(ChessBoard b, Move? lm, int selected) {
    final bySquare = <int, TesseraOverlay>{};
    if (lm != null) {
      for (final sq in [lm.from, lm.to]) {
        bySquare[sq] = TesseraOverlay(
          x: fileOf(sq) + boardOff,
          y: rankOf(sq) + boardOff,
          tint: const [0.95, 0.82, 0.35, 0.28],
        );
      }
    }
    if (selected >= 0) {
      for (final m in genLegal(b.toGameState())) {
        if (m.from != selected) continue;
        final capture = b.sq[m.to] != 0 || m.isEp != 0;
        bySquare[m.to] = capture
            ? TesseraOverlay(
                x: fileOf(m.to) + boardOff,
                y: rankOf(m.to) + boardOff,
                shape: TesseraOverlayShape.ring,
                tint: const [0.95, 0.28, 0.22, 0.85],
                pulseS: 1.2,
                pulseAlphaMin: 0.5,
                pulseAlphaMax: 0.9,
                pulseScaleMin: 0.92,
                pulseScaleMax: 1.04,
              )
            : TesseraOverlay(
                x: fileOf(m.to) + boardOff,
                y: rankOf(m.to) + boardOff,
                tint: const [0.35, 0.85, 0.45, 0.5],
              );
      }
    }
    return bySquare.values.toList();
  }

  /// A golden pulsing outline on the selected piece, and a red glow on a king
  /// standing in check (which is how checkmate reads on a finished board too).
  /// The two never stack: if the checked king is itself the selection, the
  /// outline wins — highlights are keyed by (kind, id), one per object.
  List<TesseraHighlight> _highlights(ChessBoard b, int selected) {
    final out = <TesseraHighlight>[];
    if (selected >= 0 && b.ids[selected] != 0) {
      out.add(TesseraHighlight(
        targetId: b.ids[selected],
        color: const [1.0, 0.85, 0.25, 1.0],
        thickness: 3,
        pulseS: 1.4,
        pulseMin: 0.55,
        pulseMax: 1.0,
      ));
    }
    final g = b.toGameState();
    final ks = kingSquare(g, b.side);
    if (ks >= 0 && ks != selected && b.ids[ks] != 0 && isAttacked(g, ks, b.side ^ 1)) {
      out.add(TesseraHighlight(
        targetId: b.ids[ks],
        style: TesseraHighlightStyle.glow,
        color: const [1.0, 0.25, 0.18, 1.0],
        pulseS: 1.0,
        pulseMin: 0.4,
        pulseMax: 1.0,
      ));
    }
    return out;
  }

  TesseraScene _scene(ChessState s) {
    final b = s.board;
    final lm = switch (s) {
      ChessPlaying(:final lastMove) => lastMove,
      ChessOver(:final lastMove) => lastMove,
    };
    final selected = switch (s) {
      ChessPlaying(:final selected) => selected,
      ChessOver() => -1,
    };
    final tiles = <TesseraTile>[];
    final entities = <TesseraEntity>[];
    for (var r = 0; r < 8; r++) {
      for (var f = 0; f < 8; f++) {
        final td = ((f + r) & 1) != 0 ? _tileLight : _tileDark;
        tiles.add(TesseraTile(x: f + boardOff, y: r + boardOff, def: td, id: tileId(sqOf(f, r))));
      }
    }
    for (var sq = 0; sq < 64; sq++) {
      final v = b.sq[sq];
      if (v == 0 || b.ids[sq] == 0) continue;
      entities.add(TesseraEntity(
        id: b.ids[sq],
        def: _piece[pieceColor(v)][pieceKind(v)],
        x: fileOf(sq) + boardOff,
        y: rankOf(sq) + boardOff,
        // A knight can't slide in a straight line without cutting through the
        // board, so route it through an L corner (long axis first). Other pieces
        // move in a line, so a plain glide is correct.
        path: (lm != null && sq == lm.to && pieceKind(v) == chessKnight)
            ? _knightPath(lm.from, lm.to)
            : const [],
      ));
    }
    return TesseraScene(
      tiles: tiles,
      entities: entities,
      overlays: _overlays(b, lm, selected),
      highlights: _highlights(b, selected),
      camera: _cameraFor(b.side),
      epoch: b.ply,
    );
  }

  @override
  List<TesseraScene> render(ChessState s) => [_scene(s)];

  @override
  ChessAction? autoAction(ChessState state, math.Random rng) {
    if (state is! ChessPlaying) return null;
    final m = aiPickAction(state.board.toGameState(), _rng);
    return m == null ? null : ChessPlayMove(m);
  }

  @override
  ChessAction? onTap(ChessState state, TesseraPickResult? pick,
      {Offset? local, Size? view}) {
    if (state is! ChessPlaying || pick == null || !pick.hitTile) return null;
    final sq = coordToSquare(pick.tileX, pick.tileY);
    return sq < 0 ? null : ChessTap(sq);
  }

  @override
  List<GameButton<ChessAction>> buttons(ChessState state) => const [
        GameButton('New game', ChessReset(), icon: Icons.refresh),
      ];

  @override
  List<TesseraScene>? onResize(TesseraController c, ChessState state, double w, double h) {
    final fit = c.cameraFitDistance(tileIds: _cornerTileIds, padding: 0.06);
    if (fit == null || fit == _camDistance) return null;
    _camDistance = fit;
    return [_scene(state)];
  }

  /// Is the side to move's king attacked? (Drives the glow and the status.)
  bool _inCheck(ChessBoard b) {
    final g = b.toGameState();
    final ks = kingSquare(g, b.side);
    return ks >= 0 && isAttacked(g, ks, b.side ^ 1);
  }

  @override
  String status(ChessState state) {
    final b = state.board;
    if (state is ChessOver) {
      final loser = b.side == white ? 'White' : 'Black';
      final winner = b.side == white ? 'Black' : 'White';
      return _inCheck(b)
          ? 'Checkmate — $winner wins.'
          : 'Stalemate — $loser has no legal move.';
    }
    final side = b.side == white ? 'White' : 'Black';
    final chk = _inCheck(b) ? '  ·  check!' : '';
    final sel = state is ChessPlaying && state.selected >= 0
        ? '  ·  selected ${_squareName(state.selected)}'
        : '';
    return '$side to move$chk  ·  ply ${b.ply}$sel';
  }

  String _squareName(int s) =>
      '${String.fromCharCode('a'.codeUnitAt(0) + fileOf(s))}${rankOf(s) + 1}';
}
