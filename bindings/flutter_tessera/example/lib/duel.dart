// duel.dart — "Duel", a small trading-card battler in the spirit of Magic:
// The Gathering: two 16-card decks, mana that ramps each round, creatures with
// cost / attack / health, and a race to drop the opposing hero from 20 life.
//
// Its showcase (unlike the other games, which paint art at runtime) is BUNDLED
// FLUTTER ASSETS REFERENCED FROM SDL: the card faces are composited from real
// PNG assets (a front template + one illustration per creature, see
// duel_art.dart) and registered as engine atlases, and the sound effects are
// WAV assets loaded with rootBundle and played through the engine's SDL audio
// (registerSound / playSound), triggered off the engine event stream and the
// game's beats.
//
// Same architecture as the rest of the app (see game.dart): sealed action +
// sealed state phases, a pure reducer `duelUpdate`, `render` projecting a
// phase into scenes (an attack renders as a two-beat lunge-then-settle
// sequence), the foe playing itself through `autoAdvance`.

import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter_tessera/flutter_tessera.dart';

import 'duel_art.dart';
import 'game.dart';
import 'quat.dart';

// ---- the card catalogue ---------------------------------------------------
class DuelSpec {
  const DuelSpec(this.name, this.artKey, this.cost, this.attack, this.health,
      this.flavor);
  final String name;
  final String artKey; // illustration asset under assets/duel/
  final int cost;
  final int attack;
  final int health;
  final String flavor;
}

const List<DuelSpec> duelSpecs = [
  DuelSpec('Arcane Wisp', 'art_arcane_wisp.png', 1, 1, 2,
      'A mote of borrowed starlight, eager to be spent.'),
  DuelSpec('Ember Whelp', 'art_ember_whelp.png', 1, 2, 1,
      'Small, loud, and already on fire.'),
  DuelSpec('Moon Wolf', 'art_moon_wolf.png', 2, 2, 3,
      'It hunts by the tide of the moon.'),
  DuelSpec('Storm Archer', 'art_storm_archer.png', 2, 3, 1,
      'Every arrow rides its own lightning.'),
  DuelSpec('Silver Knight', 'art_silver_knight.png', 3, 3, 3,
      'Oath first. Armor second. Fear never.'),
  DuelSpec('Forest Treant', 'art_forest_treant.png', 4, 2, 6,
      'Its patience is measured in rings.'),
  DuelSpec('Stone Golem', 'art_stone_golem.png', 5, 5, 6,
      'It still remembers being a mountain.'),
  DuelSpec('Elder Dragon', 'art_elder_dragon.png', 6, 7, 5,
      'The sky itself bows to elder wings.'),
];

const int duelStartLife = 20;
const int duelHandTarget = 4; // opening hand size
const int duelSlots = 4; // creatures per side
const int duelManaCap = 6;

/// A deterministic shuffled deck (two of each spec) from [seed].
List<int> duelShuffledDeck(int seed) {
  final ids = <int>[for (var t = 0; t < duelSpecs.length; t++) ...[t, t]];
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

// ---- immutable game pieces ------------------------------------------------
/// A card instance in a hand (its [id] is stable — the engine diff key).
class DuelCardInst {
  const DuelCardInst(this.id, this.type);
  final int id;
  final int type;
}

/// A creature on the board. [hp] is its remaining health (damage persists);
/// [ready] means it may attack this turn (false the turn it was played).
class DuelCreature {
  const DuelCreature(this.id, this.type, this.hp, {required this.ready});
  final int id;
  final int type;
  final int hp;
  final bool ready;

  int get attack => duelSpecs[type].attack;

  DuelCreature copy({int? hp, bool? ready}) =>
      DuelCreature(id, type, hp ?? this.hp, ready: ready ?? this.ready);
}

/// One player's whole side: life, unspent mana, deck (draw from the end), the
/// fanned hand, and [duelSlots] fixed board slots (null = empty).
class DuelSide {
  const DuelSide({
    required this.life,
    required this.mana,
    required this.deck,
    required this.hand,
    required this.slots,
  });

  factory DuelSide.fresh(int seed) => DuelSide(
      life: duelStartLife,
      mana: 0,
      deck: duelShuffledDeck(seed),
      hand: const [],
      slots: const [null, null, null, null]);

  final int life;
  final int mana;
  final List<int> deck;
  final List<DuelCardInst> hand;
  final List<DuelCreature?> slots;

  List<DuelCreature> get creatures => [...slots.whereType<DuelCreature>()];
  bool get hasFreeSlot => slots.contains(null);

  DuelSide copy({
    int? life,
    int? mana,
    List<int>? deck,
    List<DuelCardInst>? hand,
    List<DuelCreature?>? slots,
  }) =>
      DuelSide(
          life: life ?? this.life,
          mana: mana ?? this.mana,
          deck: deck ?? this.deck,
          hand: hand ?? this.hand,
          slots: slots ?? this.slots);
}

/// A just-resolved attack, kept on the state for one beat so render can play
/// the lunge: card [attackerId] (of [attackerType], post-combat [hp], normally
/// living in [homeSlot] on the [byYou] side) struck [targetSlot] (-1 = the
/// enemy hero).
class DuelStrike {
  const DuelStrike({
    required this.attackerId,
    required this.attackerType,
    required this.hp,
    required this.homeSlot,
    required this.byYou,
    required this.targetSlot,
  });

  final int attackerId;
  final int attackerType;
  final int hp; // <= 0: the attacker died striking
  final int homeSlot;
  final bool byYou;
  final int targetSlot; // -1 = hero
}

// ---- actions --------------------------------------------------------------
sealed class DuelAction {
  const DuelAction();
}

/// Shuffle both decks and begin dealing the opening hands.
class DlStart extends DuelAction {
  const DlStart();
}

/// Intrinsic: deal the next opening-hand card (alternating sides).
class DlDealStep extends DuelAction {
  const DlDealStep();
}

/// Play card [id] from your hand onto your first free slot.
class DlPlayCard extends DuelAction {
  const DlPlayCard(this.id);
  final int id;
}

/// Select your creature [id] as the attacker (0 clears the selection).
class DlSelect extends DuelAction {
  const DlSelect(this.id);
  final int id;
}

/// Attack with your creature [attacker]: [target] is a foe creature id, or 0
/// to strike the hero directly.
class DlAttack extends DuelAction {
  const DlAttack(this.attacker, this.target);
  final int attacker;
  final int target;
}

/// End your turn; the foe draws, ramps and plays.
class DlEndTurn extends DuelAction {
  const DlEndTurn();
}

/// Intrinsic: the foe takes its next step (play a card / attack / end turn).
class DlFoeStep extends DuelAction {
  const DlFoeStep();
}

/// Back to the start screen with fresh decks.
class DlNewGame extends DuelAction {
  const DlNewGame();
}

// ---- state ----------------------------------------------------------------
sealed class DuelState {
  const DuelState({
    required this.you,
    required this.foe,
    required this.turn,
    required this.nextId,
    required this.seed,
    this.strike,
  });

  final DuelSide you;
  final DuelSide foe;
  final int turn; // full round counter, 1-based once play begins
  final int nextId; // next card instance id to hand out
  final int seed;

  /// The attack this state was reached by, if any — rendered as a lunge beat.
  final DuelStrike? strike;

  /// This round's mana ceiling (both sides ramp together).
  int get manaCeiling => math.min(turn, duelManaCap);
}

/// The empty arena, waiting for the player to start.
class DuelIdle extends DuelState {
  // ignore: use_super_parameters — `seed` also derives both fresh sides.
  DuelIdle({required int seed})
      : super(
            you: DuelSide.fresh(seed),
            foe: DuelSide.fresh(_nextSeed(seed)),
            turn: 0,
            nextId: 1,
            seed: seed);
}

/// Dealing the opening hands, one card per beat.
class DuelDealing extends DuelState {
  const DuelDealing({
    required super.you,
    required super.foe,
    required super.nextId,
    required super.seed,
  }) : super(turn: 0);
}

/// Your main phase: play cards, pick attackers, end the turn.
class DuelYourTurn extends DuelState {
  const DuelYourTurn({
    required super.you,
    required super.foe,
    required super.turn,
    required super.nextId,
    required super.seed,
    this.selected = 0,
    super.strike,
  });

  final int selected; // your creature picked as attacker (0 = none)
}

/// The foe's turn, advancing one visible step at a time.
class DuelFoeTurn extends DuelState {
  const DuelFoeTurn({
    required super.you,
    required super.foe,
    required super.turn,
    required super.nextId,
    required super.seed,
    super.strike,
  });
}

/// One hero has fallen (the winning blow still animates via [strike]).
class DuelOver extends DuelState {
  const DuelOver({
    required super.you,
    required super.foe,
    required super.turn,
    required super.nextId,
    required super.seed,
    required this.youWon,
    super.strike,
  });

  final bool youWon;
}

// ---- reducer --------------------------------------------------------------
DuelState duelUpdate(DuelState s, DuelAction a) {
  switch (a) {
    case DlStart():
      return s is DuelIdle
          ? DuelDealing(you: s.you, foe: s.foe, nextId: s.nextId, seed: s.seed)
          : s;
    case DlDealStep():
      return s is DuelDealing ? _dealStep(s) : s;
    case DlPlayCard(:final id):
      return s is DuelYourTurn ? _playCard(s, id) : s;
    case DlSelect(:final id):
      return s is DuelYourTurn ? _select(s, id) : s;
    case DlAttack(:final attacker, :final target):
      return s is DuelYourTurn ? _attack(s, attacker, target) : s;
    case DlEndTurn():
      return s is DuelYourTurn ? _endYourTurn(s) : s;
    case DlFoeStep():
      return s is DuelFoeTurn ? _foeStep(s) : s;
    case DlNewGame():
      return DuelIdle(seed: _nextSeed(_nextSeed(s.seed)));
  }
}

/// Draw one card off [side]'s deck (a no-op when it is empty).
(DuelSide, int) _draw(DuelSide side, int nextId) {
  if (side.deck.isEmpty || side.hand.length >= 8) return (side, nextId);
  final deck = List<int>.of(side.deck);
  final type = deck.removeLast();
  return (
    side.copy(deck: deck, hand: [...side.hand, DuelCardInst(nextId, type)]),
    nextId + 1,
  );
}

DuelState _dealStep(DuelDealing s) {
  var you = s.you, foe = s.foe, next = s.nextId;
  // Alternate you, foe, you, foe…
  if (you.hand.length <= foe.hand.length) {
    (you, next) = _draw(you, next);
  } else {
    (foe, next) = _draw(foe, next);
  }
  if (you.hand.length + foe.hand.length < duelHandTarget * 2) {
    return DuelDealing(you: you, foe: foe, nextId: next, seed: s.seed);
  }
  // Both hands full: round 1, you go first with 1 mana (no extra first draw).
  return DuelYourTurn(
      you: you.copy(mana: 1), foe: foe, turn: 1, nextId: next, seed: s.seed);
}

DuelState _playCard(DuelYourTurn s, int id) {
  final side = _playOnto(s.you, id);
  if (side == null) return s;
  return DuelYourTurn(
      you: side, foe: s.foe, turn: s.turn, nextId: s.nextId, seed: s.seed,
      selected: s.selected);
}

/// Move hand card [id] onto [side]'s first free slot, spending its cost.
/// Null when the card is missing, unaffordable, or the board is full.
DuelSide? _playOnto(DuelSide side, int id) {
  final i = side.hand.indexWhere((h) => h.id == id);
  if (i < 0) return null;
  final card = side.hand[i];
  final spec = duelSpecs[card.type];
  final slot = side.slots.indexOf(null);
  if (spec.cost > side.mana || slot < 0) return null;
  final slots = List<DuelCreature?>.of(side.slots);
  slots[slot] = DuelCreature(card.id, card.type, spec.health, ready: false);
  return side.copy(
      mana: side.mana - spec.cost,
      hand: [...side.hand]..removeAt(i),
      slots: slots);
}

DuelState _select(DuelYourTurn s, int id) {
  if (id != 0) {
    final c = s.you.creatures.where((c) => c.id == id).firstOrNull;
    if (c == null || !c.ready) return s;
  }
  return DuelYourTurn(
      you: s.you, foe: s.foe, turn: s.turn, nextId: s.nextId, seed: s.seed,
      selected: id);
}

/// Resolve one attack on copies of the two sides. Returns the new (attacking
/// side, defending side, strike) or null when illegal.
(DuelSide, DuelSide, DuelStrike)? _resolveAttack(
    DuelSide atkSide, DuelSide defSide, int attackerId, int targetId,
    {required bool byYou}) {
  final homeSlot = atkSide.slots.indexWhere((c) => c?.id == attackerId);
  if (homeSlot < 0) return null;
  final attacker = atkSide.slots[homeSlot]!;
  if (!attacker.ready) return null;

  var atkSlots = List<DuelCreature?>.of(atkSide.slots);
  var defSlots = List<DuelCreature?>.of(defSide.slots);
  var defLife = defSide.life;
  int targetSlot = -1;
  int attackerHp = attacker.hp;

  if (targetId == 0) {
    defLife -= attacker.attack;
  } else {
    targetSlot = defSide.slots.indexWhere((c) => c?.id == targetId);
    if (targetSlot < 0) return null;
    final target = defSlots[targetSlot]!;
    attackerHp = attacker.hp - target.attack; // creatures trade blows
    final targetHp = target.hp - attacker.attack;
    defSlots[targetSlot] = targetHp > 0 ? target.copy(hp: targetHp) : null;
  }
  atkSlots[homeSlot] =
      attackerHp > 0 ? attacker.copy(hp: attackerHp, ready: false) : null;

  return (
    atkSide.copy(slots: atkSlots),
    defSide.copy(slots: defSlots, life: defLife),
    DuelStrike(
        attackerId: attacker.id,
        attackerType: attacker.type,
        hp: attackerHp,
        homeSlot: homeSlot,
        byYou: byYou,
        targetSlot: targetSlot),
  );
}

DuelState _attack(DuelYourTurn s, int attackerId, int targetId) {
  final r = _resolveAttack(s.you, s.foe, attackerId, targetId, byYou: true);
  if (r == null) return s;
  final (you, foe, strike) = r;
  if (foe.life <= 0) {
    return DuelOver(
        you: you, foe: foe, turn: s.turn, nextId: s.nextId, seed: s.seed,
        youWon: true, strike: strike);
  }
  return DuelYourTurn(
      you: you, foe: foe, turn: s.turn, nextId: s.nextId, seed: s.seed,
      strike: strike);
}

DuelState _endYourTurn(DuelYourTurn s) {
  // The foe refills to this round's ceiling, readies its board and draws.
  var (foe, next) = _draw(s.foe, s.nextId);
  foe = foe.copy(
      mana: s.manaCeiling,
      slots: [for (final c in foe.slots) c?.copy(ready: true)]);
  return DuelFoeTurn(
      you: s.you, foe: foe, turn: s.turn, nextId: next, seed: s.seed);
}

/// The foe's tiny brain: develop the board, then attack — trade into a kill
/// when one is on offer, go face when the way is clear (or lethal) — then
/// pass the round back.
DuelState _foeStep(DuelFoeTurn s) {
  // 1. Play the most expensive affordable card.
  if (s.foe.hasFreeSlot) {
    DuelCardInst? best;
    for (final h in s.foe.hand) {
      if (duelSpecs[h.type].cost > s.foe.mana) continue;
      if (best == null || duelSpecs[h.type].cost > duelSpecs[best.type].cost) {
        best = h;
      }
    }
    if (best != null) {
      final foe = _playOnto(s.foe, best.id)!;
      return DuelFoeTurn(
          you: s.you, foe: foe, turn: s.turn, nextId: s.nextId, seed: s.seed);
    }
  }

  // 2. Attack with the first ready creature.
  final ready = s.foe.creatures.where((c) => c.ready).firstOrNull;
  if (ready != null) {
    final defenders = s.you.creatures;
    final lethal = s.you.life <=
        s.foe.creatures.where((c) => c.ready).fold(0, (t, c) => t + c.attack);
    int target = 0;
    if (defenders.isNotEmpty && !lethal) {
      // Prefer a defender this attack kills (biggest first); else chip the
      // weakest one down.
      final kills = [...defenders.where((d) => d.hp <= ready.attack)]
        ..sort((a, b) => b.attack.compareTo(a.attack));
      final chip = [...defenders]..sort((a, b) => a.hp.compareTo(b.hp));
      target = (kills.firstOrNull ?? chip.first).id;
    }
    final r = _resolveAttack(s.foe, s.you, ready.id, target, byYou: false);
    if (r != null) {
      final (foe, you, strike) = r;
      if (you.life <= 0) {
        return DuelOver(
            you: you, foe: foe, turn: s.turn, nextId: s.nextId, seed: s.seed,
            youWon: false, strike: strike);
      }
      return DuelFoeTurn(
          you: you, foe: foe, turn: s.turn, nextId: s.nextId, seed: s.seed,
          strike: strike);
    }
  }

  // 3. Nothing left: a new round — you draw, both ramp, your board readies.
  final turn = s.turn + 1;
  var (you, next) = _draw(s.you, s.nextId);
  you = you.copy(
      mana: math.min(turn, duelManaCap),
      slots: [for (final c in you.slots) c?.copy(ready: true)]);
  return DuelYourTurn(
      you: you, foe: s.foe, turn: turn, nextId: next, seed: s.seed);
}

// ---- controller (rendering + wiring) --------------------------------------
class DuelController extends GameController<DuelState, DuelAction> {
  static const int _yourHand = 1;
  static const int _foeHand = 2;
  static const int _yourDeck = 8001;
  static const int _foeDeck = 8002;
  static const int _foeLifeLabel = 9101;
  static const int _yourLifeLabel = 9102;
  static const int _manaLabel = 9103;
  static const int _bannerLabel = 9104;
  static const int _statLabelBase = 5000;
  static const List<int> _fitCorners = [9001, 9002, 9003, 9004];

  static const List<double> _slotX = [-3, -1, 1, 3];
  static const double _yourRowZ = 1.5;
  static const double _foeRowZ = -1.5;

  final List<int> _cardDef = List<int>.filled(duelSpecs.length, 0);
  int _feltYou = 0;
  int _feltFoe = 0;
  int _deckDef = 0;
  int _font = 0;
  double _camDistance = 14.0;

  // Sound ids (0 until registered; playSound(0) is a safe no-op).
  int _sndDraw = 0, _sndPlay = 0, _sndAttack = 0, _sndHurt = 0;
  int _sndWin = 0, _sndLose = 0, _sndTurn = 0;

  TesseraController? _tessera;
  StreamSubscription<TesseraEvent>? _events;

  @override
  String get title => 'Duel';
  @override
  String get subtitle => 'Cards · a small mana-and-creatures battler';
  @override
  IconData get icon => Icons.local_fire_department;

  @override
  TesseraLightData get light => const TesseraLightData(
        dir: [-0.3, -1.0, -0.4],
        color: [1.0, 0.95, 0.88],
        intensity: 1.15,
        ambient: [0.30, 0.31, 0.40],
      );

  @override
  TesseraTimingData get timing => const TesseraTimingData(
        addS: 0.3,
        removeS: 0.3,
        moveS: 0.38,
        tileS: 0.3,
        cameraS: 0.6,
      );

  @override
  Future<void> registerDefs(TesseraController c) async {
    // A fresh engine per mount: re-arm the event tap (feeds the draw sound).
    await _events?.cancel();
    _tessera = c;
    _events = c.events.listen(_onEngineEvent, onDone: () {
      _events = null;
      if (identical(_tessera, c)) _tessera = null;
    });

    _font = await registerGameFont(c);
    _feltYou = c.registerTileType(
        const TesseraTileType(thickness: 0.2, tint: [0.13, 0.20, 0.30, 1.0]));
    _feltFoe = c.registerTileType(
        const TesseraTileType(thickness: 0.2, tint: [0.26, 0.13, 0.16, 1.0]));

    // The showcase: every image below is a bundled Flutter asset (PNG files
    // generated by tool/generate_duel_assets.py), loaded through rootBundle
    // and handed to the engine as an SDL texture.
    final hidden =
        c.registerAtlas(await loadAssetBytes('assets/duel/card_hidden.png'));
    final back =
        c.registerAtlas(await loadAssetBytes('assets/duel/card_back.png'));
    _deckDef = c.registerCardType(TesseraCardType(
        visibleAtlas: back, hiddenAtlas: hidden, backAtlas: back));
    for (var t = 0; t < duelSpecs.length; t++) {
      final spec = duelSpecs[t];
      final face = c.registerAtlas(await composeCardFace(
        artKey: spec.artKey,
        name: spec.name,
        cost: spec.cost,
        attack: spec.attack,
        health: spec.health,
        flavor: spec.flavor,
      ));
      _cardDef[t] = c.registerCardType(TesseraCardType(
        visibleAtlas: face,
        hiddenAtlas: hidden,
        backAtlas: back,
        width: 1.6,
        height: 2.4,
        cornerRadius: 0.11,
      ));
    }

    // …and every sound is a bundled WAV asset played through SDL audio.
    Future<int> snd(String name) async =>
        c.registerSound(await loadAssetBytes('assets/duel/sfx/$name.wav'));
    _sndDraw = await snd('draw');
    _sndPlay = await snd('play');
    _sndAttack = await snd('attack');
    _sndHurt = await snd('hurt');
    _sndWin = await snd('win');
    _sndLose = await snd('lose');
    _sndTurn = await snd('turn');
  }

  /// Engine-beat audio: a card physically leaving a deck pile is the moment
  /// the draw swish belongs to (the reducer never looks at events).
  void _onEngineEvent(TesseraEvent e) {
    if (e.type == TesseraEventType.cardDealt) {
      _tessera?.playSound(_sndDraw, gain: 0.8);
    }
  }

  @override
  DuelState initial() => DuelIdle(seed: 0xD0E1);

  /// The pure reducer plus action-beat audio (sounds are fire-and-forget side
  /// effects of the *controller*, not of `duelUpdate`, which stays pure and
  /// unit-tested).
  @override
  DuelState update(DuelState state, DuelAction action) {
    final next = duelUpdate(state, action);
    if (!identical(next, state)) _actionSounds(state, next);
    return next;
  }

  void _actionSounds(DuelState before, DuelState after) {
    final c = _tessera;
    if (c == null) return;
    if (after is DuelOver) {
      c.playSound(after.youWon ? _sndWin : _sndLose);
      return;
    }
    if (after.strike != null) {
      c.playSound(_sndAttack);
      if (after.you.life < before.you.life || after.foe.life < before.foe.life) {
        c.playSound(_sndHurt, gain: 0.9);
      }
      return;
    }
    final crossedTurn = (before is DuelYourTurn && after is DuelFoeTurn) ||
        (before is DuelFoeTurn && after is DuelYourTurn) ||
        (before is DuelDealing && after is DuelYourTurn);
    if (crossedTurn) {
      c.playSound(_sndTurn, gain: 0.7);
      return;
    }
    // A creature arriving on either board thumps onto the table.
    int onBoard(DuelState s) =>
        s.you.creatures.length + s.foe.creatures.length;
    if (onBoard(after) > onBoard(before)) c.playSound(_sndPlay);
  }

  // ---- scene --------------------------------------------------------------
  int _cornerId(int x, int z) {
    if (x == -6 && z == -5) return 9001;
    if (x == 6 && z == -5) return 9002;
    if (x == -6 && z == 5) return 9003;
    if (x == 6 && z == 5) return 9004;
    return 0;
  }

  List<double> _slotPos(int slot, {required bool you}) =>
      [_slotX[slot], 0.02, you ? _yourRowZ : _foeRowZ];

  /// Where a lunge lands: on top of the struck creature, or at the enemy
  /// hero's edge of the arena for a face hit.
  List<double> _strikeTarget(DuelStrike k) {
    if (k.targetSlot >= 0) {
      final p = _slotPos(k.targetSlot, you: !k.byYou);
      return [p[0], 0.45, p[2]];
    }
    return k.byYou ? const [0, 0.6, -4.0] : const [0, 0.6, 4.2];
  }

  TesseraScene _scene(DuelState s, {bool lunge = false}) {
    final strike = s.strike;

    final tiles = <TesseraTile>[
      for (var z = -5; z <= 5; z++)
        for (var x = -6; x <= 6; x++)
          TesseraTile(
              x: x, y: z, def: z < 0 ? _feltFoe : _feltYou,
              id: _cornerId(x, z)),
    ];

    final cards = <TesseraCard>[];
    var attackerPlaced = false;

    void boardCard(DuelCreature c, int slot, {required bool you}) {
      var position = _slotPos(slot, you: you);
      if (lunge && strike != null && c.id == strike.attackerId) {
        position = _strikeTarget(strike);
        attackerPlaced = true;
      }
      cards.add(TesseraCard(
        id: c.id,
        def: _cardDef[c.type],
        position: position,
        sourceDraw: you ? _yourDeck : _foeDeck,
      ));
    }

    for (var i = 0; i < duelSlots; i++) {
      final yc = s.you.slots[i], fc = s.foe.slots[i];
      if (yc != null) boardCard(yc, i, you: true);
      if (fc != null) boardCard(fc, i, you: false);
    }
    // A lunging attacker that died in the exchange is no longer in any slot:
    // show it at the target for this beat (the settle scene then fades it).
    if (lunge && strike != null && !attackerPlaced) {
      cards.add(TesseraCard(
        id: strike.attackerId,
        def: _cardDef[strike.attackerType],
        position: _strikeTarget(strike),
      ));
    }

    for (var i = 0; i < s.you.hand.length; i++) {
      cards.add(TesseraCard(
        id: s.you.hand[i].id,
        def: _cardDef[s.you.hand[i].type],
        hand: _yourHand,
        handSlot: i,
        sourceDraw: _yourDeck,
      ));
    }
    for (var i = 0; i < s.foe.hand.length; i++) {
      cards.add(TesseraCard(
        id: s.foe.hand[i].id,
        def: _cardDef[s.foe.hand[i].type],
        hidden: true, // the concealing steel front — its face stays secret
        hand: _foeHand,
        handSlot: i,
        sourceDraw: _foeDeck,
      ));
    }

    final hands = <TesseraHand>[
      TesseraHand(
        id: _yourHand,
        position: const [0, 1.15, 4.4],
        orientation: quatAxis(1, 0, 0, -0.7),
        spreadDeg: 36,
        radius: 3.0,
        cardSpacing: 0.85,
      ),
      TesseraHand(
        id: _foeHand,
        position: const [0, 0.06, -4.2],
        orientation: quatAxis(1, 0, 0, -math.pi / 2),
        spreadDeg: 24,
        radius: 3.0,
        cardSpacing: 0.9,
      ),
    ];

    final draws = <TesseraCardDraw>[
      if (s.you.deck.isNotEmpty)
        TesseraCardDraw(
            id: _yourDeck,
            def: _deckDef,
            count: s.you.deck.length,
            position: const [5.1, 0.02, 3.1]),
      if (s.foe.deck.isNotEmpty)
        TesseraCardDraw(
            id: _foeDeck,
            def: _deckDef,
            count: s.foe.deck.length,
            position: const [-5.1, 0.02, -3.1]),
    ];

    return TesseraScene(
      tiles: tiles,
      cards: cards,
      hands: hands,
      cardDraws: draws,
      labels: _labels(s),
      highlights: _highlights(s),
      camera: TesseraCameraPose(
        focusX: 0,
        focusY: 0.2,
        distance: _camDistance,
        yaw: 0,
        pitch: 0.98,
        fov: 0.72,
      ),
    );
  }

  List<TesseraLabel> _labels(DuelState s) {
    if (_font == 0) return const [];
    final labels = <TesseraLabel>[];

    if (s is! DuelIdle) {
      labels.add(TesseraLabel(
        id: _foeLifeLabel,
        font: _font,
        text: 'Foe ${math.max(0, s.foe.life)}',
        position: const [-5.1, 1.5, -3.1], // floats above the foe's deck pile
        size: 0.5,
        color: const [1.0, 0.62, 0.55, 1.0],
      ));
      labels.add(TesseraLabel(
        id: _yourLifeLabel,
        font: _font,
        text: 'You ${math.max(0, s.you.life)}',
        position: const [5.1, 1.5, 3.1], // floats above your deck pile
        size: 0.5,
        color: const [0.62, 0.82, 1.0, 1.0],
      ));
    }
    if (s is DuelYourTurn) {
      labels.add(TesseraLabel(
        id: _manaLabel,
        font: _font,
        text: 'Mana ${s.you.mana}/${s.manaCeiling}',
        position: const [-5.1, 0.5, 3.1],
        size: 0.42,
        color: const [0.55, 0.75, 1.0, 1.0],
      ));
    }

    final banner = switch (s) {
      DuelIdle() => 'DUEL',
      DuelFoeTurn() => 'Enemy turn',
      DuelOver(:final youWon) => youWon ? 'VICTORY!' : 'DEFEAT',
      _ => null,
    };
    if (banner != null) {
      labels.add(TesseraLabel(
        id: _bannerLabel,
        font: _font,
        text: banner,
        position: const [0, 2.6, -2.6], // high over the foe's half, clear of cards
        size: s is DuelOver || s is DuelIdle ? 0.85 : 0.5,
        color: switch (s) {
          DuelOver(:final youWon) => youWon
              ? const [1.0, 0.85, 0.35, 1.0]
              : const [1.0, 0.42, 0.38, 1.0],
          DuelIdle() => const [0.95, 0.88, 0.7, 1.0],
          _ => const [1.0, 0.62, 0.55, 0.9],
        },
      ));
    }

    // Live attack/health riding each board creature (anchored to its card, so
    // the tag follows deals, lunges and fades).
    void statTag(DuelCreature c) {
      labels.add(TesseraLabel(
        id: _statLabelBase + c.id,
        font: _font,
        anchor: TesseraLabelAnchor.card,
        anchorId: c.id,
        text: '${c.attack}/${c.hp}',
        position: const [0, 0.3, 1.05],
        size: 0.36,
        color: c.hp < duelSpecs[c.type].health
            ? const [1.0, 0.55, 0.45, 1.0]
            : const [0.95, 0.97, 1.0, 1.0],
      ));
    }

    s.you.creatures.forEach(statTag);
    s.foe.creatures.forEach(statTag);
    return labels;
  }

  List<TesseraHighlight> _highlights(DuelState s) {
    if (s is DuelOver) {
      // The winning side's survivors bathe in gold.
      final side = s.youWon ? s.you : s.foe;
      return [
        for (final c in side.creatures)
          TesseraHighlight(
            targetId: c.id,
            kind: TesseraHighlightKind.card,
            style: TesseraHighlightStyle.glow,
            color: const [1.0, 0.84, 0.35, 1.0],
            thickness: 14,
            pulseS: 1.6,
            pulseMin: 0.5,
            pulseMax: 1.0,
          ),
      ];
    }
    if (s is! DuelYourTurn || s.strike != null) return const [];

    final hl = <TesseraHighlight>[];
    if (s.selected != 0) {
      // Chosen attacker in gold; every legal target pulses red.
      hl.add(TesseraHighlight(
        targetId: s.selected,
        kind: TesseraHighlightKind.card,
        style: TesseraHighlightStyle.glow,
        color: const [1.0, 0.84, 0.35, 1.0],
        pulseS: 1.1,
        pulseMin: 0.5,
        pulseMax: 1.0,
      ));
      for (final c in s.foe.creatures) {
        hl.add(TesseraHighlight(
          targetId: c.id,
          kind: TesseraHighlightKind.card,
          style: TesseraHighlightStyle.outline,
          color: const [1.0, 0.32, 0.26, 1.0],
          thickness: 4,
          pulseS: 1.1,
          pulseMin: 0.4,
          pulseMax: 1.0,
        ));
      }
    } else {
      // Affordable cards glow green; rested attackers get a quiet outline.
      for (final h in s.you.hand) {
        if (duelSpecs[h.type].cost <= s.you.mana && s.you.hasFreeSlot) {
          hl.add(TesseraHighlight(
            targetId: h.id,
            kind: TesseraHighlightKind.card,
            style: TesseraHighlightStyle.glow,
            color: const [0.45, 1.0, 0.55, 1.0],
            pulseS: 1.6,
            pulseMin: 0.35,
            pulseMax: 0.8,
          ));
        }
      }
      for (final c in s.you.creatures) {
        if (c.ready) {
          hl.add(TesseraHighlight(
            targetId: c.id,
            kind: TesseraHighlightKind.card,
            style: TesseraHighlightStyle.outline,
            color: const [0.95, 0.97, 1.0, 0.85],
            thickness: 3,
          ));
        }
      }
    }
    return hl;
  }

  @override
  List<TesseraScene> render(DuelState s) {
    // An attack plays as two beats: the attacker lunges onto its target, then
    // the board settles (survivor slides home, casualties fade out).
    if (s.strike != null) return [_scene(s, lunge: true), _scene(s)];
    return [_scene(s)];
  }

  // ---- host wiring --------------------------------------------------------
  @override
  DuelAction? autoAdvance(DuelState state) => switch (state) {
        DuelDealing() => const DlDealStep(),
        DuelFoeTurn() => const DlFoeStep(),
        _ => null,
      };

  @override
  Duration holdFor(DuelState state) => switch (state) {
        DuelFoeTurn() => const Duration(milliseconds: 550),
        _ => Duration.zero,
      };

  @override
  DuelAction? autoAction(DuelState state, math.Random rng) {
    switch (state) {
      case DuelIdle():
        return const DlStart();
      case DuelOver():
        return const DlNewGame();
      case DuelYourTurn():
        // Mirror the foe's heuristic: develop, then attack, then pass.
        if (state.you.hasFreeSlot) {
          DuelCardInst? best;
          for (final h in state.you.hand) {
            if (duelSpecs[h.type].cost > state.you.mana) continue;
            if (best == null ||
                duelSpecs[h.type].cost > duelSpecs[best.type].cost) {
              best = h;
            }
          }
          if (best != null) return DlPlayCard(best.id);
        }
        final ready = state.you.creatures.where((c) => c.ready).firstOrNull;
        if (ready != null) {
          final foes = state.foe.creatures;
          final kill =
              foes.where((d) => d.hp <= ready.attack).firstOrNull;
          return DlAttack(ready.id, foes.isEmpty ? 0 : (kill ?? foes.first).id);
        }
        return const DlEndTurn();
      default:
        return null;
    }
  }

  @override
  DuelAction? onTap(DuelState state, TesseraPickResult? pick,
      {Offset? local, Size? view}) {
    if (state is! DuelYourTurn || pick == null || !pick.hitCard) return null;
    final id = pick.card;

    if (state.you.hand.any((h) => h.id == id)) return DlPlayCard(id);
    if (state.you.creatures.any((c) => c.id == id)) {
      return DlSelect(state.selected == id ? 0 : id);
    }
    if (state.selected != 0 && state.foe.creatures.any((c) => c.id == id)) {
      return DlAttack(state.selected, id);
    }
    return null;
  }

  @override
  List<GameButton<DuelAction>> buttons(DuelState state) => switch (state) {
        DuelIdle() => const [
            GameButton('Start duel', DlStart(),
                icon: Icons.play_arrow, tone: GameButtonTone.primary)
          ],
        DuelYourTurn(:final selected) => [
            if (selected != 0)
              GameButton('Strike the hero', DlAttack(selected, 0),
                  icon: Icons.bolt, tone: GameButtonTone.danger),
            if (selected != 0)
              const GameButton('Cancel', DlSelect(0), icon: Icons.close),
            const GameButton('End turn', DlEndTurn(),
                icon: Icons.hourglass_bottom, tone: GameButtonTone.primary),
          ],
        DuelOver() => const [
            GameButton('New duel', DlNewGame(),
                icon: Icons.refresh, tone: GameButtonTone.primary)
          ],
        _ => const [],
      };

  @override
  String status(DuelState state) {
    switch (state) {
      case DuelIdle():
        return 'Two decks, twenty life. Tap Start duel.';
      case DuelDealing():
        return 'Dealing the opening hands…';
      case DuelYourTurn():
        final plays = state.you.hand
            .where((h) =>
                duelSpecs[h.type].cost <= state.you.mana &&
                state.you.hasFreeSlot)
            .length;
        final atk = state.you.creatures.where((c) => c.ready).length;
        if (state.selected != 0) {
          return 'Pick a target: tap a red foe card, or Strike the hero.';
        }
        return 'Round ${state.turn} · Mana ${state.you.mana}/${state.manaCeiling}'
            ' · $plays playable · $atk ready to attack';
      case DuelFoeTurn():
        return 'Enemy turn…';
      case DuelOver():
        return state.youWon
            ? 'Victory! The foe has fallen.'
            : 'Defeat — your hero has fallen.';
    }
  }

  @override
  List<TesseraScene>? onResize(
      TesseraController c, DuelState state, double w, double h) {
    final fit = c.cameraFitDistance(tileIds: _fitCorners, padding: 0.08);
    if (fit == null) return null;
    final dist = math.max(fit, 14.0);
    if (dist == _camDistance) return null;
    _camDistance = dist;
    final scenes = render(state);
    return scenes.isEmpty ? null : [scenes.last];
  }
}
