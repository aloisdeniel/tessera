// examples/chess (Dart) — a full game of chess rendered with Tessera.
//
// A port of the headless demo from the C `examples/chess`. A heuristic AI plays
// both sides over the fully-legal move list; each ply is projected into a
// Tessera state (tile + entity placements + a camera behind the side to move),
// pushed to the engine, ticked so the slide / capture-poof animations play out,
// and captured to a PNG.
//
// Interactive click-to-move (SPACE = AI, R = reset) needs SDL event handling,
// which is not available from pure Dart, so this port focuses on the AI-vs-AI
// capture mode — the C `--demo <dir>` path — which drives the whole engine
// through the C ABI alone.
//
// Usage:
//   dart run bin/chess.dart [--demo <dir>] [--plies N] [--seed 0xNNNN]
//
// Remember Tessera links SDL3: run with DYLD_LIBRARY_PATH=/opt/homebrew/lib on
// macOS/Homebrew, and point the loader at the built library if it is not on a
// conventional path (TESSERA_LIBRARY_PATH / TESSERA_LIBRARY_DIR).

import 'dart:ffi';
import 'dart:io';

import 'package:ffi/ffi.dart';
import 'package:tessera/tessera.dart';
import 'package:tessera_chess/chess_game.dart';
import 'package:tessera_chess/chess_render.dart';

const String kindChar = ' PNBRQK';

void main(List<String> args) {
  var demoDir = 'out';
  var maxPlies = 40;
  var seed = 0x1234abcd;

  for (var i = 0; i < args.length; ++i) {
    final a = args[i];
    if (a == '--demo' && i + 1 < args.length) {
      demoDir = args[++i];
    } else if (a == '--plies' && i + 1 < args.length) {
      maxPlies = int.parse(args[++i]);
    } else if (a == '--seed' && i + 1 < args.length) {
      final s = args[++i];
      seed = s.startsWith('0x') || s.startsWith('0X')
          ? int.parse(s.substring(2), radix: 16)
          : int.parse(s);
    } else if (a == '-h' || a == '--help') {
      stdout.writeln('usage: dart run bin/chess.dart '
          '[--demo <dir>] [--plies N] [--seed 0xNNNN]');
      return;
    }
  }

  final Tessera engine;
  try {
    engine = Tessera(width: 1280, height: 800, pixelDensity: 1.0);
  } on TesseraLibraryNotFound catch (e) {
    stderr.writeln(e);
    exitCode = 1;
    return;
  }

  final err = engine.lastError;
  if (err.isNotEmpty) {
    stderr.writeln('init failed: $err');
    engine.dispose();
    exitCode = 1;
    return;
  }
  stdout.writeln('Tessera ${engine.versionString} · ${engine.backendName}');

  _setLight(engine);
  _setQuality(engine);
  _setTiming(engine);

  final renderer = ChessRenderer(engine)..registerDefs();

  final rc = _runDemo(engine, renderer, demoDir, maxPlies, seed);
  engine.dispose();
  exitCode = rc;
}

int _runDemo(
    Tessera engine, ChessRenderer renderer, String dir, int maxPlies, int seed) {
  Directory(dir).createSync(recursive: true);
  final rng = ChessRng(seed);

  final c = Game();
  renderer.buildAndPush(c);
  renderer.settle(1.2);
  var path = '$dir/chess_000.png';
  engine.capturePng(1280, 800, path);
  stdout.writeln('wrote $path (start)');

  for (var i = 1; i <= maxPlies && !c.gameOver; ++i) {
    final a = aiPickAction(c.state, rng);
    if (a == null) break;
    final ms = moveStr(a);
    final mover = pieceKind(c.state.sq[a.from]);
    final capture = c.state.sq[a.to] != 0 || a.isEp != 0;
    final side = c.state.side;

    c.apply(a);
    renderer.buildAndPush(c);
    renderer.settle(0.9); // let the slide + any capture poof play out

    path = '$dir/chess_${i.toString().padLeft(3, '0')}.png';
    engine.capturePng(1280, 800, path);
    stdout.writeln('wrote $path  (${side == white ? "white" : "black"} '
        '${kindChar[mover]} $ms${capture ? " x" : ""})');
  }
  stdout.writeln('game over after ${c.state.ply} plies '
      '(${c.gameOver ? "no legal moves" : "ply cap"})');
  return 0;
}

void _setLight(Tessera engine) {
  final l = calloc<TesseraLight>();
  l.ref.dir[0] = -0.4;
  l.ref.dir[1] = -1.0;
  l.ref.dir[2] = -0.5;
  l.ref.color[0] = 1.0;
  l.ref.color[1] = 0.97;
  l.ref.color[2] = 0.9;
  l.ref.intensity = 1.15;
  l.ref.ambient[0] = 0.30;
  l.ref.ambient[1] = 0.33;
  l.ref.ambient[2] = 0.4;
  engine.setLight(l);
  calloc.free(l);
}

void _setQuality(Tessera engine) {
  final q = calloc<TesseraQuality>();
  q.ref
    ..shadows = TesseraShadowMode.blob
    ..msaa = 1
    ..renderScale = 1.0;
  engine.setQuality(q);
  calloc.free(q);
}

void _setTiming(Tessera engine) {
  final t = calloc<TesseraTiming>();
  t.ref
    ..moveS = 0.5
    ..addS = 0.35
    ..removeS = 0.4
    ..tileS = 0.4
    ..reflowS = 0.4
    ..cameraS = 0.9
    ..speedMultiplier = 1.0;
  engine.setTiming(t);
  calloc.free(t);
}
