/*
 * tessera_web.c — Emscripten/WebGPU embedding shim.
 *
 * Compiled only for the web build (see tools/web/build_web.sh). The JS/Dart
 * host drives the engine through the regular tessera_* C ABI; this file adds
 * the few pieces that need to happen inside the wasm module: pointing SDL at
 * the host canvas, pointing the shader loader at the MEMFS-embedded assets,
 * and routing engine logs to the browser console.
 *
 * All exports are called through Emscripten ccall with ASYNCIFY enabled — the
 * WebGPU backend suspends during device creation and swapchain waits, so the
 * host must serialize calls into the module (see the Dart TesseraWasm wrapper).
 */
#include <emscripten/emscripten.h>
#include <stdio.h>

#include <SDL3/SDL.h>

#include "tessera.h"

/* Shaders are embedded at this virtual path by build_web.sh (--embed-file). */
#define TESSERA_WEB_ASSET_DIR "/tessera_assets"

static void web_log(void* userdata, int level, const char* msg) {
    (void)userdata;
    if (level >= TESSERA_LOG_ERROR) {
        emscripten_log(EM_LOG_ERROR, "[tessera] %s", msg);
    } else if (level >= TESSERA_LOG_WARN) {
        emscripten_log(EM_LOG_WARN, "[tessera] %s", msg);
    } else {
        emscripten_log(EM_LOG_CONSOLE, "[tessera] %s", msg);
    }
}

/* Create an engine bound to the canvas identified by the CSS selector
 * (e.g. "#tessera-canvas"). The canvas element must already be in the DOM. */
EMSCRIPTEN_KEEPALIVE
TesseraEngine* tessera_web_create(const char* canvas_selector, int width,
                                  int height, float pixel_density, int debug) {
    SDL_SetHint(SDL_HINT_EMSCRIPTEN_CANVAS_SELECTOR, canvas_selector);
    tessera_set_asset_dir(TESSERA_WEB_ASSET_DIR);

    TesseraConfig cfg = {
        .native_window = NULL,
        .width = width,
        .height = height,
        .pixel_density = pixel_density,
        .engine_driven_loop = false,
        .debug = debug != 0,
        .log = web_log,
    };
    return tessera_create(&cfg);
}
