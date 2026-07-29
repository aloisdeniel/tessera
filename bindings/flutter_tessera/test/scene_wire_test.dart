// Tests for the pure-Dart scene wire writer (lib/src/web/tessera_scene_wire.dart),
// which must produce exactly the blob format of src/serialize.c
// (`tessera_state_serialize`, version 3). Pure Dart — no wasm involved.

import 'dart:typed_data';

import 'package:flutter_tessera/src/types.dart';
import 'package:flutter_tessera/src/web/tessera_scene_wire.dart';
import 'package:flutter_test/flutter_test.dart';

const le = Endian.little;

// Wire sizes from serialize.c (TS_SER_* constants).
const blobHdr = 24;
const serCamera = 88;
const serTile = 24;
const serEntity = 30; // + path_count * 8
const serEffect = 36;
const serDice = 36;
const serLabel = 121;

void main() {
  test('header: magic TSST, version 3, patched total size, epoch', () {
    final blob = encodeSceneBlob(
        const TesseraScene(camera: TesseraCameraPose(), epoch: 7));
    final d = ByteData.sublistView(blob);
    expect(d.getUint32(0, le), tesseraStateBlobMagic);
    expect(String.fromCharCodes(blob.sublist(0, 4)), 'TSST');
    expect(d.getUint32(4, le), 3);
    expect(d.getUint32(8, le), blob.length); // total size lo word
    expect(d.getUint32(12, le), 0); //            hi word
    expect(d.getUint32(16, le), 7); // epoch lo word
    // Empty scene: header + camera + 12 empty u64 array counts.
    expect(blob.length, blobHdr + serCamera + 12 * 8);
  });

  test('camera block mirrors _fillCamera (orbit pose)', () {
    final blob = encodeSceneBlob(const TesseraScene(
        camera: TesseraCameraPose(
            focusX: 1.5, focusY: -2.5, distance: 9, yaw: 0.25, pitch: 0.5,
            fov: 0.7)));
    final d = ByteData.sublistView(blob);
    const cam = blobHdr;
    expect(d.getUint32(cam + 0, le), 0); // mode ORBIT
    expect(d.getFloat32(cam + 4, le), 1.5); // focus.x
    expect(d.getFloat32(cam + 8, le), -2.5); // focus.y
    expect(d.getFloat32(cam + 12, le), 9); // distance
    expect(d.getFloat32(cam + 16, le), closeTo(0.25, 1e-6)); // yaw
    expect(d.getFloat32(cam + 20, le), closeTo(0.5, 1e-6)); // pitch
    expect(d.getFloat32(cam + 24, le), closeTo(0.7, 1e-6)); // fov
    // target_id u64 @ 68, focus_card_id @ 76, fit_padding @ 84 — all zero.
    expect(d.getUint32(cam + 68, le), 0);
    expect(d.getFloat32(cam + 84, le), 0); // fit_padding (last camera field)
  });

  test('focus-hand camera carries target/focus-card ids and padding', () {
    final blob = encodeSceneBlob(const TesseraScene(
        camera: TesseraCameraFocusHand(11, cardId: 42, padding: 0.25)));
    final d = ByteData.sublistView(blob);
    const cam = blobHdr;
    expect(d.getUint32(cam + 0, le), 8); // mode FOCUS_HAND
    expect(d.getUint32(cam + 68, le), 11); // target_id lo
    expect(d.getUint32(cam + 76, le), 42); // focus_card_id lo
    expect(d.getFloat32(cam + 84, le), closeTo(0.25, 1e-6)); // fit_padding
  });

  test('tile + entity path offsets and values', () {
    final blob = encodeSceneBlob(const TesseraScene(
      tiles: [TesseraTile(x: -3, y: 4, def: 0x01000000, id: 99, variant: 2)],
      entities: [
        TesseraEntity(
            id: 5,
            def: 0x01000001,
            x: 2,
            y: 3,
            facing: 1,
            anim: 4,
            path: [(0, 0), (1, 1), (2, 3)]),
      ],
      camera: TesseraCameraPose(),
    ));
    final d = ByteData.sublistView(blob);

    var off = blobHdr + serCamera;
    // tiles: u64 count, then one 24-byte tile.
    expect(d.getUint32(off, le), 1);
    off += 8;
    expect(d.getInt32(off, le), -3); // coord.x
    expect(d.getInt32(off + 4, le), 4); // coord.y
    expect(d.getUint32(off + 8, le), 0x01000000); // tile_def
    expect(d.getUint32(off + 12, le), 2); // variant
    expect(d.getUint32(off + 16, le), 99); // id lo
    off += serTile;

    // entities: u64 count, then one 30-byte entity + 3 path coords.
    expect(d.getUint32(off, le), 1);
    off += 8;
    expect(d.getUint32(off, le), 5); // id lo
    expect(d.getUint32(off + 8, le), 0x01000001); // def
    expect(d.getInt32(off + 12, le), 2); // coord.x
    expect(d.getInt32(off + 16, le), 3); // coord.y
    expect(d.getUint16(off + 20, le), 1); // facing
    expect(d.getUint32(off + 22, le), 4); // anim
    expect(d.getUint32(off + 26, le), 3); // path_count
    expect(d.getInt32(off + serEntity + 8 + 4, le), 1); // path[1].y
    expect(d.getInt32(off + serEntity + 16, le), 2); // path[2].x
    expect(d.getInt32(off + serEntity + 20, le), 3); // path[2].y

    // total size = header + camera + tiles + entities (+path) + 10 empty
    // section counts.
    expect(blob.length,
        blobHdr + serCamera + 8 + serTile + 8 + serEntity + 3 * 8 + 10 * 8);
  });

  test('single-step entity path is dropped (io _withNativeScene rule)', () {
    final blob = encodeSceneBlob(const TesseraScene(
      entities: [TesseraEntity(id: 1, def: 1, x: 0, y: 0, path: [(0, 0)])],
      camera: TesseraCameraPose(),
    ));
    final d = ByteData.sublistView(blob);
    final off = blobHdr + serCamera + 8; // empty tiles + entity count
    expect(d.getUint32(off + 8 + 26, le), 0); // path_count == 0
    expect(blob.length, blobHdr + serCamera + 12 * 8 + serEntity);
  });

  test('label text is NUL-padded to 64 bytes and truncated at 63', () {
    final long = 'x' * 100;
    final blob = encodeSceneBlob(TesseraScene(
      labels: [TesseraLabel(id: 1, font: 2, text: long)],
      camera: const TesseraCameraPose(),
    ));
    final d = ByteData.sublistView(blob);
    // labels are the 9th section: 8 empty counts precede it.
    final labels = blobHdr + serCamera + 8 * 8;
    expect(d.getUint32(labels, le), 1);
    final rec = labels + 8;
    expect(d.getUint32(rec + 8, le), 2); // font
    final text = blob.sublist(rec + 12, rec + 12 + 64);
    expect(text.sublist(0, 63), everyElement(0x78)); // 'x'
    expect(text[63], 0); // NUL terminator
    expect(blob.length, blobHdr + serCamera + 12 * 8 + serLabel);
  });

  test('dice/effect record sizes match TS_SER_* wire sizes', () {
    final dice = encodeSceneBlob(const TesseraScene(
      dice: [TesseraDie(id: 1, def: 2, face: 3, seed: 4, throwS: 1.5)],
      camera: TesseraCameraPose(),
    ));
    expect(dice.length, blobHdr + serCamera + 12 * 8 + serDice);

    final fx = encodeSceneBlob(const TesseraScene(
      effects: [TesseraEffect(id: 1, def: 2, attachEntity: 3, attachCard: 4)],
      camera: TesseraCameraPose(),
    ));
    expect(fx.length, blobHdr + serCamera + 12 * 8 + serEffect);
  });

  test('sceneBlobLooksValid accepts good blobs, rejects tampering', () {
    final blob =
        encodeSceneBlob(const TesseraScene(camera: TesseraCameraPose()));
    expect(sceneBlobLooksValid(blob), isTrue);
    expect(sceneBlobLooksValid(blob.sublist(0, blob.length - 1)), isFalse);
    final bad = Uint8List.fromList(blob);
    bad[0] ^= 0xff; // corrupt the magic
    expect(sceneBlobLooksValid(bad), isFalse);
  });

  test('replay container round-trips (TSRP, version 1)', () {
    final a = encodeSceneBlob(const TesseraScene(camera: TesseraCameraPose()));
    final b = encodeSceneBlob(const TesseraScene(
        camera: TesseraCameraPose(), epoch: 3));
    final container = encodeReplayContainer([(100, a), (250, b)]);
    final d = ByteData.sublistView(container);
    expect(String.fromCharCodes(container.sublist(0, 4)), 'TSRP');
    expect(d.getUint32(4, le), 1); // version
    expect(d.getUint32(8, le), container.length); // total size
    expect(d.getUint32(16, le), 2); // count

    final back = decodeReplayContainer(container)!;
    expect(back.length, 2);
    expect(back[0].$1, 100);
    expect(back[1].$1, 250);
    expect(back[0].$2, a);
    expect(back[1].$2, b);

    expect(decodeReplayContainer(container.sublist(0, 30)), isNull);
  });
}
