// chess_game.dart — the pure chess rules engine + heuristic AI + controller.
//
// A direct port of the rules half of examples/chess/main.c. The architecture is
// the classic reducer loop:
//
//     GameState + Action  --gameApply-->  GameState   (pure, no side effects)
//
// Board squares hold `kind` in 1..6 (chess*), sign carries color: +white,
// -black, 0 empty. The `Game` controller threads a persistent entity id through
// each square so the renderer animates a *slide* rather than a teleport.

import 'chess_gen.dart';

const int white = 0;
const int black = 1;

int sqOf(int f, int r) => r * 8 + f;
int fileOf(int s) => s & 7;
int rankOf(int s) => s >> 3;
int pieceKind(int v) => v < 0 ? -v : v;
int pieceColor(int v) => v < 0 ? black : white;

/// A single move.
class Action {
  Action(this.from, this.to, {this.promo = 0, this.isCastle = 0, this.isEp = 0});
  final int from;
  final int to;
  final int promo; // promoted kind, or 0
  final int isCastle; // 1 king-side, 2 queen-side
  final int isEp; // en-passant capture
}

/// The pure, immutable rules state (board, side, castling, en-passant).
class GameState {
  GameState()
      : sq = List<int>.filled(64, 0),
        side = white,
        castle = 0x0F,
        ep = -1,
        ply = 0;

  GameState.from(GameState o)
      : sq = List<int>.of(o.sq),
        side = o.side,
        castle = o.castle,
        ep = o.ep,
        ply = o.ply;

  final List<int> sq; // rank*8 + file
  int side; // side to move
  int castle; // bit0 WK, 1 WQ, 2 BK, 3 BQ
  int ep; // en-passant target square, or -1
  int ply;

  void reset() {
    for (var i = 0; i < 64; ++i) {
      sq[i] = 0;
    }
    const back = [
      chessRook, chessKnight, chessBishop, chessQueen, //
      chessKing, chessBishop, chessKnight, chessRook,
    ];
    for (var f = 0; f < 8; ++f) {
      sq[sqOf(f, 0)] = back[f]; // white back rank
      sq[sqOf(f, 1)] = chessPawn; // white pawns
      sq[sqOf(f, 6)] = -chessPawn; // black pawns
      sq[sqOf(f, 7)] = -back[f]; // black back rank
    }
    side = white;
    castle = 0x0F;
    ep = -1;
    ply = 0;
  }
}

/// Is square [s] attacked by side [by] (white/black)?
bool isAttacked(GameState g, int s, int by) {
  final tf = fileOf(s), tr = rankOf(s);
  final sign = (by == white) ? 1 : -1;

  // pawns (attack "forward" from `by`'s perspective)
  final pr = tr - (by == white ? 1 : -1);
  if (pr >= 0 && pr < 8) {
    for (var df = -1; df <= 1; df += 2) {
      final pf = tf + df;
      if (pf < 0 || pf > 7) continue;
      if (g.sq[sqOf(pf, pr)] == sign * chessPawn) return true;
    }
  }
  // knights
  const kn = [
    [1, 2], [2, 1], [2, -1], [1, -2], //
    [-1, -2], [-2, -1], [-2, 1], [-1, 2],
  ];
  for (var i = 0; i < 8; ++i) {
    final f = tf + kn[i][0], r = tr + kn[i][1];
    if (f < 0 || f > 7 || r < 0 || r > 7) continue;
    if (g.sq[sqOf(f, r)] == sign * chessKnight) return true;
  }
  // king
  for (var df = -1; df <= 1; ++df) {
    for (var dr = -1; dr <= 1; ++dr) {
      if (df == 0 && dr == 0) continue;
      final f = tf + df, r = tr + dr;
      if (f < 0 || f > 7 || r < 0 || r > 7) continue;
      if (g.sq[sqOf(f, r)] == sign * chessKing) return true;
    }
  }
  // sliders: rook/queen orthogonal, bishop/queen diagonal
  const orth = [
    [1, 0], [-1, 0], [0, 1], [0, -1],
  ];
  const diag = [
    [1, 1], [1, -1], [-1, 1], [-1, -1],
  ];
  for (var d = 0; d < 4; ++d) {
    var f = tf, r = tr;
    for (;;) {
      f += orth[d][0];
      r += orth[d][1];
      if (f < 0 || f > 7 || r < 0 || r > 7) break;
      final v = g.sq[sqOf(f, r)];
      if (v == 0) continue;
      if (pieceColor(v) == by &&
          (pieceKind(v) == chessRook || pieceKind(v) == chessQueen)) {
        return true;
      }
      break;
    }
  }
  for (var d = 0; d < 4; ++d) {
    var f = tf, r = tr;
    for (;;) {
      f += diag[d][0];
      r += diag[d][1];
      if (f < 0 || f > 7 || r < 0 || r > 7) break;
      final v = g.sq[sqOf(f, r)];
      if (v == 0) continue;
      if (pieceColor(v) == by &&
          (pieceKind(v) == chessBishop || pieceKind(v) == chessQueen)) {
        return true;
      }
      break;
    }
  }
  return false;
}

int kingSquare(GameState g, int color) {
  final want = (color == white ? 1 : -1) * chessKing;
  for (var s = 0; s < 64; ++s) {
    if (g.sq[s] == want) return s;
  }
  return -1;
}

/// (PrevState, Action) -> NewState — pure.
GameState gameApply(GameState prev, Action a) {
  final next = GameState.from(prev);
  final moving = next.sq[a.from];
  final color = pieceColor(moving);
  final kind = pieceKind(moving);

  next.ep = -1;

  // en-passant: remove the pawn that was passed
  if (a.isEp != 0) {
    final capR = rankOf(a.to) - (color == white ? 1 : -1);
    next.sq[sqOf(fileOf(a.to), capR)] = 0;
  }

  // move the piece (with promotion)
  next.sq[a.from] = 0;
  next.sq[a.to] = a.promo != 0 ? (color == white ? 1 : -1) * a.promo : moving;

  // castling: shuffle the rook
  if (a.isCastle != 0) {
    final r = rankOf(a.from);
    if (a.isCastle == 1) {
      next.sq[sqOf(5, r)] = next.sq[sqOf(7, r)];
      next.sq[sqOf(7, r)] = 0;
    } else {
      next.sq[sqOf(3, r)] = next.sq[sqOf(0, r)];
      next.sq[sqOf(0, r)] = 0;
    }
  }

  // double pawn push sets the ep target
  if (kind == chessPawn && (rankOf(a.to) - rankOf(a.from)).abs() == 2) {
    next.ep = sqOf(fileOf(a.from), (rankOf(a.from) + rankOf(a.to)) ~/ 2);
  }

  // update castling rights
  if (kind == chessKing) next.castle &= (color == white) ? ~0x03 : ~0x0C;
  if (a.from == sqOf(0, 0) || a.to == sqOf(0, 0)) next.castle &= ~0x02;
  if (a.from == sqOf(7, 0) || a.to == sqOf(7, 0)) next.castle &= ~0x01;
  if (a.from == sqOf(0, 7) || a.to == sqOf(0, 7)) next.castle &= ~0x08;
  if (a.from == sqOf(7, 7) || a.to == sqOf(7, 7)) next.castle &= ~0x04;

  next.side = color ^ 1;
  next.ply = prev.ply + 1;
  return next;
}

// ---- move generation (pseudo-legal, then filtered to fully legal) ----

void _addPawn(List<Action> list, int from, int to, bool promo, bool ep) {
  if (promo) {
    list.add(Action(from, to, promo: chessQueen));
  } else {
    list.add(Action(from, to, isEp: ep ? 1 : 0));
  }
}

List<Action> genPseudo(GameState g) {
  final list = <Action>[];
  final me = g.side;
  for (var s = 0; s < 64; ++s) {
    final v = g.sq[s];
    if (v == 0 || pieceColor(v) != me) continue;
    final f = fileOf(s), r = rankOf(s), kind = pieceKind(v);

    if (kind == chessPawn) {
      final dr = (me == white) ? 1 : -1;
      final start = (me == white) ? 1 : 6, last = (me == white) ? 7 : 0;
      final r1 = r + dr;
      if (r1 >= 0 && r1 < 8 && g.sq[sqOf(f, r1)] == 0) {
        _addPawn(list, s, sqOf(f, r1), r1 == last, false);
        final r2 = r + 2 * dr;
        if (r == start && g.sq[sqOf(f, r2)] == 0) {
          _addPawn(list, s, sqOf(f, r2), false, false);
        }
      }
      for (var df = -1; df <= 1; df += 2) {
        final cf = f + df;
        if (cf < 0 || cf > 7 || r1 < 0 || r1 >= 8) continue;
        final ts = sqOf(cf, r1);
        final tv = g.sq[ts];
        if (tv != 0 && pieceColor(tv) != me) {
          _addPawn(list, s, ts, r1 == last, false);
        } else if (ts == g.ep) {
          _addPawn(list, s, ts, false, true);
        }
      }
    } else if (kind == chessKnight) {
      const kn = [
        [1, 2], [2, 1], [2, -1], [1, -2], //
        [-1, -2], [-2, -1], [-2, 1], [-1, 2],
      ];
      for (var i = 0; i < 8; ++i) {
        final nf = f + kn[i][0], nr = r + kn[i][1];
        if (nf < 0 || nf > 7 || nr < 0 || nr > 7) continue;
        final tv = g.sq[sqOf(nf, nr)];
        if (tv == 0 || pieceColor(tv) != me) list.add(Action(s, sqOf(nf, nr)));
      }
    } else if (kind == chessKing) {
      for (var df = -1; df <= 1; ++df) {
        for (var dr = -1; dr <= 1; ++dr) {
          if (df == 0 && dr == 0) continue;
          final nf = f + df, nr = r + dr;
          if (nf < 0 || nf > 7 || nr < 0 || nr > 7) continue;
          final tv = g.sq[sqOf(nf, nr)];
          if (tv == 0 || pieceColor(tv) != me) list.add(Action(s, sqOf(nf, nr)));
        }
      }
      // castling: rights set, squares empty, king not passing through check
      final hr = (me == white) ? 0 : 7;
      final kbit = (me == white) ? 0x01 : 0x04;
      final qbit = (me == white) ? 0x02 : 0x08;
      if ((g.castle & kbit) != 0 &&
          g.sq[sqOf(5, hr)] == 0 &&
          g.sq[sqOf(6, hr)] == 0 &&
          !isAttacked(g, sqOf(4, hr), me ^ 1) &&
          !isAttacked(g, sqOf(5, hr), me ^ 1) &&
          !isAttacked(g, sqOf(6, hr), me ^ 1)) {
        list.add(Action(sqOf(4, hr), sqOf(6, hr), isCastle: 1));
      }
      if ((g.castle & qbit) != 0 &&
          g.sq[sqOf(3, hr)] == 0 &&
          g.sq[sqOf(2, hr)] == 0 &&
          g.sq[sqOf(1, hr)] == 0 &&
          !isAttacked(g, sqOf(4, hr), me ^ 1) &&
          !isAttacked(g, sqOf(3, hr), me ^ 1) &&
          !isAttacked(g, sqOf(2, hr), me ^ 1)) {
        list.add(Action(sqOf(4, hr), sqOf(2, hr), isCastle: 2));
      }
    } else {
      // sliders
      const orth = [
        [1, 0], [-1, 0], [0, 1], [0, -1],
      ];
      const diag = [
        [1, 1], [1, -1], [-1, 1], [-1, -1],
      ];
      const all = [
        [1, 0], [-1, 0], [0, 1], [0, -1], //
        [1, 1], [1, -1], [-1, 1], [-1, -1],
      ];
      final List<List<int>> dirs;
      if (kind == chessRook) {
        dirs = orth;
      } else if (kind == chessBishop) {
        dirs = diag;
      } else {
        dirs = all; // queen
      }
      for (var d = 0; d < dirs.length; ++d) {
        var nf = f, nr = r;
        for (;;) {
          nf += dirs[d][0];
          nr += dirs[d][1];
          if (nf < 0 || nf > 7 || nr < 0 || nr > 7) break;
          final tv = g.sq[sqOf(nf, nr)];
          if (tv == 0) {
            list.add(Action(s, sqOf(nf, nr)));
            continue;
          }
          if (pieceColor(tv) != me) list.add(Action(s, sqOf(nf, nr)));
          break;
        }
      }
    }
  }
  return list;
}

/// Fully legal moves: pseudo-legal minus those leaving our king in check.
List<Action> genLegal(GameState g) {
  final pseudo = genPseudo(g);
  final me = g.side;
  final out = <Action>[];
  for (final a in pseudo) {
    final nx = gameApply(g, a);
    final ks = kingSquare(nx, me);
    if (ks < 0 || !isAttacked(nx, ks, me ^ 1)) out.add(a);
  }
  return out;
}

// ---- the "player": pick an item and move it (heuristic over legal moves) ----

/// Deterministic xorshift RNG matching the C example, so `--seed` reproduces
/// the same game.
class ChessRng {
  ChessRng([this._state = 0x1234abcd]);
  int _state;

  set seed(int s) => _state = s & 0xFFFFFFFF;

  int next() {
    _state ^= (_state << 13) & 0xFFFFFFFF;
    _state ^= _state >> 17;
    _state ^= (_state << 5) & 0xFFFFFFFF;
    _state &= 0xFFFFFFFF;
    return _state;
  }
}

int _pieceValue(int kind) {
  const val = [0, 1, 3, 3, 5, 9, 0];
  return val[kind];
}

/// Pick a move for the side to move, or null if there are none (game over).
Action? aiPickAction(GameState g, ChessRng rng) {
  final moves = genLegal(g);
  if (moves.isEmpty) return null;

  var best = -1000000, bestI = 0;
  for (var i = 0; i < moves.length; ++i) {
    final a = moves[i];
    var score = 0;
    final victim = g.sq[a.to];
    if (victim != 0) score += 100 * _pieceValue(pieceKind(victim)); // grab material
    if (a.promo != 0) score += 90;
    if (a.isCastle != 0) score += 25; // king safety
    // centre control for the destination
    final df = fileOf(a.to), dr = rankOf(a.to);
    final cf = df < 4 ? df : 7 - df, cr = dr < 4 ? dr : 7 - dr;
    score += (cf + cr) * 2;
    // develop minor pieces off the back rank early
    final kind = pieceKind(g.sq[a.from]);
    final home = (g.side == white) ? 0 : 7;
    if ((kind == chessKnight || kind == chessBishop) && rankOf(a.from) == home) {
      score += 14;
    }
    if (kind == chessPawn) score += 4;
    // don't shuffle the king around for no reason
    if (kind == chessKing && a.isCastle == 0) score -= 12;
    // avoid moving onto a square attacked by an enemy pawn
    final nx = gameApply(g, a);
    if (isAttacked(nx, a.to, g.side ^ 1)) score -= 8 * _pieceValue(kind);
    score += rng.next() % 6; // variety
    if (score > best) {
      best = score;
      bestI = i;
    }
  }
  return moves[bestI];
}

// ---- controller: threads a persistent entity id through each square ----

/// Threads a stable entity id through each square so the renderer animates a
/// move (a piece sliding) rather than a teleport.
class Game {
  Game() {
    reset();
  }

  final GameState state = GameState();
  final List<int> ids = List<int>.filled(64, 0); // entity id per square (0 = none)
  Action? last; // last action applied (for logging)
  bool gameOver = false;

  void reset() {
    state.reset();
    for (var i = 0; i < 64; ++i) {
      ids[i] = 0;
    }
    var id = 1;
    for (var s = 0; s < 64; ++s) {
      if (state.sq[s] != 0) ids[s] = id++;
    }
    last = null;
    gameOver = false;
  }

  /// Apply an action to BOTH the rules state and the id grid, in lockstep.
  void apply(Action a) {
    final color = pieceColor(state.sq[a.from]);

    if (a.isEp != 0) {
      final capR = rankOf(a.to) - (color == white ? 1 : -1);
      ids[sqOf(fileOf(a.to), capR)] = 0; // captured pawn vanishes
    }
    ids[a.to] = ids[a.from]; // mover keeps its id (slides)
    ids[a.from] = 0;
    if (a.isCastle != 0) {
      final r = rankOf(a.from);
      if (a.isCastle == 1) {
        ids[sqOf(5, r)] = ids[sqOf(7, r)];
        ids[sqOf(7, r)] = 0;
      } else {
        ids[sqOf(3, r)] = ids[sqOf(0, r)];
        ids[sqOf(0, r)] = 0;
      }
    }

    final nx = gameApply(state, a);
    state
      ..sq.setAll(0, nx.sq)
      ..side = nx.side
      ..castle = nx.castle
      ..ep = nx.ep
      ..ply = nx.ply;
    last = a;

    if (genLegal(state).isEmpty) gameOver = true;
  }
}

/// Algebraic-ish move string, e.g. "e2e4".
String moveStr(Action a) {
  final ff = String.fromCharCode('a'.codeUnitAt(0) + fileOf(a.from));
  final tf = String.fromCharCode('a'.codeUnitAt(0) + fileOf(a.to));
  return '$ff${rankOf(a.from) + 1}$tf${rankOf(a.to) + 1}';
}
