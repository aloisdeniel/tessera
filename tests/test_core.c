/* test_core.c — unit tests for foundation + pure-logic subsystems.
 * No GPU required; safe to run headless in CI. */
#include "core/core.h"
#include "scene/scene.h"
#include "anim/anim.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)
#define CHECKF(a, b) CHECK(fabsf((a) - (b)) < 1e-4f)

static void test_arena(void) {
    TsArena a; ts_arena_init(&a, 128);
    int* p = TS_ARENA_ARR(&a, int, 100);   /* forces a fresh oversized block */
    CHECK(p != NULL);
    for (int i = 0; i < 100; ++i) p[i] = i;
    CHECK(p[99] == 99);
    ts_arena_reset(&a);
    CHECK(a.total_used == 0);
    ts_arena_destroy(&a);
}

static void test_slotmap_generation(void) {
    TsSlotMap m; CHECK(ts_slotmap_init(&m, sizeof(int), 4));
    void* pa; TsHandle ha = ts_slotmap_alloc(&m, &pa);
    *(int*)pa = 42;
    CHECK(ts_slotmap_get(&m, ha) == pa);
    ts_slotmap_free(&m, ha);
    /* stale handle must be rejected */
    CHECK(ts_slotmap_get(&m, ha) == NULL);
    /* reused slot gets a new generation -> old handle still invalid */
    void* pb; TsHandle hb = ts_slotmap_alloc(&m, &pb);
    CHECK(hb != ha);
    CHECK(ts_slotmap_get(&m, hb) == pb);
    CHECK(ts_slotmap_get(&m, ha) == NULL);
    /* growth beyond capacity */
    for (int i = 0; i < 20; ++i) { void* p; ts_slotmap_alloc(&m, &p); }
    CHECK(m.count >= 20);
    ts_slotmap_destroy(&m);
}

static void test_layout_determinism(void) {
    TsLayout l1, l2;
    ts_layout_solve(1, &l1);
    CHECK(l1.count == 1);
    CHECKF(l1.offset[0][0], 0.0f);
    CHECKF(l1.offset[0][1], 0.0f);

    /* determinism: same n -> identical result */
    ts_layout_solve(5, &l1);
    ts_layout_solve(5, &l2);
    CHECK(l1.count == 5);
    for (uint32_t i = 0; i < 5; ++i) {
        CHECKF(l1.offset[i][0], l2.offset[i][0]);
        CHECKF(l1.offset[i][1], l2.offset[i][1]);
    }
    CHECK(l1.scale < 1.0f);           /* shrinks as n grows */

    /* larger groups shrink further */
    TsLayout l9; ts_layout_solve(9, &l9);
    CHECK(l9.scale <= l1.scale);
}

static void test_tween(void) {
    CHECKF(ts_ease(TS_EASE_LINEAR, 0.5f), 0.5f);
    CHECKF(ts_ease(TS_EASE_LINEAR, 0.0f), 0.0f);
    CHECKF(ts_ease(TS_EASE_LINEAR, 1.0f), 1.0f);
    /* eased curves pinned at endpoints */
    CHECKF(ts_ease(TS_EASE_IN_OUT_CUBIC, 0.0f), 0.0f);
    CHECKF(ts_ease(TS_EASE_IN_OUT_CUBIC, 1.0f), 1.0f);

    TsTween tw;
    ts_tween_start(&tw, 1.0f, 0.0f, TS_EASE_LINEAR);
    CHECK(!ts_tween_done(&tw));
    ts_tween_advance(&tw, 0.5f);
    CHECKF(ts_tween_value01(&tw), 0.5f);
    ts_tween_advance(&tw, 0.6f);
    CHECK(ts_tween_done(&tw));
    CHECKF(ts_tween_value01(&tw), 1.0f);

    /* arc peaks at 0.5 */
    CHECK(ts_arc(0.5f) > ts_arc(0.1f));
    CHECKF(ts_arc(0.0f), 0.0f);
    CHECKF(ts_arc(1.0f), 0.0f);
}

int main(void) {
    test_arena();
    test_slotmap_generation();
    test_layout_determinism();
    test_tween();
    if (g_fail == 0) { printf("all core tests passed\n"); return 0; }
    printf("%d checks failed\n", g_fail);
    return 1;
}
