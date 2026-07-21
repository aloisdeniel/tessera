/* examples/fx — effects & particles (M6).
 *
 * Headless captures:
 *   fx_burst.png   — an on_add burst firing where an effect was just added
 *   fx_aura.png    — a continuous aura attached to a moving entity
 *   fx_removed.png — an on_remove poof after the effect is dropped
 *
 * Run:  example_fx <out_dir>
 */
#include "tessera.h"
#include <stdio.h>
#include <string.h>

static void logfn(void* ud, int level, const char* msg) {
    (void)ud; (void)level; fprintf(stderr, "  %s\n", msg);
}
static void settle(TesseraEngine* e, double secs) {
    const double step = 1.0 / 120.0;
    for (double t = 0; t < secs; t += step) tessera_tick(e, step);
}

int main(int argc, char** argv) {
    const char* dir = (argc > 1) ? argv[1] : ".";
    char path[512];

    TesseraConfig cfg = { .width = 1280, .height = 720, .pixel_density = 1.0f, .log = logfn };
    TesseraEngine* e = tessera_create(&cfg);
    if (!e || strlen(tessera_last_error(e))) {
        fprintf(stderr, "init failed: %s\n", e ? tessera_last_error(e) : "null");
        return 1;
    }

    TesseraTileDef grass = { .thickness = 0.25f, .tint = {0.42f, 0.70f, 0.38f, 1} };
    TesseraTileDef stone = { .thickness = 0.25f, .tint = {0.60f, 0.60f, 0.64f, 1} };
    TesseraDefId d_grass = tessera_register_tile_def(e, &grass);
    TesseraDefId d_stone = tessera_register_tile_def(e, &stone);

    /* A bright additive spark burst for effect on_add. */
    TesseraParticleSpec spark_add = {
        .mode = TESSERA_EMIT_BURST, .count = 90,
        .lifetime_s = 0.8f, .lifetime_var = 0.25f,
        .speed = 3.2f, .speed_var = 1.0f, .spread_deg = 55.0f, .gravity = -3.5f,
        .size_start = 0.22f, .size_end = 0.02f,
        .color_start = {1.0f, 0.85f, 0.35f, 1.0f}, .color_end = {1.0f, 0.25f, 0.05f, 0.0f},
        .blend = TESSERA_BLEND_ADD, .duration_s = 0.0f,
    };
    /* A soft smoke poof for on_remove (alpha-blended). */
    TesseraParticleSpec smoke_remove = {
        .mode = TESSERA_EMIT_BURST, .count = 40,
        .lifetime_s = 1.1f, .lifetime_var = 0.3f,
        .speed = 1.4f, .speed_var = 0.5f, .spread_deg = 40.0f, .gravity = 0.6f,
        .size_start = 0.15f, .size_end = 0.6f,
        .color_start = {0.8f, 0.8f, 0.85f, 0.7f}, .color_end = {0.5f, 0.5f, 0.55f, 0.0f},
        .blend = TESSERA_BLEND_ALPHA, .duration_s = 0.0f,
    };
    TesseraEffectDef burst_def = { .on_add = spark_add, .on_remove = smoke_remove };
    TesseraDefId d_burst = tessera_register_effect_def(e, &burst_def);

    /* A continuous rising-mote aura for an attached effect. */
    TesseraParticleSpec aura = {
        .mode = TESSERA_EMIT_CONTINUOUS, .count = 40,   /* per second */
        .lifetime_s = 1.0f, .lifetime_var = 0.2f,
        .speed = 0.8f, .speed_var = 0.3f, .spread_deg = 25.0f, .gravity = 1.2f,
        .size_start = 0.10f, .size_end = 0.01f,
        .color_start = {0.4f, 0.9f, 1.0f, 1.0f}, .color_end = {0.2f, 0.5f, 1.0f, 0.0f},
        .blend = TESSERA_BLEND_ADD, .duration_s = 6.0f,
    };
    TesseraEffectDef aura_def = { .on_add = aura, .on_remove = smoke_remove };
    TesseraDefId d_aura = tessera_register_effect_def(e, &aura_def);

    TesseraEntityDef unit = { .scale = 1.0f };
    TesseraDefId d_unit = tessera_register_entity_def(e, &unit);

    /* board */
    TesseraTilePlacement tiles[49]; size_t nt = 0;
    for (int z = -3; z <= 3; ++z) for (int x = -3; x <= 3; ++x)
        tiles[nt++] = (TesseraTilePlacement){ .coord = {x, z},
            .tile_def = ((x + z) & 1) ? d_stone : d_grass };

    TesseraEntityPlacement ents[] = {
        { .id = 1, .def = d_unit, .coord = {-2, 0} },
        { .id = 2, .def = d_unit, .coord = { 2, 0} },
    };
    TesseraCamera cam = { .focus = {0, 0}, .distance = 9.0f, .yaw = 0.7f, .pitch = 0.6f, .fov = 0.9f };

    /* State 0: settle the board, then add a burst effect on a tile + an aura on unit 1. */
    TesseraEffectPlacement fx0[] = {
        { .id = 100, .def = d_burst, .coord = {0, 0} },
        { .id = 101, .def = d_aura,  .coord = {-2, 0}, .attach_entity_id = 1 },
    };
    TesseraState s0 = { .tiles = tiles, .tile_count = nt, .entities = ents, .entity_count = 2,
                        .effects = fx0, .effect_count = 2, .camera = cam, .epoch = 1 };
    tessera_set_state(e, &s0);
    settle(e, 0.18);   /* catch the burst near its peak */
    snprintf(path, sizeof path, "%s/fx_burst.png", dir);
    printf("burst:  %s -> %s\n", tessera_capture_png(e, 1280, 720, path) ? "OK" : "FAIL", path);

    /* State 1: move unit 1 across the board; the aura (attached) follows it. */
    ents[0].coord = (TesseraCoord){2, 2};
    TesseraEffectPlacement fx1[] = { { .id = 101, .def = d_aura, .coord = {2, 2}, .attach_entity_id = 1 } };
    TesseraState s1 = { .tiles = tiles, .tile_count = nt, .entities = ents, .entity_count = 2,
                        .effects = fx1, .effect_count = 1, .camera = cam, .epoch = 2 };
    tessera_set_state(e, &s1);
    settle(e, 0.25);
    snprintf(path, sizeof path, "%s/fx_aura.png", dir);
    printf("aura:   %s -> %s\n", tessera_capture_png(e, 1280, 720, path) ? "OK" : "FAIL", path);

    /* State 2: drop the aura effect -> on_remove poof; board otherwise unchanged. */
    settle(e, 0.8);
    TesseraState s2 = { .tiles = tiles, .tile_count = nt, .entities = ents, .entity_count = 2,
                        .effects = NULL, .effect_count = 0, .camera = cam, .epoch = 3 };
    tessera_set_state(e, &s2);
    settle(e, 0.2);
    snprintf(path, sizeof path, "%s/fx_removed.png", dir);
    printf("remove: %s -> %s idle=%d\n",
           tessera_capture_png(e, 1280, 720, path) ? "OK" : "FAIL", path, tessera_is_idle(e));

    tessera_destroy(e);
    return 0;
}
