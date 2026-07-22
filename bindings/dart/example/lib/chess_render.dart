// chess_render.dart — definitions + visual projection onto Tessera.
//
// A port of the rendering half of examples/chess/main.c: registers the tile and
// piece definitions, projects a `Game` into a TesseraState of tile/entity
// placements + a camera that sits behind the side to move, and pushes it. The
// engine diffs successive states and animates the transitions.

import 'dart:ffi';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';
import 'package:tessera/tessera.dart';

import 'chess_game.dart';
import 'chess_gen.dart';

const int boardOff = -4; // file/rank 0..7 -> world -4..3

/// Stable per-tile instance id, so the corners can be projected / fitted.
/// Square s -> id s + 1 (1 = a1 .. 64 = h8; 0 is reserved for "no id").
int tileId(int square) => square + 1;

class ChessRenderer {
  ChessRenderer(this.engine);

  final Tessera engine;

  late final int _dTileLight;
  late final int _dTileDark;
  final List<List<int>> _dPiece =
      List.generate(2, (_) => List<int>.filled(7, 0)); // [color][kind]

  double camDistance = 11.5;

  /// Register the two tile defs, the capture-poof effect, and the twelve piece
  /// entity defs (six kinds × two armies).
  void registerDefs() {
    _dTileLight = _registerTile(0.22, [0.82, 0.78, 0.68, 1.0]);
    _dTileDark = _registerTile(0.22, [0.32, 0.36, 0.42, 1.0]);

    // slightly warm ivory vs. cool charcoal, baked into each model's material
    const whiteCol = [0.90, 0.87, 0.80, 1.0];
    const blackCol = [0.16, 0.17, 0.20, 1.0];

    final dPoof = _registerPoof();

    for (var color = 0; color < 2; ++color) {
      final col = (color == white) ? whiteCol : blackCol;
      final face = (color == white) ? 1 : -1; // knights face the enemy
      for (var kind = chessPawn; kind <= chessKing; ++kind) {
        final glb = buildPieceGlb(kind, face, col);
        _dPiece[color][kind] = _registerPiece(glb, dPoof);
      }
    }
  }

  int _registerTile(double thickness, List<double> tint) {
    final def = calloc<TesseraTileDef>();
    def.ref.thickness = thickness;
    for (var i = 0; i < 4; ++i) {
      def.ref.tint[i] = tint[i];
    }
    final id = engine.registerTileDef(def);
    calloc.free(def);
    return id;
  }

  int _registerPoof() {
    // smoke poof played when a piece is captured (despawn)
    final def = calloc<TesseraEffectDef>();
    final p = def.ref.onRemove;
    p
      ..mode = TesseraEmitMode.burst
      ..count = 44
      ..lifetimeS = 0.7
      ..lifetimeVar = 0.25
      ..speed = 1.6
      ..speedVar = 0.6
      ..spreadDeg = 55.0
      ..gravity = 0.8
      ..sizeStart = 0.14
      ..sizeEnd = 0.5
      ..blend = TesseraBlendMode.alpha;
    p.colorStart[0] = 0.85;
    p.colorStart[1] = 0.85;
    p.colorStart[2] = 0.9;
    p.colorStart[3] = 0.75;
    p.colorEnd[0] = 0.5;
    p.colorEnd[1] = 0.5;
    p.colorEnd[2] = 0.55;
    p.colorEnd[3] = 0.0;
    final id = engine.registerEffectDef(def);
    calloc.free(def);
    return id;
  }

  int _registerPiece(Uint8List glb, int poofEffect) {
    final bytes = calloc<Uint8>(glb.length);
    bytes.asTypedList(glb.length).setAll(0, glb);
    final name = 'chesspiece'.toNativeUtf8();

    final def = calloc<TesseraEntityDef>();
    def.ref.gltf
      ..data = bytes.cast<Void>()
      ..size = glb.length
      ..debugName = name;
    def.ref
      ..scale = 1.1
      ..defaultAnim = -1
      ..moveAnim = -1
      ..spawnAnim = -1
      ..despawnAnim = -1
      ..onDespawnEffect = poofEffect;

    final id = engine.registerEntityDef(def); // copies the bytes it needs
    calloc.free(def);
    calloc.free(bytes);
    calloc.free(name);
    return id;
  }

  /// Zoom so all four board corners stay on screen at the current window size.
  void refitCamera() {
    final corners = calloc<Uint64>(4);
    corners[0] = tileId(sqOf(0, 0));
    corners[1] = tileId(sqOf(7, 0));
    corners[2] = tileId(sqOf(0, 7));
    corners[3] = tileId(sqOf(7, 7));
    final out = calloc<Float>();
    if (engine.cameraFitDistance(corners, 4, nullptr, 0, 0.06, out)) {
      camDistance = out.value;
    }
    calloc.free(corners);
    calloc.free(out);
  }

  /// Set the camera pose for the side to move: it swings behind that player.
  void _setSideCamera(TesseraCamera cam, int side) {
    // yaw = PI puts the eye on white's side (-z); yaw = 0 on black's side (+z).
    cam
      ..distance = camDistance
      ..yaw = (side == white) ? 3.14159 : 0.0
      ..pitch = 0.82
      ..fov = 0.72;
    // Board files/ranks 0..7 map to grid -4..3, so the centre sits between
    // tiles at (-0.5, -0.5).
    cam.focus
      ..x = boardOff + 3.5
      ..y = boardOff + 3.5;
  }

  /// Build a VisualState from the rules state + id grid, and push it.
  void buildAndPush(Game c) {
    final tiles = calloc<TesseraTilePlacement>(64);
    final ents = calloc<TesseraEntityPlacement>(32);
    var nt = 0, ne = 0;

    for (var r = 0; r < 8; ++r) {
      for (var f = 0; f < 8; ++f) {
        final td = ((f + r) & 1) != 0 ? _dTileLight : _dTileDark;
        final t = tiles[nt++];
        t.coord
          ..x = f + boardOff
          ..y = r + boardOff;
        t
          ..tileDef = td
          ..id = tileId(sqOf(f, r));
      }
    }
    for (var s = 0; s < 64; ++s) {
      final v = c.state.sq[s];
      if (v == 0 || c.ids[s] == 0) continue;
      final color = pieceColor(v), kind = pieceKind(v);
      final e = ents[ne++];
      e
        ..id = c.ids[s]
        ..def = _dPiece[color][kind]
        ..facing = 0;
      e.coord
        ..x = fileOf(s) + boardOff
        ..y = rankOf(s) + boardOff;
    }

    final st = calloc<TesseraState>();
    st.ref
      ..tiles = tiles
      ..tileCount = nt
      ..entities = ents
      ..entityCount = ne
      ..effects = nullptr
      ..effectCount = 0
      ..epoch = c.state.ply;
    _setSideCamera(st.ref.camera, c.state.side);

    engine.setState(st); // deep-copies; safe to free immediately after

    calloc.free(st);
    calloc.free(tiles);
    calloc.free(ents);
  }

  /// Tick the engine forward by [secs], letting transitions play out.
  void settle(double secs) {
    const step = 1.0 / 120.0;
    for (var t = 0.0; t < secs; t += step) {
      engine.tick(step);
    }
  }
}
