// dungeon.dart — "Tessera Dungeon", the one game that exercises *every*
// subsystem at once: tiles (the winding track), board entities (the hero and
// the monsters, which stack when they share a tile), dice (a thrown d6 to move,
// and a two-dice duel to fight) and cards (an item deck you draw into a fanned
// hand and play mid-combat).
//
// Same reducer shape as the other examples (see game.dart): a sealed action + a
// sealed state carrying its phase data, a pure reducer `dgUpdate`, and a
// controller that projects the state to one — or a short sequence of — scenes.
//
// The loop: roll to move → the hero walks the track one tile at a time → if a
// monster blocks the path you stop and duel (hero d6 + item bonuses vs monster
// d6 + power) → landing on a loot tile draws a card → reach the treasure to win.

import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter_tessera/flutter_tessera.dart';

import 'chess_gen.dart';
import 'dice_art.dart';
import 'dungeon_art.dart';
import 'game.dart';
import 'quat.dart';

// ---- the fixed track ------------------------------------------------------
// A snake of tiles winding across the grid. Cell i sits at (kPathX[i], kPathZ[i]).

class Cell {
  const Cell(this.x, this.z);
  final int x;
  final int z;
}

List<Cell> _buildPath() {
  final p = <Cell>[];
  for (var x = -3; x <= 3; x++) {
    p.add(Cell(x, 2)); // idx 0..6  (start row, nearest the player)
  }
  p.add(const Cell(3, 1)); // idx 7   (connector)
  for (var x = 3; x >= -3; x--) {
    p.add(Cell(x, 0)); // idx 8..14
  }
  p.add(const Cell(-3, -1)); // idx 15  (connector)
  for (var x = -3; x <= 3; x++) {
    p.add(Cell(x, -2)); // idx 16..22 (far row, goal at the end)
  }
  return p;
}

final List<Cell> kPath = _buildPath();
final int kGoal = kPath.length - 1; // 22

// Track cells keyed by "x,z" so the floor can skip them (one tile per coord).
final Map<String, int> kTrackAt = {
  for (var i = 0; i < kPath.length; i++) '${kPath[i].x},${kPath[i].z}': i,
};

// Monsters block the path at these indices; loot tiles hand out a card.
const Map<int, int> kMonsters = {8: 1, 16: 2}; // index -> power bonus
const Set<int> kLoot = {3, 6, 11, 14, 19};

const int kMaxHp = 3;
const int kSwordBonus = 3;

int _lcg(int seed) => (seed * 1103515245 + 12345) & 0x7fffffff;
int _die6(int seed) => 1 + _lcg(seed) % 6;

// ---- immutable value types -----------------------------------------------

class Monster {
  const Monster({required this.idx, required this.power, required this.alive});
  final int idx;
  final int power;
  final bool alive;
  Monster kill() => Monster(idx: idx, power: power, alive: false);
}

class ItemCard {
  const ItemCard(this.id, this.kind);
  final int id; // stable instance id (the renderer diff key)
  final int kind; // itemSword / itemShield / itemPotion
}

/// Everything shared by every phase. Phases wrap one of these plus extras.
class Core {
  const Core({
    required this.heroPos,
    required this.hp,
    required this.deck,
    required this.hand,
    required this.monsters,
    required this.nextId,
    required this.seed,
    required this.moveSeed,
    required this.moveValue,
  });

  final int heroPos;
  final int hp;
  final List<int> deck; // remaining item kinds, top = last
  final List<ItemCard> hand;
  final List<Monster> monsters;
  final int nextId; // next card instance id
  final int seed; // rng stream
  final int moveSeed; // last movement-die throw seed (0 = none thrown yet)
  final int moveValue; // last movement-die value (1..6)

  Core copy({
    int? heroPos,
    int? hp,
    List<int>? deck,
    List<ItemCard>? hand,
    List<Monster>? monsters,
    int? nextId,
    int? seed,
    int? moveSeed,
    int? moveValue,
  }) =>
      Core(
        heroPos: heroPos ?? this.heroPos,
        hp: hp ?? this.hp,
        deck: deck ?? this.deck,
        hand: hand ?? this.hand,
        monsters: monsters ?? this.monsters,
        nextId: nextId ?? this.nextId,
        seed: seed ?? this.seed,
        moveSeed: moveSeed ?? this.moveSeed,
        moveValue: moveValue ?? this.moveValue,
      );

  int handCount(int kind) => hand.where((c) => c.kind == kind).length;

  factory Core.initial() => Core(
        heroPos: 0,
        hp: kMaxHp,
        deck: const [
          itemSword, itemPotion, itemShield, itemSword, //
          itemShield, itemPotion, itemSword,
        ],
        hand: const [],
        monsters: [
          for (final e in kMonsters.entries)
            Monster(idx: e.key, power: e.value, alive: true),
        ],
        nextId: 1000,
        seed: 0x51E5A7,
        moveSeed: 0,
        moveValue: 0,
      );
}

// ---- actions -------------------------------------------------------------
sealed class DgAction {
  const DgAction();
}

class DgRollMove extends DgAction {
  const DgRollMove();
}

/// Intrinsic: finish the move once the multi-step walk has played out.
class DgStep extends DgAction {
  const DgStep();
}

/// Intrinsic: leave the "card drawn" beat and return to the roll phase.
class DgAfterDraw extends DgAction {
  const DgAfterDraw();
}

class DgPlaySword extends DgAction {
  const DgPlaySword();
}

class DgFight extends DgAction {
  const DgFight();
}

class DgDrinkPotion extends DgAction {
  const DgDrinkPotion();
}

class DgReset extends DgAction {
  const DgReset();
}

// ---- states --------------------------------------------------------------
sealed class DgState {
  const DgState(this.c);
  final Core c;
}

/// Awaiting the movement roll.
class DgRoll extends DgState {
  const DgRoll(super.c);
}

/// The hero is walking toward [target] — the renderer plays the whole run as a
/// single multi-step move (one hop per tile), then [DgStep] finishes it.
class DgMoving extends DgState {
  const DgMoving(super.c, this.target);
  final int target;
}

/// A card was just drawn onto the hand — a one-beat pause to show it.
class DgDrew extends DgState {
  const DgDrew(super.c, this.card);
  final ItemCard card;
}

/// A duel: both dice are on the table; play a Sword and/or commit to Fight.
class DgCombat extends DgState {
  const DgCombat(
    super.c, {
    required this.mon,
    required this.heroSeed,
    required this.monSeed,
    required this.heroDie,
    required this.monDie,
    required this.bonus,
  });
  final int mon; // index into c.monsters
  final int heroSeed;
  final int monSeed;
  final int heroDie;
  final int monDie;
  final int bonus; // accumulated from played swords

  int get heroTotal => heroDie + bonus;
  int get monTotal => monDie + c.monsters[mon].power;
}

class DgWon extends DgState {
  const DgWon(super.c);
}

class DgLost extends DgState {
  const DgLost(super.c);
}

// ---- reducer -------------------------------------------------------------
DgState dgUpdate(DgState s, DgAction a) {
  switch (a) {
    case DgReset():
      return DgRoll(Core.initial());
    case DgRollMove():
      return (s is DgRoll) ? _roll(s) : s;
    case DgStep():
      return (s is DgMoving) ? _step(s) : s;
    case DgAfterDraw():
      return (s is DgDrew) ? DgRoll(s.c) : s;
    case DgDrinkPotion():
      return (s is DgRoll) ? _drink(s) : s;
    case DgPlaySword():
      return (s is DgCombat) ? _playSword(s) : s;
    case DgFight():
      return (s is DgCombat) ? _fight(s) : s;
  }
}

DgState _roll(DgRoll s) {
  final seed = _lcg(s.c.seed);
  final value = _die6(seed);
  var target = math.min(s.c.heroPos + value, kGoal);
  // A live monster in the way forces you to stop and duel the nearest one.
  for (final m in s.c.monsters) {
    if (m.alive && m.idx > s.c.heroPos && m.idx <= target) {
      target = math.min(target, m.idx);
    }
  }
  final c = s.c.copy(seed: _lcg(seed), moveSeed: seed, moveValue: value);
  return DgMoving(c, target);
}

// The whole walk plays as a single multi-step move (see `_scene`), so one beat
// jumps the hero straight to the target tile and resolves what's on it.
DgState _step(DgMoving s) => _arrive(s.c.copy(heroPos: s.target));

/// The hero has reached a tile — resolve what's on it.
DgState _arrive(Core c) {
  final mi = c.monsters.indexWhere((m) => m.alive && m.idx == c.heroPos);
  if (mi >= 0) return _startCombat(c, mi);
  if (c.heroPos == kGoal) return DgWon(c);
  if (kLoot.contains(c.heroPos) && c.deck.isNotEmpty) return _draw(c);
  return DgRoll(c);
}

DgState _draw(Core c) {
  final deck = List<int>.of(c.deck);
  final kind = deck.removeLast();
  final card = ItemCard(c.nextId, kind);
  final hand = [...c.hand, card];
  return DgDrew(c.copy(deck: deck, hand: hand, nextId: c.nextId + 1), card);
}

DgState _startCombat(Core c, int mi) {
  final hs = _lcg(c.seed);
  final ms = _lcg(hs);
  return DgCombat(
    c.copy(seed: _lcg(ms)),
    mon: mi,
    heroSeed: hs,
    monSeed: ms,
    heroDie: _die6(hs),
    monDie: _die6(ms),
    bonus: 0,
  );
}

DgState _playSword(DgCombat s) {
  final i = s.c.hand.indexWhere((card) => card.kind == itemSword);
  if (i < 0) return s;
  final hand = List<ItemCard>.of(s.c.hand)..removeAt(i);
  return DgCombat(
    s.c.copy(hand: hand),
    mon: s.mon,
    heroSeed: s.heroSeed,
    monSeed: s.monSeed,
    heroDie: s.heroDie,
    monDie: s.monDie,
    bonus: s.bonus + kSwordBonus,
  );
}

DgState _fight(DgCombat s) {
  if (s.heroTotal >= s.monTotal) {
    // Victory: the monster is defeated and fades out; the hero holds the tile.
    final monsters = List<Monster>.of(s.c.monsters);
    monsters[s.mon] = monsters[s.mon].kill();
    return DgRoll(s.c.copy(monsters: monsters));
  }
  // Defeat: a held Shield soaks the hit; otherwise lose a life. Either way the
  // hero is knocked back and must approach the (still-living) monster again.
  var hand = s.c.hand;
  var hp = s.c.hp;
  final shield = hand.indexWhere((card) => card.kind == itemShield);
  if (shield >= 0) {
    hand = List<ItemCard>.of(hand)..removeAt(shield);
  } else {
    hp -= 1;
  }
  if (hp <= 0) return DgLost(s.c.copy(hp: 0, hand: hand));
  final back = math.max(0, s.c.heroPos - 2);
  return DgRoll(s.c.copy(hp: hp, hand: hand, heroPos: back));
}

DgState _drink(DgRoll s) {
  final i = s.c.hand.indexWhere((card) => card.kind == itemPotion);
  if (i < 0 || s.c.hp >= kMaxHp) return s;
  final hand = List<ItemCard>.of(s.c.hand)..removeAt(i);
  return DgRoll(s.c.copy(hp: s.c.hp + 1, hand: hand));
}

// ---- controller (rendering + wiring) -------------------------------------
class DungeonController extends GameController<DgState, DgAction> {
  int _floor = 0, _stone = 0, _start = 0, _lootTile = 0, _lair = 0, _goalTile = 0;
  int _hero = 0, _monster = 0;
  int _die = 0;
  int _poof = 0;
  final List<int> _itemDef = List<int>.filled(itemKinds, 0);
  int _deckDef = 0;
  double _camDistance = 17.5;

  // Corner tiles hugging the track + hand + deck, for cameraFitDistance so the
  // whole crawl stays framed in portrait as well as landscape.
  static const List<int> _fitCorners = [9001, 9002, 9003, 9004];
  int _cornerId(int x, int z) {
    if (x == -5 && z == -4) return 9001;
    if (x == 5 && z == -4) return 9002;
    if (x == -5 && z == 4) return 9003;
    if (x == 5 && z == 4) return 9004;
    return 0;
  }

  static const int _hand = 1;
  static const int _heroId = 1;
  int _monsterId(int mi) => 100 + mi;
  static const int _moveDieId = 200;
  static const int _heroDieId = 201;
  static const int _monDieId = 202;
  static const int _deckId = 300;

  @override
  String get title => 'Dungeon';
  @override
  String get subtitle => 'Everything · dice, cards, tiles & entities in one crawl';
  @override
  IconData get icon => Icons.castle;

  @override
  TesseraLightData get light => const TesseraLightData(
        dir: [-0.4, -1.0, -0.45],
        color: [1.0, 0.96, 0.88],
        intensity: 1.15,
        ambient: [0.3, 0.31, 0.4],
      );

  @override
  TesseraTimingData get timing => const TesseraTimingData(
        moveS: 0.32,
        addS: 0.35,
        removeS: 0.45,
        tileS: 0.4,
        reflowS: 0.35,
        cameraS: 0.9,
      );

  @override
  Future<void> registerDefs(TesseraController c) async {
    _floor = c.registerTileType(
        const TesseraTileType(thickness: 0.22, tint: [0.28, 0.29, 0.34, 1.0]));
    _stone = c.registerTileType(
        const TesseraTileType(thickness: 0.28, tint: [0.5, 0.5, 0.56, 1.0]));
    _start = c.registerTileType(
        const TesseraTileType(thickness: 0.28, tint: [0.32, 0.62, 0.4, 1.0]));
    _lootTile = c.registerTileType(
        const TesseraTileType(thickness: 0.28, tint: [0.3, 0.5, 0.78, 1.0]));
    _lair = c.registerTileType(
        const TesseraTileType(thickness: 0.28, tint: [0.62, 0.26, 0.28, 1.0]));
    _goalTile = c.registerTileType(
        const TesseraTileType(thickness: 0.34, tint: [0.86, 0.7, 0.28, 1.0]));

    _poof = c.registerEffectType(const TesseraEffectType(
      onRemove: TesseraParticle(
        count: 54,
        lifetime: 0.7,
        lifetimeVar: 0.25,
        speed: 2.0,
        speedVar: 0.7,
        spreadDeg: 62,
        gravity: 1.2,
        sizeStart: 0.16,
        sizeEnd: 0.5,
        colorStart: [0.95, 0.5, 0.4, 0.85],
        colorEnd: [0.5, 0.2, 0.2, 0.0],
      ),
    ));

    _die = c.registerDiceType(TesseraDiceType(faces: await renderDiceFaces(), size: 1.25));

    _hero = c.registerEntityType(TesseraEntityType(
      glb: buildPieceGlb(chessKing, 1, const [0.9, 0.75, 0.34, 1.0]),
      scale: 1.15,
    ));
    _monster = c.registerEntityType(TesseraEntityType(
      glb: buildPieceGlb(chessKnight, -1, const [0.72, 0.22, 0.24, 1.0]),
      scale: 1.1,
      onDespawnEffect: _poof,
    ));

    final hidden = c.registerAtlas(await renderItemHidden());
    final back = c.registerAtlas(await renderItemBack());
    _deckDef =
        c.registerCardType(TesseraCardType(visibleAtlas: back, hiddenAtlas: hidden, backAtlas: back));
    for (var k = 0; k < itemKinds; k++) {
      final face = c.registerAtlas(await renderItemCard(k));
      _itemDef[k] =
          c.registerCardType(TesseraCardType(visibleAtlas: face, hiddenAtlas: hidden, backAtlas: back));
    }
  }

  @override
  DgState initial() => DgRoll(Core.initial());

  @override
  DgState update(DgState state, DgAction action) => dgUpdate(state, action);

  int _tileDef(int idx) {
    if (idx == 0) return _start;
    if (idx == kGoal) return _goalTile;
    if (kMonsters.containsKey(idx)) return _lair;
    if (kLoot.contains(idx)) return _lootTile;
    return _stone;
  }

  TesseraScene _scene(DgState s) {
    final c = s.c;
    final tiles = <TesseraTile>[];
    // a dark stone floor under the whole play area for context (wide enough to
    // seat the hand + deck, and its corners double as camera-fit anchors);
    // skip cells the track covers so there's exactly one tile per coord.
    for (var z = -4; z <= 4; z++) {
      for (var x = -5; x <= 5; x++) {
        if (kTrackAt.containsKey('$x,$z')) continue;
        tiles.add(TesseraTile(x: x, y: z, def: _floor, id: _cornerId(x, z)));
      }
    }
    // the track laid over the floor (same coords -> replaces the floor tile)
    for (var i = 0; i < kPath.length; i++) {
      tiles.add(TesseraTile(
          x: kPath[i].x, y: kPath[i].z, def: _tileDef(i), id: 1000 + i));
    }

    final entities = <TesseraEntity>[];
    if (s is! DgLost) {
      // While moving, place the hero on the destination tile and hand the
      // renderer the whole run of intermediate tiles as a multi-step path — it
      // hops the hero along them in one move, one tile at a time.
      var heroIdx = c.heroPos;
      var heroPath = const <(int, int)>[];
      if (s is DgMoving && s.target > c.heroPos) {
        heroIdx = s.target;
        heroPath = [
          for (var i = c.heroPos + 1; i <= s.target; i++) (kPath[i].x, kPath[i].z),
        ];
      }
      entities.add(TesseraEntity(
        id: _heroId,
        def: _hero,
        x: kPath[heroIdx].x,
        y: kPath[heroIdx].z,
        path: heroPath,
      ));
    }
    for (var i = 0; i < c.monsters.length; i++) {
      final m = c.monsters[i];
      if (!m.alive) continue;
      entities.add(TesseraEntity(
        id: _monsterId(i),
        def: _monster,
        x: kPath[m.idx].x,
        y: kPath[m.idx].z,
      ));
    }

    // dice: the movement die whenever one has been thrown; the duel pair only
    // while a combat is on the table.
    final dice = <TesseraDie>[];
    if (c.moveSeed != 0 && s is! DgCombat) {
      dice.add(TesseraDie(
        id: _moveDieId,
        def: _die,
        face: c.moveValue - 1,
        position: const [3.4, 0.6, 3.4],
        seed: c.moveSeed,
        throwS: 0.9,
      ));
    }
    if (s is DgCombat) {
      dice.add(TesseraDie(
        id: _heroDieId,
        def: _die,
        face: s.heroDie - 1,
        position: const [-1.3, 0.6, 3.2],
        seed: s.heroSeed,
        throwS: 0.9,
      ));
      dice.add(TesseraDie(
        id: _monDieId,
        def: _die,
        face: s.monDie - 1,
        position: const [1.3, 0.6, 3.2],
        seed: s.monSeed,
        throwS: 0.9,
      ));
    }

    // cards: the item deck (a face-down pile) + the hero's fanned hand.
    final cards = <TesseraCard>[
      for (var i = 0; i < c.hand.length; i++)
        TesseraCard(
          id: c.hand[i].id,
          def: _itemDef[c.hand[i].kind],
          hidden: false,
          hand: _hand,
          handSlot: i,
          sourceDraw: _deckId, // drawn off the deck: fly in + flip face-up
        ),
    ];
    final hands = <TesseraHand>[
      TesseraHand(
        id: _hand,
        position: const [-3.6, 1.0, 3.9],
        orientation: quatAxis(1, 0, 0, -0.7),
        spreadDeg: 30,
        radius: 3.2,
        cardSpacing: 0.9,
      ),
    ];
    final draws = <TesseraCardDraw>[
      if (c.deck.isNotEmpty)
        TesseraCardDraw(
          id: _deckId,
          def: _deckDef,
          count: c.deck.length,
          position: const [4.4, 0.02, 3.6],
          topHidden: true,
        ),
    ];

    return TesseraScene(
      tiles: tiles,
      entities: entities,
      dice: dice,
      cards: cards,
      hands: hands,
      cardDraws: draws,
      camera: TesseraCameraPose(
        focusX: 0,
        focusY: 0.4,
        distance: _camDistance,
        yaw: 0,
        pitch: 0.92,
        fov: 0.72,
      ),
    );
  }

  @override
  List<TesseraScene> render(DgState s) => [_scene(s)];

  @override
  List<TesseraScene>? onResize(TesseraController c, DgState state, double w, double h) {
    final fit = c.cameraFitDistance(tileIds: _fitCorners, padding: 0.09);
    if (fit == null) return null;
    // Fit only ever pulls back from the tuned landscape distance (the ground-
    // plane fit ignores the raised hand cards), so portrait stops cropping
    // without landscape ever zooming in tighter than 17.5.
    final dist = math.max(fit, 17.5);
    if (dist == _camDistance) return null;
    _camDistance = dist;
    return [_scene(state)];
  }

  /// Intrinsic animation: walk the hero tile-by-tile, and clear the "drawn"
  /// beat once the renderer has shown it.
  @override
  DgAction? autoAdvance(DgState state) => switch (state) {
        DgMoving() => const DgStep(),
        DgDrew() => const DgAfterDraw(),
        _ => null,
      };

  @override
  DgAction? autoAction(DgState state, math.Random rng) => switch (state) {
        DgRoll() => (state.c.hp < kMaxHp && state.c.handCount(itemPotion) > 0)
            ? const DgDrinkPotion()
            : const DgRollMove(),
        // Play a sword if it would turn a loss into a win, then commit.
        DgCombat() => (state.heroTotal < state.monTotal &&
                state.c.handCount(itemSword) > 0)
            ? const DgPlaySword()
            : const DgFight(),
        DgWon() || DgLost() => const DgReset(),
        _ => null,
      };

  @override
  List<GameButton<DgAction>> buttons(DgState state) {
    switch (state) {
      case DgRoll():
        final out = <GameButton<DgAction>>[
          const GameButton('Roll to move', DgRollMove(),
              icon: Icons.casino, tone: GameButtonTone.primary),
        ];
        if (state.c.hp < kMaxHp && state.c.handCount(itemPotion) > 0) {
          out.add(const GameButton('Drink potion (+1 life)', DgDrinkPotion(),
              icon: Icons.local_drink, tone: GameButtonTone.normal));
        }
        return out;
      case DgCombat():
        final out = <GameButton<DgAction>>[];
        if (state.c.handCount(itemSword) > 0) {
          out.add(const GameButton('Play sword (+3)', DgPlaySword(),
              icon: Icons.colorize, tone: GameButtonTone.normal));
        }
        out.add(const GameButton('Fight!', DgFight(),
            icon: Icons.bolt, tone: GameButtonTone.danger));
        return out;
      case DgWon():
      case DgLost():
        return const [
          GameButton('New crawl', DgReset(),
              icon: Icons.refresh, tone: GameButtonTone.primary),
        ];
      default:
        return const [];
    }
  }

  String _hearts(int hp) => '${'♥' * hp}${'♡' * (kMaxHp - hp)}';

  @override
  String status(DgState state) {
    final c = state.c;
    final life = _hearts(c.hp);
    switch (state) {
      case DgRoll():
        return 'Tile ${c.heroPos}/$kGoal  ·  $life  ·  '
            'deck ${c.deck.length}  ·  hand ${c.hand.length}  —  roll to move';
      case DgMoving():
        return 'Rolled a ${c.moveValue} — advancing…  ·  $life';
      case DgDrew():
        return 'Found a card: ${itemNames[state.card.kind]} (${itemBlurb[state.card.kind]})  ·  $life';
      case DgCombat():
        final m = c.monsters[state.mon];
        return 'Duel!  you ${state.heroDie}${state.bonus > 0 ? '+${state.bonus}' : ''} = ${state.heroTotal}'
            '  vs  monster ${state.monDie}+${m.power} = ${state.monTotal}  ·  $life';
      case DgWon():
        return 'You reached the treasure!  🏆  Tap New crawl.';
      case DgLost():
        return 'You fell in the dungeon.  ☠  Tap New crawl.';
    }
  }
}
