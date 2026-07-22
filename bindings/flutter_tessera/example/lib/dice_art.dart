// dice_art.dart — generates the six pip faces of a d6 at runtime with a Canvas,
// so the Yahtzee example ships no image assets. Returns encoded PNG bytes for
// TesseraController.registerDiceType.
import 'dart:typed_data';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';

const int _s = 160;

Future<Uint8List> _encode(ui.Picture pic) async {
  final img = await pic.toImage(_s, _s);
  final data = await img.toByteData(format: ui.ImageByteFormat.png);
  img.dispose();
  return data!.buffer.asUint8List();
}

// Pip layout: which of the 3x3 grid cells are filled for each value 1..6.
const List<List<int>> _pips = [
  [4], // 1
  [0, 8], // 2
  [0, 4, 8], // 3
  [0, 2, 6, 8], // 4
  [0, 2, 4, 6, 8], // 5
  [0, 2, 3, 5, 6, 8], // 6
];

/// Render die face [value] (1..6) as a rounded ivory tile with dark pips.
Future<Uint8List> renderDieFace(int value) async {
  final rec = ui.PictureRecorder();
  final c = Canvas(rec);
  const bg = Color(0xFFEDEAE0);
  const pip = Color(0xFF20242E);

  final r = RRect.fromRectAndRadius(
      Rect.fromLTWH(8, 8, _s - 16.0, _s - 16.0), const Radius.circular(28));
  c.drawRRect(r, Paint()..color = bg);
  c.drawRRect(
    r,
    Paint()
      ..color = pip.withValues(alpha: 0.35)
      ..style = PaintingStyle.stroke
      ..strokeWidth = 5,
  );

  final cells = _pips[value - 1];
  const margin = 44.0;
  final span = (_s - 2 * margin) / 2;
  final paint = Paint()..color = pip;
  for (final cell in cells) {
    final gx = cell % 3, gy = cell ~/ 3;
    c.drawCircle(Offset(margin + gx * span, margin + gy * span), 15, paint);
  }
  return _encode(rec.endRecording());
}

/// The six d6 face sprites, ordered so face index f shows value f + 1.
Future<List<Uint8List>> renderDiceFaces() async =>
    [for (var v = 1; v <= 6; v++) await renderDieFace(v)];
