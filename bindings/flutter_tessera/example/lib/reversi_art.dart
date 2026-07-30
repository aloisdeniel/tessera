// reversi_art.dart — generates the two Reversi disc face textures at runtime
// with a Canvas (same no-image-assets spirit as card_art.dart). A disc is a
// square card whose corner radius rounds it into a circle, so each texture is
// full-bleed disc art: a radial sheen over the face color with a turned rim.
import 'dart:typed_data';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';

const int _s = 256;

Future<Uint8List> _encode(ui.Picture pic) async {
  final img = await pic.toImage(_s, _s);
  final data = await img.toByteData(format: ui.ImageByteFormat.png);
  img.dispose();
  return data!.buffer.asUint8List();
}

/// One disc face. [dark] is the black side; otherwise the ivory side.
Future<Uint8List> renderDiscFace({required bool dark}) async {
  final rec = ui.PictureRecorder();
  final c = Canvas(rec);
  const center = Offset(_s / 2, _s / 2);
  const radius = _s / 2.0;

  final base = dark ? const Color(0xFF20242B) : const Color(0xFFF2EEE2);
  final sheen = dark ? const Color(0xFF3C424D) : const Color(0xFFFFFFFF);
  final edge = dark ? const Color(0xFF14171C) : const Color(0xFFCFC9B8);

  // Face: a soft off-center sheen so the disc reads as glossy under any light.
  c.drawCircle(
    center,
    radius,
    Paint()
      ..shader = ui.Gradient.radial(
        const Offset(_s * 0.38, _s * 0.34),
        _s * 0.85,
        [sheen, base, edge],
        [0.0, 0.55, 1.0],
      ),
  );
  // Turned rim ring near the edge, like a lathed game piece.
  c.drawCircle(
    center,
    radius * 0.82,
    Paint()
      ..style = PaintingStyle.stroke
      ..strokeWidth = _s * 0.035
      ..color = (dark ? Colors.black : edge).withValues(alpha: 0.35),
  );

  return _encode(rec.endRecording());
}
