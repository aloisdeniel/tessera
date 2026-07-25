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
//
// It is also the showcase for the engine's persistence + event features:
//  · Save / Load — the whole crawl (reducer core + engine-serialized scene) is
//    written to disk; loading restores through `restoreScene`, so the board
//    *animates* from wherever it is straight back to the snapshot.
//  · Replay last turn — every beat is appended to a `TesseraReplayRecorder`
//    turn by turn; the button plays the last full turn back record by record.
//  · Engine events — ENTITY_WAYPOINT_REACHED / ENTITY_HOP_LANDED fired during
//    the hero's multi-step walk stamp a fading footprint trail on the tiles it
//    actually crossed and feed a footstep counter (UI juice only; game logic
//    never depends on them).
//  · 3D labels — a live HP/name tag rides above the hero (and each monster
//    advertises its strength), anchored to the entities' animating transforms.

import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:math' as math;
import 'dart:typed_data';

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

/// Where the camera is looking: the whole board, or in close on the hand.
enum CamFocus { board, hand }

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
    required this.camFocus,
    required this.focusSlot,
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

  // View-only camera focus, threaded through state because the harness routes
  // every change through the reducer. Persists across phases; that's fine.
  final CamFocus camFocus; // board (default) or in on the hand
  final int focusSlot; // when focused on the hand, which card (index into hand)

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
    CamFocus? camFocus,
    int? focusSlot,
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
        camFocus: camFocus ?? this.camFocus,
        focusSlot: focusSlot ?? this.focusSlot,
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
        camFocus: CamFocus.board,
        focusSlot: 0,
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

/// View-only: pull the camera in on the hand (focused on its first card).
class DgFocusHand extends DgAction {
  const DgFocusHand();
}

/// View-only: while focused on the hand, step the focused card by [d] (±1).
class DgFocusCardDelta extends DgAction {
  const DgFocusCardDelta(this.d);
  final int d;
}

/// View-only: return the camera to the whole-board framing.
class DgUnfocus extends DgAction {
  const DgUnfocus();
}

/// Side effect: snapshot the crawl (reducer core + engine-serialized scene) to
/// disk. Handled by the controller; inert in the pure reducer.
class DgSave extends DgAction {
  const DgSave();
}

/// Side effect: restore the on-disk snapshot — the board animates from
/// wherever it currently is back to the saved scene via `restoreScene`.
class DgLoad extends DgAction {
  const DgLoad();
}

/// Side effect: play the last recorded turn back through the replay container.
class DgReplayTurn extends DgAction {
  const DgReplayTurn();
}

/// Intrinsic: a restore / replay driven outside the host's scene queue has
/// fully settled — resume normal play.
class DgAsyncDone extends DgAction {
  const DgAsyncDone();
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

/// A saved crawl is being restored: `restoreScene` is animating the board from
/// wherever it was to the snapshot, and [c] is the *loaded* core the game
/// resumes on (via [DgAsyncDone]) once the engine settles. Input is ignored.
class DgLoading extends DgState {
  const DgLoading(super.c);
}

/// The replay container is pushing the last turn's recorded beats back through
/// the engine; input is ignored until it finishes and play resumes on [c].
class DgReplaying extends DgState {
  const DgReplaying(super.c);
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
    // Camera-focus actions are view-only: they touch camFocus/focusSlot on the
    // Core and keep the current phase, so they're valid from any state.
    case DgFocusHand():
      if (s.c.hand.isEmpty) return s;
      return _reface(s, s.c.copy(camFocus: CamFocus.hand, focusSlot: 0));
    case DgFocusCardDelta():
      if (s.c.camFocus != CamFocus.hand || s.c.hand.isEmpty) return s;
      final slot = (s.c.focusSlot + a.d).clamp(0, s.c.hand.length - 1);
      return _reface(s, s.c.copy(focusSlot: slot));
    case DgUnfocus():
      return _reface(s, s.c.copy(camFocus: CamFocus.board));
    // Save / load / replay are side effects: DungeonController.update
    // intercepts them before the pure reducer runs. Inert here so the sealed
    // switch stays exhaustive without polluting the game rules.
    case DgSave():
    case DgLoad():
    case DgReplayTurn():
    case DgAsyncDone():
      return s;
  }
}

/// Rebuild [s] with a new [core], preserving its phase subclass + extras — used
/// by the view-only camera-focus actions, which mutate only the Core.
DgState _reface(DgState s, Core core) => switch (s) {
      DgRoll() => DgRoll(core),
      DgMoving() => DgMoving(core, s.target),
      DgDrew() => DgDrew(core, s.card),
      DgCombat() => DgCombat(core,
          mon: s.mon,
          heroSeed: s.heroSeed,
          monSeed: s.monSeed,
          heroDie: s.heroDie,
          monDie: s.monDie,
          bonus: s.bonus),
      DgWon() => DgWon(core),
      DgLost() => DgLost(core),
      DgLoading() => DgLoading(core),
      DgReplaying() => DgReplaying(core),
    };

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
  static const int _heroLabelId = 700;
  int _monsterLabelId(int mi) => 710 + mi;

  // ---- persistence / events / replay plumbing ------------------------------
  TesseraController? _c; // for the serialize/replay APIs outside the queue
  int _font = 0; // shared label font def (0 = unavailable, labels skipped)
  StreamSubscription<TesseraEvent>? _events;
  final List<Cell> _trail = []; // tiles the hero crossed, oldest first (events)
  int _stepsTotal = 0; // lifetime hops landed, event-fed (status garnish)
  TesseraReplayRecorder? _turnRec; // the turn currently being recorded
  Uint8List? _lastTurn; // flattened container of the last completed turn
  final Stopwatch _clock = Stopwatch()..start(); // replay-record timestamps
  bool _asyncDone = false; // an out-of-queue restore/replay has settled
  bool _quietSteps = false; // don't count replayed/restored hops as footsteps
  String? _note; // one-shot status suffix ("saved" / "load failed")

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
        // Snappier board<->hero dolly (was 0.9): each move triggers a dolly in
        // and back out, so a long camera tween doubled up around a 0.64s walk.
        cameraS: 0.6,
      );

  @override
  Future<void> registerDefs(TesseraController c) async {
    _c = c;
    _font = await registerGameFont(c);
    // Step feedback rides the engine's event stream (see _onEvent). Cancel any
    // previous subscription first (defensive: registerDefs runs once per view),
    // and tear down with the stream — it closes on controller.dispose(), and
    // onDone also releases the native replay recorder.
    await _events?.cancel();
    _events = c.events.listen(_onEvent, onDone: _teardown);

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

  /// Engine-event juice: while the hero's multi-step walk animates, each
  /// ENTITY_WAYPOINT_REACHED stamps a footprint on the tile the engine reports
  /// it crossed, and each ENTITY_HOP_LANDED bumps the footstep counter. Purely
  /// visual — the reducer never reads either (state already owns the walk).
  void _onEvent(TesseraEvent e) {
    if (e.subject != TesseraEventSubject.entity || e.subjectId != _heroId) {
      return;
    }
    if (e.type == TesseraEventType.entityHopLanded) {
      if (!_quietSteps) _stepsTotal++;
    } else if (e.type == TesseraEventType.entityWaypointReached) {
      _trail.removeWhere((t) => t.x == e.x && t.z == e.y);
      _trail.add(Cell(e.x, e.y));
      if (_trail.length > 8) _trail.removeAt(0); // longest walk is 6 steps
    }
  }

  /// Fired when the controller's event stream closes (i.e. the view was
  /// disposed): drop the dead subscription and free the native recorder.
  void _teardown() {
    _events = null;
    _turnRec?.dispose();
    _turnRec = null;
  }

  @override
  DgState initial() => DgRoll(Core.initial());

  @override
  DgState update(DgState state, DgAction action) {
    _note = null; // one-shot: any action clears the last save/load note
    switch (action) {
      case DgSave():
        return (state is DgRoll) ? _save(state) : state;
      case DgLoad():
        return (state is DgRoll || state is DgWon || state is DgLost)
            ? _load(state)
            : state;
      case DgReplayTurn():
        return (state is DgRoll && _lastTurn != null)
            ? _replayLastTurn(state)
            : state;
      case DgAsyncDone():
        // The out-of-queue restore / replay settled: resume normal play. The
        // resumed state's render pushes its scene the ordinary way, which the
        // engine diffs to (almost) nothing since the board is already there.
        return (state is DgLoading || state is DgReplaying)
            ? DgRoll(state.c)
            : state;
      case DgRollMove():
        // A fresh walk lays a fresh footprint trail (see _onEvent); the old
        // one drops out of the next scene and fades away.
        if (state is DgRoll) _trail.clear();
        return dgUpdate(state, action);
      case DgReset():
        _trail.clear();
        _stepsTotal = 0;
        _lastTurn = null;
        _turnRec?.dispose();
        _turnRec = null;
        return dgUpdate(state, action);
      default:
        // While a restore / replay drives the engine, everything else waits.
        if (state is DgLoading || state is DgReplaying) return state;
        return dgUpdate(state, action);
    }
  }

  // ---- save / load / replay ------------------------------------------------
  // The save file pairs a tiny JSON header (the reducer's Core, so the *game*
  // resumes exactly) with the engine's own serialized-scene blob (so the
  // *board* restores through tessera's deserialize path):
  //   'DGS1' · u32-le header length · header JSON · serializeScene bytes.

  static final String _savePath =
      '${Directory.systemTemp.path}/tessera_dungeon_save.bin';

  DgState _save(DgRoll s) {
    final ctrl = _c;
    if (ctrl == null) return s;
    try {
      final blob = ctrl.serializeScene(_scene(s));
      final head = utf8.encode(jsonEncode(<String, dynamic>{
        'heroPos': s.c.heroPos,
        'hp': s.c.hp,
        'deck': s.c.deck,
        'hand': [
          for (final h in s.c.hand) [h.id, h.kind],
        ],
        'monsters': [
          for (final m in s.c.monsters) [m.idx, m.power, m.alive ? 1 : 0],
        ],
        'nextId': s.c.nextId,
        'seed': s.c.seed,
        'moveSeed': s.c.moveSeed,
        'moveValue': s.c.moveValue,
        'steps': _stepsTotal,
        'trail': [
          for (final t in _trail) [t.x, t.z],
        ],
      }));
      final len = ByteData(4)..setUint32(0, head.length, Endian.little);
      final out = BytesBuilder(copy: false)
        ..add('DGS1'.codeUnits)
        ..add(len.buffer.asUint8List())
        ..add(head)
        ..add(blob);
      File(_savePath).writeAsBytesSync(out.takeBytes());
      _note = 'saved ✓';
    } catch (_) {
      _note = 'save failed'; // disk trouble is a status note, never a crash
    }
    return s;
  }

  DgState _load(DgState s) {
    final ctrl = _c;
    if (ctrl == null) return s;
    final Core core;
    final Uint8List blob;
    try {
      final bytes = File(_savePath).readAsBytesSync();
      if (bytes.length < 8 || String.fromCharCodes(bytes, 0, 4) != 'DGS1') {
        throw const FormatException('bad save magic');
      }
      final headLen =
          ByteData.sublistView(bytes, 4, 8).getUint32(0, Endian.little);
      final head = jsonDecode(utf8.decode(bytes.sublist(8, 8 + headLen)))
          as Map<String, dynamic>;
      blob = bytes.sublist(8 + headLen);
      core = Core(
        heroPos: head['heroPos'] as int,
        hp: head['hp'] as int,
        deck: [for (final k in head['deck'] as List) k as int],
        hand: [
          for (final h in head['hand'] as List)
            ItemCard((h as List)[0] as int, h[1] as int),
        ],
        monsters: [
          for (final m in head['monsters'] as List)
            Monster(
              idx: (m as List)[0] as int,
              power: m[1] as int,
              alive: m[2] as int != 0,
            ),
        ],
        nextId: head['nextId'] as int,
        seed: head['seed'] as int,
        moveSeed: head['moveSeed'] as int,
        moveValue: head['moveValue'] as int,
        camFocus: CamFocus.board,
        focusSlot: 0,
      );
      _stepsTotal = head['steps'] as int? ?? 0;
      _trail
        ..clear()
        ..addAll([
          for (final t in (head['trail'] as List? ?? const []))
            Cell((t as List)[0] as int, t[1] as int),
        ]);
    } catch (_) {
      _note = 'load failed'; // missing/corrupt file: stay where we are
      return s;
    }
    // Restore through the engine's own deserialize path: the saved scene is
    // pushed with setScene semantics, so the board *animates* from wherever it
    // currently is straight to the snapshot. The transitional DgLoading state
    // renders nothing — the host queue stays empty while the engine settles —
    // and autoAdvance resumes on the loaded core via DgAsyncDone. (A load can
    // never race a walk: the host disables buttons while any transition is in
    // flight, and update() ignores DgLoad during DgLoading/DgReplaying.) If
    // the blob is somehow stale the game still recovers, because the resumed
    // state's own render pushes the same scene the normal way.
    _lastTurn = null; // the last recorded turn belongs to the pre-load board
    _asyncDone = false;
    _quietSteps = true;
    () async {
      try {
        await ctrl.restoreScene(blob);
      } catch (_) {
        // Malformed blob: recovery happens via the resumed state's render.
      }
      _quietSteps = false;
      _asyncDone = true;
    }();
    return DgLoading(core);
  }

  DgState _replayLastTurn(DgRoll s) {
    final ctrl = _c;
    final data = _lastTurn;
    if (ctrl == null || data == null) return s;
    // Step the container's records in order; each play() resolves once that
    // beat has fully animated, so the turn re-runs with its original pacing
    // (record 0 is the pre-roll resting frame, so playback opens by gliding
    // the board back to where the turn began).
    _asyncDone = false;
    _quietSteps = true;
    () async {
      try {
        final player = ctrl.openReplay(data);
        try {
          for (var i = 0; i < player.length; i++) {
            // _events nulls out when the controller's stream closes, i.e. the
            // view is being disposed mid-replay — stop pushing scenes then.
            // (Belt and braces: play() itself also no-ops once the controller
            // is disposed, so a scene can't reach a torn-down engine even if
            // that teardown ordering ever changes.)
            if (_events == null) break;
            await player.play(i);
          }
        } finally {
          player.dispose();
        }
      } catch (_) {
        // A corrupt container just ends the replay early.
      }
      _quietSteps = false;
      _asyncDone = true;
    }();
    return DgReplaying(s.c);
  }

  /// Feed the replay recorder with every scene the host is about to play. A
  /// "turn" spans from one resting state (roll prompt / game over) to the
  /// next: the resting frame seeds the container so playback opens on the
  /// board as it stood, every beat between (die throw, walk, draw, duel) is
  /// appended, and the closing resting frame flattens the container into
  /// [_lastTurn] for the "Replay last turn" button.
  void _record(DgState s, List<TesseraScene> scenes) {
    final ctrl = _c;
    if (ctrl == null) return;
    try {
      final now = _clock.elapsedMilliseconds;
      final boundary = s is DgRoll || s is DgWon || s is DgLost;
      var rec = _turnRec;
      if (boundary) {
        if (rec != null && rec.length > 1) {
          for (final scene in scenes) {
            rec.add(scene, timestampMs: now);
          }
          _lastTurn = rec.serialize();
        }
        rec?.dispose();
        _turnRec = ctrl.createReplayRecorder()
          ..add(scenes.last, timestampMs: now);
        return;
      }
      rec ??= _turnRec = ctrl.createReplayRecorder();
      for (final scene in scenes) {
        rec.add(scene, timestampMs: now);
      }
    } catch (_) {
      // Recording is pure garnish — never let it break a render.
    }
  }

  // Tap wiring for the camera-focus harness:
  //  - board focus: tapping one of the hero's hand cards pulls the camera in on
  //    the hand (focused on its first card);
  //  - hand focus: tapping the left/right screen edge steps the focused card;
  //    a tap anywhere else does nothing (the 'Back to board' button unfocuses).
  @override
  DgAction? onTap(DgState state, TesseraPickResult? pick,
      {Offset? local, Size? view}) {
    if (state is DgLoading || state is DgReplaying) return null;
    final c = state.c;
    if (c.camFocus == CamFocus.hand) {
      if (local != null && view != null) {
        final dx = local.dx;
        if (dx < view.width * 0.22) return const DgFocusCardDelta(-1);
        if (dx > view.width * 0.78) return const DgFocusCardDelta(1);
      }
      return null;
    }
    if (pick != null &&
        pick.hitCard &&
        c.hand.any((card) => card.id == pick.card)) {
      return const DgFocusHand();
    }
    return null;
  }

  int _tileDef(int idx) {
    if (idx == 0) return _start;
    if (idx == kGoal) return _goalTile;
    if (kMonsters.containsKey(idx)) return _lair;
    if (kLoot.contains(idx)) return _lootTile;
    return _stone;
  }

  // [moveHero] false renders the movement beat's *first* frame: the die is on
  // the table but the hero has not yet stepped off its tile (see [render]).
  TesseraScene _scene(DgState s, {bool moveHero = true}) {
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
      if (moveHero && s is DgMoving && s.target > c.heroPos) {
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

    // Footprint decals on the tiles the hero actually crossed, fed by the
    // engine's ENTITY_WAYPOINT_REACHED events during the walk (see _onEvent):
    // the trail is empty while the walk itself renders, fades in with the
    // arrival scene, lingers while you plan, and fades out when the next roll
    // clears it. Older steps are fainter; the hero's own tile stays clean.
    final overlays = <TesseraOverlay>[
      for (var i = 0; i < _trail.length; i++)
        if (_trail[i].x != kPath[c.heroPos].x ||
            _trail[i].z != kPath[c.heroPos].z)
          TesseraOverlay(
            x: _trail[i].x,
            y: _trail[i].z,
            shape: TesseraOverlayShape.disc,
            tint: [0.55, 0.85, 1.0, 0.12 + 0.26 * (i + 1) / _trail.length],
          ),
    ];

    // 3D labels: the hero's name + HP tag rides along above the piece (the
    // offset tracks its live animating transform, so it walks, hops and gets
    // knocked back with him), and every living monster advertises its power.
    final labels = <TesseraLabel>[
      if (_font != 0 && s is! DgLost)
        TesseraLabel(
          id: _heroLabelId,
          font: _font,
          text: 'Hero · HP ${c.hp}/$kMaxHp',
          anchor: TesseraLabelAnchor.entity,
          anchorId: _heroId,
          position: const [0, 2.25, 0],
          size: 0.34,
          color: switch (c.hp) {
            >= kMaxHp => const [0.72, 1.0, 0.75, 1.0],
            2 => const [1.0, 0.88, 0.5, 1.0],
            _ => const [1.0, 0.5, 0.45, 1.0],
          },
        ),
      if (_font != 0)
        for (var i = 0; i < c.monsters.length; i++)
          if (c.monsters[i].alive)
            TesseraLabel(
              id: _monsterLabelId(i),
              font: _font,
              text: '${c.monsters[i].power > 1 ? 'Ogre' : 'Grunt'} · '
                  '+${c.monsters[i].power}',
              anchor: TesseraLabelAnchor.entity,
              anchorId: _monsterId(i),
              position: const [0, 2.0, 0],
              size: 0.28,
              color: const [1.0, 0.62, 0.58, 0.95],
            ),
    ];

    return TesseraScene(
      tiles: tiles,
      entities: entities,
      dice: dice,
      cards: cards,
      hands: hands,
      cardDraws: draws,
      overlays: overlays,
      labels: labels,
      camera: _cameraFor(s, moveHero: moveHero),
    );
  }

  // Whole-board orbit by default; when focused on the hand, frame the hand with
  // the current card fullscreen-centre (its fan neighbours fall to the edges).
  // The engine tweens between the two poses, so board<->hand and card<->card
  // glide smoothly.
  TesseraCamera _cameraFor(DgState s, {bool moveHero = true}) {
    final c = s.c;
    // While the hero is *walking* the track, ride along with it (FOCUS_ENTITY
    // tracks the piece's live position each tick). The throw beat (moveHero
    // false) instead keeps the board framing the roll used, so the movement die
    // — planted at the far corner — is read in a stable frame rather than
    // dollying in tight on the still-stationary hero (which threw the die
    // off-screen and wasted the whole beat). Landing falls back to the board
    // view, so the camera glides back out after each move.
    if (s is DgMoving && moveHero) {
      return const TesseraCameraFocusEntity(_heroId,
          distance: 9.0, yaw: 0, pitch: 0.92, fov: 0.72);
    }
    // A duel is decided by two dice thrown at the front edge; frame them close
    // instead of reading them tiny from the far whole-board pose.
    if (s is DgCombat) {
      return const TesseraCameraFocusDice(_heroDieId,
          distance: 9.0, yaw: 0, pitch: 0.95, fov: 0.72);
    }
    if (c.camFocus == CamFocus.hand && c.hand.isNotEmpty) {
      final slot = c.focusSlot.clamp(0, c.hand.length - 1);
      return TesseraCameraFocusHand(_hand,
          cardId: c.hand[slot].id, padding: 0.10);
    }
    return TesseraCameraPose(
      focusX: 0,
      focusY: 0.4,
      distance: _camDistance,
      yaw: 0,
      pitch: 0.92,
      fov: 0.72,
    );
  }

  @override
  List<TesseraScene> render(DgState s) {
    // While a restore or replay drives the engine directly (outside the host's
    // scene queue), hand the host nothing to push — autoAdvance fires
    // DgAsyncDone once the engine settles and normal rendering resumes.
    if (s is DgLoading || s is DgReplaying) return const [];
    // A movement roll plays in two beats so the die is read *before* the hero
    // commits: first throw the die with the hero still on its tile, then — once
    // the host has awaited that scene settling (TesseraController.setScene's
    // Future) — walk the hero to the destination. Every other state is one scene.
    final scenes = (s is DgMoving && s.target > s.c.heroPos)
        ? [_scene(s, moveHero: false), _scene(s, moveHero: true)]
        : [_scene(s)];
    _record(s, scenes); // every beat lands in the turn's replay container
    return scenes;
  }

  @override
  List<TesseraScene>? onResize(TesseraController c, DgState state, double w, double h) {
    // Never push a re-fit while a restore/replay owns the engine.
    if (state is DgLoading || state is DgReplaying) return null;
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
        // A restore/replay runs outside the scene queue: once its Future has
        // flagged completion (and the engine is idle again), resume play.
        DgLoading() || DgReplaying() =>
          _asyncDone ? const DgAsyncDone() : null,
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
    // While the camera is in on the hand, always offer a way back to the board.
    if (state.c.camFocus == CamFocus.hand) {
      return [
        const GameButton('Back to board', DgUnfocus(),
            icon: Icons.close, tone: GameButtonTone.primary),
        ..._stateButtons(state),
      ];
    }
    return _stateButtons(state);
  }

  List<GameButton<DgAction>> _stateButtons(DgState state) {
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
        if (_lastTurn != null) {
          out.add(const GameButton('Replay last turn', DgReplayTurn(),
              icon: Icons.replay));
        }
        out.add(const GameButton('Save', DgSave(), icon: Icons.save_outlined));
        if (File(_savePath).existsSync()) {
          out.add(const GameButton('Load', DgLoad(), icon: Icons.folder_open));
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
        return [
          const GameButton('New crawl', DgReset(),
              icon: Icons.refresh, tone: GameButtonTone.primary),
          if (File(_savePath).existsSync())
            const GameButton('Load save', DgLoad(), icon: Icons.folder_open),
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
            'deck ${c.deck.length}  ·  hand ${c.hand.length}'
            '${_stepsTotal > 0 ? '  ·  👣 $_stepsTotal' : ''}'
            '  —  roll to move${_note != null ? '  ·  $_note' : ''}';
      case DgMoving():
        return 'Rolled a ${c.moveValue} — advancing…  ·  $life';
      case DgDrew():
        return 'Found a card: ${itemNames[state.card.kind]} (${itemBlurb[state.card.kind]})  ·  $life';
      case DgCombat():
        final m = c.monsters[state.mon];
        return 'Duel!  you ${state.heroDie}${state.bonus > 0 ? '+${state.bonus}' : ''} = ${state.heroTotal}'
            '  vs  monster ${state.monDie}+${m.power} = ${state.monTotal}  ·  $life';
      case DgWon():
        return 'You reached the treasure!  🏆  Tap New crawl.'
            '${_note != null ? '  ·  $_note' : ''}';
      case DgLost():
        return 'You fell in the dungeon.  ☠  Tap New crawl.'
            '${_note != null ? '  ·  $_note' : ''}';
      case DgLoading():
        return 'Loading — the board glides back to your saved crawl…';
      case DgReplaying():
        return 'Instant replay — last turn, straight from the engine\'s '
            'recording…';
    }
  }
}
