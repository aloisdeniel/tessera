// duel_world.dart — procedurally generated GLB scenery for Duel's arena,
// placed through the engine's WORLD-MODEL layer (TesseraScene.world): a rocky
// island the board sits on, a pair of braziers that anchor the arena's point
// lights, and a few crystal shards. Same GLB-emit approach as chess_gen /
// checkers_gen; all models are authored feet-at-y=0, +Y up, so a world-model
// placement at y 0 rests them on the tiles' underside plane.
import 'dart:convert';
import 'dart:math' as math;
import 'dart:typed_data';

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

  /// A flat-shaded triangle (three fresh vertices sharing the face normal —
  /// the faceted, low-poly rock look), wound so its normal points AWAY from
  /// the shape's interior reference point [inside].
  void facet(List<double> a, List<double> b, List<double> c, List<double> inside) {
    var ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
    final vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
    var nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
    final cx = (a[0] + b[0] + c[0]) / 3 - inside[0];
    final cy = (a[1] + b[1] + c[1]) / 3 - inside[1];
    final cz = (a[2] + b[2] + c[2]) / 3 - inside[2];
    if (nx * cx + ny * cy + nz * cz < 0) {
      final t = b;
      b = c;
      c = t;
      nx = -nx;
      ny = -ny;
      nz = -nz;
    }
    tri(vert(a[0], a[1], a[2], nx, ny, nz), vert(b[0], b[1], b[2], nx, ny, nz),
        vert(c[0], c[1], c[2], nx, ny, nz));
  }
}

class _PP {
  const _PP(this.y, this.r);
  final double y;
  final double r;
}

/// Smooth-shaded lathe of a side profile (see checkers_gen for the original).
void _lathe(_Mesh m, List<_PP> p, {int seg = 24}) {
  final n = p.length;
  final ring = List<int>.filled(n, 0);
  for (var i = 0; i < n; ++i) {
    final y0 = p[i > 0 ? i - 1 : i].y, r0 = p[i > 0 ? i - 1 : i].r;
    final y1 = p[i < n - 1 ? i + 1 : i].y, r1 = p[i < n - 1 ? i + 1 : i].r;
    var nr = y1 - y0, ny = -(r1 - r0);
    var nl = math.sqrt(nr * nr + ny * ny);
    if (nl < 1e-6) {
      nr = 1;
      ny = 0;
      nl = 1;
    }
    nr /= nl;
    ny /= nl;
    ring[i] = m.vc;
    for (var k = 0; k < seg; ++k) {
      final a = _tau * k / seg;
      final c = math.cos(a), s = math.sin(a);
      m.vert(p[i].r * c, p[i].y, p[i].r * s, nr * c, ny, nr * s);
    }
  }
  for (var i = 0; i < n - 1; ++i) {
    if (p[i].r < 1e-5 && p[i + 1].r < 1e-5) continue;
    for (var k = 0; k < seg; ++k) {
      final k2 = (k + 1) % seg;
      m.tri(ring[i] + k, ring[i + 1] + k, ring[i + 1] + k2);
      m.tri(ring[i] + k, ring[i + 1] + k2, ring[i] + k2);
    }
  }
  if (p[n - 1].r > 1e-5) {
    final ty = p[n - 1].y, tr = p[n - 1].r;
    final c = m.vert(0, ty, 0, 0, 1, 0);
    for (var k = 0; k < seg; ++k) {
      final k2 = (k + 1) % seg;
      final a = m.vert(
          tr * math.cos(_tau * k / seg), ty, tr * math.sin(_tau * k / seg), 0, 1, 0);
      final b = m.vert(
          tr * math.cos(_tau * k2 / seg), ty, tr * math.sin(_tau * k2 / seg), 0, 1, 0);
      m.tri(c, b, a);
    }
  }
}

/// The floating rock the arena rests on: an irregular faceted spire tapering
/// from a wide, slightly jagged rim at y=0 down to a point — the board sits on
/// its flat top and the silhouette drops away beneath the tiles.
Uint8List buildIslandGlb() {
  final m = _Mesh();
  final rng = math.Random(0xD0E1);
  const seg = 16;
  // rim radii with deterministic jaggedness; rings shrink and sink
  final rings = <List<List<double>>>[];
  const levels = [
    (y: 0.0, r: 9.4, jag: 0.55),
    (y: -1.7, r: 7.2, jag: 0.9),
    (y: -3.6, r: 4.4, jag: 0.8),
    (y: -5.4, r: 1.9, jag: 0.4),
  ];
  for (final lv in levels) {
    final ring = <List<double>>[];
    for (var k = 0; k < seg; ++k) {
      final a = _tau * k / seg;
      final r = lv.r + (rng.nextDouble() * 2 - 1) * lv.jag;
      final y = lv.y + (rng.nextDouble() * 2 - 1) * 0.25;
      ring.add([r * math.cos(a), y, r * math.sin(a)]);
    }
    rings.add(ring);
  }
  const inside = [0.0, -2.0, 0.0]; // interior reference for facet orientation
  // flat top cap (hidden under the board, but shows past its edges)
  final top = rings[0];
  for (var k = 0; k < seg; ++k) {
    m.facet([0, 0, 0], top[k], top[(k + 1) % seg], inside);
  }
  // faceted flanks
  for (var i = 0; i < rings.length - 1; ++i) {
    final a = rings[i], b = rings[i + 1];
    for (var k = 0; k < seg; ++k) {
      final k2 = (k + 1) % seg;
      m.facet(a[k], b[k], b[k2], inside);
      m.facet(a[k], b[k2], a[k2], inside);
    }
  }
  // tip
  const tip = [0.0, -6.6, 0.0];
  final last = rings.last;
  for (var k = 0; k < seg; ++k) {
    m.facet(last[k], tip, last[(k + 1) % seg], inside);
  }
  return _glb(m, const [0.30, 0.27, 0.33, 1.0]);
}

/// A standing brazier: base, column, and a wide fire bowl — the arena's point
/// lights hover right above its rim so the glow reads as its flame.
Uint8List buildBrazierGlb() {
  final m = _Mesh();
  _lathe(m, const [
    _PP(0.00, 0.55),
    _PP(0.10, 0.55),
    _PP(0.16, 0.34),
    _PP(0.30, 0.22),
    _PP(0.95, 0.17), // column
    _PP(1.05, 0.30),
    _PP(1.18, 0.58), // bowl flare
    _PP(1.30, 0.62),
    _PP(1.26, 0.48), // inner lip, dipping into the bowl
    _PP(1.16, 0.0),
  ]);
  return _glb(m, const [0.42, 0.30, 0.20, 1.0]);
}

/// A crystal shard: an elongated, flat-shaded hexagonal spike.
Uint8List buildCrystalGlb() {
  final m = _Mesh();
  final rng = math.Random(0xC4);
  const seg = 6;
  final ring = <List<double>>[];
  for (var k = 0; k < seg; ++k) {
    final a = _tau * k / seg;
    final r = 0.30 + rng.nextDouble() * 0.08;
    ring.add([r * math.cos(a), 0.75 + rng.nextDouble() * 0.2, r * math.sin(a)]);
  }
  const base = [0.0, 0.0, 0.0];
  const tip = [0.08, 2.3, -0.05];
  const inside = [0.0, 1.0, 0.0]; // interior reference for facet orientation
  for (var k = 0; k < seg; ++k) {
    final k2 = (k + 1) % seg;
    m.facet(base, ring[k2], ring[k], inside);
    m.facet(ring[k], ring[k2], tip, inside);
  }
  return _glb(m, const [0.55, 0.75, 1.0, 1.0]);
}

Uint8List _glb(_Mesh m, List<double> rgba) {
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
  while ((bin.length & 3) != 0) {
    bin.addByte(0);
  }
  final binBytes = bin.toBytes();

  String f4(double v) => v.toStringAsFixed(4);
  final json = '{"asset":{"version":"2.0"},'
      '"scene":0,"scenes":[{"nodes":[0]}],'
      '"nodes":[{"mesh":0}],'
      '"materials":[{"pbrMetallicRoughness":{"baseColorFactor":'
      '[${f4(rgba[0])},${f4(rgba[1])},${f4(rgba[2])},${f4(rgba[3])}],'
      '"metallicFactor":0.0500,"roughnessFactor":0.8000}}],'
      '"meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1},'
      '"indices":2,"material":0}]}],'
      '"buffers":[{"byteLength":${binBytes.length}}],'
      '"bufferViews":['
      '{"buffer":0,"byteOffset":$oPos,"byteLength":${4 * 3 * vc}},'
      '{"buffer":0,"byteOffset":$oNrm,"byteLength":${4 * 3 * vc}},'
      '{"buffer":0,"byteOffset":$oIdx,"byteLength":${2 * ic}}],'
      '"accessors":['
      '{"bufferView":0,"componentType":5126,"count":$vc,"type":"VEC3"},'
      '{"bufferView":1,"componentType":5126,"count":$vc,"type":"VEC3"},'
      '{"bufferView":2,"componentType":5123,"count":$ic,"type":"SCALAR"}]}';

  final jchunk = BytesBuilder()..add(ascii.encode(json));
  while ((jchunk.length & 3) != 0) {
    jchunk.addByte(0x20);
  }
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
