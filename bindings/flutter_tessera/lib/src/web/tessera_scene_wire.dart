// tessera_scene_wire.dart — pure-Dart encoder for the engine's own state blob
// format (src/serialize.c, `tessera_state_serialize`, version 3), plus the
// replay container codec (`tessera_replay_serialize` / `tessera_replay_open`).
//
// On the web the scene is marshalled to the engine through this wire format:
// the blob is built here in Dart, copied into wasm memory, and reconstructed
// natively with `tessera_state_deserialize` — no in-memory TesseraState struct
// is ever mirrored. The bytes are identical to what the io (FFI) path's
// `serializeScene` produces, so saves and replays round-trip across platforms.
//
// The walk below transcribes tessera_state_serialize field by field; each
// section carries a reference to the corresponding serialize.c block. The
// VALUES come from the same TesseraScene -> placement mapping the io
// controller's `_withNativeScene` / `_fillCamera` perform (including defaults
// and the "path only when length > 1" rules), so both platforms feed the
// engine identical states.
//
// Pure data: no dart:ffi, no js interop — unit-testable anywhere.
library;

import 'dart:convert' show utf8;
import 'dart:typed_data';

import '../types.dart';

/// TESSERA_STATE_BLOB_MAGIC — bytes "TSST" when stored little-endian.
const int tesseraStateBlobMagic = 0x54535354;

/// TESSERA_STATE_BLOB_VERSION (3: effects carry attach_card_id).
const int tesseraStateBlobVersion = 3;

/// TESSERA_REPLAY_MAGIC — bytes "TSRP" when stored little-endian.
const int tesseraReplayMagic = 0x50525354;

/// TESSERA_REPLAY_VERSION.
const int tesseraReplayVersion = 1;

/// TESSERA_LABEL_TEXT_CAP — label text bytes incl. the NUL terminator.
const int tesseraLabelTextCap = 64;

/// Little-endian growing byte writer mirroring serialize.c's `TsWr`.
class _Wr {
  final _out = BytesBuilder(copy: true);
  final _scratch = ByteData(8);

  int get length => _out.length;

  void u8(int v) => _out.addByte(v & 0xff);

  void u16(int v) {
    _scratch.setUint16(0, v & 0xffff, Endian.little);
    _out.add(Uint8List.sublistView(_scratch, 0, 2).sublist(0));
  }

  void u32(int v) {
    _scratch.setUint32(0, v & 0xffffffff, Endian.little);
    _out.add(Uint8List.sublistView(_scratch, 0, 4).sublist(0));
  }

  /// Two little-endian u32 words (wr_u64). Avoids 64-bit ints/shifts, which
  /// dart2js cannot represent past 2^53 / truncates in bitwise ops.
  void u64(int v) {
    final hi = v ~/ 0x100000000;
    final lo = v - hi * 0x100000000;
    u32(lo);
    u32(hi);
  }

  void i32(int v) {
    _scratch.setInt32(0, v, Endian.little);
    _out.add(Uint8List.sublistView(_scratch, 0, 4).sublist(0));
  }

  void f32(double v) {
    _scratch.setFloat32(0, v, Endian.little);
    _out.add(Uint8List.sublistView(_scratch, 0, 4).sublist(0));
  }

  void f32s(List<double> v, int n) {
    for (var i = 0; i < n; ++i) {
      f32(v[i]);
    }
  }

  void coord(int x, int y) {
    i32(x);
    i32(y);
  }

  void bytes(List<int> b) => _out.add(b);

  Uint8List take() => _out.takeBytes();
}

/// serialize.c `wr_camera`: mode u32, focus f32x2, distance/yaw/pitch/fov,
/// position[3], orientation[4], target[3], target_id u64, focus_card_id u64,
/// fit_padding f32 — 88 bytes (TS_SER_CAMERA).
///
/// The mode/field mapping from the high-level [TesseraCamera] cases mirrors
/// the io controller's `_fillCamera` exactly; untouched fields stay 0.
void _writeCamera(_Wr w, TesseraCamera camera) {
  var mode = 0;
  var focusX = 0.0, focusY = 0.0;
  var distance = 0.0, yaw = 0.0, pitch = 0.0, fov = 0.0;
  var position = const [0.0, 0.0, 0.0];
  var orientation = const [0.0, 0.0, 0.0, 0.0];
  var target = const [0.0, 0.0, 0.0];
  var targetId = 0;
  var focusCardId = 0;
  var fitPadding = 0.0;

  switch (camera) {
    case TesseraCameraPose c: // mode 0 = ORBIT
      mode = 0;
      distance = c.distance;
      yaw = c.yaw;
      pitch = c.pitch;
      fov = c.fov;
      focusX = c.focusX;
      focusY = c.focusY;
    case TesseraCameraManual c:
      mode = 1;
      fov = c.fov;
      position = c.position;
      orientation = c.orientation;
    case TesseraCameraTarget c:
      mode = 2;
      fov = c.fov;
      position = c.source;
      target = c.target;
    case TesseraCameraFocusTile c:
      mode = 3;
      targetId = c.tileId;
      distance = c.distance;
      yaw = c.yaw;
      pitch = c.pitch;
      fov = c.fov;
    case TesseraCameraFocusEntity c:
      mode = 4;
      targetId = c.entityId;
      distance = c.distance;
      yaw = c.yaw;
      pitch = c.pitch;
      fov = c.fov;
    case TesseraCameraFocusDice c:
      mode = 5;
      targetId = c.diceId;
      distance = c.distance;
      yaw = c.yaw;
      pitch = c.pitch;
      fov = c.fov;
    case TesseraCameraFocusDraw c:
      mode = 6;
      targetId = c.drawId;
      distance = c.distance;
      yaw = c.yaw;
      pitch = c.pitch;
      fov = c.fov;
    case TesseraCameraFocusCard c:
      mode = 7;
      targetId = c.cardId;
      fitPadding = c.padding;
      fov = c.fov;
    case TesseraCameraFocusHand c:
      mode = 8;
      targetId = c.handId;
      focusCardId = c.cardId ?? 0;
      fitPadding = c.padding;
      fov = c.fov;
  }

  w.u32(mode);
  w.f32(focusX);
  w.f32(focusY);
  w.f32(distance);
  w.f32(yaw);
  w.f32(pitch);
  w.f32(fov);
  w.f32s(position, 3);
  w.f32s(orientation, 4);
  w.f32s(target, 3);
  w.u64(targetId);
  w.u64(focusCardId);
  w.f32(fitPadding);
}

/// Flatten [scene] into a `tessera_state_serialize` v3 blob — byte-identical
/// to what the io path's `serializeScene` returns for the same scene.
Uint8List encodeSceneBlob(TesseraScene scene) {
  final w = _Wr();

  // Header (serialize.c: magic u32, version u32, total size u64, epoch u64 —
  // TS_BLOB_HDR = 24; total is patched in at the end).
  w.u32(tesseraStateBlobMagic);
  w.u32(tesseraStateBlobVersion);
  w.u64(0); // total size placeholder
  w.u64(scene.epoch);

  _writeCamera(w, scene.camera);

  // Tiles (serialize.c: coord, tile_def u32, variant u32, id u64 — TS_SER_TILE
  // 24). Values per _withNativeScene: def/variant/id straight through.
  w.u64(scene.tiles.length);
  for (final s in scene.tiles) {
    w.coord(s.x, s.y);
    w.u32(s.def);
    w.u32(s.variant);
    w.u64(s.id);
  }

  // Entities (serialize.c: id u64, def u32, coord, facing u16, anim u32,
  // path_count u32 + path coords — TS_SER_ENTITY 30 + n*8). Path is emitted
  // only when it holds more than one coord (_withNativeScene rule).
  w.u64(scene.entities.length);
  for (final s in scene.entities) {
    final pc = s.path.length > 1 ? s.path.length : 0;
    w.u64(s.id);
    w.u32(s.def);
    w.coord(s.x, s.y);
    w.u16(s.facing);
    w.u32(s.anim);
    w.u32(pc);
    for (var k = 0; k < pc; ++k) {
      w.coord(s.path[k].$1, s.path[k].$2);
    }
  }

  // Effects (serialize.c: id u64, def u32, coord, attach_entity_id u64,
  // attach_card_id u64 — TS_SER_EFFECT 36).
  w.u64(scene.effects.length);
  for (final s in scene.effects) {
    w.u64(s.id);
    w.u32(s.def);
    w.coord(s.x, s.y);
    w.u64(s.attachEntity);
    w.u64(s.attachCard);
  }

  // Cards (serialize.c: id u64, def u32, position[3], orientation[4],
  // hidden u8, hand u64, hand_slot u32, source_draw u64, path_count u32 +
  // n*3 f32 — TS_SER_CARD 65 + n*12). Path only for free cards (hand == 0)
  // with more than one step (_withNativeScene rule).
  w.u64(scene.cards.length);
  for (final s in scene.cards) {
    final pc = (s.hand == 0 && s.path.length > 1) ? s.path.length : 0;
    w.u64(s.id);
    w.u32(s.def);
    w.f32s(s.position, 3);
    w.f32s(s.orientation, 4);
    w.u8(s.hidden ? 1 : 0);
    w.u64(s.hand);
    w.u32(s.handSlot);
    w.u64(s.sourceDraw);
    w.u32(pc);
    for (var k = 0; k < pc; ++k) {
      w.f32s(s.path[k], 3);
    }
  }

  // Card draws / piles (serialize.c: id u64, def u32, position[3],
  // orientation[4], count u32, top_hidden u8 — TS_SER_DRAW 45).
  w.u64(scene.cardDraws.length);
  for (final s in scene.cardDraws) {
    w.u64(s.id);
    w.u32(s.def);
    w.f32s(s.position, 3);
    w.f32s(s.orientation, 4);
    w.u32(s.count);
    w.u8(s.topHidden ? 1 : 0);
  }

  // Hands (serialize.c: id u64, position[3], orientation[4], spread_deg,
  // radius, card_spacing, selected_card u64 — TS_SER_HAND 56).
  w.u64(scene.hands.length);
  for (final s in scene.hands) {
    w.u64(s.id);
    w.f32s(s.position, 3);
    w.f32s(s.orientation, 4);
    w.f32(s.spreadDeg);
    w.f32(s.radius);
    w.f32(s.cardSpacing);
    w.u64(s.selectedCard);
  }

  // Dice (serialize.c: id u64, def u32, face u32, position[3], seed u32,
  // throw_s f32 — TS_SER_DICE 36).
  w.u64(scene.dice.length);
  for (final s in scene.dice) {
    w.u64(s.id);
    w.u32(s.def);
    w.u32(s.face);
    w.f32s(s.position, 3);
    w.u32(s.seed);
    w.f32(s.throwS);
  }

  // Overlays (serialize.c: coord, shape u32, atlas u32, uv f32x4, tint f32x4,
  // pulse_s, alpha min/max, scale min/max — TS_SER_OVERLAY 68).
  w.u64(scene.overlays.length);
  for (final s in scene.overlays) {
    w.coord(s.x, s.y);
    w.u32(s.shape.index);
    w.u32(s.atlas);
    w.f32s(s.uv, 4);
    w.f32s(s.tint, 4);
    w.f32(s.pulseS);
    w.f32(s.pulseAlphaMin);
    w.f32(s.pulseAlphaMax);
    w.f32(s.pulseScaleMin);
    w.f32(s.pulseScaleMax);
  }

  // Labels (serialize.c: id u64, font u32, text[64] NUL-padded, anchor u32,
  // anchor_id u64, position[3], size f32, color f32x4, billboard u8 —
  // TS_SER_LABEL 121). Text: UTF-8, truncated to 63 bytes + NUL like the io
  // marshaller's bounded copy.
  w.u64(scene.labels.length);
  for (final s in scene.labels) {
    w.u64(s.id);
    w.u32(s.font);
    final text = Uint8List(tesseraLabelTextCap); // zero-padded, NUL-terminated
    final enc = utf8.encode(s.text);
    final n = enc.length < tesseraLabelTextCap - 1
        ? enc.length
        : tesseraLabelTextCap - 1;
    text.setRange(0, n, enc);
    w.bytes(text);
    w.u32(s.anchor.index);
    w.u64(s.anchorId);
    w.f32s(s.position, 3);
    w.f32(s.size);
    w.f32s(s.color, 4);
    w.u8(s.billboard ? 1 : 0);
  }

  // Highlights (serialize.c: target_id u64, kind u32, style u32, color f32x4,
  // thickness, pulse_s, pulse_min, pulse_max — TS_SER_HL 48).
  w.u64(scene.highlights.length);
  for (final s in scene.highlights) {
    w.u64(s.targetId);
    w.u32(s.kind.index);
    w.u32(s.style.index);
    w.f32s(s.color, 4);
    w.f32(s.thickness);
    w.f32(s.pulseS);
    w.f32(s.pulseMin);
    w.f32(s.pulseMax);
  }

  // Point lights (serialize.c: id u64, position[3], color[3], intensity,
  // radius — TS_SER_PLIGHT 40).
  w.u64(scene.pointLights.length);
  for (final s in scene.pointLights) {
    w.u64(s.id);
    w.f32s(s.position, 3);
    w.f32s(s.color, 3);
    w.f32(s.intensity);
    w.f32(s.radius);
  }

  // World models (serialize.c: id u64, def u32, position[3], orientation[4],
  // scale — TS_SER_WMODEL 44).
  w.u64(scene.world.length);
  for (final s in scene.world) {
    w.u64(s.id);
    w.u32(s.def);
    w.f32s(s.position, 3);
    w.f32s(s.orientation, 4);
    w.f32(s.scale);
  }

  // Patch the total size into the header (serialize.c end of serialize).
  final blob = w.take();
  final d = ByteData.sublistView(blob);
  final total = blob.length;
  final hi = total ~/ 0x100000000;
  d.setUint32(8, total - hi * 0x100000000, Endian.little);
  d.setUint32(12, hi, Endian.little);
  return blob;
}

/// Quick structural check of a state blob's header (magic, version, total
/// size) — the synchronous subset of `tessera_state_deserialize`'s
/// validation, used to reject malformed input with the same eagerness as the
/// io path before handing the bytes to the wasm engine.
bool sceneBlobLooksValid(Uint8List blob) {
  if (blob.length < 24) return false;
  final d = ByteData.sublistView(blob);
  if (d.getUint32(0, Endian.little) != tesseraStateBlobMagic) return false;
  if (d.getUint32(4, Endian.little) != tesseraStateBlobVersion) return false;
  final total = d.getUint32(8, Endian.little) +
      d.getUint32(12, Endian.little) * 0x100000000;
  return total == blob.length;
}

/// One replay record: host timestamp (ms) + a state blob.
typedef ReplayRecord = (int timestampMs, Uint8List blob);

/// Flatten replay records into a `tessera_replay_serialize` container:
/// magic u32, version u32, total u64, count u32, reserved u32, then per
/// record ts u64, size u64, blob bytes (serialize.c, replay section).
Uint8List encodeReplayContainer(List<ReplayRecord> records) {
  final w = _Wr();
  w.u32(tesseraReplayMagic);
  w.u32(tesseraReplayVersion);
  w.u64(0); // total size placeholder
  w.u32(records.length);
  w.u32(0); // reserved
  for (final (ts, blob) in records) {
    w.u64(ts);
    w.u64(blob.length);
    w.bytes(blob);
  }
  final out = w.take();
  final d = ByteData.sublistView(out);
  final total = out.length;
  final hi = total ~/ 0x100000000;
  d.setUint32(8, total - hi * 0x100000000, Endian.little);
  d.setUint32(12, hi, Endian.little);
  return out;
}

/// Parse a replay container back into records (`tessera_replay_open`).
/// Returns null on malformed input (bad magic/version/size/truncation).
List<ReplayRecord>? decodeReplayContainer(Uint8List data) {
  if (data.length < 24) return null;
  final d = ByteData.sublistView(data);
  int u32(int off) => d.getUint32(off, Endian.little);
  int u64(int off) => u32(off) + u32(off + 4) * 0x100000000;
  if (u32(0) != tesseraReplayMagic) return null;
  if (u32(4) != tesseraReplayVersion) return null;
  if (u64(8) != data.length) return null;
  final count = u32(16);
  // Each record needs at least its 16-byte prefix (replay_open's guard).
  if (count > (data.length - 24) ~/ 16) return null;
  final out = <ReplayRecord>[];
  var off = 24;
  for (var i = 0; i < count; ++i) {
    if (off + 16 > data.length) return null;
    final ts = u64(off);
    final size = u64(off + 8);
    off += 16;
    if (size > data.length - off) return null;
    out.add((ts, Uint8List.sublistView(data, off, off + size).sublist(0)));
    off += size;
  }
  if (off != data.length) return null;
  return out;
}
