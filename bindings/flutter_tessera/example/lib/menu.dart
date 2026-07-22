// menu.dart — the game picker. Each entry constructs a fresh GameController and
// wraps it in a typed GameScreen when tapped.
import 'package:flutter/material.dart';

import 'blackjack.dart';
import 'chess.dart';
import 'dungeon.dart';
import 'game.dart';
import 'yahtzee.dart';

class _Entry {
  const _Entry(this.title, this.subtitle, this.icon, this.build);
  final String title;
  final String subtitle;
  final IconData icon;
  final Widget Function() build;
}

/// Build a menu entry from a controller factory, reading its metadata from a
/// throwaway instance and building a fresh one (typed) on navigation.
_Entry _entry<S, A>(GameController<S, A> Function() make) {
  final sample = make();
  return _Entry(
    sample.title,
    sample.subtitle,
    sample.icon,
    () => GameScreen<S, A>(game: make()),
  );
}

final List<_Entry> _games = [
  _entry(() => DungeonController()),
  _entry(() => ChessController()),
  _entry(() => BlackjackController()),
  _entry(() => YahtzeeController()),
];

class MenuPage extends StatelessWidget {
  const MenuPage({super.key});

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Tessera Games')),
      body: ListView(
        padding: const EdgeInsets.all(12),
        children: [
          const Padding(
            padding: EdgeInsets.fromLTRB(8, 8, 8, 16),
            child: Text(
              'Four tabletop games rendered with the Tessera engine.\n'
              'Pick one to play, or tap the robot to watch it auto-play.',
              style: TextStyle(fontSize: 14, height: 1.4),
            ),
          ),
          for (final g in _games)
            Card(
              clipBehavior: Clip.antiAlias,
              child: ListTile(
                contentPadding: const EdgeInsets.symmetric(horizontal: 16, vertical: 8),
                leading: CircleAvatar(child: Icon(g.icon)),
                title: Text(g.title, style: const TextStyle(fontWeight: FontWeight.w600)),
                subtitle: Text(g.subtitle),
                trailing: const Icon(Icons.chevron_right),
                onTap: () => Navigator.of(context)
                    .push(MaterialPageRoute<void>(builder: (_) => g.build())),
              ),
            ),
        ],
      ),
    );
  }
}
