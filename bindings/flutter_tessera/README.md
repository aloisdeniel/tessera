# flutter_tessera

Embed the **[Tessera](https://github.com/aloisdeniel/tessera)** 3D board-game
renderer in a Flutter app as a native **platform view**.

A native platform view hosts the Metal-backed engine and its render loop on the
platform main thread; Dart drives it through a `TesseraController`. You register
tile / entity / effect definitions, push immutable scene snapshots, and the
engine diffs successive scenes and animates the transitions (pieces sliding,
capture puffs, the camera swinging).

```dart
TesseraView(
  onCreated: (c) async {
    final light = c.registerTileType(const TesseraTileType(thickness: 0.22));
    // ... register entities from GLB bytes, set light/quality/timing ...
    c.setScene(TesseraScene(tiles: [...], entities: [...],
        camera: const TesseraCameraPose()));
    await c.start();
  },
)
```

## Architecture & threading

Tessera's C API is render-**main-thread** for everything GPU-touching
(create / tick / resize / register / pick / lighting), and only `set_state` /
`is_idle` are any-thread (see `tessera/docs/platforms.md`). Flutter runs Dart on
the UI thread, so the split is:

| Concern | Runs where | How |
|---|---|---|
| Engine lifecycle + render loop | native, main thread | the platform view owns an `FTessera` bridge; a main-runloop timer calls `ftessera_present` (Apple: presents straight to the engine's swapchain) / blits an RGBA frame (Android) |
| Def registration, light/quality/timing, camera fit | Dart UI thread, **before** `start()` | FFI (`Tessera.fromHandle`) — safe because the render loop is paused during setup |
| `setScene` (hot path) | Dart UI thread, anytime | FFI `tessera_set_state` (any-thread, mutex-guarded) |
| `pick`, `start` | native, main thread | per-view method channel `flutter_tessera/view_<id>` |

On Apple platforms this is **zero-copy**: the engine renders to its SDL window's
Metal swapchain, and the plugin reparents that swapchain's `CAMetalLayer`-backed
metal view into the platform view (`ftessera_native_window` +
`ftessera_metal_view_tag`), so frames scan out straight into the Flutter surface
with no CPU round-trip — how a game is meant to present under SDL. Android keeps
the offscreen path (`tessera_render_rgba` → blit into the `Surface`). Either way
Tessera composits cleanly inside the Flutter widget tree.

## Platform support

| Platform | Backend | Status |
|---|---|---|
| **macOS** | Metal (MSL) | Built + run **verified** (Swift/AppKit + C bridge) |
| **iOS** | Metal (MSL) | Reference impl (Swift/UIKit + shared C bridge); needs an iOS build of libtessera + SDL3 |
| **Android** | Vulkan (SPIR-V) | Reference impl (Kotlin/JNI + NDK); renders with the ported SPIR-V shaders (verified on Vulkan) |

All three share the same C bridge (hidden SDL window) and the same Dart
`TesseraView` / `TesseraController`. macOS/iOS reparent the engine's swapchain
`CAMetalLayer` into the platform view and present directly (`ftessera_present`,
zero-copy); Android blits an offscreen RGBA frame into the `Surface` via
`ANativeWindow`.

### Shaders (Metal vs Vulkan)

The engine ships shaders in two formats under `assets/shaders/`: hand-authored
**MSL** (Metal — macOS/iOS) and **SPIR-V** compiled from the Vulkan GLSL sources
in `shaders/*.vert|frag` (Android/Linux). The SPIR-V port was validated by
rendering the board through SDL_GPU's Vulkan backend (via MoltenVK) and matching
the Metal output. Regenerate with `./shaders/compile.sh` (needs `glslang`).

## Building the native library

flutter_tessera links the prebuilt Tessera engine and SDL3; it does not build
them. Once, from the repo root:

```sh
brew install sdl3
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j          # -> build/libtessera.dylib
```

The macOS plugin's Swift Package manifest
(`macos/flutter_tessera/Package.swift`) links `libtessera` from `<repo>/build`
and `SDL3` from Homebrew by default; override with the `TESSERA_LIB_DIR`,
`SDL3_LIB_DIR`, and `TESSERA_INCLUDE_DIR` environment variables. A CocoaPods
podspec mirrors this for non-SPM projects. For a release app you must bundle
`libtessera.dylib` and `libSDL3.dylib` into the app's `Frameworks` and set the
rpath accordingly (and disable App Sandbox during development, since SDL opens a
GPU device / window).

## Building the native library for iOS / Android

- **iOS**: cross-compile `libtessera` for iOS (device + simulator) and provide
  SDL3 for iOS. Build the engine static lib with `TESSERA_BUILD_SHARED` defined
  (so the public `tessera_*` symbols keep default visibility for Dart's FFI
  `dlsym`), and as a **universal arm64 + x86_64** slice for the Simulator. The
  Swift Package manifest (`ios/flutter_tessera/Package.swift`) is the primary
  build path; it links `libtessera.a`, `libtessera_thirdparty.a` and `libSDL3.a`
  via `-force_load` plus SDL's iOS system frameworks (override paths with
  `TESSERA_IOS_LIB_DIR` / `SDL3_IOS_LIB_DIR` / `SDL3_INCLUDE_DIR` /
  `TESSERA_INCLUDE_DIR`), and bundles the MSL shaders as SwiftPM resources
  (`Bundle.module/shaders/`). The `ios/flutter_tessera.podspec` mirrors it for
  CocoaPods projects (the example app still uses Pods), bundling the shaders as
  `tessera_assets.bundle/shaders/`; both share one source tree under
  `ios/flutter_tessera/Sources/`. Same Metal path as macOS.

  > **iOS Simulator caveat.** SDL_GPU's Metal backend requires
  > `MTLGPUFamilyApple3`, which the iOS Simulator does not advertise even on
  > Apple Silicon (see the family check in SDL's `SDL_gpu_metal.m`
  > `METAL_CreateDevice`), so `SDL_CreateGPUDevice` fails there out of the box. A
  > **physical device** passes the check unmodified. To run in the Simulator you
  > must relax that check for `TARGET_OS_SIMULATOR` in your SDL build; author
  > that change yourself — per SDL's contribution policy it must not be
  > AI-generated. The plugin itself already calls `SDL_SetMainReady()` before
  > `SDL_Init` (required when embedding SDL, since Flutter owns `main`).
- **Android**: the NDK CMake (`android/src/main/cpp/CMakeLists.txt`) builds the
  engine + C bridge + JNI into one `libtessera.so`. It needs SDL3 source for
  Android — set `SDL3_SOURCE_DIR` (default `third_party/SDL`). The SPIR-V
  shaders are bundled in `android/src/main/assets/shaders/` and extracted to
  files storage at runtime. The main integration risk is bringing up SDL_GPU
  headlessly inside a Flutter `Activity` (SDL on Android is usually the activity
  owner); the render loop blits offscreen frames into the view's `Surface`.

## Status & what is verified

**Verified:**

- `flutter analyze` clean (plugin + example).
- **macOS** builds and runs end-to-end (renders the chess board live).
- The C bridge **compiles and links** against `libtessera` + SDL3.
- The **SPIR-V shaders render correctly on Vulkan** (validated via MoltenVK,
  matching the Metal output) — so the Android render path is proven at the
  engine/shader level.
- The chess rules/model logic is the same code unit-tested in the `tessera`
  package (reproduces the C reference game move-for-move).

**Not verified here** (no iOS/Android toolchain or device/emulator in the
authoring environment): the iOS Swift/UIKit glue, the Android Kotlin/JNI/NDK
build, and cross-compiling `libtessera` + SDL3 for those platforms. These are
written to the standard platform APIs as reference implementations; expect to
shake out issues on the first on-device build.

## Example

`example/` is a full game of chess — a live `TesseraView` board, a heuristic AI
playing both sides (autoplay), and tap-to-move via `TesseraController.pick`.
Ported from the C `examples/chess`.

```sh
cd example
flutter run -d macos
```

## License

MIT — see `LICENSE`.
