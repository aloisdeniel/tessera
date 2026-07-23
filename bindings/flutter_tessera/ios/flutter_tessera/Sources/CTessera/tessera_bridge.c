/*
 * tessera_bridge.c — implementation of the Swift<->Tessera bridge.
 *
 * Owns SDL and a hidden window. The engine's swapchain targets that window's
 * CAMetalLayer; the plugin reparents the SDL-created metal view (found via
 * ftessera_native_window + ftessera_metal_view_tag) into the Flutter platform
 * view and drives ftessera_present, so frames scan out directly with no CPU
 * round-trip. ftessera_render_rgba (offscreen readback) is kept for headless /
 * capture use. See tessera_bridge.h.
 */
#include "tessera_bridge.h"

#include "tessera.h"
#define SDL_MAIN_HANDLED   /* we embed SDL; Flutter owns main() */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdlib.h>

struct FTessera {
    SDL_Window*    window;   /* hidden; the engine's swapchain target */
    TesseraEngine* engine;
};

/* Last failure reason from ftessera_create (diagnostics; process-global). */
static char g_create_error[512] = {0};
const char* ftessera_create_error(void) { return g_create_error; }
static void set_create_error(const char* s) {
    snprintf(g_create_error, sizeof g_create_error, "%s", s ? s : "");
}

FTessera* ftessera_create(int32_t w, int32_t h, float density, const char* asset_dir) {
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;
    if (density <= 0.0f) density = 1.0f;
    g_create_error[0] = 0;

    /* Point the engine at the bundled assets (shaders) before create, which is
     * where pipeline shaders are loaded. NULL keeps the compile-time default. */
    tessera_set_asset_dir(asset_dir);

    /* We are embedding SDL inside a host app (Flutter owns main), so tell SDL
     * that main is ready. Required on iOS — SDL_Init(VIDEO) fails otherwise. */
    SDL_SetMainReady();

    if (!SDL_WasInit(SDL_INIT_VIDEO)) {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            snprintf(g_create_error, sizeof g_create_error,
                     "SDL_Init(VIDEO): %s", SDL_GetError());
            return NULL;
        }
    }

    /* A hidden Metal window: the engine claims it for its GPU device/swapchain.
     * We never show it — instead the plugin reparents its CAMetalLayer-backed
     * metal view into the on-screen Flutter view and presents straight to that
     * swapchain (ftessera_present). The window's hidden state is irrelevant once
     * the layer is composited by the host view. */
    SDL_Window* win = SDL_CreateWindow(
        "tessera-embedded", w, h,
        SDL_WINDOW_HIDDEN | SDL_WINDOW_METAL | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!win) {
        snprintf(g_create_error, sizeof g_create_error,
                 "SDL_CreateWindow: %s", SDL_GetError());
        return NULL;
    }

    TesseraConfig cfg = {0};
    cfg.native_window = win;
    cfg.width = w;
    cfg.height = h;
    cfg.pixel_density = density;
    cfg.engine_driven_loop = false;
    cfg.debug = false;

    TesseraEngine* e = tessera_create(&cfg);
    if (!e) {
        set_create_error("tessera_create returned NULL");
        SDL_DestroyWindow(win);
        return NULL;
    }
    if (tessera_last_error(e)[0] != '\0') {
        set_create_error(tessera_last_error(e));
        tessera_destroy(e);
        SDL_DestroyWindow(win);
        return NULL;
    }

    FTessera* f = (FTessera*)calloc(1, sizeof *f);
    if (!f) { tessera_destroy(e); SDL_DestroyWindow(win); return NULL; }
    f->window = win;
    f->engine = e;
    return f;
}

uintptr_t ftessera_engine_handle(FTessera* f) {
    return f ? (uintptr_t)f->engine : 0;
}

bool ftessera_render_rgba(FTessera* f, double dt, int32_t w, int32_t h,
                          void* out_rgba, size_t out_size) {
    if (!f) return false;
    return tessera_render_rgba(f->engine, dt, w, h, out_rgba, out_size);
}

void ftessera_present(FTessera* f, double dt) {
    if (!f) return;
    /* Advance + render + present to the engine's swapchain (the reparented
     * metal view's CAMetalLayer). No CPU copy. */
    tessera_tick(f->engine, dt);
}

void* ftessera_native_window(FTessera* f) {
    if (!f || !f->window) return NULL;
    SDL_PropertiesID props = SDL_GetWindowProperties(f->window);
#if defined(SDL_PLATFORM_IOS) || defined(SDL_PLATFORM_TVOS)
    return SDL_GetPointerProperty(props, SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, NULL);
#else
    return SDL_GetPointerProperty(props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL);
#endif
}

int64_t ftessera_metal_view_tag(FTessera* f) {
    if (!f || !f->window) return 0;
    SDL_PropertiesID props = SDL_GetWindowProperties(f->window);
#if defined(SDL_PLATFORM_IOS) || defined(SDL_PLATFORM_TVOS)
    return (int64_t)SDL_GetNumberProperty(props, SDL_PROP_WINDOW_UIKIT_METAL_VIEW_TAG_NUMBER, 0);
#else
    return (int64_t)SDL_GetNumberProperty(props, SDL_PROP_WINDOW_COCOA_METAL_VIEW_TAG_NUMBER, 0);
#endif
}

void ftessera_resize(FTessera* f, int32_t w, int32_t h, float density) {
    if (!f) return;
    tessera_resize(f->engine, w, h, density);
}

bool ftessera_pick(FTessera* f, float x, float y, FTesseraPick* out) {
    if (!f || !out) return false;
    TesseraPick pk = {0};
    bool hit = tessera_pick(f->engine, x, y, &pk);
    out->hit_tile = pk.hit_tile ? 1 : 0;
    out->tile_x = pk.tile.x;
    out->tile_y = pk.tile.y;
    out->tile_distance = pk.tile_distance;
    out->hit_entity = pk.hit_entity ? 1 : 0;
    out->entity = pk.entity;
    out->entity_distance = pk.entity_distance;
    return hit;
}

const char* ftessera_last_error(FTessera* f) {
    return f ? tessera_last_error(f->engine) : "null bridge";
}

void ftessera_destroy(FTessera* f) {
    if (!f) return;
    if (f->engine) tessera_destroy(f->engine);
    if (f->window) SDL_DestroyWindow(f->window);
    free(f);
}
