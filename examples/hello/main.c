/* examples/hello — open a window, clear + render the demo board, orbit. */
#include "tessera.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void logfn(void* ud, int level, const char* msg) {
    (void)ud;
    static const char* n[] = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR"};
    fprintf(stderr, "[%s] %s\n", (level >= 0 && level <= 4) ? n[level] : "?", msg);
}

int main(int argc, char** argv) {
    int max_frames = -1;      /* -1 = run until quit */
    bool headless = false;
    const char* shot = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) max_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--headless")) headless = true;
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
    }

    TesseraConfig cfg = {0};
    cfg.width = 1280;
    cfg.height = 720;
    cfg.pixel_density = 1.0f;
    cfg.debug = true;
    cfg.log = logfn;

    TesseraEngine* e = tessera_create(&cfg);
    if (!e) { fprintf(stderr, "tessera_create returned NULL\n"); return 1; }
    if (strlen(tessera_last_error(e)) > 0) {
        fprintf(stderr, "init error: %s\n", tessera_last_error(e));
        tessera_destroy(e);
        return 2;
    }
    fprintf(stderr, "backend: %s\n", tessera_backend_name(e));

    if (shot) {
        bool ok = tessera_capture_png(e, 1280, 720, shot);
        fprintf(stderr, "capture %s -> %s\n", ok ? "OK" : "FAILED", shot);
        if (!ok) fprintf(stderr, "  err: %s\n", tessera_last_error(e));
        tessera_destroy(e);
        return ok ? 0 : 3;
    }

    Uint64 prev = SDL_GetPerformanceCounter();
    double freq = (double)SDL_GetPerformanceFrequency();
    int frame = 0;
    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) running = false;
            if (ev.type == SDL_EVENT_KEY_DOWN) {
                if (ev.key.key == SDLK_ESCAPE) running = false;
                if (ev.key.key == SDLK_LEFT)  tessera__debug_orbit(e, -0.1f, 0, 0);
                if (ev.key.key == SDLK_RIGHT) tessera__debug_orbit(e,  0.1f, 0, 0);
                if (ev.key.key == SDLK_UP)    tessera__debug_orbit(e, 0, -0.1f, 0);
                if (ev.key.key == SDLK_DOWN)  tessera__debug_orbit(e, 0,  0.1f, 0);
            }
            if (ev.type == SDL_EVENT_WINDOW_RESIZED)
                tessera_resize(e, ev.window.data1, ev.window.data2, 1.0f);
        }

        Uint64 now = SDL_GetPerformanceCounter();
        double dt = (double)(now - prev) / freq;
        prev = now;
        /* slow auto-orbit so the demo is visibly 3D */
        tessera__debug_orbit(e, (float)dt * 0.3f, 0, 0);
        tessera_tick(e, dt);

        if (max_frames >= 0 && ++frame >= max_frames) running = false;
        if (headless) SDL_Delay(1);
    }

    fprintf(stderr, "rendered %d frames, backend=%s\n", frame, tessera_backend_name(e));
    tessera_destroy(e);
    return 0;
}
