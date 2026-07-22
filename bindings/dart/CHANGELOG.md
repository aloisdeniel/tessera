# Changelog

## 0.3.0

- Add the dice API: `TesseraDiceFace` / `TesseraDiceDef` / `TesseraDiceThrow`
  structs and `Tessera.registerDiceDef`, `diceDefFaceCount`, `addDice`,
  `removeDice`, `clearDice`, `diceCount`, `diceFace`, `diceAllIdle` — register
  procedural per-face-sprite dice and throw them into the scene. Mirrors the new
  C API.

## 0.2.0

- Add `tessera_render_rgba` binding (`Tessera.renderRgba`) — advance + render one
  frame offscreen into a caller RGBA buffer, for embedders that present the frame
  themselves (e.g. a Flutter platform view). Mirrors the new C API.
- Add `Tessera.fromHandle(int)` — attach to an engine created and owned by a host
  (a native plugin) without owning its lifecycle; `dispose()` becomes a no-op.
- Add `tesseraSetAssetDir(dir)` binding for `tessera_set_asset_dir` — point the
  engine at bundled assets (shaders) at runtime; needed on iOS/Android.

## 0.1.0

- Initial release: complete `dart:ffi` binding for the frozen Tessera C ABI
  (`include/tessera.h`), covering lifecycle, asset/definition registration,
  immutable state, picking, world→screen projection, camera fit, timing,
  lighting, quality, projection mode, depth-of-field, and PNG capture.
- Automatic native-library discovery via `openTesseraLibrary`, with
  `TESSERA_LIBRARY_PATH` / `TESSERA_LIBRARY_DIR` overrides.
- `example/`: a full game of chess (rules engine, procedural GLB piece models,
  AI player, animated slides + capture poofs) ported from `examples/chess` in C.
