/* test_multistep.c — multi-step entity moves.
 *
 * A placement may carry a `path` of waypoints; an entity whose coord changed
 * then walks *through* them (in one move_s, split across the steps) instead of
 * gliding straight to the destination. Covers: the walk visits intermediate
 * tiles in order, it still lands exactly on the destination, a multi-step move
 * takes the same wall-clock as a single move (so each hop is faster), and a
 * one-element path behaves like a plain single move. Reaches into the
 * orchestrator (internal header) to read the live interpolated transform. */
#include "tessera.h"
#include "engine.h"
#include "orchestration/orch.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

static void log_fn(void* ud, int level, const char* msg) {
    (void)ud;
    if (level >= TESSERA_LOG_ERROR) printf("[engine] %s\n", msg);
}

static void tick(TesseraEngine* e, unsigned char* buf) {
    tessera_render_rgba(e, 1.0 / 60.0, 64, 64, buf, 64 * 64 * 4);
}

static TsEntityInst* find_ent(TesseraEngine* e, TesseraEntityId id) {
    if (!e->orch) return NULL;
    for (size_t i = 0; i < e->orch->entity_count; ++i)
        if (e->orch->entities[i].id == id) return &e->orch->entities[i];
    return NULL;
}

/* Advance until idle (or a cap); returns the number of ticks consumed. */
static int settle(TesseraEngine* e, unsigned char* buf) {
    int steps = 0;
    while (!tessera_is_idle(e) && steps < 600) { tick(e, buf); steps++; }
    CHECK(steps < 600);
    return steps;
}

int main(void) {
    TesseraConfig cfg = { .width = 64, .height = 64, .pixel_density = 1.0f, .log = log_fn };
    TesseraEngine* e = tessera_create(&cfg);
    if (!e) { printf("create NULL\n"); return 1; }
    if (strcmp(tessera_backend_name(e), "none") == 0) {
        printf("SKIP: no GPU backend (%s)\n", tessera_last_error(e));
        tessera_destroy(e);
        return 0;
    }
    unsigned char buf[64 * 64 * 4];

    /* Deterministic timings; a generous move so the walk spans many ticks. */
    TesseraTiming tm = { .move_s = 0.6f, .add_s = 0.2f, .remove_s = 0.2f,
                         .tile_s = 0.2f, .reflow_s = 0.2f, .camera_s = 0.2f,
                         .speed_multiplier = 1.0f };
    tessera_set_timing(e, &tm);

    TesseraDefId unit = tessera_register_entity_def(e, &(TesseraEntityDef){ .scale = 1.0f });
    CHECK(unit != 0);

    TesseraCamera cam = { .focus = {0, 0}, .distance = 14.0f, .pitch = 0.9f, .fov = 0.9f };

    /* ---- seed: two entities at the origin column ---- */
    TesseraEntityPlacement seed[2] = {
        { .id = 1, .def = unit, .coord = {0, 0} },  /* will walk a path      */
        { .id = 2, .def = unit, .coord = {0, 4} },  /* single-move baseline  */
    };
    TesseraState s0 = { .entities = seed, .entity_count = 2, .camera = cam, .epoch = 1 };
    tessera_set_state(e, &s0);
    tick(e, buf);          /* promote (seed snaps) */
    CHECK(tessera_is_idle(e));

    /* ---- move: id 1 walks (0,0)->(1,0)->(2,0)->(3,0); id 2 glides straight
     *      (0,4)->(3,4) with NO path. Both cover 3 tiles. ---- */
    const TesseraCoord path1[3] = { {1, 0}, {2, 0}, {3, 0} };
    TesseraEntityPlacement mv[2] = {
        { .id = 1, .def = unit, .coord = {3, 0}, .path = path1, .path_count = 3 },
        { .id = 2, .def = unit, .coord = {3, 4} },
    };
    TesseraState s1 = { .entities = mv, .entity_count = 2, .camera = cam, .epoch = 2 };
    tessera_set_state(e, &s1);
    tick(e, buf);          /* promote (starts the tweens) */

    TsEntityInst* w = find_ent(e, 1);
    TsEntityInst* b = find_ent(e, 2);
    CHECK(w && b);
    /* the walker split the move into 3 segments; the baseline stayed single */
    CHECK(w && w->seg_count == 3);
    CHECK(b && b->seg_count == 1);

    /* Sample the walk: x must climb ~monotonically through the waypoints and
     * never overshoot the destination or backtrack past a passed waypoint. Also
     * track the peak hop height *within each segment* — every step must jump. */
    float prev_x = w ? w->pos[0] : 0.0f;
    bool  saw_mid = false;   /* observed somewhere strictly between tiles 0 and 3 */
    float seg_peak_y[3] = {0, 0, 0};
    int   guard = 0;
    while (!tessera_is_idle(e) && guard < 600) {
        tick(e, buf); guard++;
        w = find_ent(e, 1);
        if (!w) break;
        CHECK(w->pos[0] >= prev_x - 0.02f);        /* no backtracking (allow arc noise) */
        CHECK(w->pos[0] <= 3.0f + 0.02f);          /* never past the destination */
        CHECK(fabsf(w->pos[2]) < 0.02f);           /* stays in its row (z ~ 0) */
        if (w->pos[0] > 0.6f && w->pos[0] < 2.4f) saw_mid = true;
        if (w->seg_index < 3 && w->pos[1] > seg_peak_y[w->seg_index])
            seg_peak_y[w->seg_index] = w->pos[1];
        prev_x = w->pos[0];
    }
    CHECK(saw_mid);
    /* each of the 3 steps hopped clear of the ground (arc per segment) */
    for (int i = 0; i < 3; ++i) {
        if (seg_peak_y[i] <= 0.1f)
            printf("FAIL segment %d did not hop (peak y=%.3f)\n", i, seg_peak_y[i]);
        CHECK(seg_peak_y[i] > 0.1f);
    }

    /* landed exactly on the destination tile (world x=3, z=0) */
    w = find_ent(e, 1);
    CHECK(w && fabsf(w->pos[0] - 3.0f) < 0.01f && fabsf(w->pos[2]) < 0.01f);
    CHECK(w && w->seg_index + 1 == w->seg_count);

    /* ---- timing: a multi-step walk takes twice a single move (so each hop
     *      stays legible). Re-run each in isolation and compare tick counts. ---- */
    tessera_set_state(e, &s0);
    tick(e, buf);
    settle(e, buf);

    /* walk only (id 1) */
    TesseraEntityPlacement only_walk[2] = {
        { .id = 1, .def = unit, .coord = {3, 0}, .path = path1, .path_count = 3 },
        { .id = 2, .def = unit, .coord = {0, 4} },   /* unchanged */
    };
    TesseraState sw = { .entities = only_walk, .entity_count = 2, .camera = cam, .epoch = 3 };
    tessera_set_state(e, &sw);
    tick(e, buf);
    int walk_ticks = settle(e, buf);

    tessera_set_state(e, &s0);
    tick(e, buf);
    settle(e, buf);

    /* single move only (id 2) */
    TesseraEntityPlacement only_single[2] = {
        { .id = 1, .def = unit, .coord = {0, 0} },   /* unchanged */
        { .id = 2, .def = unit, .coord = {3, 4} },
    };
    TesseraState ss = { .entities = only_single, .entity_count = 2, .camera = cam, .epoch = 4 };
    tessera_set_state(e, &ss);
    tick(e, buf);
    int single_ticks = settle(e, buf);

    /* the walk runs for ~2x the single move (± a few ticks of quantisation) */
    CHECK(abs(walk_ticks - 2 * single_ticks) <= 3);

    /* ---- a one-element path is just a plain single move ---- */
    tessera_set_state(e, &s0);
    tick(e, buf);
    settle(e, buf);
    const TesseraCoord path_one[1] = { {2, 0} };
    TesseraEntityPlacement one[2] = {
        { .id = 1, .def = unit, .coord = {2, 0}, .path = path_one, .path_count = 1 },
        { .id = 2, .def = unit, .coord = {0, 4} },
    };
    TesseraState s_one = { .entities = one, .entity_count = 2, .camera = cam, .epoch = 5 };
    tessera_set_state(e, &s_one);
    tick(e, buf);
    w = find_ent(e, 1);
    CHECK(w && w->seg_count == 1);
    settle(e, buf);
    w = find_ent(e, 1);
    CHECK(w && fabsf(w->pos[0] - 2.0f) < 0.01f);

    tessera_destroy(e);
    printf(g_fail ? "test_multistep: %d FAILURES\n" : "test_multistep: OK\n", g_fail);
    return g_fail ? 1 : 0;
}
