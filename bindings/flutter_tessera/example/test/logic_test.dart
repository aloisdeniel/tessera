// Pure-reducer tests for the three games — no rendering, no engine. Exercises
// the sealed state/action transitions so a logic regression fails in CI.
import 'dart:math' as math;

import 'package:flutter_test/flutter_test.dart';
import 'package:flutter_tessera/flutter_tessera.dart'
    show
        TesseraCameraFocusCard,
        TesseraCameraFocusHand,
        TesseraCameraPose,
        TesseraCard,
        TesseraScene;

import 'package:flutter_tessera_example/blackjack.dart';
import 'package:flutter_tessera_example/chess.dart';
import 'package:flutter_tessera_example/chess_gen.dart';
import 'package:flutter_tessera_example/chess_rules.dart';
import 'package:flutter_tessera_example/duel.dart';
import 'package:flutter_tessera_example/dungeon.dart';
import 'package:flutter_tessera_example/dungeon_art.dart';
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

  group('dungeon', () {
    DgState walk(DgState s) {
      var guard = 0;
      while (s is DgMoving && guard++ < 40) {
        s = dgUpdate(s, const DgStep());
      }
      return s;
    }

    final base = Core.initial();

    test('a roll produces a bounded move, clamped at a blocking monster', () {
      // Monster sits at idx 8; standing on 7, any roll must stop on 8.
      final s = dgUpdate(DgRoll(base.copy(heroPos: 7)), const DgRollMove());
      expect(s, isA<DgMoving>());
      expect((s as DgMoving).target, 8);
      expect(walk(s), isA<DgCombat>()); // arriving on a monster starts a duel
    });

    test('landing on a loot tile draws a card, then returns to the roll', () {
      final drew = walk(DgMoving(base.copy(heroPos: 5), 6)); // loot at idx 6
      expect(drew, isA<DgDrew>());
      expect((drew as DgDrew).c.hand.length, 1);
      expect(drew.c.deck.length, base.deck.length - 1);
      expect(dgUpdate(drew, const DgAfterDraw()), isA<DgRoll>());
    });

    test('reaching the last tile wins', () {
      expect(walk(DgMoving(base.copy(heroPos: kGoal - 1), kGoal)), isA<DgWon>());
    });

    test('winning a duel removes the monster', () {
      final combat = DgCombat(base,
          mon: 0, heroSeed: 1, monSeed: 2, heroDie: 6, monDie: 1, bonus: 0);
      final r = dgUpdate(combat, const DgFight());
      expect(r, isA<DgRoll>());
      expect((r as DgRoll).c.monsters[0].alive, isFalse);
    });

    test('losing a duel costs a life; a shield soaks the hit instead', () {
      final combat = DgCombat(base,
          mon: 0, heroSeed: 1, monSeed: 2, heroDie: 1, monDie: 6, bonus: 0);
      expect((dgUpdate(combat, const DgFight()) as DgRoll).c.hp, kMaxHp - 1);

      final shielded = base.copy(hand: const [ItemCard(1, itemShield)]);
      final r = dgUpdate(
          DgCombat(shielded,
              mon: 0, heroSeed: 1, monSeed: 2, heroDie: 1, monDie: 6, bonus: 0),
          const DgFight()) as DgRoll;
      expect(r.c.hp, kMaxHp); // no life lost
      expect(r.c.handCount(itemShield), 0); // shield consumed
    });

    test('playing a sword adds to the attack and discards the card', () {
      final armed = base.copy(hand: const [ItemCard(2, itemSword)]);
      final s = dgUpdate(
          DgCombat(armed,
              mon: 0, heroSeed: 1, monSeed: 2, heroDie: 3, monDie: 6, bonus: 0),
          const DgPlaySword()) as DgCombat;
      expect(s.bonus, kSwordBonus);
      expect(s.heroTotal, 3 + kSwordBonus);
      expect(s.c.handCount(itemSword), 0);
    });

    test('a potion heals one life and is consumed', () {
      final hurt = base.copy(hp: 1, hand: const [ItemCard(3, itemPotion)]);
      final r = dgUpdate(DgRoll(hurt), const DgDrinkPotion()) as DgRoll;
      expect(r.c.hp, 2);
      expect(r.c.handCount(itemPotion), 0);
    });
  });

  group('duel', () {
    /// Deal through to the first player turn.
    DuelYourTurn dealt({int seed = 0xD0E1}) {
      DuelState s = DuelIdle(seed: seed);
      s = duelUpdate(s, const DlStart());
      var guard = 0;
      while (s is DuelDealing && guard++ < 20) {
        s = duelUpdate(s, const DlDealStep());
      }
      return s as DuelYourTurn;
    }

    test('dealing alternates to 4 cards each, then round 1 with 1 mana', () {
      final s = dealt();
      expect(s.you.hand.length, duelHandTarget);
      expect(s.foe.hand.length, duelHandTarget);
      expect(s.turn, 1);
      expect(s.you.mana, 1);
      expect(s.you.deck.length, 2 * duelSpecs.length - duelHandTarget);
      // every dealt instance id is unique
      final ids = [...s.you.hand, ...s.foe.hand].map((h) => h.id).toSet();
      expect(ids.length, duelHandTarget * 2);
    });

    test('playing a card spends mana, fills a slot, and is not ready', () {
      final s = dealt();
      final affordable =
          s.you.hand.firstWhere((h) => duelSpecs[h.type].cost <= s.you.mana);
      final after =
          duelUpdate(s, DlPlayCard(affordable.id)) as DuelYourTurn;
      expect(after.you.hand.length, s.you.hand.length - 1);
      expect(after.you.creatures.single.id, affordable.id);
      expect(after.you.creatures.single.ready, isFalse);
      expect(after.you.mana, s.you.mana - duelSpecs[affordable.type].cost);
      // an unaffordable card is refused outright
      final rich = s.you.hand
          .where((h) => duelSpecs[h.type].cost > s.you.mana)
          .firstOrNull;
      if (rich != null) {
        expect(identical(duelUpdate(s, DlPlayCard(rich.id)), s), isTrue);
      }
    });

    test('a fresh creature cannot attack; it readies next round', () {
      var s = dealt();
      final card =
          s.you.hand.firstWhere((h) => duelSpecs[h.type].cost <= s.you.mana);
      s = duelUpdate(s, DlPlayCard(card.id)) as DuelYourTurn;
      // summoning sickness: the attack is refused
      expect(identical(duelUpdate(s, DlAttack(card.id, 0)), s), isTrue);
      // pass the round: end turn, then run the foe until it hands play back
      DuelState t = duelUpdate(s, const DlEndTurn());
      var guard = 0;
      while (t is DuelFoeTurn && guard++ < 30) {
        t = duelUpdate(t, const DlFoeStep());
      }
      if (t is DuelYourTurn) {
        expect(t.turn, 2);
        expect(t.you.mana, 2); // mana ramps with the round
        expect(
            t.you.creatures.singleWhere((c) => c.id == card.id).ready, isTrue);
      } else {
        expect(t, isA<DuelOver>()); // (a rush this fast never happens, but…)
      }
    });

    test('a face strike costs the hero exactly the attack', () {
      final base = dealt();
      final you = base.you.copy(slots: [
        const DuelCreature(500, 7, 5, ready: true), // Elder Dragon 7/5
        null, null, null,
      ]);
      final s = DuelYourTurn(
          you: you, foe: base.foe, turn: base.turn, nextId: 600,
          seed: base.seed);
      final after = duelUpdate(s, const DlAttack(500, 0)) as DuelYourTurn;
      expect(after.foe.life, duelStartLife - 7);
      expect(after.strike!.targetSlot, -1);
      expect(after.you.creatures.single.ready, isFalse); // spent
    });

    test('creature combat trades damage and clears the dead', () {
      final base = dealt();
      // Storm Archer 3/1 attacks Silver Knight 3/3: both die.
      final you = base.you.copy(
          slots: [const DuelCreature(501, 3, 1, ready: true), null, null, null]);
      final foe = base.foe.copy(
          slots: [const DuelCreature(502, 4, 3, ready: true), null, null, null]);
      final s = DuelYourTurn(
          you: you, foe: foe, turn: base.turn, nextId: 600, seed: base.seed);
      final after = duelUpdate(s, const DlAttack(501, 502)) as DuelYourTurn;
      expect(after.you.creatures, isEmpty);
      expect(after.foe.creatures, isEmpty);
      expect(after.foe.life, duelStartLife); // face untouched
      expect(after.strike!.hp, lessThanOrEqualTo(0)); // attacker died striking
    });

    test('reducing the foe to 0 wins with the killing strike attached', () {
      final base = dealt();
      final you = base.you.copy(
          slots: [const DuelCreature(500, 7, 5, ready: true), null, null, null]);
      final foe = base.foe.copy(life: 6);
      final s = DuelYourTurn(
          you: you, foe: foe, turn: base.turn, nextId: 600, seed: base.seed);
      final after = duelUpdate(s, const DlAttack(500, 0));
      expect(after, isA<DuelOver>());
      expect((after as DuelOver).youWon, isTrue);
      expect(after.foe.life, lessThanOrEqualTo(0));
      expect(after.strike, isNotNull);
    });

    test('the foe develops, attacks and eventually passes the round', () {
      var s = dealt();
      DuelState t = duelUpdate(s, const DlEndTurn());
      expect(t, isA<DuelFoeTurn>());
      expect(t.foe.hand.length, duelHandTarget + 1); // it drew a card
      expect(t.foe.mana, 1);
      var guard = 0;
      var played = 0;
      while (t is DuelFoeTurn && guard++ < 30) {
        final before = t.foe.creatures.length;
        t = duelUpdate(t, const DlFoeStep());
        if (t.foe.creatures.length > before) played++;
      }
      expect(played, greaterThan(0)); // it can always afford a 1-drop
      expect(t, isA<DuelYourTurn>());
      expect((t as DuelYourTurn).turn, 2);
    });

    test('two-tap play: selecting arms a hand card, confirming plays it', () {
      final s = dealt();
      final card =
          s.you.hand.firstWhere((h) => duelSpecs[h.type].cost <= s.you.mana);
      final armed = duelUpdate(s, DlSelect(card.id)) as DuelYourTurn;
      expect(armed.selected, card.id);
      expect(armed.you.hand.length, s.you.hand.length); // not played yet
      // tapping elsewhere cancels…
      final cancelled = duelUpdate(armed, const DlSelect(0)) as DuelYourTurn;
      expect(cancelled.selected, 0);
      expect(cancelled.you.hand.length, s.you.hand.length);
      // …while confirming plays it and drops the selection
      final played = duelUpdate(armed, DlPlayCard(card.id)) as DuelYourTurn;
      expect(played.selected, 0);
      expect(played.you.creatures.single.id, card.id);
      // an unknown id is not selectable
      expect(
          identical(duelUpdate(s, const DlSelect(31337)), s), isTrue);
    });

    test('any board card can be focused; only ready own creatures attack', () {
      final base = dealt();
      final you = base.you.copy(slots: [
        const DuelCreature(500, 4, 3, ready: false), // resting Silver Knight
        null, null, null,
      ]);
      final foe = base.foe.copy(slots: [
        const DuelCreature(501, 2, 3, ready: true), // foe Moon Wolf
        null, null, null,
      ]);
      DuelYourTurn s = DuelYourTurn(
          you: you, foe: foe, turn: base.turn, nextId: 600, seed: base.seed);
      // a foe creature is selectable (inspection focus), no attack happens
      final inspecting = duelUpdate(s, const DlSelect(501)) as DuelYourTurn;
      expect(inspecting.selected, 501);
      expect(inspecting.foe.creatures.single.hp, 3);
      // your resting creature is selectable too, but its attack is refused
      final resting = duelUpdate(s, const DlSelect(500)) as DuelYourTurn;
      expect(resting.selected, 500);
      expect(identical(duelUpdate(resting, const DlAttack(500, 501)), resting),
          isTrue);
    });

    test('render arms the hand fan + focus camera for a selected hand card',
        () {
      final s = dealt();
      final card = s.you.hand.first;
      final armed = duelUpdate(s, DlSelect(card.id)) as DuelYourTurn;
      final ctl = DuelController();

      final scene = ctl.render(armed).single;
      final hand = scene.hands.firstWhere((h) => h.id == 1);
      expect(hand.selectedCard, card.id); // the fan parts around it
      final cam = scene.camera;
      expect(cam, isA<TesseraCameraFocusHand>());
      expect((cam as TesseraCameraFocusHand).cardId, card.id);

      // no selection: still the in-front-of-the-hand camera, nothing armed
      final idleScene = ctl.render(s).single;
      expect(idleScene.hands.firstWhere((h) => h.id == 1).selectedCard, 0);
      expect(idleScene.camera, isA<TesseraCameraFocusHand>());
      expect((idleScene.camera as TesseraCameraFocusHand).cardId, isNull);

      // a board selection focuses that card instead
      final board = DuelYourTurn(
          you: s.you.copy(slots: [
            const DuelCreature(500, 4, 3, ready: true), null, null, null,
          ]),
          foe: s.foe,
          turn: s.turn,
          nextId: 600,
          seed: s.seed,
          selected: 500);
      final boardScene = ctl.render(board).single;
      expect(boardScene.camera, isA<TesseraCameraFocusCard>());
      expect((boardScene.camera as TesseraCameraFocusCard).cardId, 500);
      // and the strike beat always pulls back to the table overview
      final strike = duelUpdate(board, const DlAttack(500, 0));
      expect(ctl.render(strike).first.camera, isA<TesseraCameraPose>());
    });

    test('render yields the attack/defense card effects across a strike', () {
      final s = dealt();
      final board = DuelYourTurn(
          you: s.you.copy(slots: [
            const DuelCreature(500, 4, 3, ready: true), null, null, null,
          ]),
          foe: s.foe.copy(slots: [
            const DuelCreature(501, 2, 3, ready: true), null, null, null,
          ]),
          turn: s.turn,
          nextId: 600,
          seed: s.seed);
      final ctl = DuelController();

      // Creature strike: the sync* render yields lunge then settle — the lunge
      // trails the attack effect on the attacker card, the settle flashes the
      // defense effect on the struck card.
      final strike = duelUpdate(board, const DlAttack(500, 501));
      final beats = ctl.render(strike).toList();
      expect(beats, hasLength(2));
      expect(beats[0].effects.single.attachCard, 500);
      expect(beats[1].effects.single.attachCard, 501);

      // Hero strike: no card to attach the defense burst to — it falls back to
      // the tile at the foe hero's table edge.
      final face = duelUpdate(board, const DlAttack(500, 0));
      final faceBeats = ctl.render(face).toList();
      expect(faceBeats[1].effects.single.attachCard, 0);
      expect(faceBeats[1].effects.single.x, 0);
      expect(faceBeats[1].effects.single.y, -4);

      // No strike on the state: no battle effects linger.
      expect(ctl.render(board).single.effects, isEmpty);
    });

    test('attacking engages the creature: its card lies sideways until it '
        'readies next round', () {
      final s = dealt();
      final board = DuelYourTurn(
          you: s.you.copy(slots: [
            const DuelCreature(500, 7, 5, ready: true), null, null, null,
          ]),
          foe: s.foe.copy(hand: [], deck: []),
          turn: s.turn,
          nextId: 600,
          seed: s.seed);
      final after = duelUpdate(board, const DlAttack(500, 0)) as DuelYourTurn;
      final c = after.you.creatures.single;
      expect(c.engaged, isTrue);
      expect(c.ready, isFalse);

      // The lunge beat keeps the attacker upright; the settle beat lays it
      // sideways (a quarter-turn quaternion, not the all-zero identity).
      final ctl = DuelController();
      final beats = ctl.render(after).toList();
      TesseraCard cardIn(TesseraScene sc) =>
          sc.cards.firstWhere((k) => k.id == 500);
      expect(cardIn(beats[0]).orientation, everyElement(0));
      expect(cardIn(beats[1]).orientation.any((v) => v.abs() > 0.1), isTrue);

      // A fresh (resting, never-attacked) creature is NOT rotated.
      expect(after.you.slots[0]!.engaged, isTrue);
      final fresh = DuelYourTurn(
          you: s.you.copy(slots: [
            const DuelCreature(502, 1, 2, ready: false), null, null, null,
          ]),
          foe: s.foe,
          turn: s.turn,
          nextId: 600,
          seed: s.seed);
      final freshCard =
          ctl.render(fresh).single.cards.firstWhere((k) => k.id == 502);
      expect(freshCard.orientation, everyElement(0));

      // The round passing readies it and stands the card back up.
      DuelState round = duelUpdate(after, const DlEndTurn());
      while (round is DuelFoeTurn) {
        round = duelUpdate(round, const DlFoeStep());
      }
      final readied = (round as DuelYourTurn).you.creatures.single;
      expect(readied.engaged, isFalse);
      expect(readied.ready, isTrue);
      final upright =
          ctl.render(round).single.cards.firstWhere((k) => k.id == 500);
      expect(upright.orientation, everyElement(0));
    });

    test('mana ramps with the round and caps at $duelManaCap', () {
      DuelState s = dealt();
      for (var round = 1; round <= duelManaCap + 3; round++) {
        if (s is! DuelYourTurn) break;
        expect(s.you.mana, lessThanOrEqualTo(duelManaCap));
        expect(s.you.mana,
            math.min(round, duelManaCap) - 0); // refilled at round start
        s = duelUpdate(s, const DlEndTurn());
        var guard = 0;
        // The foe never attacks here (you keep an empty board and it prefers
        // face only via strikes — those still end); just run it to the pass.
        while (s is DuelFoeTurn && guard++ < 30) {
          s = duelUpdate(s, const DlFoeStep());
        }
        if (s is DuelOver) break; // face damage can end the mock game
      }
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
