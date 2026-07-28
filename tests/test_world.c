/* test_world.c — point lights + world decoration models (state-driven).
 *
 * Covers: a point light visibly brightening the rendered frame, its fade-in /
 * fade-out driving is_idle, parameter tweening; a world model appearing in
 * the drawlist with its origin biased to the tiles' underside, growing in and
 * shrinking out, and transform tweening. Runs headless; reaches into the
 * orchestrator (via the internal header the test lib exposes) for the
 * animated values a public query can't observe. */
#include "tessera.h"
#include "engine.h"
#include "orchestration/orch.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

static void log_fn(void* ud, int level, const char* msg) {
    (void)ud;
    if (level >= TESSERA_LOG_ERROR) printf("[engine] %s\n", msg);
}

enum { W = 96, H = 96 };
static unsigned char g_buf[W * H * 4];

static void advance(TesseraEngine* e, int n) {
    for (int i = 0; i < n; ++i)
        tessera_render_rgba(e, 1.0 / 60.0, W, H, g_buf, sizeof g_buf);
}
static void settle(TesseraEngine* e) {
    int steps = 0;
    while (!tessera_is_idle(e) && steps < 400) { advance(e, 1); steps++; }
    CHECK(steps < 400);
}

/* Mean luma of the last rendered frame. */
static double frame_luma(void) {
    double sum = 0.0;
    for (int i = 0; i < W * H; ++i)
        sum += 0.2126 * g_buf[i * 4] + 0.7152 * g_buf[i * 4 + 1] +
               0.0722 * g_buf[i * 4 + 2];
    return sum / (W * H);
}

static TsPointLightInst* find_light(TesseraEngine* e, uint64_t id) {
    if (!e->orch) return NULL;
    for (size_t i = 0; i < e->orch->point_light_count; ++i)
        if (e->orch->point_lights[i].id == id) return &e->orch->point_lights[i];
    return NULL;
}

static TsWorldModelInst* find_model(TesseraEngine* e, uint64_t id) {
    if (!e->orch) return NULL;
    for (size_t i = 0; i < e->orch->world_model_count; ++i)
        if (e->orch->world_models[i].id == id) return &e->orch->world_models[i];
    return NULL;
}

int main(void) {
    TesseraConfig cfg = { .width = W, .height = H, .pixel_density = 1.0f, .log = log_fn };
    TesseraEngine* e = tessera_create(&cfg);
    if (!e) { printf("create NULL\n"); return 1; }
    if (strcmp(tessera_backend_name(e), "none") == 0) {
        printf("SKIP: no GPU backend (%s)\n", tessera_last_error(e));
        tessera_destroy(e);
        return 0;
    }

    TesseraTileDef td = {0};
    td.tint[0] = td.tint[1] = td.tint[2] = 0.5f; td.tint[3] = 1.0f;
    TesseraDefId tile = tessera_register_tile_def(e, &td);
    TesseraEntityDef ed = {0};   /* no glTF: the cube fallback mesh */
    ed.scale = 1.0f;
    TesseraDefId model = tessera_register_entity_def(e, &ed);
    CHECK(tile && model);

    /* Dim the sun so the point light's contribution dominates the delta. */
    TesseraLight sun = { .dir = {-0.4f, -1.0f, -0.3f}, .color = {1, 1, 1},
                         .intensity = 0.1f, .ambient = {0.10f, 0.10f, 0.10f} };
    tessera_set_light(e, &sun);

    TesseraTilePlacement tiles[9];
    memset(tiles, 0, sizeof tiles);
    int ti = 0;
    for (int z = -1; z <= 1; ++z)
        for (int x = -1; x <= 1; ++x) {
            tiles[ti].coord.x = x; tiles[ti].coord.y = z;
            tiles[ti].tile_def = tile; ti++;
        }

    TesseraState st = {0};
    st.tiles = tiles; st.tile_count = 9;
    st.camera.distance = 6.0f; st.camera.pitch = 1.2f; st.camera.fov = 0.8f;

    /* ---- baseline: no point lights ---- */
    tessera_set_state(e, &st);
    settle(e);
    advance(e, 1);
    double dark = frame_luma();

    /* ---- a bright warm light above the board must brighten the frame ---- */
    TesseraPointLightPlacement light = {
        .id = 7, .position = {0.0f, 1.5f, 0.0f},
        .color = {1.0f, 0.9f, 0.7f}, .intensity = 3.0f, .radius = 8.0f };
    st.point_lights = &light; st.point_light_count = 1;
    tessera_set_state(e, &st);
    advance(e, 1);                    /* promoted: the fade-in is underway */
    CHECK(!tessera_is_idle(e));       /* the spawn fade keeps the engine busy */
    TsPointLightInst* li = find_light(e, 7);
    CHECK(li != NULL);
    CHECK(li && li->intensity < 3.0f); /* still fading up */
    settle(e);
    li = find_light(e, 7);
    CHECK(li && fabsf(li->intensity - 3.0f) < 0.01f);
    CHECK(li && fabsf(li->radius - 8.0f) < 0.01f);
    advance(e, 1);
    double lit = frame_luma();
    CHECK(lit > dark + 3.0);          /* clearly brighter under the lamp */

    /* ---- parameter changes tween from the current values ---- */
    light.position[0] = 2.0f;
    light.intensity = 1.0f;
    tessera_set_state(e, &st);
    advance(e, 4);
    li = find_light(e, 7);
    CHECK(li && li->pos[0] > 0.0f && li->pos[0] < 2.0f);   /* mid-glide */
    settle(e);
    li = find_light(e, 7);
    CHECK(li && fabsf(li->pos[0] - 2.0f) < 0.01f);
    CHECK(li && fabsf(li->intensity - 1.0f) < 0.01f);

    /* radius <= 0 selects the default falloff */
    light.radius = 0.0f;
    tessera_set_state(e, &st);
    settle(e);
    li = find_light(e, 7);
    CHECK(li && li->radius > 5.9f && li->radius < 6.1f);

    /* ---- removing the light dims out, then culls ---- */
    st.point_lights = NULL; st.point_light_count = 0;
    tessera_set_state(e, &st);
    advance(e, 1);
    CHECK(!tessera_is_idle(e));
    settle(e);
    CHECK(find_light(e, 7) == NULL);
    advance(e, 1);
    CHECK(fabs(frame_luma() - dark) < 2.0);   /* back to the unlit look */

    /* ---- world model: spawns below the tiles, grows in ---- */
    TesseraWorldModelPlacement wm = {
        .id = 11, .def = model, .position = {1.5f, 0.0f, -1.0f},
        .orientation = {0, 0, 0, 1}, .scale = 2.0f };
    st.world_models = &wm; st.world_model_count = 1;
    tessera_set_state(e, &st);
    advance(e, 1);
    TsWorldModelInst* mi = find_model(e, 11);
    CHECK(mi != NULL);
    CHECK(mi && mi->scale < 2.0f);            /* still growing in */
    CHECK(!tessera_is_idle(e));
    settle(e);
    mi = find_model(e, 11);
    CHECK(mi && fabsf(mi->scale - 2.0f) < 0.01f);
    /* the origin plane sits just below the tiles (tile underside = -0.25) */
    CHECK(mi && fabsf(mi->pos[1] - (-0.25f)) < 1e-4f);
    CHECK(mi && fabsf(mi->pos[0] - 1.5f) < 1e-4f);

    /* transform change tweens */
    wm.position[0] = -1.5f;
    wm.position[1] = -0.5f;   /* sink deeper below the board */
    tessera_set_state(e, &st);
    advance(e, 4);
    mi = find_model(e, 11);
    CHECK(mi && mi->pos[0] < 1.5f && mi->pos[0] > -1.5f);  /* mid-glide */
    settle(e);
    mi = find_model(e, 11);
    CHECK(mi && fabsf(mi->pos[0] - (-1.5f)) < 1e-4f);
    CHECK(mi && fabsf(mi->pos[1] - (-0.75f)) < 1e-4f);     /* -0.5 - 0.25 */

    /* ---- removal shrinks out, then culls ---- */
    st.world_models = NULL; st.world_model_count = 0;
    tessera_set_state(e, &st);
    advance(e, 1);
    CHECK(!tessera_is_idle(e));
    mi = find_model(e, 11);
    CHECK(mi && mi->removing);
    settle(e);
    CHECK(find_model(e, 11) == NULL);

    /* ---- serialize keeps both arrays (spot check through the blob) ---- */
    st.point_lights = &light; st.point_light_count = 1;
    st.world_models = &wm; st.world_model_count = 1;
    size_t n = tessera_state_serialize(&st, NULL, 0);
    uint8_t* blob = (uint8_t*)malloc(n);
    CHECK(tessera_state_serialize(&st, blob, n) == n);
    TesseraState* rt = tessera_state_deserialize(blob, n);
    CHECK(rt != NULL);
    if (rt) {
        CHECK(rt->point_light_count == 1 && rt->point_lights[0].id == 7);
        CHECK(rt->world_model_count == 1 && rt->world_models[0].id == 11);
        tessera_state_free(rt);
    }
    free(blob);

    tessera_destroy(e);
    printf(g_fail ? "test_world: %d FAILURES\n" : "test_world: OK\n", g_fail);
    return g_fail ? 1 : 0;
}
