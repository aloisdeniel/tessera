// checkers_gen.dart — procedurally generate the two draughts pieces (a "man" and
// a crowned "king") as GLBs, in the same spirit as chess_gen.dart.
//
// A man is a squat, bevel-edged disc (a lathe of a hand-authored side profile);
// a king is that same disc with a second, smaller disc stacked on top — the
// classic "two checkers crowned" look. Both carry POSITION + NORMAL + indices
// and a single material whose baseColorFactor tints the whole piece, which
// Tessera reads and applies as the entity tint (red vs. black army).
//
// Authored feet-at-y=0, +Y up, fitting inside a 1x1 tile.

import 'dart:convert';
import 'dart:math' as math;
import 'dart:typed_data';

const int _seg = 32; // lathe segments around the Y axis
const double _tau = 6.2831853;

class _Mesh {
  final List<double> pos = <double>[];
  final List<double> nrm = <double>[];
  final List<int> idx = <int>[];
  int vc = 0;

  int vert(double x, double y, double z, double nx, double ny, double nz) {
    final i = vc++;
    pos..add(x)..add(y)..add(z);
    var l = math.sqrt(nx * nx + ny * ny + nz * nz);
    if (l < 1e-6) l = 1.0;
    nrm..add(nx / l)..add(ny / l)..add(nz / l);
    return i;
  }

  void tri(int a, int b, int c) => idx..add(a)..add(b)..add(c);
  void quad(int a, int b, int c, int d) {
    tri(a, b, c);
    tri(a, c, d);
  }
}

/// A profile point for the lathe: height [y], radius [r].
class _PP {
  const _PP(this.y, this.r);
  final double y;
  final double r;
}

/// Lathe a side profile whose points are first shifted up by [baseY] and scaled
/// radially/vertically by [scale] — so a king can stack a shrunken disc on top.
void _lathe(_Mesh m, List<_PP> profile, {double baseY = 0, double scale = 1}) {
  final n = profile.length;
  final p = [for (final q in profile) _PP(baseY + q.y * scale, q.r * scale)];
  const s = _seg;
  final ring = List<int>.filled(n, 0);
  for (var i = 0; i < n; ++i) {
    final y0 = p[i > 0 ? i - 1 : i].y, r0 = p[i > 0 ? i - 1 : i].r;
    final y1 = p[i < n - 1 ? i + 1 : i].y, r1 = p[i < n - 1 ? i + 1 : i].r;
    final dy = y1 - y0, dr = r1 - r0;
    var nr = dy, ny = -dr;
    var nl = math.sqrt(nr * nr + ny * ny);
    if (nl < 1e-6) {
      nr = 1;
      ny = 0;
      nl = 1;
    }
    nr /= nl;
    ny /= nl;
    ring[i] = m.vc;
    for (var k = 0; k < s; ++k) {
      final a = _tau * k / s;
      final c = math.cos(a), sn = math.sin(a);
      m.vert(p[i].r * c, p[i].y, p[i].r * sn, nr * c, ny, nr * sn);
    }
  }
  for (var i = 0; i < n - 1; ++i) {
    if (p[i].r < 1e-5 && p[i + 1].r < 1e-5) continue;
    for (var k = 0; k < s; ++k) {
      final s2 = (k + 1) % s;
      m.quad(ring[i] + k, ring[i + 1] + k, ring[i + 1] + s2, ring[i] + s2);
    }
  }
  // bottom cap (facing -Y)
  if (p[0].r > 1e-5) {
    final c = m.vert(0, p[0].y, 0, 0, -1, 0);
    for (var k = 0; k < s; ++k) {
      final s2 = (k + 1) % s;
      final a = m.vert(p[0].r * math.cos(_tau * k / s), p[0].y,
          p[0].r * math.sin(_tau * k / s), 0, -1, 0);
      final b = m.vert(p[0].r * math.cos(_tau * s2 / s), p[0].y,
          p[0].r * math.sin(_tau * s2 / s), 0, -1, 0);
      m.tri(c, a, b);
    }
  }
  // top cap (facing +Y)
  if (p[n - 1].r > 1e-5) {
    final ty = p[n - 1].y, tr = p[n - 1].r;
    final c = m.vert(0, ty, 0, 0, 1, 0);
    for (var k = 0; k < s; ++k) {
      final s2 = (k + 1) % s;
      final a = m.vert(
          tr * math.cos(_tau * k / s), ty, tr * math.sin(_tau * k / s), 0, 1, 0);
      final b = m.vert(
          tr * math.cos(_tau * s2 / s), ty, tr * math.sin(_tau * s2 / s), 0, 1, 0);
      m.tri(c, b, a);
    }
  }
}

// The disc side profile: a flat base, a bevelled rim, a decorative groove ring,
// and a gently domed top — reads as a turned wooden/plastic checker.
const List<_PP> _discProfile = [
  _PP(0.000, 0.34),
  _PP(0.020, 0.38),
  _PP(0.130, 0.38),
  _PP(0.150, 0.355),
  _PP(0.160, 0.305), // rim groove step in
  _PP(0.185, 0.305),
  _PP(0.200, 0.255), // groove step out
  _PP(0.215, 0.235),
  _PP(0.240, 0.0), // domed centre
];

void _buildMesh(_Mesh m, bool king) {
  _lathe(m, _discProfile);
  if (king) {
    // A second, smaller disc crowning the first (the "kinged" stack).
    _lathe(m, _discProfile, baseY: 0.235, scale: 0.82);
  }
}

void _pad4(BytesBuilder b, int fill) {
  while ((b.length & 3) != 0) {
    b.addByte(fill);
  }
}

/// Build a checker GLB. [king] stacks the crown disc; [rgba] is the material
/// base color (the army tint).
Uint8List buildCheckerGlb(bool king, List<double> rgba) {
  final m = _Mesh();
  _buildMesh(m, king);
  final vc = m.vc;
  final ic = m.idx.length;

  final bin = BytesBuilder();
  final oPos = bin.length;
  final floats = Float32List.fromList(m.pos);
  bin.add(floats.buffer.asUint8List(floats.offsetInBytes, floats.lengthInBytes));
  final oNrm = bin.length;
  final nfloats = Float32List.fromList(m.nrm);
  bin.add(nfloats.buffer.asUint8List(nfloats.offsetInBytes, nfloats.lengthInBytes));
  final oIdx = bin.length;
  final indices = Uint16List.fromList(m.idx);
  bin.add(indices.buffer.asUint8List(indices.offsetInBytes, indices.lengthInBytes));
  _pad4(bin, 0);
  final binBytes = bin.toBytes();

  final lenPos = 4 * 3 * vc;
  final lenNrm = 4 * 3 * vc;
  final lenIdx = 2 * ic;

  String f4(double v) => v.toStringAsFixed(4);
  final json = '{"asset":{"version":"2.0"},'
      '"scene":0,"scenes":[{"nodes":[0]}],'
      '"nodes":[{"mesh":0}],'
      '"materials":[{"pbrMetallicRoughness":{"baseColorFactor":'
      '[${f4(rgba[0])},${f4(rgba[1])},${f4(rgba[2])},${f4(rgba[3])}],'
      '"metallicFactor":0.0500,"roughnessFactor":0.6000}}],'
      '"meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1},'
      '"indices":2,"material":0}]}],'
      '"buffers":[{"byteLength":${binBytes.length}}],'
      '"bufferViews":['
      '{"buffer":0,"byteOffset":$oPos,"byteLength":$lenPos},'
      '{"buffer":0,"byteOffset":$oNrm,"byteLength":$lenNrm},'
      '{"buffer":0,"byteOffset":$oIdx,"byteLength":$lenIdx}],'
      '"accessors":['
      '{"bufferView":0,"componentType":5126,"count":$vc,"type":"VEC3"},'
      '{"bufferView":1,"componentType":5126,"count":$vc,"type":"VEC3"},'
      '{"bufferView":2,"componentType":5123,"count":$ic,"type":"SCALAR"}]}';

  final jchunk = BytesBuilder()..add(ascii.encode(json));
  _pad4(jchunk, 0x20);
  final jbytes = jchunk.toBytes();

  final total = 12 + 8 + jbytes.length + 8 + binBytes.length;
  final glb = BytesBuilder();
  final header = ByteData(12)
    ..setUint32(0, 0x46546C67, Endian.little)
    ..setUint32(4, 2, Endian.little)
    ..setUint32(8, total, Endian.little);
  glb.add(header.buffer.asUint8List());

  final jhdr = ByteData(8)
    ..setUint32(0, jbytes.length, Endian.little)
    ..setUint32(4, 0x4E4F534A, Endian.little);
  glb.add(jhdr.buffer.asUint8List());
  glb.add(jbytes);

  final bhdr = ByteData(8)
    ..setUint32(0, binBytes.length, Endian.little)
    ..setUint32(4, 0x004E4942, Endian.little);
  glb.add(bhdr.buffer.asUint8List());
  glb.add(binBytes);

  return glb.toBytes();
}
