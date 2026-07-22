# Changelog

## Unreleased

- **Cards & dice in the high-level API.** New value types (`TesseraCardType`,
  `TesseraDiceType`, and the `TesseraCard` / `TesseraCardDraw` / `TesseraHand` /
  `TesseraDie` placements), `TesseraController.registerAtlas` /
  `registerCardType` / `registerDiceType`, and `TesseraScene` gained
  `cards` / `cardDraws` / `hands` / `dice`. `setScene` marshals them all.
- **Example app is now a menu of three games** (`example/`): Chess (entities),
  Blackjack (cards) and Yahtzee (dice), each built on a small shared reducer
  framework — a sealed action + sealed state, a pure `update`, and a `render`
  that projects state into one or a sequence of `TesseraScene`s.

- **Zero-copy presentation on Apple platforms.** Instead of reading the engine
  back into a CPU RGBA buffer and blitting it through a Metal presenter, the
  plugin now reparents the engine's SDL swapchain `CAMetalLayer`-backed metal
  view directly into the Flutter platform view and presents to it with the new
  `ftessera_present` (macOS + iOS). New bridge accessors `ftessera_native_window`
  / `ftessera_metal_view_tag` locate the view; the `MetalPresenter` blit path is
  removed. Android keeps the `tessera_render_rgba` → `Surface` blit.
- **Camera fit works when embedded / on orientation change.** The engine's
  fit-distance and picking derived their aspect from `SDL_GetWindowSize`, which
  is stale for a host-owned window we never resize through SDL — so the fit used
  the wrong aspect (e.g. over-zoomed in portrait). It now uses the live drawable
  size + density when embedded. `TesseraController` gains an `onResize(w, h)`
  callback (fired by the plugin after each frame whose size changed, once the
  scene is live) so apps can re-fit; the example re-frames the board there,
  covering both the initial layout and rotation.
- **Fix an engine leak / hot-restart failure on iOS.** The `CADisplayLink`
  retained the platform view, so it (and its engine) never tore down — leaking on
  every teardown and, on hot restart, stacking a second engine on the old one.
  The render loop now runs through a weak proxy so `deinit` fires. `handle` also
  surfaces the native create error instead of a bare 0.

## 0.2.0

- Add **iOS** (Metal/UIKit) and **Android** (Vulkan/Kotlin+JNI+NDK) platform-view
  implementations alongside macOS. `TesseraView` selects the right native view
  per platform.
- Port the engine's 9 MSL shaders to Vulkan GLSL → **SPIR-V** (`shaders/*` +
  `shaders/compile.sh`), so the Vulkan/Android backend renders. Validated on a
  real Vulkan device (MoltenVK), matching the Metal output.
- Engine: `tessera_render_rgba` and `tessera_set_asset_dir` (runtime asset dir
  for bundled shaders on iOS/Android).

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
