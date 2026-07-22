// card_art.dart — generates playing-card textures at runtime with a Canvas, so
// the Blackjack example ships no image assets. Each returns encoded PNG bytes
// ready for TesseraController.registerAtlas.
import 'dart:math' as math;
import 'dart:typed_data';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';

const int _w = 256;
const int _h = 384;

const List<String> rankLabels = ['', 'A', '2', '3', '4', '5', '6', '7', '8', '9', '10', 'J', 'Q', 'K'];
const List<String> _suitGlyphs = ['♠', '♥', '♦', '♣']; // ♠ ♥ ♦ ♣

Color _suitColor(int suit) =>
    (suit == 1 || suit == 2) ? const Color(0xFFB0282C) : const Color(0xFF20283C);

Future<Uint8List> _encode(ui.Picture pic) async {
  final img = await pic.toImage(_w, _h);
  final data = await img.toByteData(format: ui.ImageByteFormat.png);
  img.dispose();
  return data!.buffer.asUint8List();
}

void _card(Canvas c, Color bg, Color border) {
  final r = RRect.fromRectAndRadius(
    Rect.fromLTWH(6, 6, _w - 12.0, _h - 12.0),
    const Radius.circular(26),
  );
  c.drawRRect(r, Paint()..color = bg);
  c.drawRRect(
    r,
    Paint()
      ..color = border
      ..style = PaintingStyle.stroke
      ..strokeWidth = 6,
  );
}

void _glyph(Canvas c, String s, double x, double y, double size, Color color,
    {bool center = false}) {
  final tp = TextPainter(
    text: TextSpan(
      text: s,
      style: TextStyle(color: color, fontSize: size, fontWeight: FontWeight.w700, height: 1.0),
    ),
    textDirection: TextDirection.ltr,
  )..layout();
  final ox = center ? x - tp.width / 2 : x;
  final oy = center ? y - tp.height / 2 : y;
  tp.paint(c, Offset(ox, oy));
}

/// A face card: rank + suit in two corners and a large centre suit.
Future<Uint8List> renderCardFace(int rank, int suit) async {
  final rec = ui.PictureRecorder();
  final c = Canvas(rec);
  final ink = _suitColor(suit);
  _card(c, const Color(0xFFF3F1E9), ink.withValues(alpha: 0.55));
  final rs = rankLabels[rank], ss = _suitGlyphs[suit];

  _glyph(c, rs, 20, 14, 46, ink);
  _glyph(c, ss, 24, 66, 34, ink);
  _glyph(c, ss, _w / 2, _h / 2, 150, ink, center: true);

  c.save();
  c.translate(_w.toDouble(), _h.toDouble());
  c.rotate(math.pi);
  _glyph(c, rs, 20, 14, 46, ink);
  _glyph(c, ss, 24, 66, 34, ink);
  c.restore();

  return _encode(rec.endRecording());
}

void _lattice(Canvas c, Color base, Color line) {
  _card(c, base, line.withValues(alpha: 0.9));
  final p = Paint()
    ..color = line
    ..strokeWidth = 3;
  const step = 26.0;
  for (double d = -_h.toDouble(); d < _w + _h; d += step) {
    c.drawLine(Offset(d, 0), Offset(d + _h, _h.toDouble()), p);
    c.drawLine(Offset(d + _h, 0), Offset(d, _h.toDouble()), p);
  }
}

/// The card back (a woven lattice).
Future<Uint8List> renderCardBack() async {
  final rec = ui.PictureRecorder();
  final c = Canvas(rec);
  c.clipRRect(RRect.fromRectAndRadius(
      Rect.fromLTWH(6, 6, _w - 12.0, _h - 12.0), const Radius.circular(26)));
  _lattice(c, const Color(0xFF7A1D26), const Color(0xFFC6A04E));
  return _encode(rec.endRecording());
}

/// The concealing front (a neutral lattice; shown for face-down player secrecy).
Future<Uint8List> renderCardHidden() async {
  final rec = ui.PictureRecorder();
  final c = Canvas(rec);
  c.clipRRect(RRect.fromRectAndRadius(
      Rect.fromLTWH(6, 6, _w - 12.0, _h - 12.0), const Radius.circular(26)));
  _lattice(c, const Color(0xFF2C3450), const Color(0xFF5A6E96));
  return _encode(rec.endRecording());
}
