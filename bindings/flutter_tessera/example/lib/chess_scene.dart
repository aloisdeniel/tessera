// chess_scene.dart — registers the chess definitions and projects a `Game` into
// a Tessera scene, using flutter_tessera's high-level value types (no FFI).
//
// The rendering half of the chess port; the C original is examples/chess/main.c
// (register_defs / build_and_push / side_camera).

import 'package:flutter_tessera/flutter_tessera.dart';

import 'chess_game.dart';
import 'chess_gen.dart';

const int boardOff = -4; // file/rank 0..7 -> world -4..3

/// Stable per-tile instance id: square s -> id s + 1 (0 reserved for "no id").
int tileId(int square) => square + 1;

/// Owns the def ids and builds scenes for a chess game.
class ChessScene {
  late final int _tileLight;
  late final int _tileDark;
  final List<List<int>> _piece =
      List.generate(2, (_) => List<int>.filled(7, 0)); // [color][kind]

  double camDistance = 11.5;

  /// Register the tiles, the capture-poof effect, and the twelve piece defs.
  void registerDefs(TesseraController c) {
    _tileLight = c.registerTileType(
        const TesseraTileType(thickness: 0.22, tint: [0.82, 0.78, 0.68, 1.0]));
    _tileDark = c.registerTileType(
        const TesseraTileType(thickness: 0.22, tint: [0.32, 0.36, 0.42, 1.0]));

    const whiteCol = [0.90, 0.87, 0.80, 1.0];
    const blackCol = [0.16, 0.17, 0.20, 1.0];

    final poof = c.registerEffectType(const TesseraEffectType(
      onRemove: TesseraParticle(
        count: 44,
        lifetime: 0.7,
        lifetimeVar: 0.25,
        speed: 1.6,
        speedVar: 0.6,
        spreadDeg: 55,
        gravity: 0.8,
        sizeStart: 0.14,
        sizeEnd: 0.5,
        colorStart: [0.85, 0.85, 0.9, 0.75],
        colorEnd: [0.5, 0.5, 0.55, 0.0],
      ),
    ));

    for (var color = 0; color < 2; ++color) {
      final col = (color == white) ? whiteCol : blackCol;
      final face = (color == white) ? 1 : -1; // knights face the enemy
      for (var kind = chessPawn; kind <= chessKing; ++kind) {
        _piece[color][kind] = c.registerEntityType(TesseraEntityType(
          glb: buildPieceGlb(kind, face, col),
          scale: 1.1,
          onDespawnEffect: poof,
        ));
      }
    }
  }

  /// The four board corners, for fitting the camera zoom.
  List<int> get cornerTileIds => [
        tileId(sqOf(0, 0)),
        tileId(sqOf(7, 0)),
        tileId(sqOf(0, 7)),
        tileId(sqOf(7, 7)),
      ];

  /// Camera pose for the side to move: swings behind that player.
  TesseraCameraPose cameraFor(int side) => TesseraCameraPose(
        focusX: boardOff + 3.5,
        focusY: boardOff + 3.5,
        distance: camDistance,
        // yaw = PI puts the eye on white's side (-z); yaw = 0 on black's (+z).
        yaw: (side == white) ? 3.14159 : 0.0,
        pitch: 0.82,
        fov: 0.72,
      );

  /// Project the rules state + id grid into a scene.
  TesseraScene build(Game g) {
    final tiles = <TesseraTile>[];
    final entities = <TesseraEntity>[];

    for (var r = 0; r < 8; ++r) {
      for (var f = 0; f < 8; ++f) {
        final td = ((f + r) & 1) != 0 ? _tileLight : _tileDark;
        tiles.add(TesseraTile(
          x: f + boardOff,
          y: r + boardOff,
          def: td,
          id: tileId(sqOf(f, r)),
        ));
      }
    }
    for (var s = 0; s < 64; ++s) {
      final v = g.state.sq[s];
      if (v == 0 || g.ids[s] == 0) continue;
      final color = pieceColor(v), kind = pieceKind(v);
      entities.add(TesseraEntity(
        id: g.ids[s],
        def: _piece[color][kind],
        x: fileOf(s) + boardOff,
        y: rankOf(s) + boardOff,
      ));
    }

    return TesseraScene(
      tiles: tiles,
      entities: entities,
      camera: cameraFor(g.state.side),
      epoch: g.state.ply,
    );
  }
}

/// Convert a picked tile coord back to a board square, or -1 if off-board.
int coordToSquare(int x, int y) {
  final f = x - boardOff, r = y - boardOff;
  if (f < 0 || f > 7 || r < 0 || r > 7) return -1;
  return sqOf(f, r);
}
