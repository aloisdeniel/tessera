# Milestone 0 — Scaffolding, build system, GPU device & window

**Goal:** A buildable library on macOS + Linux that opens a window (or attaches to
a supplied native view), initializes an SDL_GPU device, clears the screen every
frame, and exposes `tessera_create` / `tessera_destroy` through `tessera.h`.
This is the skeleton every later milestone hangs on.

## Deliverables
- CMake project producing `libtessera` (shared + static) and an `examples/hello`
  executable.
- Vendored dependencies wired up: SDL3, cglm, cgltf, stb_image (headers), and
  SDL_shadercross as a build-time tool.
- `include/tessera.h` with lifecycle API + opaque handle.
- A window that clears to a color and swaps at vsync on macOS (Metal) and Linux
  (Vulkan).
- CI that builds both platforms and runs a smoke test.

## Repo layout
```
tessera/
  include/tessera.h            # the one public header
  src/
    tessera.c                  # public API impl (thin)
    engine.c / engine.h        # TesseraEngine internals
    gpu/ gpu_device.c          # SDL_GPU device + swapchain
    platform/ window.c         # window vs attached-view creation
    core/ log.c mem.c handles.c
  third_party/                 # SDL3, cglm, cgltf, stb, shadercross
  assets/shaders/              # compiled shader blobs (generated)
  shaders/                     # HLSL sources
  examples/hello/
  tests/
  cmake/
  plan/
```

## Tasks
1. **CMake**: C11, warnings-as-errors, sanitizer option (`TESSERA_ASAN`),
   `BUILD_SHARED`/`BUILD_STATIC`. Vendor SDL3 via `add_subdirectory` or find_package.
2. **Public symbol visibility**: `TESSERA_API` macro (`__attribute__((visibility("default")))`
   / `__declspec(dllexport)`), `-fvisibility=hidden` by default. Extern "C" guards.
3. **Opaque handle + config**: define `TesseraConfig` and `TesseraEngine`.
   - `TesseraConfig { void* native_window; int width, height; float pixel_density;
     bool engine_driven_loop; TesseraLogFn log; void* log_userdata; }`
   - `native_window == NULL` → engine creates its own SDL_Window (desktop/dev).
     Non-NULL → attach to a host-provided view (mobile/embedding), wrapped via
     `SDL_CreatePropertiesID` + the platform window properties.
4. **GPU device**: `SDL_CreateGPUDevice(SPIRV|MSL, debug, NULL)`, claim the window
   for the swapchain, choose present mode (VSync), pick swapchain composition.
5. **Frame loop skeleton**: acquire command buffer → acquire swapchain texture →
   begin/end a render pass that just clears → submit. Wrap in `engine_tick`.
6. **Core services**: logging (routes to `TesseraLogFn` or stderr), a bump/arena
   allocator + a fixed-block pool, and a slot-map for `Def`/handle ids
   (generation-tagged to catch use-after-free).
7. **Error model**: per-engine last-error string; `tessera_last_error`.
8. **examples/hello**: create engine, pump SDL events, tick, destroy. Clears to a
   pulsing color.
9. **CI**: GitHub Actions matrix (macos, ubuntu). Build + run `hello --frames 3 --headless`.

## Public API added
```c
typedef struct TesseraEngine TesseraEngine;
typedef void (*TesseraLogFn)(void* userdata, int level, const char* msg);

typedef struct {
    void*  native_window;      // NULL => engine creates its own window
    int    width, height;
    float  pixel_density;      // 1.0 desktop, 2.0/3.0 retina/mobile
    bool   engine_driven_loop; // desktop convenience; ignored on mobile
    TesseraLogFn log; void* log_userdata;
} TesseraConfig;

TESSERA_API TesseraEngine* tessera_create(const TesseraConfig*);
TESSERA_API void           tessera_destroy(TesseraEngine*);
TESSERA_API void           tessera_resize(TesseraEngine*, int w, int h, float density);
TESSERA_API void           tessera_tick(TesseraEngine*, double dt_seconds);
TESSERA_API const char*    tessera_last_error(TesseraEngine*);
```

## Acceptance criteria
- `hello` opens a window that clears every frame at vsync on macOS + Linux.
- `tessera_create`/`tessera_destroy` leak-clean under ASan (no GPU objects left).
- Resizing the window keeps the swapchain valid.
- Headless smoke test returns 0 in CI on both platforms.

## Risks / notes
- SDL_GPU API churn — pin an SDL3 release commit and document it.
- Vulkan validation layers must be optional (dev only) so Linux CI without a GPU
  can still run headless (use an offscreen path or skip actual present under
  `--headless`).
