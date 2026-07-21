# Milestone 8 — Cross-platform integration & FFI bindings

**Goal:** Ship the same core on **macOS, Linux, iOS, Android**, finalize the
threading/loop model on each, and prove the FFI boundary by driving a demo from
**both Lua and Dart**. This is where "attach to a host-provided native view" and
"engine-driven loop vs host-driven tick" get made real per platform.

## Deliverables
- Native view attachment on all four platforms (`native_window` → SDL_GPU
  swapchain).
- Frame-pacing strategy per platform via SDL3 main-callbacks + engine tick clock.
- Build/packaging: macOS/Linux libs, iOS framework, Android `.so` + Gradle/CMake.
- FFI-friendliness audit of `tessera.h` (see checklist) + generated/hand-written
  bindings for **Dart (dart:ffi)** and **Lua (luajit/FFI or C module)**.
- Two working sample apps (Dart, Lua) rendering the board and transitions.

## Threading & loop model (final)
- **Desktop, engine-driven** (`engine_driven_loop=true`): engine runs its own
  loop/thread pumping `engine_tick(dt)`; host just calls `set_state`.
- **Mobile / embedded**: host (SDL app callbacks, or the OS view) drives frames;
  the engine exposes `tessera_tick(dt)` and the platform calls it once per
  display-link/Choreographer callback. Same internal `engine_tick`.
- `set_state` is always thread-safe (mutex-guarded pointer swap + off-lock deep
  copy), so callers on any thread are safe.
- Document which functions are main-thread-only (GPU calls) vs any-thread
  (`set_state`, `register_*` if guarded, `last_error`).

## Tasks
1. **View attachment** per platform:
   - macOS: `CAMetalLayer` / `NSView*` via SDL window properties.
   - iOS: `UIView`/`CAMetalLayer`; integrate with `CADisplayLink` via SDL.
   - Android: `ANativeWindow*` from a `SurfaceView`; drive via Choreographer; JNI
     bootstrap + asset access through AAssetManager (feed bytes to loaders).
   - Linux: X11/Wayland window (dev + CI).
2. **SDL main-callbacks**: provide a thin platform layer using
   `SDL_AppInit/Iterate/Event/Quit` so mobile pacing is handled by SDL; the engine
   core stays loop-agnostic.
3. **Asset delivery on mobile**: since loaders already take bytes (M2), wire
   Android AAssetManager / iOS bundle → `TesseraBytes`. No filesystem assumptions.
4. **Packaging**:
   - macOS/Linux: shared + static lib + `tessera.h`.
   - iOS: `.xcframework`.
   - Android: prebuilt `.so` per ABI + CMake package.
   - Shaders bundled as precompiled blobs per backend.
5. **FFI header audit** (checklist below) and freeze the ABI.
6. **Dart bindings** (`bindings/dart`): `dart:ffi` structs matching the flat state;
   a thin Dart wrapper class; example Flutter/standalone app pushing states.
7. **Lua bindings** (`bindings/lua`): LuaJIT FFI `cdef` of `tessera.h` (or a small
   C Lua module); example script building states/definitions.
8. **Sample game** (shared): a tiny turn-based scenario (move units, spawn effects,
   pan camera) implemented once in Lua and once in Dart against the same lib.

## FFI header checklist
- Pure C, `extern "C"`, no C++/inline/macros in the public surface.
- Only fixed-width types (`stdint.h`), `bool`, `float`, and pointers.
- Flat structs, explicit padding/alignment, documented layout; arrays as
  `(pointer, count)`.
- Opaque handles only across the boundary; no ownership of engine internals leaks.
- No required callbacks for basic use (logging callback optional); if callbacks are
  used, document threading + reentrancy.
- Stable struct layouts (append-only evolution; version field if needed).
- Every function's threading contract documented.

## Public API added
```c
// Explicit tick for host-driven platforms (already declared in M0; finalized here)
TESSERA_API void tessera_tick(TesseraEngine*, double dt_seconds);
// Optional: query engine capabilities / backend for host diagnostics
TESSERA_API const char* tessera_backend_name(TesseraEngine*);
```

## Acceptance criteria
- The same core renders the board on macOS, Linux, iOS, and Android.
- A Dart app and a Lua script both drive the identical scenario correctly.
- Native-view attachment works (no engine-owned window on mobile).
- ABI documented and frozen; bindings compile against the shipped header.

## Risks / notes
- Android surface lifecycle (surface created/destroyed on rotation/background) —
  handle swapchain recreation on `resize`/surface events.
- Struct alignment mismatches are the classic FFI bug — add a self-test that
  asserts `sizeof`/`offsetof` from both C and each binding.
