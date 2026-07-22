// yahtzee.dart — a solitaire Yahtzee example driven by the Dice APIs.
//
// Same reducer shape as the others (see game.dart): a sealed action + a sealed
// state, a pure reducer `yUpdate`, and a `render` projecting the five dice into
// a scene. Rolling gives the un-held dice new faces + new seeds, so the engine
// re-throws exactly those (held dice keep their placement and sit still).

import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter_tessera/flutter_tessera.dart';

import 'dice_art.dart';
import 'game.dart';

const List<String> categoryNames = [
  'Ones', 'Twos', 'Threes', 'Fours', 'Fives', 'Sixes', //
  '3 of a kind', '4 of a kind', 'Full house', //
  'Sm straight', 'Lg straight', 'Yahtzee', 'Chance',
];

int _next(int seed) => (seed * 1103515245 + 12345) & 0x7fffffff;
int _nextSeed(int seed) => _next(seed == 0 ? 1 : seed);

/// Score dice under category [cat] (0..12).
int scoreCategory(int cat, List<int> dice) {
  final counts = List<int>.filled(7, 0);
  for (final v in dice) {
    counts[v]++;
  }
  final sum = dice.fold(0, (a, b) => a + b);
  if (cat <= 5) return counts[cat + 1] * (cat + 1); // upper: ones..sixes
  switch (cat) {
    case 6:
      return counts.any((x) => x >= 3) ? sum : 0; // three of a kind
    case 7:
      return counts.any((x) => x >= 4) ? sum : 0; // four of a kind
    case 8:
      final has3 = counts.any((x) => x == 3), has2 = counts.any((x) => x == 2);
      final has5 = counts.any((x) => x == 5);
      return (has3 && has2) || has5 ? 25 : 0; // full house
    case 9:
      return _hasStraight(counts, 4) ? 30 : 0; // small straight
    case 10:
      return _hasStraight(counts, 5) ? 40 : 0; // large straight
    case 11:
      return counts.any((x) => x == 5) ? 50 : 0; // yahtzee
    case 12:
      return sum; // chance
    default:
      return 0;
  }
}

bool _hasStraight(List<int> counts, int len) {
  var run = 0;
  for (var v = 1; v <= 6; v++) {
    if (counts[v] > 0) {
      if (++run >= len) return true;
    } else {
      run = 0;
    }
  }
  return false;
}

// ---- actions -------------------------------------------------------------
sealed class YAction {
  const YAction();
}

class YRoll extends YAction {
  const YRoll();
}

class YToggleKeep extends YAction {
  const YToggleKeep(this.index);
  final int index;
}

class YScore extends YAction {
  const YScore(this.category);
  final int category;
}

class YNewGame extends YAction {
  const YNewGame();
}

// ---- state ---------------------------------------------------------------
sealed class YState {
  const YState({
    required this.dice,
    required this.kept,
    required this.dieSeed,
    required this.rollsLeft,
    required this.scorecard,
    required this.seed,
  });

  final List<int> dice; // 5 face values 1..6 (0 = un-rolled)
  final List<bool> kept; // 5
  final List<int> dieSeed; // 5 throw seeds
  final int rollsLeft;
  final Map<int, int> scorecard; // category -> score
  final int seed;

  int get grandTotal => scorecard.values.fold(0, (a, b) => a + b);
  bool get rolled => dice.any((v) => v != 0);
}

/// Start of a turn — no dice on the table yet, three rolls available.
class YTurnStart extends YState {
  const YTurnStart({
    required super.scorecard,
    required super.seed,
  }) : super(
          dice: const [0, 0, 0, 0, 0],
          kept: const [false, false, false, false, false],
          dieSeed: const [0, 0, 0, 0, 0],
          rollsLeft: 3,
        );
}

/// Dice are on the table: re-roll the un-held ones or score a category.
class YRolled extends YState {
  const YRolled({
    required super.dice,
    required super.kept,
    required super.dieSeed,
    required super.rollsLeft,
    required super.scorecard,
    required super.seed,
  });
}

/// All thirteen categories filled.
class YOver extends YState {
  const YOver({
    required super.dice,
    required super.scorecard,
    required super.seed,
  }) : super(
          kept: const [true, true, true, true, true],
          dieSeed: const [0, 0, 0, 0, 0],
          rollsLeft: 0,
        );
}

// ---- reducer -------------------------------------------------------------
YState yUpdate(YState s, YAction a) {
  switch (a) {
    case YRoll():
      return _roll(s);
    case YToggleKeep(:final index):
      if (s is! YRolled) return s;
      final kept = List<bool>.of(s.kept);
      kept[index] = !kept[index];
      return YRolled(
          dice: s.dice, kept: kept, dieSeed: s.dieSeed, rollsLeft: s.rollsLeft, scorecard: s.scorecard, seed: s.seed);
    case YScore(:final category):
      return _score(s, category);
    case YNewGame():
      return YTurnStart(scorecard: const {}, seed: _nextSeed(s.seed));
  }
}

YState _roll(YState s) {
  if (s.rollsLeft <= 0) return s;
  final firstRoll = s is YTurnStart;
  var seed = s.seed;
  final dice = List<int>.of(s.dice);
  final dieSeed = List<int>.of(s.dieSeed);
  for (var i = 0; i < 5; i++) {
    if (!firstRoll && s.kept[i]) continue; // held dice keep their value + seed
    seed = _next(seed);
    dice[i] = 1 + seed % 6;
    seed = _next(seed);
    dieSeed[i] = seed;
  }
  return YRolled(
    dice: dice,
    kept: firstRoll ? const [false, false, false, false, false] : s.kept,
    dieSeed: dieSeed,
    rollsLeft: s.rollsLeft - 1,
    scorecard: s.scorecard,
    seed: seed,
  );
}

YState _score(YState s, int category) {
  if (s is! YRolled || s.scorecard.containsKey(category)) return s;
  final card = Map<int, int>.of(s.scorecard);
  card[category] = scoreCategory(category, s.dice);
  if (card.length >= 13) {
    return YOver(dice: s.dice, scorecard: card, seed: s.seed);
  }
  return YTurnStart(scorecard: card, seed: s.seed);
}

// ---- controller (rendering + wiring) -------------------------------------
class YahtzeeController extends GameController<YState, YAction> {
  int _felt = 0;
  int _die = 0;
  double _camDistance = 12.0;

  // Four corner tiles hugging the dice area — fed to cameraFitDistance so the
  // whole table stays framed in portrait as well as landscape.
  static const List<int> _fitCorners = [9001, 9002, 9003, 9004];
  int _cornerId(int x, int z) {
    if (x == -4 && z == -3) return 9001;
    if (x == 4 && z == -3) return 9002;
    if (x == -4 && z == 3) return 9003;
    if (x == 4 && z == 3) return 9004;
    return 0;
  }

  @override
  String get title => 'Yahtzee';
  @override
  String get subtitle => 'Dice · roll, hold, and fill the scorecard';
  @override
  IconData get icon => Icons.casino;

  @override
  TesseraLightData get light => const TesseraLightData(
        dir: [-0.4, -1.0, -0.4],
        color: [1.0, 0.98, 0.92],
        intensity: 1.15,
        ambient: [0.3, 0.33, 0.4],
      );

  @override
  Future<void> registerDefs(TesseraController c) async {
    _felt = c.registerTileType(
        const TesseraTileType(thickness: 0.2, tint: [0.1, 0.42, 0.26, 1.0]));
    _die = c.registerDiceType(TesseraDiceType(faces: await renderDiceFaces(), size: 1.3));
  }

  @override
  YState initial() => const YTurnStart(scorecard: {}, seed: 0xBEEF01);

  @override
  YState update(YState state, YAction action) => yUpdate(state, action);

  @override
  List<TesseraScene> render(YState s) => [_scene(s)];

  TesseraScene _scene(YState s) {
    final tiles = <TesseraTile>[];
    for (var z = -4; z <= 4; z++) {
      for (var x = -6; x <= 6; x++) {
        tiles.add(TesseraTile(x: x, y: z, def: _felt, id: _cornerId(x, z)));
      }
    }

    final dice = <TesseraDie>[];
    if (s.rolled) {
      for (var i = 0; i < 5; i++) {
        // Held dice sit forward (toward the player); rolling dice stay centred.
        final z = s.kept[i] ? 2.4 : 0.2;
        dice.add(TesseraDie(
          id: i + 1,
          def: _die,
          face: s.dice[i] - 1, // face index f shows value f + 1
          position: [(-3.2 + i * 1.6), 0.6, z],
          seed: s.dieSeed[i],
          throwS: 0.95,
        ));
      }
    }

    return TesseraScene(
      tiles: tiles,
      dice: dice,
      camera: TesseraCameraPose(
        focusX: 0,
        focusY: 0.6,
        distance: _camDistance,
        yaw: 0,
        pitch: 0.98,
        fov: 0.72,
      ),
    );
  }

  @override
  List<TesseraScene>? onResize(TesseraController c, YState state, double w, double h) {
    final fit = c.cameraFitDistance(tileIds: _fitCorners, padding: 0.09);
    if (fit == null) return null;
    // Fit only ever pulls back from the tuned landscape distance (the ground-
    // plane fit ignores the raised dice height), so portrait stops cropping
    // without landscape ever zooming in tighter than 12.
    final dist = math.max(fit, 12.0);
    if (dist == _camDistance) return null;
    _camDistance = dist;
    return [_scene(state)];
  }

  @override
  YAction? autoAction(YState state, math.Random rng) {
    switch (state) {
      case YTurnStart():
        return const YRoll();
      case YOver():
        return const YNewGame();
      case YRolled():
        final desired = _desiredKeeps(state.dice);
        for (var i = 0; i < 5; i++) {
          if (state.kept[i] != desired[i]) return YToggleKeep(i);
        }
        final held = desired.where((k) => k).length;
        if (state.rollsLeft > 0 && held < 4) return const YRoll();
        return YScore(_bestOpenCategory(state));
    }
  }

  List<bool> _desiredKeeps(List<int> dice) {
    final counts = List<int>.filled(7, 0);
    for (final v in dice) {
      counts[v]++;
    }
    var modal = 1;
    for (var v = 2; v <= 6; v++) {
      if (counts[v] > counts[modal]) modal = v;
    }
    return [for (final v in dice) v == modal];
  }

  int _bestOpenCategory(YState s) {
    var bestCat = -1, bestScore = -1;
    for (var cat = 0; cat < 13; cat++) {
      if (s.scorecard.containsKey(cat)) continue;
      final sc = scoreCategory(cat, s.dice);
      if (sc > bestScore) {
        bestScore = sc;
        bestCat = cat;
      }
    }
    return bestCat < 0 ? 12 : bestCat;
  }

  @override
  List<GameButton<YAction>> buttons(YState state) {
    switch (state) {
      case YOver():
        return const [
          GameButton('New game', YNewGame(), icon: Icons.refresh, tone: GameButtonTone.primary)
        ];
      case YTurnStart():
        return const [
          GameButton('Roll', YRoll(), icon: Icons.casino, tone: GameButtonTone.primary)
        ];
      case YRolled():
        final out = <GameButton<YAction>>[];
        if (state.rollsLeft > 0) {
          out.add(GameButton('Roll (${state.rollsLeft})', const YRoll(),
              icon: Icons.casino, tone: GameButtonTone.primary));
        }
        for (var i = 0; i < 5; i++) {
          out.add(GameButton(
            'die ${i + 1}: ${state.dice[i]}${state.kept[i] ? ' ✓' : ''}',
            YToggleKeep(i),
            tone: state.kept[i] ? GameButtonTone.primary : GameButtonTone.normal,
          ));
        }
        for (var cat = 0; cat < 13; cat++) {
          if (state.scorecard.containsKey(cat)) continue;
          out.add(GameButton(
            '${categoryNames[cat]} · ${scoreCategory(cat, state.dice)}',
            YScore(cat),
            tone: GameButtonTone.danger,
          ));
        }
        return out;
    }
  }

  @override
  String status(YState state) {
    final total = state.grandTotal;
    switch (state) {
      case YTurnStart():
        final filled = state.scorecard.length;
        return filled == 0
            ? 'Roll the dice to start.  ·  Total $total'
            : 'Next turn ($filled/13 scored)  ·  Total $total';
      case YRolled():
        final held = [for (var i = 0; i < 5; i++) if (state.kept[i]) '${i + 1}'].join(',');
        final dice = state.dice.join(' ');
        return 'Dice: $dice  ·  rolls left ${state.rollsLeft}'
            '${held.isEmpty ? '' : '  ·  holding $held'}  ·  Total $total';
      case YOver():
        return 'Game over — final score $total.  Tap New game.';
    }
  }
}
