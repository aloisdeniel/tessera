/*
 * tessera_bridge.h — a self-contained C bridge between the Flutter macOS plugin
 * (Swift) and the Tessera engine + SDL.
 *
 * The Swift side imports ONLY this header (via the CTessera module map): it has
 * no SDL or tessera.h dependency, just POD types. The implementation
 * (tessera_bridge.c) owns the SDL init, a hidden SDL window (so the engine never
 * shows a window of its own), and the engine lifecycle.
 *
 * Presentation is zero-copy: the plugin reparents the SDL-created metal view
 * (ftessera_native_window + ftessera_metal_view_tag) into the Flutter platform
 * view and calls ftessera_present, so the engine's swapchain scans out directly
 * into the on-screen view. ftessera_render_rgba (offscreen readback into a
 * caller RGBA buffer) remains for headless / capture use.
 *
 * All calls must run on the platform main thread (they touch the GPU), except
 * that the raw engine pointer returned by ftessera_engine() may be used from
 * Dart's UI isolate for the any-thread tessera_set_state / tessera_is_idle.
 */
#ifndef TESSERA_BRIDGE_H
#define TESSERA_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque bridge instance: owns the SDL window + the TesseraEngine. */
typedef struct FTessera FTessera;

/* Result of ftessera_pick (mirrors TesseraPick's reported fields). */
typedef struct {
    int32_t  hit_tile;       /* 0/1 */
    int32_t  tile_x, tile_y;
    float    tile_distance;
    int32_t  hit_entity;     /* 0/1 */
    uint64_t entity;
    float    entity_distance;
} FTesseraPick;

/* Create the engine bound to a hidden SDL window at (w,h) pixels, pixel density
 * `density`. `asset_dir` overrides where the engine loads its bundled shaders
 * (NULL/"" => the compile-time default; pass the app bundle / extracted-assets
 * path on iOS/Android). Returns NULL on failure (SDL init, window, or engine). */
FTessera* ftessera_create(int32_t w, int32_t h, float density, const char* asset_dir);

/* The underlying TesseraEngine* as an integer address, for Dart FFI
 * (Tessera.fromHandle). 0 if `f` is NULL. */
uintptr_t ftessera_engine_handle(FTessera* f);

/* Advance animation by dt and render one frame into out_rgba (RGBA8, w*h*4
 * bytes; out_size must be >= that). Offscreen readback; for headless / capture.
 * On-screen embedding uses ftessera_present instead. Returns false on failure. */
bool ftessera_render_rgba(FTessera* f, double dt, int32_t w, int32_t h,
                          void* out_rgba, size_t out_size);

/* Advance animation by dt, render, and present directly to the engine's
 * swapchain — the CAMetalLayer backing the reparented native metal view (see
 * ftessera_metal_view_tag). Zero-copy on-screen path; call once per display
 * refresh in place of ftessera_render_rgba + a blit. Main/render thread. */
void ftessera_present(FTessera* f, double dt);

/* The engine's native window for reparenting its GPU surface into the host
 * view: an NSWindow* on macOS, a UIWindow* on iOS, returned as an opaque
 * pointer (cast on the Swift side). NULL if unavailable. */
void* ftessera_native_window(FTessera* f);

/* Tag identifying the SDL-created metal view inside that native window's view
 * hierarchy (locate it with -viewWithTag:). The view is backed by the
 * CAMetalLayer the engine presents to; reparent it into the host platform view
 * for the zero-copy path. 0 if unavailable. */
int64_t ftessera_metal_view_tag(FTessera* f);

/* Update the drawable size on view resize. */
void ftessera_resize(FTessera* f, int32_t w, int32_t h, float density);

/* Ray-pick under a logical view pixel (top-left origin). Fills *out; returns
 * true if a tile or entity was hit. */
bool ftessera_pick(FTessera* f, float x, float y, FTesseraPick* out);

/* Last engine error string (never NULL; "" when none). Valid until the next
 * bridge call. */
const char* ftessera_last_error(FTessera* f);

/* Destroy the engine and the hidden SDL window. */
void ftessera_destroy(FTessera* f);

#ifdef __cplusplus
}
#endif

#endif /* TESSERA_BRIDGE_H */
