/* examples/polish — camera transitions, cel lighting & blob shadows (M7).
 *
 * Headless captures:
 *   polish_overview.png  — the lit, shadowed board from an overview pose
 *   polish_gliding.png   — mid-glide as the camera moves to focus a corner
 *   polish_focused.png   — settled on the focused entity
 *   polish_noshadow.png  — same scene with shadows disabled (quality toggle)
 *
 * Run:  example_polish <out_dir>
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

    /* A warm key light + cool ambient reads well for the cel look. */
    TesseraLight light = { .dir = {-0.5f, -1.0f, -0.35f}, .color = {1.0f, 0.95f, 0.85f},
                           .intensity = 1.1f, .ambient = {0.26f, 0.30f, 0.40f} };
    tessera_set_light(e, &light);

    TesseraTiming t = { .move_s = 0.4f, .add_s = 0.35f, .remove_s = 0.3f, .tile_s = 0.35f,
                        .reflow_s = 0.35f, .camera_s = 0.9f, .speed_multiplier = 1.0f };
    tessera_set_timing(e, &t);

    TesseraDefId d_grass = tessera_register_tile_def(e, &(TesseraTileDef){ .thickness = 0.25f, .tint = {0.45f, 0.72f, 0.40f, 1} });
    TesseraDefId d_stone = tessera_register_tile_def(e, &(TesseraTileDef){ .thickness = 0.25f, .tint = {0.62f, 0.62f, 0.66f, 1} });
    TesseraDefId d_unit  = tessera_register_entity_def(e, &(TesseraEntityDef){ .scale = 1.0f });

    TesseraTilePlacement tiles[81]; size_t nt = 0;
    for (int z = -4; z <= 4; ++z) for (int x = -4; x <= 4; ++x)
        tiles[nt++] = (TesseraTilePlacement){ .coord = {x, z},
            .tile_def = ((x + z) & 1) ? d_stone : d_grass };

    TesseraEntityPlacement ents[] = {
        { .id = 1, .def = d_unit, .coord = {-3, -3} }, { .id = 2, .def = d_unit, .coord = {3, -3} },
        { .id = 3, .def = d_unit, .coord = {-3,  3} }, { .id = 4, .def = d_unit, .coord = {3,  3} },
        { .id = 5, .def = d_unit, .coord = { 0,  0} },
    };
    TesseraCamera overview = { .focus = {0, 0}, .distance = 15.0f, .yaw = 0.7f, .pitch = 0.62f, .fov = 0.8f };

    TesseraState s = { .tiles = tiles, .tile_count = nt, .entities = ents, .entity_count = 5,
                       .camera = overview, .epoch = 1 };
    tessera_set_state(e, &s);
    settle(e, 1.2);
    snprintf(path, sizeof path, "%s/polish_overview.png", dir);
    printf("overview: %s\n", tessera_capture_png(e, 1280, 720, path) ? "OK" : "FAIL");

    /* Glide the camera to focus a corner unit (board unchanged -> pure camera move). */
    s.camera = (TesseraCamera){ .focus = {-3, -3}, .distance = 7.0f, .yaw = 1.1f, .pitch = 0.4f, .fov = 0.8f };
    s.epoch = 2;
    tessera_set_state(e, &s);
    settle(e, 0.3);
    snprintf(path, sizeof path, "%s/polish_gliding.png", dir);
    printf("gliding:  %s (idle=%d)\n", tessera_capture_png(e, 1280, 720, path) ? "OK" : "FAIL", tessera_is_idle(e));
    settle(e, 1.0);
    snprintf(path, sizeof path, "%s/polish_focused.png", dir);
    printf("focused:  %s (idle=%d)\n", tessera_capture_png(e, 1280, 720, path) ? "OK" : "FAIL", tessera_is_idle(e));

    /* Toggle shadows off to show the quality control has a visible effect. */
    TesseraQuality q = { .shadows = TESSERA_SHADOW_NONE, .msaa = 1, .render_scale = 1.0f };
    tessera_set_quality(e, &q);
    settle(e, 0.05);
    snprintf(path, sizeof path, "%s/polish_noshadow.png", dir);
    printf("noshadow: %s\n", tessera_capture_png(e, 1280, 720, path) ? "OK" : "FAIL");

    tessera_destroy(e);
    return 0;
}
