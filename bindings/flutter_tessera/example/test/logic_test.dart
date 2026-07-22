// Pure-reducer tests for the three games — no rendering, no engine. Exercises
// the sealed state/action transitions so a logic regression fails in CI.
import 'dart:math' as math;

import 'package:flutter_test/flutter_test.dart';

import 'package:flutter_tessera_example/blackjack.dart';
import 'package:flutter_tessera_example/chess.dart';
import 'package:flutter_tessera_example/chess_gen.dart';
import 'package:flutter_tessera_example/chess_rules.dart';
import 'package:flutter_tessera_example/yahtzee.dart';

void main() {
  group('blackjack', () {
    test('deal reaches the player turn (or a natural)', () {
      BjState s = const BjTable(seed: 42);
      s = bjUpdate(s, const BjDeal());
      expect(s, isA<BjDealing>());
      // deal four cards
      var guard = 0;
      while (s is BjDealing && guard++ < 10) {
        s = bjUpdate(s, const BjDealStep());
      }
      expect(s, anyOf(isA<BjPlayerTurn>(), isA<BjRoundOver>()));
      expect(s.player.length + s.dealer.length, 4);
    });

    test('stand resolves the round with a dealer >= 17', () {
      BjState s = const BjTable(seed: 7);
      s = bjUpdate(s, const BjDeal());
      while (s is BjDealing) {
        s = bjUpdate(s, const BjDealStep());
      }
      if (s is BjPlayerTurn) {
        s = bjUpdate(s, const BjStand());
      }
      expect(s, isA<BjRoundOver>());
      expect(handValue(s.dealer).total, greaterThanOrEqualTo(17));
    });

    test('handValue counts aces softly', () {
      // A + 6 = 17 (soft); A + 6 + K = 17 (hard)
      expect(handValue([0, 5]).total, 17); // A(0), 6(5)
      expect(handValue([0, 5]).soft, isTrue);
      expect(handValue([0, 5, 12]).total, 17); // + K(12) -> ace drops to 1
      expect(handValue([0, 5, 12]).soft, isFalse);
    });
  });

  group('yahtzee', () {
    test('scoring categories', () {
      expect(scoreCategory(0, [1, 1, 3, 4, 1]), 3); // three ones
      expect(scoreCategory(11, [5, 5, 5, 5, 5]), 50); // yahtzee
      expect(scoreCategory(8, [2, 2, 5, 5, 5]), 25); // full house
      expect(scoreCategory(10, [1, 2, 3, 4, 5]), 40); // large straight
      expect(scoreCategory(9, [1, 2, 3, 4, 6]), 30); // small straight
      expect(scoreCategory(12, [6, 6, 6, 6, 6]), 30); // chance
    });

    test('roll then score advances the turn and fills the card', () {
      YState s = const YTurnStart(scorecard: {}, seed: 99);
      s = yUpdate(s, const YRoll());
      expect(s, isA<YRolled>());
      expect((s as YRolled).rollsLeft, 2);
      expect(s.dice.every((v) => v >= 1 && v <= 6), isTrue);
      s = yUpdate(s, const YScore(12)); // chance
      expect(s.scorecard.length, 1);
      expect(s, isA<YTurnStart>());
    });

    test('held dice keep their value across a roll', () {
      YState s = const YTurnStart(scorecard: {}, seed: 5);
      s = yUpdate(s, const YRoll()) as YRolled;
      final held = (s as YRolled).dice[0];
      s = yUpdate(s, const YToggleKeep(0));
      s = yUpdate(s, const YRoll());
      expect((s as YRolled).dice[0], held);
    });
  });

  group('chess', () {
    test('a legal move advances the ply and swaps side', () {
      ChessState s = ChessPlaying(ChessBoard.initial());
      final board = (s as ChessPlaying).board;
      expect(board.side, white);
      // e2 (12) -> e4 (28): select then move
      s = chessUpdate(s, const ChessTap(12));
      expect((s as ChessPlaying).selected, 12);
      s = chessUpdate(s, const ChessTap(28));
      final next = (s as ChessPlaying).board;
      expect(next.ply, 1);
      expect(next.side, black);
      expect(next.sq[28], chessPawn);
      expect(next.sq[12], 0);
    });

    test('the AI produces a legal reply', () {
      final s = ChessPlaying(ChessBoard.initial());
      final a = ChessController().autoAction(s, math.Random(0));
      expect(a, isA<ChessPlayMove>());
      final next = chessUpdate(s, a!);
      expect(next, isA<ChessPlaying>());
      expect((next as ChessPlaying).board.ply, 1);
    });
  });
}
