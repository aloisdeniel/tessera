/* test_overlays.c — declarative tile-overlay decals (state-driven).
 *
 * Covers: fade-in on add / fade-out + cull on remove (diff keyed by coord),
 * the tint crossfade retargeting from the *current interpolated* tint on a
 * mid-flight re-push, the pulse modulating the built draw items without ever
 * keeping the engine from reporting idle, and a render proof: a frame with a
 * bright overlay must differ from the same settled frame without it. Runs
 * headless (render_rgba); reaches into the orchestrator via the internal
 * headers the test lib exposes, like test_cards.c. */
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

#define W 96
#define H 96

static void log_fn(void* ud, int level, const char* msg) {
    (void)ud;
    if (level >= TESSERA_LOG_ERROR) printf("[engine] %s\n", msg);
}

static TsOverlayInst* find_overlay(TesseraEngine* e, int x, int y) {
    if (!e->orch) return NULL;
    for (size_t i = 0; i < e->orch->overlay_count; ++i)
        if (e->orch->overlays[i].coord.x == x && e->orch->overlays[i].coord.y == y)
            return &e->orch->overlays[i];
    return NULL;
}

static void advance(TesseraEngine* e, unsigned char* buf, int n) {
    for (int i = 0; i < n; ++i) tessera_render_rgba(e, 1.0 / 60.0, W, H, buf, W * H * 4);
}
static void settle(TesseraEngine* e, unsigned char* buf) {
    int steps = 0;
    while (!tessera_is_idle(e) && steps < 400) { advance(e, buf, 1); steps++; }
    CHECK(steps < 400);
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
    static unsigned char buf[W * H * 4];
    static unsigned char base[W * H * 4];

    TesseraTileDef tdef = { .thickness = 0.25f, .tint = {0.35f, 0.4f, 0.5f, 1.0f} };
    TesseraDefId tile = tessera_register_tile_def(e, &tdef);
    CHECK(tile != 0);

    TesseraTilePlacement tiles[9];
    int ti = 0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            tiles[ti++] = (TesseraTilePlacement){ .coord = {x, y}, .tile_def = tile };

    TesseraState st = {
        .tiles = tiles, .tile_count = 9,
        .camera = { .focus = {0, 0}, .distance = 6.0f,
                    .yaw = 0.0f, .pitch = 1.2f, .fov = 0.9f },
    };

    /* ---- baseline: board without overlays, settled + captured ---- */
    tessera_set_state(e, &st);
    advance(e, buf, 1);
    settle(e, buf);
    memcpy(base, buf, sizeof base);

    /* ---- add two overlays: a green disc and a pulsing white ring ---- */
    TesseraOverlayPlacement ovls[2] = {
        { .coord = {0, 0}, .shape = TESSERA_OVERLAY_DISC,
          .tint = { 0.2f, 1.0f, 0.3f, 0.9f } },
        { .coord = {1, 1}, .shape = TESSERA_OVERLAY_RING,
          .tint = { 1.0f, 1.0f, 1.0f, 1.0f },
          .pulse_s = 0.5f, .pulse_alpha_min = 0.2f, .pulse_alpha_max = 1.0f },
    };
    st.overlays = ovls; st.overlay_count = 2;
    st.epoch++;
    tessera_set_state(e, &st);
    advance(e, buf, 2);                       /* promote + one fade step */

    TsOverlayInst* disc = find_overlay(e, 0, 0);
    TsOverlayInst* ring = find_overlay(e, 1, 1);
    CHECK(disc && ring);
    CHECK(disc && disc->alpha < 0.999f && !disc->removing);   /* fading in */
    CHECK(ring && ring->shape == TESSERA_OVERLAY_RING);

    settle(e, buf);
    disc = find_overlay(e, 0, 0);
    CHECK(disc && fabsf(disc->alpha - 1.0f) < 1e-3f);         /* fully in */

    /* the frame with overlays must differ from the settled baseline */
    advance(e, buf, 1);
    CHECK(memcmp(base, buf, sizeof base) != 0);

    /* ---- pulse: the built item's alpha breathes across ticks, yet the
     * engine still reports idle (the pulse never blocks completion) ---- */
    CHECK(tessera_is_idle(e));
    TsOverlayItem* items = NULL;
    size_t ni = ts_orch_build_overlays(e->orch, e, &e->frame_arena, &items);
    CHECK(ni == 2);
    float a0 = 0.0f;
    for (size_t i = 0; i < ni; ++i)
        if (items[i].shape == TESSERA_OVERLAY_RING) a0 = items[i].color[3];
    advance(e, buf, 8);                       /* ~0.13s into the 0.5s pulse */
    CHECK(tessera_is_idle(e));
    ni = ts_orch_build_overlays(e->orch, e, &e->frame_arena, &items);
    CHECK(ni == 2);
    float a1 = -1.0f;
    for (size_t i = 0; i < ni; ++i)
        if (items[i].shape == TESSERA_OVERLAY_RING) a1 = items[i].color[3];
    advance(e, buf, 5);                       /* third sample dodges symmetry */
    ni = ts_orch_build_overlays(e->orch, e, &e->frame_arena, &items);
    CHECK(ni == 2);
    float a2 = -1.0f;
    for (size_t i = 0; i < ni; ++i)
        if (items[i].shape == TESSERA_OVERLAY_RING) a2 = items[i].color[3];
    CHECK(fabsf(a1 - a0) > 0.01f || fabsf(a2 - a0) > 0.01f);  /* alpha pulsed */

    /* ---- tint change: crossfade retargets from the current tint ---- */
    ovls[0].tint[0] = 1.0f; ovls[0].tint[1] = 0.1f; ovls[0].tint[2] = 0.1f; /* -> red */
    st.epoch++;
    tessera_set_state(e, &st);
    advance(e, buf, 4);                       /* mid crossfade */
    disc = find_overlay(e, 0, 0);
    CHECK(disc && !tessera_is_idle(e));
    CHECK(disc && fabsf(disc->from_tint[1] - 1.0f) < 1e-3f);  /* from old green */
    CHECK(disc && fabsf(disc->to_tint[0] - 1.0f) < 1e-3f);    /* toward red */
    float mid_r = disc ? disc->tint[0] : 0.0f;
    float mid_g = disc ? disc->tint[1] : 0.0f;
    CHECK(mid_r > 0.2f && mid_r < 0.999f);                    /* between the two */

    /* re-push ANOTHER tint mid-flight: must retarget from the interpolated
     * value, not snap back to either endpoint */
    ovls[0].tint[0] = 0.1f; ovls[0].tint[1] = 0.1f; ovls[0].tint[2] = 1.0f; /* -> blue */
    st.epoch++;
    tessera_set_state(e, &st);
    advance(e, buf, 1);                       /* promote */
    disc = find_overlay(e, 0, 0);
    CHECK(disc && fabsf(disc->from_tint[0] - mid_r) < 0.15f); /* from ~the mid tint */
    CHECK(disc && fabsf(disc->from_tint[1] - mid_g) < 0.15f);
    settle(e, buf);
    disc = find_overlay(e, 0, 0);
    CHECK(disc && fabsf(disc->tint[2] - 1.0f) < 1e-3f);       /* arrived at blue */

    /* an unchanged re-push of a settled overlay must NOT hold the engine busy */
    st.epoch++;
    tessera_set_state(e, &st);
    advance(e, buf, 1);
    CHECK(tessera_is_idle(e));

    /* ---- remove: fade out then cull; frame returns to the baseline ---- */
    st.overlays = NULL; st.overlay_count = 0;
    st.epoch++;
    tessera_set_state(e, &st);
    advance(e, buf, 2);
    disc = find_overlay(e, 0, 0);
    CHECK(disc && disc->removing);            /* fading out, not yet culled */
    settle(e, buf);
    CHECK(e->orch->overlay_count == 0);
    advance(e, buf, 1);
    CHECK(memcmp(base, buf, sizeof base) == 0);   /* pixel-identical again */

    tessera_destroy(e);
    printf(g_fail ? "test_overlays: %d FAILED\n" : "test_overlays: OK\n", g_fail);
    return g_fail ? 1 : 0;
}
