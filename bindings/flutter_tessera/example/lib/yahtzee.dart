// yahtzee.dart — a solitaire Yahtzee example driven by the Dice APIs.
//
// Same reducer shape as the others (see game.dart): a sealed action + a sealed
// state, a pure reducer `yUpdate`, and a `render` projecting the five dice into
// a scene. Rolling gives the un-held dice new faces + new seeds, so the engine
// re-throws exactly those (held dice keep their placement and sit still).
//
// Holding/un-holding a die (YToggleKeep) changes ONLY that die's z position —
// its id/def/face/seed/throwS are untouched — so the engine slides it forward to
// the hold row (or back) with an easeInOut curve instead of re-throwing it.
//
// Labels / highlights / events give the roll its juice:
//
//   • A fresh roll renders TWO scenes: the throw itself, then the same scene
//     plus a world-anchored 3D label with the roll total. setScene resolves
//     only once the tumble settles, so the label fades in exactly when the
//     last die stops — no spoilers mid-tumble. The status line and the
//     category score buttons hold back their numbers the same way.
//   • Held dice carry a pulsing golden GLOW highlight in every scene, so
//     "kept for the next roll" reads at a glance.
//   • The engine event stream drives widget-side juice ONLY (never game
//     logic): DICE_CONTACT flashes the screen edges with the impact speed as
//     intensity (plus a haptic tick on hard hits), and DICE_SETTLED gives a
//     soft tick per die and a golden beat once the whole roll is down.

import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart' show HapticFeedback;
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
  int _font = 0;
  double _camDistance = 12.0;

  static const int _totalLabelId = 900;
  static const List<double> _gold = [1.0, 0.85, 0.25, 1.0];

  // Presentation-only roll bookkeeping (the reducer never reads any of this):
  // a fresh YRoll notes how many dice the engine will actually re-throw, and
  // the DICE_SETTLED events tick them off so the UI knows when the tumble is
  // honest to look at. `_freshRoll` makes the next render() play the roll as
  // two beats (throw, then reveal).
  bool _freshRoll = false;
  int _expectedSettles = 0;
  final Set<int> _settledDice = <int>{};
  bool _revealFired = false;
  bool get _rolling => _expectedSettles > 0 && !_revealFired;

  // Widget-side juice: engine events pulse this notifier, and a screen-edge
  // flash overlay (inserted into the app's root overlay, since the shared
  // GameScreen owns the widget tree) listens to it.
  StreamSubscription<TesseraEvent>? _events;
  OverlayEntry? _juiceEntry;
  final ValueNotifier<_DicePulse> _juicePulse =
      ValueNotifier<_DicePulse>(const _DicePulse(0));

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
  String get subtitle => 'Dice · roll, tap a die to hold, fill the scorecard';
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
    _font = await registerGameFont(c);

    // Juice wiring. GameController has no dispose hook, so teardown rides the
    // stream's onDone: GameScreen.dispose() disposes the TesseraController,
    // which closes the event stream, which removes the flash overlay and
    // cancels the subscription.
    await _events?.cancel();
    _events = c.events.listen(_onEvent, onDone: _teardownJuice);
    _mountJuiceFlash();
  }

  @override
  YState initial() => const YTurnStart(scorecard: {}, seed: 0xBEEF01);

  @override
  YState update(YState state, YAction action) {
    final next = yUpdate(state, action);
    if (action is YRoll && next is YRolled && !identical(next, state)) {
      // Mirror the reducer's throw rule: a first roll throws all five dice, a
      // re-roll only the un-held ones (all-held rolls re-throw nothing).
      var thrown = 0;
      for (var i = 0; i < 5; i++) {
        if (state is YTurnStart || !state.kept[i]) thrown++;
      }
      _expectedSettles = thrown;
      _settledDice.clear();
      _revealFired = thrown == 0;
      _freshRoll = thrown > 0;
    }
    return next;
  }

  @override
  List<TesseraScene> render(YState s) {
    if (s is YRolled && _freshRoll) {
      _freshRoll = false;
      // Two beats: the throw with the total held back, then — setScene only
      // resolves once the tumble settles — the reveal with the total label.
      return [_scene(s, showTotal: false), _scene(s)];
    }
    return [_scene(s)];
  }

  TesseraScene _scene(YState s, {bool showTotal = true}) {
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
          // Stagger the throw times a touch so the dice don't start and settle
          // in perfect lockstep (real dice come to rest at scattered moments).
          throwS: 0.85 + 0.06 * i,
        ));
      }
    }

    // Held dice glow gold (pulsing) so "kept for the next roll" reads at a
    // glance even while the un-held dice are still tumbling behind them.
    final highlights = <TesseraHighlight>[
      if (s is YRolled)
        for (var i = 0; i < 5; i++)
          if (s.kept[i])
            TesseraHighlight(
              targetId: i + 1,
              kind: TesseraHighlightKind.dice,
              style: TesseraHighlightStyle.glow,
              color: _gold,
              thickness: 12,
              pulseS: 1.6,
              pulseMin: 0.45,
              pulseMax: 1.0,
            ),
    ];

    // The roll total floats above the tumble area. A fresh roll's first scene
    // omits it (showTotal: false, see render), so it fades in only once every
    // thrown die has settled; at game over it becomes the final score.
    final labels = <TesseraLabel>[
      if (showTotal && s.rolled && _font != 0)
        TesseraLabel(
          id: _totalLabelId,
          font: _font,
          text: s is YOver
              ? 'Final ${s.grandTotal}'
              : 'Roll ${s.dice.fold(0, (a, b) => a + b)}',
          position: const [0, 2.1, 0.2],
          size: 0.62,
          color: s is YOver ? _gold : const [1, 1, 1, 1],
        ),
    ];

    return TesseraScene(
      tiles: tiles,
      dice: dice,
      highlights: highlights,
      labels: labels,
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
    // A re-fit mid-tumble must not reveal the total early — but it replaces
    // the roll's queued reveal beat, so re-queue both: the re-fit with the
    // label held back, then (setScene resolves once the tumble settles) the
    // reveal with the total. Settled boards just get the one re-fit scene.
    if (_rolling) return [_scene(state, showTotal: false), _scene(state)];
    return [_scene(state)];
  }

  // Tap a die to hold/un-hold it. render() gives die i the id `i + 1`, so the
  // picked die id maps straight back to its index. Only meaningful once rolled.
  @override
  YAction? onTap(YState state, TesseraPickResult? pick,
      {Offset? local, Size? view}) {
    if (state is! YRolled || pick == null || !pick.hitDice) return null;
    final index = pick.dice - 1;
    if (index < 0 || index >= 5) return null;
    return YToggleKeep(index);
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
        // Dice are held by tapping them in the scene (see onTap), not via buttons.
        // Category scores are part of the roll's reveal: hold them back until
        // the settle events say every thrown die is down (the buttons are
        // disabled while animating anyway, so this only hides the numbers).
        if (_rolling) return out;
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
        if (_rolling) {
          // The faces are decided but the dice are still tumbling — don't
          // spoil the roll before the last one settles.
          return 'Rolling…  ·  rolls left ${state.rollsLeft}  ·  Total $total';
        }
        final held = [for (var i = 0; i < 5; i++) if (state.kept[i]) '${i + 1}'].join(',');
        final dice = state.dice.join(' ');
        return 'Dice: $dice  ·  rolls left ${state.rollsLeft}'
            '${held.isEmpty ? '' : '  ·  holding $held'}  ·  Total $total';
      case YOver():
        return 'Game over — final score $total.  Tap New game.';
    }
  }

  // ---- event-driven juice (widget-side only) ------------------------------

  void _onEvent(TesseraEvent e) {
    switch (e.type) {
      case TesseraEventType.diceContact:
        // First-drop impacts land around 8–13 world units/s; each bounce
        // decays by the restitution down to the engine's 0.25 emission
        // cutoff. Normalize so the opening slam flashes hard and the last
        // skitters barely glimmer.
        final strength = (e.value / 10.0).clamp(0.12, 1.0).toDouble();
        _flash(strength);
        if (strength >= 0.5) HapticFeedback.lightImpact();
      case TesseraEventType.diceSettled:
        _settledDice.add(e.subjectId);
        if (!_revealFired &&
            _expectedSettles > 0 &&
            _settledDice.length >= _expectedSettles) {
          // The last thrown die is down — the roll is honest now. A golden
          // beat marks the reveal (the total label lands with it, see render).
          _revealFired = true;
          _flash(1.0, settled: true);
          HapticFeedback.mediumImpact();
        } else {
          // One die (or a held die's hold-row slide) coming to rest.
          _flash(0.3);
        }
      case TesseraEventType.opCompleted:
        // Fallback: an operation completes only once the whole transition —
        // every thrown die included — has settled, so if any DICE_SETTLED
        // event was lost to ring overflow the roll is still honest now. This
        // keeps the reveal (and the category-score buttons, which _rolling
        // gates) from soft-locking on a dropped event; on the normal path the
        // settle handler above has already fired and this is a no-op.
        if (_rolling) {
          _revealFired = true;
          _flash(1.0, settled: true);
          HapticFeedback.mediumImpact();
        }
      default:
        break;
    }
  }

  void _flash(double strength, {bool settled = false}) {
    _juicePulse.value = _DicePulse(strength, settled: settled);
  }

  /// Insert the screen-edge flash into the app's root overlay. The shared
  /// GameScreen owns the widget tree (and is off-limits to this file), so the
  /// flash lives in an [OverlayEntry] found by walking the element tree to the
  /// navigator's overlay — self-contained here, removed in [_teardownJuice].
  void _mountJuiceFlash() {
    if (_juiceEntry != null) return;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (_events == null || _juiceEntry != null) return; // torn down / dup
      final overlay = _findRootOverlay();
      if (overlay == null) return;
      final entry = OverlayEntry(
        builder: (_) => Positioned.fill(
          child: _DiceJuiceFlash(pulses: _juicePulse),
        ),
      );
      _juiceEntry = entry;
      overlay.insert(entry);
    });
  }

  OverlayState? _findRootOverlay() {
    OverlayState? found;
    void visit(Element element) {
      if (found != null) return;
      if (element is StatefulElement && element.state is OverlayState) {
        found = element.state as OverlayState;
        return;
      }
      element.visitChildren(visit);
    }

    WidgetsBinding.instance.rootElement?.visitChildren(visit);
    return found;
  }

  void _teardownJuice() {
    _events?.cancel();
    _events = null;
    _juiceEntry?.remove();
    _juiceEntry = null;
  }
}

// ---- widget-side juice ----------------------------------------------------

/// One juice pulse off the engine event stream: a dice ground contact
/// ([strength] normalized from the impact speed) or, with [settled], the final
/// settle of a roll (rendered as a golden beat instead of a white one).
class _DicePulse {
  const _DicePulse(this.strength, {this.settled = false});
  final double strength; // 0..1; 0 = the notifier's inert initial value
  final bool settled;
}

/// A full-screen, hit-transparent edge flash: each pulse lights a vignette at
/// the screen edges — intensity from the pulse strength — and fades it out
/// over ~a third of a second. Overlapping contacts re-trigger the flash but
/// keep the strongest of what is still fading, so a big bounce isn't cut
/// short by a tiny follow-up tick.
class _DiceJuiceFlash extends StatefulWidget {
  const _DiceJuiceFlash({required this.pulses});
  final ValueNotifier<_DicePulse> pulses;

  @override
  State<_DiceJuiceFlash> createState() => _DiceJuiceFlashState();
}

class _DiceJuiceFlashState extends State<_DiceJuiceFlash>
    with SingleTickerProviderStateMixin {
  late final AnimationController _anim = AnimationController(
    vsync: this,
    duration: const Duration(milliseconds: 340),
  );
  double _strength = 0;
  bool _settled = false;

  @override
  void initState() {
    super.initState();
    widget.pulses.addListener(_onPulse);
  }

  @override
  void dispose() {
    widget.pulses.removeListener(_onPulse);
    _anim.dispose();
    super.dispose();
  }

  void _onPulse() {
    final p = widget.pulses.value;
    if (p.strength <= 0) return;
    final carried = _anim.isAnimating ? _strength * (1 - _anim.value) : 0.0;
    _strength = math.max(p.strength, carried);
    _settled = p.settled;
    _anim.forward(from: 0);
  }

  @override
  Widget build(BuildContext context) {
    return IgnorePointer(
      child: AnimatedBuilder(
        animation: _anim,
        builder: (context, _) {
          final fade = 1.0 - Curves.easeOutCubic.transform(_anim.value);
          final alpha = 0.4 * _strength * fade;
          if (alpha <= 0.004) return const SizedBox.shrink();
          final color = _settled ? const Color(0xFFFFD54F) : Colors.white;
          return DecoratedBox(
            decoration: BoxDecoration(
              gradient: RadialGradient(
                radius: 1.25,
                stops: const [0.62, 1.0],
                colors: [
                  color.withValues(alpha: 0),
                  color.withValues(alpha: alpha),
                ],
              ),
            ),
            child: const SizedBox.expand(),
          );
        },
      ),
    );
  }
}
