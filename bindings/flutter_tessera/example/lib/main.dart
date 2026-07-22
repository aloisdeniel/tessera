// A small collection of tabletop games rendered with Tessera and embedded in
// Flutter. The app opens on a menu; each game is a `GameController` (a sealed
// action + sealed state + pure reducer, see game.dart) driven by `GameScreen`.
//
//   • Chess     — entities on a grid (tap to move, or watch the AI).
//   • Blackjack — the Card APIs (hands, a draw pile, a dealer reveal).
//   • Yahtzee   — the Dice APIs (roll, hold, and score).

import 'package:flutter/material.dart';

import 'menu.dart';

void main() => runApp(const TesseraGamesApp());

class TesseraGamesApp extends StatelessWidget {
  const TesseraGamesApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Tessera Games',
      debugShowCheckedModeBanner: false,
      theme: ThemeData.dark(useMaterial3: true),
      home: const MenuPage(),
    );
  }
}
