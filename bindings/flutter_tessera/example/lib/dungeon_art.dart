// dungeon_art.dart — runtime-generated item-card textures for the Dungeon game.
//
// Like card_art / dice_art, this ships no image assets: each item card face is
// drawn with a Canvas and returned as encoded PNG bytes for
// TesseraController.registerAtlas. The concealing front and the card back are
// generated here too, so the whole game is asset-free.
import 'dart:typed_data';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';

const int itemSword = 0;
const int itemShield = 1;
const int itemPotion = 2;
const int itemKinds = 3;

const List<String> itemNames = ['Sword', 'Shield', 'Potion'];
const List<String> itemBlurb = ['+3 attack', 'block a hit', '+1 life'];

const int _w = 256;
const int _h = 384;

Future<Uint8List> _encode(ui.Picture pic) async {
  final img = await pic.toImage(_w, _h);
  final data = await img.toByteData(format: ui.ImageByteFormat.png);
  img.dispose();
  return data!.buffer.asUint8List();
}

void _frame(Canvas c, Color bg, Color border) {
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

void _text(Canvas c, String s, double y, double size, Color color,
    {FontWeight weight = FontWeight.w700}) {
  final tp = TextPainter(
    text: TextSpan(
      text: s,
      style: TextStyle(color: color, fontSize: size, fontWeight: weight, height: 1.0),
    ),
    textDirection: TextDirection.ltr,
  )..layout();
  tp.paint(c, Offset(_w / 2 - tp.width / 2, y));
}

// ---- item icons (centred on the card, ~centre y=190) ---------------------

void _sword(Canvas c, Color ink) {
  c.save();
  c.translate(_w / 2, 190);
  c.rotate(-0.78); // point up-right
  final steel = Paint()..color = ink;
  // blade
  final blade = Path()
    ..moveTo(-14, 60)
    ..lineTo(14, 60)
    ..lineTo(14, -70)
    ..lineTo(0, -96)
    ..lineTo(-14, -70)
    ..close();
  c.drawPath(blade, Paint()..color = const Color(0xFFC7CDDA));
  c.drawPath(blade, Paint()
    ..color = ink
    ..style = PaintingStyle.stroke
    ..strokeWidth = 5);
  // crossguard
  c.drawRRect(
      RRect.fromRectAndRadius(const Rect.fromLTWH(-46, 58, 92, 20), const Radius.circular(8)), steel);
  // grip + pommel
  c.drawRRect(
      RRect.fromRectAndRadius(const Rect.fromLTWH(-9, 78, 18, 46), const Radius.circular(6)), steel);
  c.drawCircle(const Offset(0, 132), 13, steel);
  c.restore();
}

void _shield(Canvas c, Color ink) {
  c.save();
  c.translate(_w / 2, 190);
  final p = Path()
    ..moveTo(0, -84)
    ..lineTo(70, -58)
    ..lineTo(70, 20)
    ..quadraticBezierTo(70, 78, 0, 104)
    ..quadraticBezierTo(-70, 78, -70, 20)
    ..lineTo(-70, -58)
    ..close();
  c.drawPath(p, Paint()..color = const Color(0xFF7C8AA8));
  c.drawPath(p, Paint()
    ..color = ink
    ..style = PaintingStyle.stroke
    ..strokeWidth = 7);
  // a boss + cross
  c.drawCircle(const Offset(0, 8), 16, Paint()..color = ink);
  final cross = Paint()
    ..color = ink
    ..strokeWidth = 12
    ..strokeCap = StrokeCap.round;
  c.drawLine(const Offset(0, -50), const Offset(0, 66), cross);
  c.drawLine(const Offset(-42, 8), const Offset(42, 8), cross);
  c.restore();
}

void _potion(Canvas c, Color ink) {
  c.save();
  c.translate(_w / 2, 190);
  // flask body
  final body = Path()
    ..moveTo(-18, -50)
    ..lineTo(-18, -12)
    ..lineTo(-56, 66)
    ..quadraticBezierTo(-56, 104, 0, 104)
    ..quadraticBezierTo(56, 104, 56, 66)
    ..lineTo(18, -12)
    ..lineTo(18, -50)
    ..close();
  c.drawPath(body, Paint()..color = const Color(0xFFDCE6EC));
  // liquid
  c.save();
  c.clipPath(body);
  final liquid = Path()
    ..moveTo(-56, 30)
    ..quadraticBezierTo(0, 12, 56, 30)
    ..lineTo(56, 110)
    ..lineTo(-56, 110)
    ..close();
  c.drawPath(liquid, Paint()..color = const Color(0xFFCB2E5A));
  c.drawCircle(const Offset(-14, 64), 7, Paint()..color = const Color(0x66FFFFFF));
  c.restore();
  c.drawPath(body, Paint()
    ..color = ink
    ..style = PaintingStyle.stroke
    ..strokeWidth = 6);
  // cork
  c.drawRRect(
      RRect.fromRectAndRadius(const Rect.fromLTWH(-14, -74, 28, 26), const Radius.circular(5)),
      Paint()..color = const Color(0xFF9A6B3A));
  c.restore();
}

/// The face of item [kind] (0..2): banner title, icon, effect blurb.
Future<Uint8List> renderItemCard(int kind) async {
  final rec = ui.PictureRecorder();
  final c = Canvas(rec);
  const ink = Color(0xFF23293A);
  _frame(c, const Color(0xFFF3EFE2), ink.withValues(alpha: 0.55));

  // title banner
  final banner = RRect.fromRectAndRadius(
      const Rect.fromLTWH(20, 26, _w - 40.0, 56), const Radius.circular(14));
  c.drawRRect(banner, Paint()..color = ink.withValues(alpha: 0.9));
  _text(c, itemNames[kind], 38, 34, const Color(0xFFF3EFE2));

  switch (kind) {
    case itemSword:
      _sword(c, ink);
      break;
    case itemShield:
      _shield(c, ink);
      break;
    case itemPotion:
      _potion(c, ink);
      break;
  }

  _text(c, itemBlurb[kind], _h - 66.0, 26, ink.withValues(alpha: 0.8),
      weight: FontWeight.w600);
  return _encode(rec.endRecording());
}

/// The concealing front shown while a card is face-down / in the deck.
Future<Uint8List> renderItemHidden() async {
  final rec = ui.PictureRecorder();
  final c = Canvas(rec);
  const ink = Color(0xFF2A2440);
  _frame(c, const Color(0xFF3A3357), ink);
  final glow = Paint()
    ..color = const Color(0xFFB9A7F0)
    ..style = PaintingStyle.stroke
    ..strokeWidth = 5;
  c.drawCircle(const Offset(_w / 2, _h / 2), 66, glow);
  _text(c, '?', _h / 2 - 62, 120, const Color(0xFFB9A7F0));
  return _encode(rec.endRecording());
}

/// The card back (a simple crest).
Future<Uint8List> renderItemBack() async {
  final rec = ui.PictureRecorder();
  final c = Canvas(rec);
  const ink = Color(0xFF231C12);
  _frame(c, const Color(0xFF4A3A22), ink);
  final line = Paint()
    ..color = const Color(0xFFCBA96A)
    ..style = PaintingStyle.stroke
    ..strokeWidth = 4;
  c.drawRRect(
      RRect.fromRectAndRadius(
          Rect.fromLTWH(24, 24, _w - 48.0, _h - 48.0), const Radius.circular(16)),
      line);
  _text(c, '✦', _h / 2 - 54, 96, const Color(0xFFCBA96A));
  return _encode(rec.endRecording());
}
