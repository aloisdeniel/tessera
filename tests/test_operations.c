/* test_operations.c — set_state operation ids + completion events.
 *
 * tessera_set_state returns a monotonic, nonzero operation id; the transition
 * it triggers is "complete" once the engine has promoted it and the resulting
 * animation has fully settled. Covers: ids increase and are nonzero; an op is
 * not complete until the animation drains; last-completed tracks the highest id;
 * the completion callback fires once per settle with the right id; a superseded
 * (never-promoted) op completes when its successor does; op 0 is always done.
 */
#include "tessera.h"
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

/* Advance until idle (or a cap); returns ticks consumed. */
static int settle(TesseraEngine* e, unsigned char* buf) {
    int steps = 0;
    while (!tessera_is_idle(e) && steps < 600) { tick(e, buf); steps++; }
    CHECK(steps < 600);
    return steps;
}

/* Callback: record the last completed id + a call counter (via user ptr). */
static TesseraOpId g_last_cb = 0;
static void op_cb(TesseraOpId op, void* user) {
    g_last_cb = op;
    if (user) *(int*)user += 1;
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

    TesseraTiming tm = { .move_s = 0.3f, .add_s = 0.2f, .remove_s = 0.2f,
                         .tile_s = 0.2f, .reflow_s = 0.2f, .camera_s = 0.2f,
                         .speed_multiplier = 1.0f };
    tessera_set_timing(e, &tm);

    int cb_calls = 0;
    tessera_set_operation_callback(e, op_cb, &cb_calls);

    TesseraDefId unit = tessera_register_entity_def(e, &(TesseraEntityDef){ .scale = 1.0f });
    CHECK(unit != 0);
    TesseraCamera cam = { .focus = {0, 0}, .distance = 14.0f, .pitch = 0.9f, .fov = 0.9f };

    /* ---- nothing set yet ---- */
    CHECK(tessera_last_completed_operation(e) == 0);
    CHECK(tessera_operation_completed(e, 0));       /* op 0 always done */
    CHECK(!tessera_operation_completed(e, 1));      /* nothing completed yet */

    /* ---- op1: seed an entity (a spawn animation, so not instant) ---- */
    TesseraEntityPlacement seed = { .id = 1, .def = unit, .coord = {0, 0} };
    TesseraState s0 = { .entities = &seed, .entity_count = 1, .camera = cam, .epoch = 1 };
    TesseraOpId op1 = tessera_set_state(e, &s0);
    CHECK(op1 != 0);
    CHECK(!tessera_operation_completed(e, op1));     /* pending, not promoted */
    CHECK(tessera_last_completed_operation(e) == 0);

    settle(e, buf);
    CHECK(tessera_operation_completed(e, op1));
    CHECK(tessera_last_completed_operation(e) == op1);
    CHECK(g_last_cb == op1);
    CHECK(cb_calls >= 1);

    /* ---- op2: move the entity — ids increase, op2 not done mid-flight ---- */
    TesseraEntityPlacement mv = { .id = 1, .def = unit, .coord = {4, 0} };
    TesseraState s1 = { .entities = &mv, .entity_count = 1, .camera = cam, .epoch = 2 };
    TesseraOpId op2 = tessera_set_state(e, &s1);
    CHECK(op2 > op1);
    tick(e, buf);                                    /* promote -> tween starts */
    CHECK(!tessera_is_idle(e));
    CHECK(!tessera_operation_completed(e, op2));
    CHECK(tessera_operation_completed(e, op1));      /* still done */

    int before = cb_calls;
    settle(e, buf);
    CHECK(tessera_operation_completed(e, op2));
    CHECK(tessera_last_completed_operation(e) == op2);
    CHECK(g_last_cb == op2);
    CHECK(cb_calls == before + 1);                   /* exactly one settle */

    /* ---- supersede: two set_states before a tick. Only the latest promotes;
     *      completing it marks the earlier one complete too (monotonic). ---- */
    TesseraEntityPlacement a = { .id = 1, .def = unit, .coord = {0, 0} };
    TesseraState sa = { .entities = &a, .entity_count = 1, .camera = cam, .epoch = 3 };
    TesseraOpId opA = tessera_set_state(e, &sa);
    TesseraEntityPlacement b = { .id = 1, .def = unit, .coord = {0, 4} };
    TesseraState sb = { .entities = &b, .entity_count = 1, .camera = cam, .epoch = 4 };
    TesseraOpId opB = tessera_set_state(e, &sb);
    CHECK(opB > opA);

    settle(e, buf);
    CHECK(tessera_operation_completed(e, opA));       /* superseded, still done */
    CHECK(tessera_operation_completed(e, opB));
    CHECK(tessera_last_completed_operation(e) == opB);
    CHECK(g_last_cb == opB);

    /* ---- clearing the callback stops events but not the id bookkeeping ---- */
    tessera_set_operation_callback(e, NULL, NULL);
    int quiet = cb_calls;
    TesseraEntityPlacement c = { .id = 1, .def = unit, .coord = {2, 2} };
    TesseraState sc = { .entities = &c, .entity_count = 1, .camera = cam, .epoch = 5 };
    TesseraOpId opC = tessera_set_state(e, &sc);
    settle(e, buf);
    CHECK(cb_calls == quiet);                         /* no callback fired */
    CHECK(tessera_operation_completed(e, opC));
    CHECK(tessera_last_completed_operation(e) == opC);

    tessera_destroy(e);
    printf(g_fail ? "test_operations: %d FAIL\n" : "test_operations: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
