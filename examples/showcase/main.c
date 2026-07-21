/* examples/showcase — a bigger board with predefined states you loop through.
 *
 *   SPACE : advance to the next predefined state (transitions animate live)
 *   C     : glide the camera to focus the next entity (loops around)
 *   R     : reset the camera to the state's overview pose
 *   arrows: orbit the camera manually
 *   ESC   : quit
 *
 * The states show off every transition: tiles rising/sinking as the board grows
 * and shrinks, entities hopping between tiles, stacking (layout solver) and
 * reflowing, and entities spawning / despawning.
 */
#include "tessera.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ board */
#define MAX_TILES 512
#define MAX_ENTS  32

static TesseraTilePlacement   g_tiles[MAX_TILES];
static TesseraEntityPlacement g_ents[MAX_ENTS];
static size_t g_tcount, g_ecount;
static TesseraCamera g_cam;

static TesseraDefId d_grass, d_stone, d_water, d_unit;

/* Fill a rectangular board [x0,x1] x [z0,z1] with a checker of grass/stone,
 * one water row, and an optional square hole. */
static size_t board_rect(int x0, int x1, int z0, int z1, int water_row,
                         int hole_x, int hole_z) {
    size_t n = 0;
    for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x) {
            if (x == hole_x && z == hole_z) continue;
            TesseraDefId d = (z == water_row) ? d_water
                           : (((x + z) & 1) ? d_stone : d_grass);
            g_tiles[n++] = (TesseraTilePlacement){ .coord = {x, z}, .tile_def = d };
        }
    return n;
}

static void put_ent(size_t i, TesseraEntityId id, int x, int z) {
    g_ents[i] = (TesseraEntityPlacement){ .id = id, .def = d_unit, .coord = {x, z} };
}

/* Build predefined state `idx` into the globals. Entities keep stable ids so
 * the diff animates them (move/reflow) instead of remove+add. */
#define NUM_STATES 4
static void build_state(int idx) {
    g_cam = (TesseraCamera){ .focus = {0, 0}, .distance = 16.0f,
                             .yaw = 0.7f, .pitch = 0.62f, .fov = 0.85f };
    switch (idx) {
    case 0: /* full 9x9 board, six units spread to the corners/edges */
        g_tcount = board_rect(-4, 4, -4, 4, -4, 99, 99);
        put_ent(0, 1, -4, -3); put_ent(1, 2, 4, -3);
        put_ent(2, 3, -4,  3); put_ent(3, 4, 4,  3);
        put_ent(4, 5, -2,  0); put_ent(5, 6, 2,  0);
        g_ecount = 6;
        break;
    case 1: /* everyone converges onto the centre (stacking + reflow), water strip */
        g_tcount = board_rect(-4, 4, -4, 4, 0, 99, 99);
        put_ent(0, 1, 0, 0); put_ent(1, 2, 0, 0); put_ent(2, 3, 0, 0);
        put_ent(3, 4, 0, 0); put_ent(4, 5, 0, 0); put_ent(5, 6, 0, 0);
        g_ecount = 6;
        break;
    case 2: /* board shrinks to 5x5 with a hole; two units despawn */
        g_tcount = board_rect(-2, 2, -2, 2, -2, 2, 2);
        put_ent(0, 1, -2, -1); put_ent(1, 2, 2, -1);
        put_ent(2, 3, -1,  1); put_ent(3, 4, 1,  1);
        g_ecount = 4;                       /* ids 5 and 6 removed -> despawn */
        break;
    case 3: /* board grows to a wide 11x7; units respawn and fan out */
        g_tcount = board_rect(-5, 5, -3, 3, 3, 99, 99);
        put_ent(0, 1, -5, -3); put_ent(1, 2, 5, -3);
        put_ent(2, 3, -5,  3); put_ent(3, 4, 5,  3);
        put_ent(4, 5,  0, -3); put_ent(5, 6, 0,  3);   /* ids 5,6 spawn back */
        g_ecount = 6;
        break;
    }
}

static void push_current(TesseraEngine* e) {
    TesseraState s = {
        .tiles = g_tiles, .tile_count = g_tcount,
        .entities = g_ents, .entity_count = g_ecount,
        .camera = g_cam, .epoch = (uint64_t)SDL_GetTicks(),
    };
    tessera_set_state(e, &s);
}

static void logfn(void* ud, int level, const char* msg) {
    (void)ud; (void)level; fprintf(stderr, "  %s\n", msg);
}

static void settle(TesseraEngine* e, double secs) {
    const double step = 1.0 / 120.0;
    for (double t = 0; t < secs; t += step) tessera_tick(e, step);
}

/* Headless verification: drive the same code paths as the key handlers and
 * write PNGs, so the example is checkable without a display. */
static int run_demo(TesseraEngine* e, const char* dir) {
    char p[512];
    build_state(0); push_current(e); settle(e, 1.6);
    snprintf(p, sizeof p, "%s/showcase_0_spread.png", dir);
    tessera_capture_png(e, 1280, 720, p); printf("wrote %s\n", p);

    build_state(1); push_current(e); settle(e, 0.22);   /* mid converge */
    snprintf(p, sizeof p, "%s/showcase_1_converging.png", dir);
    tessera_capture_png(e, 1280, 720, p); printf("wrote %s\n", p);
    settle(e, 1.6);
    snprintf(p, sizeof p, "%s/showcase_1_stacked.png", dir);
    tessera_capture_png(e, 1280, 720, p); printf("wrote %s\n", p);

    build_state(2); push_current(e); settle(e, 1.6);    /* shrink + despawn */
    snprintf(p, sizeof p, "%s/showcase_2_shrunk.png", dir);
    tessera_capture_png(e, 1280, 720, p); printf("wrote %s\n", p);

    /* camera focus on the first entity, then glide to another */
    g_cam.focus = g_ents[0].coord; g_cam.distance = 8.0f; push_current(e);
    settle(e, 0.35);
    snprintf(p, sizeof p, "%s/showcase_3_cam_gliding.png", dir);
    tessera_capture_png(e, 1280, 720, p); printf("wrote %s (idle=%d)\n", p, tessera_is_idle(e));
    settle(e, 1.0);
    snprintf(p, sizeof p, "%s/showcase_4_cam_focused.png", dir);
    tessera_capture_png(e, 1280, 720, p); printf("wrote %s (idle=%d)\n", p, tessera_is_idle(e));
    return 0;
}

int main(int argc, char** argv) {
    const char* demo_dir = NULL;
    for (int i = 1; i < argc; ++i)
        if (!strcmp(argv[i], "--demo") && i + 1 < argc) demo_dir = argv[++i];

    TesseraConfig cfg = { .width = 1280, .height = 720, .pixel_density = 1.0f,
                          .debug = false, .log = logfn };
    TesseraEngine* e = tessera_create(&cfg);
    if (!e || strlen(tessera_last_error(e))) {
        fprintf(stderr, "init failed: %s\n", e ? tessera_last_error(e) : "null");
        return 1;
    }

    /* a snappier feel + a slightly longer camera glide */
    TesseraTiming t = { .move_s = 0.45f, .add_s = 0.4f, .remove_s = 0.35f,
                        .tile_s = 0.4f, .reflow_s = 0.4f, .camera_s = 0.8f,
                        .speed_multiplier = 1.0f };
    tessera_set_timing(e, &t);

    d_grass = tessera_register_tile_def(e, &(TesseraTileDef){ .thickness = 0.25f, .tint = {0.45f, 0.72f, 0.40f, 1} });
    d_stone = tessera_register_tile_def(e, &(TesseraTileDef){ .thickness = 0.25f, .tint = {0.62f, 0.62f, 0.66f, 1} });
    d_water = tessera_register_tile_def(e, &(TesseraTileDef){ .thickness = 0.16f, .tint = {0.30f, 0.55f, 0.85f, 1} });
    d_unit  = tessera_register_entity_def(e, &(TesseraEntityDef){ .scale = 1.0f });

    if (demo_dir) {
        int rc = run_demo(e, demo_dir);
        tessera_destroy(e);
        return rc;
    }

    int state_idx = 0;
    int cam_ent = -1;     /* -1 = overview */
    build_state(state_idx);
    push_current(e);

    printf("SPACE = next state   C = focus next entity   R = overview   ESC = quit\n");

    Uint64 prev = SDL_GetPerformanceCounter();
    double freq = (double)SDL_GetPerformanceFrequency();
    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) running = false;
            else if (ev.type == SDL_EVENT_WINDOW_RESIZED)
                tessera_resize(e, ev.window.data1, ev.window.data2, 1.0f);
            else if (ev.type == SDL_EVENT_KEY_DOWN) {
                switch (ev.key.key) {
                case SDLK_ESCAPE: running = false; break;
                case SDLK_SPACE:
                    state_idx = (state_idx + 1) % NUM_STATES;
                    cam_ent = -1;
                    build_state(state_idx);
                    push_current(e);
                    printf("state %d  (%zu tiles, %zu entities)\n",
                           state_idx, g_tcount, g_ecount);
                    break;
                case SDLK_C:
                    if (g_ecount > 0) {
                        cam_ent = (cam_ent + 1) % (int)g_ecount;
                        TesseraCoord c = g_ents[cam_ent].coord;
                        g_cam.focus = c;
                        g_cam.distance = 8.0f;   /* zoom in on the unit */
                        push_current(e);         /* board unchanged -> camera glides */
                        printf("focus entity %llu at (%d,%d)\n",
                               (unsigned long long)g_ents[cam_ent].id, c.x, c.y);
                    }
                    break;
                case SDLK_R:
                    cam_ent = -1;
                    g_cam.focus = (TesseraCoord){0, 0};
                    g_cam.distance = 16.0f;
                    push_current(e);
                    break;
                case SDLK_LEFT:  tessera__debug_orbit(e, -0.12f, 0, 0); break;
                case SDLK_RIGHT: tessera__debug_orbit(e,  0.12f, 0, 0); break;
                case SDLK_UP:    tessera__debug_orbit(e, 0, -0.08f, 0); break;
                case SDLK_DOWN:  tessera__debug_orbit(e, 0,  0.08f, 0); break;
                default: break;
                }
            }
        }

        Uint64 now = SDL_GetPerformanceCounter();
        double dt = (double)(now - prev) / freq;
        prev = now;
        tessera_tick(e, dt);
    }

    tessera_destroy(e);
    return 0;
}
