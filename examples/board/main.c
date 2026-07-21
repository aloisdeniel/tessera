/* examples/board — exercise state, the layout solver, and transitions.
 *
 * Renders three captures:
 *   board_settled.png — a board with several tile types + stacked entities
 *   board_move.png     — mid-animation, one entity hopping to a new tile
 *   board_reflow.png   — after the move settles (source/dest tiles reflowed)
 */
#include "tessera.h"
#include <stdio.h>
#include <string.h>

static void logfn(void* ud, int level, const char* msg) {
    (void)ud; (void)level; fprintf(stderr, "  %s\n", msg);
}

/* advance the engine by `secs` in small steps (promotes + animates) */
static void settle(TesseraEngine* e, double secs) {
    const double step = 1.0 / 120.0;
    for (double t = 0; t < secs; t += step) tessera_tick(e, step);
}

int main(int argc, char** argv) {
    const char* dir = (argc > 1) ? argv[1] : ".";
    char path[512];

    TesseraConfig cfg = { .width = 1280, .height = 720, .pixel_density = 1.0f,
                          .debug = false, .log = logfn };
    TesseraEngine* e = tessera_create(&cfg);
    if (!e || strlen(tessera_last_error(e))) {
        fprintf(stderr, "init failed: %s\n", e ? tessera_last_error(e) : "null");
        return 1;
    }

    /* --- definitions --- */
    TesseraTileDef grass = { .thickness = 0.25f, .tint = {0.45f, 0.72f, 0.40f, 1.0f} };
    TesseraTileDef stone = { .thickness = 0.25f, .tint = {0.62f, 0.62f, 0.66f, 1.0f} };
    TesseraTileDef water = { .thickness = 0.18f, .tint = {0.30f, 0.55f, 0.85f, 1.0f} };
    TesseraDefId d_grass = tessera_register_tile_def(e, &grass);
    TesseraDefId d_stone = tessera_register_tile_def(e, &stone);
    TesseraDefId d_water = tessera_register_tile_def(e, &water);

    TesseraEntityDef unit = { .scale = 1.0f };
    TesseraDefId d_unit = tessera_register_entity_def(e, &unit);

    /* --- board: 5x5, a water strip, a couple holes --- */
    TesseraTilePlacement tiles[25];
    size_t nt = 0;
    for (int z = -2; z <= 2; ++z) for (int x = -2; x <= 2; ++x) {
        if (x == 2 && z == 0) continue;             /* a hole */
        TesseraDefId d = (z == -2) ? d_water : ((x + z) & 1 ? d_stone : d_grass);
        tiles[nt++] = (TesseraTilePlacement){ .coord = {x, z}, .tile_def = d };
    }

    /* --- entities: three stacked on (0,0) to show the layout solver, plus two others --- */
    TesseraEntityPlacement ents[] = {
        { .id = 1, .def = d_unit, .coord = {0, 0} },
        { .id = 2, .def = d_unit, .coord = {0, 0} },
        { .id = 3, .def = d_unit, .coord = {0, 0} },
        { .id = 4, .def = d_unit, .coord = {-2, 2} },
        { .id = 5, .def = d_unit, .coord = {2, -2} },
    };
    TesseraCamera cam = { .focus = {0, 0}, .distance = 9.0f, .yaw = 0.7f,
                          .pitch = 0.65f, .fov = 0.9f };
    TesseraState s0 = {
        .tiles = tiles, .tile_count = nt,
        .entities = ents, .entity_count = 5,
        .camera = cam, .epoch = 1,
    };
    tessera_set_state(e, &s0);
    settle(e, 1.5);
    snprintf(path, sizeof path, "%s/board_settled.png", dir);
    printf("settled: %s -> %s\n", tessera_capture_png(e, 1280, 720, path) ? "OK" : "FAIL", path);

    /* --- move entity 1 off the stack to an empty tile: hop + reflow --- */
    ents[0].coord = (TesseraCoord){2, 2};
    TesseraState s1 = s0; s1.epoch = 2;
    tessera_set_state(e, &s1);
    settle(e, 0.14);   /* catch it mid-hop */
    snprintf(path, sizeof path, "%s/board_move.png", dir);
    printf("mid-move: %s -> %s\n", tessera_capture_png(e, 1280, 720, path) ? "OK" : "FAIL", path);

    settle(e, 1.2);    /* let it and the reflow settle */
    snprintf(path, sizeof path, "%s/board_reflow.png", dir);
    printf("reflowed: %s -> %s idle=%d\n",
           tessera_capture_png(e, 1280, 720, path) ? "OK" : "FAIL", path, tessera_is_idle(e));

    tessera_destroy(e);
    return 0;
}
