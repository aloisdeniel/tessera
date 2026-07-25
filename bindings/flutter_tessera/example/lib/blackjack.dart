// blackjack.dart — a Blackjack example driven entirely by the Card APIs.
//
// Architecture (see game.dart): a sealed action + a sealed state (phases), a
// pure reducer `bjUpdate`, and a `render` that projects a phase into one — or a
// sequence of — TesseraScenes (the dealer reveal is a multi-scene sequence).
// The deck is a draw pile; each hand is a world-space fan of cards.
//
// Engine features on show: 3D hand-value labels beside each fan whose counters
// tick the moment a card *physically* lands or flips — the scenes exclude
// in-flight cards from the shown totals and the engine's CARD_DEALT /
// CARD_FLIPPED / op-settle events push the fully counted frame when the motion
// resolves (UI juice only; the reducer never looks at events) — plus a verdict
// banner and a pulsing glow on the winning hand at round end.

import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter_tessera/flutter_tessera.dart';

import 'card_art.dart';
import 'game.dart';
import 'quat.dart';

// ---- cards ---------------------------------------------------------------
// A physical card is 0..51: suit = id ~/ 13 (0..3), rank = id % 13 + 1 (A..K).
int _rankOf(int id) => id % 13 + 1;

/// Blackjack value of a hand + whether it is "soft" (an ace still counts 11).
({int total, bool soft}) handValue(List<int> cards) {
  var total = 0, aces = 0;
  for (final id in cards) {
    final r = _rankOf(id);
    if (r == 1) {
      aces++;
      total += 11;
    } else {
      total += r >= 10 ? 10 : r;
    }
  }
  var soft = aces > 0;
  while (total > 21 && aces > 0) {
    total -= 10;
    aces--;
  }
  if (aces == 0) soft = false;
  return (total: total, soft: soft && total <= 21);
}

/// A deterministic 52-card shuffle from [seed] (keeps the reducer pure).
List<int> _shuffledShoe(int seed) {
  final ids = List<int>.generate(52, (i) => i);
  var s = seed & 0x7fffffff;
  if (s == 0) s = 1;
  for (var i = 51; i > 0; i--) {
    s = (s * 1103515245 + 12345) & 0x7fffffff;
    final j = s % (i + 1);
    final t = ids[i];
    ids[i] = ids[j];
    ids[j] = t;
  }
  return ids;
}

int _nextSeed(int seed) => (seed * 1103515245 + 12345) & 0x7fffffff;

// ---- actions -------------------------------------------------------------
sealed class BjAction {
  const BjAction();
}

/// Shuffle and begin dealing a fresh round.
class BjDeal extends BjAction {
  const BjDeal();
}

/// Intrinsic: deal the next card while dealing.
class BjDealStep extends BjAction {
  const BjDealStep();
}

/// Player takes another card.
class BjHit extends BjAction {
  const BjHit();
}

/// Player stands; the dealer then plays to completion.
class BjStand extends BjAction {
  const BjStand();
}

/// Clear the table and return to the idle "deal" screen.
class BjNewRound extends BjAction {
  const BjNewRound();
}

// ---- state ---------------------------------------------------------------
enum BjResult { playerWin, dealerWin, push, playerBust, dealerBust, blackjack }

sealed class BjState {
  const BjState({
    required this.shoe,
    required this.player,
    required this.dealer,
    required this.seed,
  });

  final List<int> shoe;
  final List<int> player;
  final List<int> dealer;
  final int seed;

  int get playerTotal => handValue(player).total;
  int get dealerTotal => handValue(dealer).total;
}

/// Empty table, waiting for the player to deal.
class BjTable extends BjState {
  const BjTable({required super.seed})
      : super(shoe: const [], player: const [], dealer: const []);
}

/// Dealing the opening four cards (player, dealer, player, dealer).
class BjDealing extends BjState {
  const BjDealing({
    required super.shoe,
    required super.player,
    required super.dealer,
    required super.seed,
  });
}

/// The player's turn: hit or stand.
class BjPlayerTurn extends BjState {
  const BjPlayerTurn({
    required super.shoe,
    required super.player,
    required super.dealer,
    required super.seed,
  });
}

/// The round is decided; render reveals the dealer's hole card and any draws.
class BjRoundOver extends BjState {
  const BjRoundOver({
    required super.shoe,
    required super.player,
    required super.dealer,
    required super.seed,
    required this.result,
  });

  final BjResult result;
}

// ---- reducer -------------------------------------------------------------
BjState bjUpdate(BjState s, BjAction a) {
  switch (a) {
    case BjDeal():
      return BjDealing(
          shoe: _shuffledShoe(s.seed), player: const [], dealer: const [], seed: s.seed);
    case BjDealStep():
      return _dealStep(s as BjDealing);
    case BjHit():
      return _hit(s as BjPlayerTurn);
    case BjStand():
      return _stand(s as BjPlayerTurn);
    case BjNewRound():
      return BjTable(seed: _nextSeed(s.seed));
  }
}

BjState _dealStep(BjDealing d) {
  final shoe = List<int>.of(d.shoe);
  final card = shoe.removeLast();
  final player = List<int>.of(d.player);
  final dealer = List<int>.of(d.dealer);
  // Deal order p, d, p, d: player gets a card whenever it isn't ahead.
  if (player.length <= dealer.length) {
    player.add(card);
  } else {
    dealer.add(card);
  }
  if (player.length + dealer.length < 4) {
    return BjDealing(shoe: shoe, player: player, dealer: dealer, seed: d.seed);
  }
  // Naturals resolve immediately; otherwise it's the player's turn.
  if (handValue(player).total == 21 || handValue(dealer).total == 21) {
    return _resolve(shoe, player, dealer, d.seed);
  }
  return BjPlayerTurn(shoe: shoe, player: player, dealer: dealer, seed: d.seed);
}

BjState _hit(BjPlayerTurn p) {
  final shoe = List<int>.of(p.shoe);
  final player = List<int>.of(p.player)..add(shoe.removeLast());
  if (handValue(player).total > 21) {
    return BjRoundOver(
        shoe: shoe, player: player, dealer: p.dealer, seed: p.seed, result: BjResult.playerBust);
  }
  return BjPlayerTurn(shoe: shoe, player: player, dealer: p.dealer, seed: p.seed);
}

BjState _stand(BjPlayerTurn p) {
  final shoe = List<int>.of(p.shoe);
  final dealer = List<int>.of(p.dealer);
  while (handValue(dealer).total < 17) {
    dealer.add(shoe.removeLast());
  }
  return _resolve(shoe, p.player, dealer, p.seed);
}

BjState _resolve(List<int> shoe, List<int> player, List<int> dealer, int seed) {
  final pv = handValue(player).total, dv = handValue(dealer).total;
  final pBj = player.length == 2 && pv == 21;
  final dBj = dealer.length == 2 && dv == 21;
  BjResult r;
  if (pv > 21) {
    r = BjResult.playerBust;
  } else if (dv > 21) {
    r = BjResult.dealerBust;
  } else if (pBj && !dBj) {
    r = BjResult.blackjack;
  } else if (pv > dv) {
    r = BjResult.playerWin;
  } else if (pv < dv) {
    r = BjResult.dealerWin;
  } else {
    r = BjResult.push;
  }
  return BjRoundOver(shoe: shoe, player: player, dealer: dealer, seed: seed, result: r);
}

// ---- controller (rendering + wiring) -------------------------------------
class BlackjackController extends GameController<BjState, BjAction> {
  static const int _playerHand = 1;
  static const int _dealerHand = 2;
  static const int _deckId = 9;
  static const int _fullDeck = 52;
  static const int _playerLabelId = 901;
  static const int _dealerLabelId = 902;
  static const int _verdictLabelId = 903;

  final List<int> _cardDef = List<int>.filled(52, 0);
  int _felt = 0;
  int _deckDef = 0;
  int _font = 0;
  double _camDistance = 13.0;

  // Beat bookkeeping (see _scene and _onEngineEvent): which cards the
  // previously *built* scene already showed (newcomers are still flying off
  // the shoe) and which of them it showed face-down (a card leaving that set
  // is mid-flip), which cards the engine has confirmed as animating, and the
  // fully counted label frame parked until that motion lands.
  TesseraController? _engine;
  StreamSubscription<TesseraEvent>? _events;
  Set<int> _shownCards = {};
  Set<int> _shownHidden = {};
  final Set<int> _airborne = {};
  TesseraScene? _pendingSettle;

  // Corner tiles hugging the dealer/player/deck area, for cameraFitDistance so
  // the cards stay in frame in portrait as well as landscape.
  static const List<int> _fitCorners = [9001, 9002, 9003, 9004];
  int _cornerId(int x, int z) {
    if (x == -5 && z == -4) return 9001;
    if (x == 5 && z == -4) return 9002;
    if (x == -5 && z == 4) return 9003;
    if (x == 5 && z == 4) return 9004;
    return 0;
  }

  @override
  String get title => 'Blackjack';
  @override
  String get subtitle => 'Cards · hit or stand against the dealer';
  @override
  IconData get icon => Icons.style;

  @override
  TesseraLightData get light => const TesseraLightData(
        dir: [-0.35, -1.0, -0.45],
        color: [1.0, 0.97, 0.9],
        intensity: 1.1,
        ambient: [0.32, 0.35, 0.42],
      );

  @override
  TesseraTimingData get timing => const TesseraTimingData(
        addS: 0.32,
        removeS: 0.28,
        moveS: 0.4,
        tileS: 0.35,
        cameraS: 0.6,
      );

  @override
  Future<void> registerDefs(TesseraController c) async {
    // Fresh engine: reset the beat bookkeeping and (re)wire the event stream,
    // cancelling any subscription left from a previous attach. The stream
    // itself closes when the controller is disposed, which ends the
    // subscription and drops our reference via onDone.
    _engine = c;
    _shownCards = {};
    _shownHidden = {};
    _airborne.clear();
    _pendingSettle = null;
    await _events?.cancel();
    _events = c.events.listen(_onEngineEvent, onDone: () {
      _events = null;
      if (identical(_engine, c)) _engine = null;
    });

    _font = await registerGameFont(c);
    _felt = c.registerTileType(
        const TesseraTileType(thickness: 0.2, tint: [0.09, 0.4, 0.24, 1.0]));
    final hidden = c.registerAtlas(await renderCardHidden());
    final back = c.registerAtlas(await renderCardBack());
    _deckDef = c.registerCardType(
        TesseraCardType(visibleAtlas: back, hiddenAtlas: hidden, backAtlas: back));
    for (var id = 0; id < 52; id++) {
      final suit = id ~/ 13, rank = id % 13 + 1;
      final face = c.registerAtlas(await renderCardFace(rank, suit));
      _cardDef[id] =
          c.registerCardType(TesseraCardType(visibleAtlas: face, hiddenAtlas: hidden, backAtlas: back));
    }
  }

  /// UI-side juice only: the engine reports the exact moments cards move, so
  /// the hand-value counters can tick when a card *lands* rather than when its
  /// state was pushed. The reducer never looks at these events.
  void _onEngineEvent(TesseraEvent e) {
    switch (e.type) {
      case TesseraEventType.cardDealt:
      case TesseraEventType.cardFlipped:
        // The engine confirmed this card is in motion (flying off the shoe or
        // mid-flip); its beat settles with the operation animating it.
        if (e.subject == TesseraEventSubject.card) _airborne.add(e.subjectId);
      case TesseraEventType.opCompleted:
        _onBeatSettled();
      default:
        break;
    }
  }

  /// A transition finished. If cards just landed, a fully counted label frame
  /// is parked, and the engine is genuinely idle (mid-deal the next card's
  /// scene is already in flight and carries the fresh totals itself), push it:
  /// a label-only diff the engine resolves as a quick crossfade — everything
  /// else in the frame is settled and snaps — so the counter ticks exactly on
  /// the landing.
  void _onBeatSettled() {
    if (_airborne.isEmpty) return; // not a card beat (camera nudge, refit…)
    _airborne.clear();
    final c = _engine;
    final settle = _pendingSettle;
    if (c == null || settle == null || !c.isIdle) return;
    _pendingSettle = null;
    // Events already queued when the screen pops can land after the
    // controller's dispose (onDone nulls _engine only afterwards); setScene
    // no-ops on a disposed controller, so this late push is safe.
    unawaited(c.setScene(settle));
  }

  @override
  BjState initial() => const BjTable(seed: 0x1234abcd);

  @override
  BjState update(BjState state, BjAction action) => bjUpdate(state, action);

  @override
  List<TesseraScene> render(BjState s) {
    switch (s) {
      case BjTable():
        return [_scene(player: const [], dealer: const [], hideHole: true, deck: _fullDeck)];
      case BjDealing():
        return [_scene(player: s.player, dealer: s.dealer, hideHole: true, deck: s.shoe.length)];
      case BjPlayerTurn():
        return [_scene(player: s.player, dealer: s.dealer, hideHole: true, deck: s.shoe.length)];
      case BjRoundOver():
        return _revealSequence(s);
    }
  }

  /// The dealer reveal as a *sequence* of scenes: flip the hole card, then turn
  /// over each additional dealer draw one at a time.
  List<TesseraScene> _revealSequence(BjRoundOver s) {
    final out = <TesseraScene>[];
    // The player was already looking at the hole-hidden frame, so start straight
    // on the flip — restating that frame first was a wasted beat. The deck slab
    // is sized as of the pre-dealer-draw moment and thins by one as each extra
    // dealer card is turned off it (no thickness jump at round-over).
    int deckAt(int shown) => s.shoe.length + s.dealer.length - shown;
    // flip the hole up
    out.add(_scene(player: s.player, dealer: s.dealer.take(2).toList(), hideHole: false, deck: deckAt(2)));
    // reveal each extra dealer card
    for (var k = 3; k <= s.dealer.length; k++) {
      out.add(_scene(player: s.player, dealer: s.dealer.take(k).toList(), hideHole: false, deck: deckAt(k)));
    }
    // …then the verdict beat: the final totals tick in, the verdict banner
    // fades up, and the winning hand takes on its pulsing glow.
    out.add(_scene(
        player: s.player,
        dealer: s.dealer,
        hideHole: false,
        deck: deckAt(s.dealer.length),
        result: s.result));
    return out;
  }

  TesseraScene _scene({
    required List<int> player,
    required List<int> dealer,
    required bool hideHole,
    required int deck,
    BjResult? result,
  }) {
    final tiles = <TesseraTile>[];
    for (var z = -5; z <= 5; z++) {
      for (var x = -6; x <= 6; x++) {
        tiles.add(TesseraTile(x: x, y: z, def: _felt, id: _cornerId(x, z)));
      }
    }

    final cards = <TesseraCard>[];
    for (var i = 0; i < player.length; i++) {
      cards.add(TesseraCard(
        id: player[i] + 1,
        def: _cardDef[player[i]],
        hidden: false,
        hand: _playerHand,
        handSlot: i,
        sourceDraw: _deckId, // dealt off the deck: fly in from the pile
      ));
    }
    for (var i = 0; i < dealer.length; i++) {
      cards.add(TesseraCard(
        id: dealer[i] + 1,
        def: _cardDef[dealer[i]],
        hidden: i == 0 && hideHole, // the hole card
        hand: _dealerHand,
        handSlot: i,
        sourceDraw: _deckId, // dealt off the deck: fly in from the pile
      ));
    }

    // Player fan: held up near the camera. Dealer fan: laid flat across the felt.
    final hands = <TesseraHand>[
      TesseraHand(
        id: _playerHand,
        position: const [0, 1.1, 3.4],
        orientation: quatAxis(1, 0, 0, -0.7),
        spreadDeg: 34,
        radius: 3.0,
        cardSpacing: 0.95,
      ),
      TesseraHand(
        id: _dealerHand,
        position: const [0, 0.06, -3.2],
        orientation: quatAxis(1, 0, 0, -math.pi / 2),
        spreadDeg: 26,
        radius: 3.0,
        cardSpacing: 1.0,
      ),
    ];

    final draws = <TesseraCardDraw>[
      TesseraCardDraw(
        id: _deckId,
        def: _deckDef,
        count: deck,
        position: const [4.0, 0.02, -0.2],
        topHidden: false,
      ),
    ];

    // ---- hand-value labels, ticking on the engine's beat -----------------
    // A card only counts once it has physically arrived: cards new to this
    // scene are still flying off the shoe and a card whose hidden flag just
    // cleared is mid-flip. The shown labels exclude both; the fully counted
    // variant is parked in [_pendingSettle] and pushed by the event stream
    // the moment the engine reports the motion landed (_onBeatSettled), so
    // the counters tick with the 3D cards, not with the state push.
    final ids = <int>{
      for (final card in player) card + 1,
      for (final card in dealer) card + 1,
    };
    final hiddenIds = <int>{if (hideHole && dealer.isNotEmpty) dealer[0] + 1};
    final flying = ids.difference(_shownCards);
    final flipping = _shownHidden.difference(hiddenIds).intersection(ids);
    _shownCards = ids;
    _shownHidden = hiddenIds;

    String labelText(String name, List<int> hand, {required bool settled}) {
      final counted = <int>[
        for (final card in hand)
          if (!hiddenIds.contains(card + 1) &&
              (settled ||
                  (!flying.contains(card + 1) && !flipping.contains(card + 1))))
            card,
      ];
      if (counted.isEmpty) return '';
      final v = handValue(counted);
      return v.soft ? '$name ${v.total - 10}/${v.total}' : '$name ${v.total}';
    }

    List<TesseraLabel> handLabels({required bool settled}) {
      if (_font == 0) return const [];
      final you = labelText('You', player, settled: settled);
      final dlr = labelText('Dealer', dealer, settled: settled);
      return [
        if (you.isNotEmpty)
          TesseraLabel(
            id: _playerLabelId,
            font: _font,
            text: you,
            position: const [-3.6, 1.3, 3.4],
            size: 0.45,
            color: const [0.93, 0.96, 1.0, 1.0],
          ),
        if (dlr.isNotEmpty)
          TesseraLabel(
            id: _dealerLabelId,
            font: _font,
            text: dlr,
            position: const [-3.6, 0.6, -3.2],
            size: 0.45,
            color: const [0.93, 0.96, 1.0, 1.0],
          ),
        if (result != null)
          TesseraLabel(
            id: _verdictLabelId,
            font: _font,
            text: _verdictText(result),
            position: const [0, 1.7, -0.6],
            size: 0.62,
            color: _verdictColor(result),
          ),
      ];
    }

    // The winning hand glows on the verdict beat (no glow on a push).
    final highlights = <TesseraHighlight>[
      if (result != null)
        for (final card in _winningHand(result, player, dealer))
          TesseraHighlight(
            targetId: card + 1,
            kind: TesseraHighlightKind.card,
            style: TesseraHighlightStyle.glow,
            color: const [1.0, 0.84, 0.35, 1.0],
            thickness: 14,
            pulseS: 1.6,
            pulseMin: 0.55,
            pulseMax: 1.0,
          ),
    ];

    final camera = TesseraCameraPose(
      focusX: 0,
      focusY: 0,
      distance: _camDistance,
      yaw: 0,
      pitch: 0.95,
      fov: 0.72,
    );

    TesseraScene sceneWith(List<TesseraLabel> labels) => TesseraScene(
          tiles: tiles,
          cards: cards,
          hands: hands,
          cardDraws: deck > 0 ? draws : const [],
          labels: labels,
          highlights: highlights,
          camera: camera,
        );

    final shown = handLabels(settled: false);
    final settled = handLabels(settled: true);
    _pendingSettle = _sameLabelText(shown, settled) ? null : sceneWith(settled);
    return sceneWith(shown);
  }

  /// The hand bathed in the winner's glow at round end (none on a push).
  List<int> _winningHand(BjResult r, List<int> player, List<int> dealer) =>
      switch (r) {
        BjResult.playerWin || BjResult.blackjack || BjResult.dealerBust => player,
        BjResult.dealerWin || BjResult.playerBust => dealer,
        BjResult.push => const [],
      };

  /// The 3D verdict banner: short, and ASCII-only (the baked glyph atlas
  /// covers ASCII + Latin-1, so no em dashes here — unlike [_resultText]).
  String _verdictText(BjResult r) => switch (r) {
        BjResult.blackjack => 'Blackjack!',
        BjResult.playerWin => 'You win',
        BjResult.dealerWin => 'Dealer wins',
        BjResult.push => 'Push',
        BjResult.playerBust => 'Bust!',
        BjResult.dealerBust => 'Dealer busts',
      };

  List<double> _verdictColor(BjResult r) => switch (r) {
        BjResult.blackjack ||
        BjResult.playerWin ||
        BjResult.dealerBust =>
          const [1.0, 0.85, 0.35, 1.0],
        BjResult.push => const [0.85, 0.88, 0.95, 1.0],
        BjResult.dealerWin || BjResult.playerBust => const [1.0, 0.5, 0.45, 1.0],
      };

  static bool _sameLabelText(List<TesseraLabel> a, List<TesseraLabel> b) {
    if (a.length != b.length) return false;
    for (var i = 0; i < a.length; i++) {
      if (a[i].id != b[i].id || a[i].text != b[i].text) return false;
    }
    return true;
  }

  @override
  List<TesseraScene>? onResize(TesseraController c, BjState state, double w, double h) {
    final fit = c.cameraFitDistance(tileIds: _fitCorners, padding: 0.09);
    if (fit == null) return null;
    // Fit only ever pulls back from the tuned landscape distance (the ground-
    // plane fit ignores the raised hand cards), so portrait stops cropping
    // without landscape ever zooming in tighter than 13.
    final dist = math.max(fit, 13.0);
    if (dist == _camDistance) return null;
    _camDistance = dist;
    final scenes = render(state); // settled (last) frame, re-fitted
    return scenes.isEmpty ? null : [scenes.last];
  }

  @override
  BjAction? autoAdvance(BjState state) => switch (state) {
        BjDealing() => const BjDealStep(), // deal cards one at a time
        _ => null,
      };

  @override
  BjAction? autoAction(BjState state, math.Random rng) => switch (state) {
        BjTable() => const BjDeal(),
        BjPlayerTurn() => state.playerTotal < 17 ? const BjHit() : const BjStand(),
        BjRoundOver() => const BjNewRound(),
        _ => null,
      };

  @override
  List<GameButton<BjAction>> buttons(BjState state) => switch (state) {
        BjTable() => const [GameButton('Deal', BjDeal(), icon: Icons.play_arrow, tone: GameButtonTone.primary)],
        BjPlayerTurn() => const [
            GameButton('Hit', BjHit(), icon: Icons.add, tone: GameButtonTone.primary),
            GameButton('Stand', BjStand(), icon: Icons.pan_tool),
          ],
        BjRoundOver() => const [
            GameButton('New round', BjNewRound(), icon: Icons.refresh, tone: GameButtonTone.primary)
          ],
        _ => const [],
      };

  @override
  String status(BjState state) {
    final p = state.player.isEmpty ? '—' : '${state.playerTotal}';
    switch (state) {
      case BjTable():
        return 'Tap Deal to start a round.';
      case BjDealing():
        return 'Dealing…  ·  You $p';
      case BjPlayerTurn():
        final soft = handValue(state.player).soft ? ' (soft)' : '';
        return 'Your turn  ·  You $p$soft  ·  Dealer shows ${_upcard(state.dealer)}';
      case BjRoundOver():
        return '${_resultText(state.result)}  ·  You ${state.playerTotal}  ·  Dealer ${state.dealerTotal}';
    }
  }

  String _upcard(List<int> dealer) {
    if (dealer.length < 2) return '—';
    final r = _rankOf(dealer[1]);
    return r == 1 ? 'A' : (r >= 10 ? '10' : '$r');
  }

  String _resultText(BjResult r) => switch (r) {
        BjResult.blackjack => 'Blackjack! You win',
        BjResult.playerWin => 'You win',
        BjResult.dealerWin => 'Dealer wins',
        BjResult.push => 'Push',
        BjResult.playerBust => 'Bust — dealer wins',
        BjResult.dealerBust => 'Dealer busts — you win',
      };
}
