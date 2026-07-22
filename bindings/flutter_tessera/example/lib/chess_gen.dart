// chess_gen.dart — procedurally generate the six chess piece models as GLBs.
//
// A direct port of examples/chess/chess_gen.h. Each piece is a lathe (surface
// of revolution) of a hand-authored side profile, capped top and bottom, plus a
// couple of extra boxes for the knight's head and the king's cross. The mesh
// carries POSITION + NORMAL + indices and a single material whose
// baseColorFactor tints the whole piece (white vs. black army) — Tessera reads
// that factor and applies it as the entity tint.
//
// All models are authored feet-at-y=0, +Y up, and fit inside a 1x1 tile.
// `buildPieceGlb` returns the GLB as a `Uint8List` ready to hand to Tessera via
// a `TesseraBytes.data` pointer.

import 'dart:convert';
import 'dart:math' as math;
import 'dart:typed_data';

/// Piece kinds (also used by the game logic). 0 = empty square.
const int chessPawn = 1;
const int chessKnight = 2;
const int chessBishop = 3;
const int chessRook = 4;
const int chessQueen = 5;
const int chessKing = 6;

const int _seg = 28; // lathe segments around the Y axis

class _Mesh {
  final List<double> pos = <double>[]; // x,y,z triples
  final List<double> nrm = <double>[]; // x,y,z triples
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

  double px(int i) => pos[i * 3];
  double py(int i) => pos[i * 3 + 1];
  double pz(int i) => pos[i * 3 + 2];

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

const double _tau = 6.2831853;

void _lathe(_Mesh m, List<_PP> p) {
  final n = p.length;
  const s = _seg;
  final ring = List<int>.filled(n, 0);
  for (var i = 0; i < n; ++i) {
    // smooth 2D outward normal from neighbouring profile points
    final y0 = p[i > 0 ? i - 1 : i].y, r0 = p[i > 0 ? i - 1 : i].r;
    final y1 = p[i < n - 1 ? i + 1 : i].y, r1 = p[i < n - 1 ? i + 1 : i].r;
    final dy = y1 - y0, dr = r1 - r0;
    var nr = dy, ny = -dr; // rotate tangent +90 deg
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
      // wound CCW-outward so back-face culling keeps the outer shell.
      m.quad(ring[i] + k, ring[i + 1] + k, ring[i + 1] + s2, ring[i] + s2);
    }
  }
  // bottom cap (fan), facing -Y
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
  // top cap (fan), facing +Y
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

/// An axis-aligned box, optionally leaned forward about the X axis.
void _box(_Mesh m, double cx, double cy, double cz, double hx, double hy,
    double hz, double lean) {
  final ca = math.cos(lean), sa = math.sin(lean);
  const sg = <List<int>>[
    [-1, -1, -1], [1, -1, -1], [1, 1, -1], [-1, 1, -1], //
    [-1, -1, 1], [1, -1, 1], [1, 1, 1], [-1, 1, 1],
  ];
  final v = List<int>.filled(8, 0);
  for (var i = 0; i < 8; ++i) {
    final lx = sg[i][0] * hx, ly = sg[i][1] * hy, lz = sg[i][2] * hz;
    final ry = ly * ca - lz * sa, rz = ly * sa + lz * ca;
    v[i] = m.vert(cx + lx, cy + ry, cz + rz, 0, 1, 0); // normal fixed below
  }
  // faces (CCW outward): -Z,+Z,-Y,+Y,-X,+X
  const fc = <List<int>>[
    [0, 3, 2, 1], [4, 5, 6, 7], [0, 1, 5, 4], //
    [3, 7, 6, 2], [0, 4, 7, 3], [1, 2, 6, 5],
  ];
  const fn = <List<double>>[
    [0, 0, -1], [0, 0, 1], [0, -1, 0], //
    [0, 1, 0], [-1, 0, 0], [1, 0, 0],
  ];
  for (var f = 0; f < 6; ++f) {
    // rotate the face normal by the lean too
    final ny = fn[f][1] * ca - fn[f][2] * sa, nz = fn[f][1] * sa + fn[f][2] * ca;
    final q = List<int>.filled(4, 0);
    for (var k = 0; k < 4; ++k) {
      final src = v[fc[f][k]];
      q[k] = m.vert(m.px(src), m.py(src), m.pz(src), fn[f][0], ny, nz);
    }
    m.quad(q[0], q[1], q[2], q[3]);
  }
}

void _buildMesh(_Mesh m, int kind, int faceDir) {
  switch (kind) {
    case chessPawn:
      _lathe(m, const [
        _PP(0.00, 0.30), _PP(0.05, 0.30), _PP(0.08, 0.22), _PP(0.11, 0.14),
        _PP(0.13, 0.11), _PP(0.25, 0.10), _PP(0.29, 0.18), _PP(0.32, 0.13),
        _PP(0.34, 0.11), _PP(0.37, 0.115), _PP(0.40, 0.15), _PP(0.47, 0.16),
        _PP(0.54, 0.135), _PP(0.585, 0.06), _PP(0.60, 0.0),
      ]);
    case chessRook:
      _lathe(m, const [
        _PP(0.00, 0.33), _PP(0.05, 0.33), _PP(0.08, 0.25), _PP(0.12, 0.18),
        _PP(0.20, 0.165), _PP(0.42, 0.165), _PP(0.46, 0.19), _PP(0.49, 0.235),
        _PP(0.60, 0.235), _PP(0.615, 0.25), _PP(0.62, 0.25),
      ]);
    case chessKnight:
      _lathe(m, const [
        _PP(0.00, 0.33), _PP(0.05, 0.33), _PP(0.08, 0.25), _PP(0.12, 0.18),
        _PP(0.28, 0.165), _PP(0.32, 0.20), _PP(0.35, 0.175),
      ]);
      // neck + head lean toward the opponent (faceDir = +1 or -1 along z)
      final d = faceDir.toDouble();
      _box(m, 0.0, 0.46, 0.02 * d, 0.12, 0.16, 0.10, 0.35 * d);
      _box(m, 0.0, 0.60, 0.15 * d, 0.11, 0.085, 0.19, 0.75 * d); // muzzle
      _box(m, 0.0, 0.66, -0.06 * d, 0.09, 0.10, 0.06, 0.2 * d); // ears
    case chessBishop:
      _lathe(m, const [
        _PP(0.00, 0.31), _PP(0.05, 0.31), _PP(0.08, 0.23), _PP(0.11, 0.15),
        _PP(0.28, 0.115), _PP(0.32, 0.19), _PP(0.35, 0.135), _PP(0.37, 0.115),
        _PP(0.44, 0.155), _PP(0.52, 0.165), _PP(0.59, 0.135), _PP(0.63, 0.085),
        _PP(0.645, 0.11), _PP(0.66, 0.065), _PP(0.68, 0.05), _PP(0.72, 0.06),
        _PP(0.75, 0.0),
      ]);
    case chessQueen:
      _lathe(m, const [
        _PP(0.00, 0.35), _PP(0.05, 0.35), _PP(0.08, 0.27), _PP(0.12, 0.17),
        _PP(0.32, 0.125), _PP(0.36, 0.21), _PP(0.39, 0.145), _PP(0.41, 0.125),
        _PP(0.50, 0.175), _PP(0.58, 0.165), _PP(0.65, 0.13), _PP(0.69, 0.165),
        _PP(0.735, 0.205), _PP(0.745, 0.12), _PP(0.76, 0.09), _PP(0.80, 0.115),
        _PP(0.85, 0.0),
      ]);
    case chessKing:
      _lathe(m, const [
        _PP(0.00, 0.36), _PP(0.05, 0.36), _PP(0.08, 0.28), _PP(0.12, 0.18),
        _PP(0.34, 0.135), _PP(0.38, 0.22), _PP(0.41, 0.15), _PP(0.43, 0.13),
        _PP(0.52, 0.185), _PP(0.61, 0.175), _PP(0.68, 0.14), _PP(0.72, 0.17),
        _PP(0.765, 0.205), _PP(0.775, 0.13), _PP(0.79, 0.10), _PP(0.83, 0.12),
        _PP(0.86, 0.10),
      ]);
      // the crown cross
      _box(m, 0.0, 0.955, 0.0, 0.028, 0.085, 0.028, 0.0);
      _box(m, 0.0, 0.945, 0.0, 0.075, 0.028, 0.028, 0.0);
    default:
      _lathe(m, const [_PP(0.0, 0.3), _PP(0.5, 0.3)]);
  }
}

void _pad4(BytesBuilder b, int fill) {
  while ((b.length & 3) != 0) {
    b.addByte(fill);
  }
}

/// Build a piece GLB. [kind] in `chess*`, [rgba] is the material base color,
/// [faceDir] orients the knight (+1 / -1 along +z).
Uint8List buildPieceGlb(int kind, int faceDir, List<double> rgba) {
  final m = _Mesh();
  _buildMesh(m, kind, faceDir);
  final vc = m.vc;
  final ic = m.idx.length;

  // ---- binary chunk: positions, then normals, then indices (padded) ----
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
      '"metallicFactor":0.0500,"roughnessFactor":0.6500}}],'
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

  // ---- GLB container ----
  final jchunk = BytesBuilder()..add(ascii.encode(json));
  _pad4(jchunk, 0x20); // pad JSON with spaces
  final jbytes = jchunk.toBytes();

  final total = 12 + 8 + jbytes.length + 8 + binBytes.length;
  final glb = BytesBuilder();
  final header = ByteData(12)
    ..setUint32(0, 0x46546C67, Endian.little) // magic 'glTF'
    ..setUint32(4, 2, Endian.little) // version
    ..setUint32(8, total, Endian.little);
  glb.add(header.buffer.asUint8List());

  final jhdr = ByteData(8)
    ..setUint32(0, jbytes.length, Endian.little)
    ..setUint32(4, 0x4E4F534A, Endian.little); // 'JSON'
  glb.add(jhdr.buffer.asUint8List());
  glb.add(jbytes);

  final bhdr = ByteData(8)
    ..setUint32(0, binBytes.length, Endian.little)
    ..setUint32(4, 0x004E4942, Endian.little); // 'BIN\0'
  glb.add(bhdr.buffer.asUint8List());
  glb.add(binBytes);

  return glb.toBytes();
}
