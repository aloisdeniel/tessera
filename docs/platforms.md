# Platforms, threading & the loop model

Tessera ships the same C core on **macOS, Linux, iOS, and Android**. Rendering
goes through SDL3's GPU API; on this build the active backend is Metal (query it
at runtime with `tessera_backend_name`). What differs per platform is how frames
are paced and how the native view is attached.

## Native-view attachment

`TesseraConfig.native_window` selects the surface:

- `native_window == NULL` — the engine creates and owns its own SDL window
  (desktop convenience).
- `native_window != NULL` — a host-provided native view is attached, and the
  engine owns no window. **In the current build this pointer is an
  `SDL_Window*`**; the platform layer is responsible for creating that window
  around the OS view:
  - **macOS** — `NSView` / `CAMetalLayer` behind the SDL window.
  - **iOS** — `UIView` / `CAMetalLayer`, driven from `CADisplayLink` via SDL.
  - **Android** — an `ANativeWindow*` from a `SurfaceView`, driven by
    Choreographer; JNI bootstrap and asset access go through `AAssetManager`,
    whose bytes are fed to the loaders as `TesseraBytes` (no filesystem needed).
  - **Linux** — an X11 or Wayland window (development + CI).

On mobile the OS surface can be created and destroyed on rotation or
backgrounding; recreate the swapchain by calling `tessera_resize` on surface
events.

## The two loop models

### Desktop, engine-driven (`engine_driven_loop = true`)

The engine runs its own frame loop and advances animation itself; the host only
pushes state with `tessera_set_state`. Convenient for desktop tools and the
example apps. This flag is ignored on mobile.

### Host-driven tick (mobile / embedded)

The host (SDL app callbacks, a `CADisplayLink`, or a `Choreographer` callback)
drives frames and calls once per display refresh:

```c
tessera_tick(engine, dt_seconds);   // promote pending state, advance tweens, render one frame
```

Both paths funnel into the same internal per-frame update, so behavior is
identical; only *who calls the clock* changes. `tessera_resize(e, w, h,
density)` adjusts the drawable on window/surface size changes.

## Threading contract

`tessera_set_state` is always thread-safe: it deep-copies the snapshot off-lock
and publishes it under a mutex, so any thread may call it and the caller may
free its arrays the instant the call returns. GPU-touching calls must run on the
render (main) thread.

| Function | Thread |
|---|---|
| `tessera_create` / `tessera_destroy` | render (main) thread |
| `tessera_tick` / `tessera_resize` | render thread (GPU) |
| `tessera_register_atlas` / `_tile_def` / `_entity_def` / `_effect_def` | render thread (GPU uploads) |
| `tessera_entity_def_anim_count` / `_anim_name` | render thread (reads def data) |
| `tessera_capture_png` | render thread (GPU) |
| `tessera__debug_orbit` | render thread |
| `tessera_set_state` | **any thread** (deep-copies, mutex-guarded) |
| `tessera_set_timing` / `tessera_set_light` / `tessera_set_quality` / `tessera_set_projection` / `tessera_set_focus` | render thread (unsynchronised POD writes read live by the tick) |
| `tessera_pick` | render thread (reads live scene + camera) |
| `tessera_is_idle` | any thread |
| `tessera_last_error` / `tessera_backend_name` | any thread |
| `tessera_version` / `tessera_version_string` | any thread (pure) |

The optional `TesseraConfig.log` callback may be invoked from whichever thread
generated the message; keep it reentrant and cheap.

## Quality & lighting per platform

`tessera_set_quality` tunes `shadows` (`NONE` / `BLOB` / `MAP`), `msaa`
(`1`/`2`/`4`), and `render_scale` (a sub-native resolution scale for mobile GPUs
— `1.0` = native). `tessera_set_light` sets the single directional light plus
ambient. Both take effect on the next frame. Because the tick reads these
structs live (no lock), call them from the render/tick thread — or, in an
engine-driven setup, before starting the loop — rather than from an arbitrary
thread; only `tessera_set_state` is safe to call concurrently with the tick.

## Packaging (intended)

- **macOS / Linux** — shared + static `libtessera` and `tessera.h`.
- **iOS** — `.xcframework`.
- **Android** — prebuilt `.so` per ABI plus a CMake package.
- Shaders are bundled as backend blobs (runtime-loaded MSL on this build).
