# Changelog

## 0.1.0

- Initial release. Embeds the Tessera renderer in Flutter as a native
  **platform view** (`TesseraView`) on macOS (Metal).
- `TesseraController`: drives the native-owned engine — registration, lighting,
  quality, timing, camera fit, scene push, and screen picking — over a per-view
  method channel plus FFI (`tessera` package `Tessera.fromHandle`) for the
  any-thread `set_state` hot path.
- High-level value types (`TesseraScene`, `TesseraTile`, `TesseraEntity`,
  `TesseraTileType`, `TesseraEntityType`, `TesseraEffectType`,
  `TesseraCameraPose`, …) so app code never touches `dart:ffi`.
- macOS native reference implementation: a C bridge (SDL hidden window + engine
  + `tessera_render_rgba`) and a Swift `CAMetalLayer` presenter.
- `example/`: a full game of chess (AI autoplay + tap-to-move), ported from the
  C `examples/chess`.
