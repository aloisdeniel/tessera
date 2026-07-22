// Verifies the ported chess logic + model generation against the C reference.
//
// The move sequence below was captured from the C `examples/chess --demo
// --seed 0x1234abcd`. Because the Dart RNG, move ordering, and heuristic are a
// faithful port, the Dart engine must reproduce it ply-for-ply — a strong
// end-to-end check of the rules engine, legal-move generation, AI, and the
// controller, none of which need the native renderer.

import 'dart:typed_data';

import 'package:tessera_chess/chess_game.dart';
import 'package:tessera_chess/chess_gen.dart';
import 'package:test/test.dart';

// The first 30 plies the C example plays from the initial position, seed
// 0x1234abcd.
const _reference = <String>[
  'g1f3', 'b8c6', 'b1c3', 'g8f6', 'd2d3', 'd7d6', 'c1e3', 'c8f5', 'e3a7',
  'c6a7', 'd3d4', 'f5c2', 'd1c2', 'e7e6', 'c2h7', 'h8h7', 'e1c1', 'h7h2',
  'h1h2', 'f8e7', 'a2a4', 'c7c6', 'e2e3', 'g7g6', 'f1d3', 'f6g4', 'd3g6',
  'g4h2', 'f3h2', 'f7g6',
];

void main() {
  test('AI reproduces the C reference game (seed 0x1234abcd)', () {
    final rng = ChessRng(0x1234abcd);
    final c = Game();
    final played = <String>[];
    for (var i = 0; i < _reference.length && !c.gameOver; ++i) {
      final a = aiPickAction(c.state, rng);
      expect(a, isNotNull, reason: 'ran out of legal moves at ply $i');
      played.add(moveStr(a!));
      c.apply(a);
    }
    expect(played, _reference);
  });

  test('initial position has 20 legal moves', () {
    final g = GameState()..reset();
    expect(genLegal(g).length, 20);
  });

  test('controller keeps entity ids stable across a slide', () {
    final c = Game();
    // white plays g1f3 (a knight slide); its id must follow it to the new square.
    final knight = aiPickAction(c.state, ChessRng(0x1234abcd))!;
    final srcId = c.ids[knight.from];
    expect(srcId, isNonZero);
    c.apply(knight);
    expect(c.ids[knight.to], srcId, reason: 'mover keeps its id (slides)');
    expect(c.ids[knight.from], 0);
  });

  test('buildPieceGlb emits a well-formed GLB for every piece', () {
    for (var kind = chessPawn; kind <= chessKing; ++kind) {
      final glb = buildPieceGlb(kind, 1, const [0.9, 0.87, 0.8, 1.0]);
      expect(glb.length, greaterThan(20));
      final bd = ByteData.sublistView(glb);
      expect(bd.getUint32(0, Endian.little), 0x46546C67, reason: "'glTF' magic");
      expect(bd.getUint32(4, Endian.little), 2, reason: 'version 2');
      expect(bd.getUint32(8, Endian.little), glb.length, reason: 'total length');
      // First chunk is JSON.
      final jsonLen = bd.getUint32(12, Endian.little);
      expect(bd.getUint32(16, Endian.little), 0x4E4F534A, reason: "'JSON' chunk");
      // Second chunk is BIN and the container length adds up.
      final binHdr = 12 + 8 + jsonLen;
      final binLen = bd.getUint32(binHdr, Endian.little);
      expect(bd.getUint32(binHdr + 4, Endian.little), 0x004E4942,
          reason: "'BIN\\0' chunk");
      expect(binHdr + 8 + binLen, glb.length);
    }
  });
}
