/* test_highlights.c — selection outline & glow post pass (state-driven).
 *
 * Covers: fade-in on add / fade-out + cull on remove (diff keyed by
 * kind+target id), the color crossfade retargeting from the *current
 * interpolated* color on a mid-flight re-push, the pulse modulating the built
 * items without ever keeping the engine from reporting idle, a render proof
 * (frame with an outline differs from the settled baseline and returns
 * byte-identical after removal), the glow style, the highlight tracking a
 * moving entity, and coexistence with the depth-of-field post pass. Runs
 * headless (render_rgba); reaches into the orchestrator via the internal
 * headers the test lib exposes, like test_overlays.c. */
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

static TsHighlightInst* find_hl(TesseraEngine* e, uint32_t kind, uint64_t id) {
    if (!e->orch) return NULL;
    for (size_t i = 0; i < e->orch->highlight_count; ++i)
        if (e->orch->highlights[i].kind == kind &&
            e->orch->highlights[i].target_id == id)
            return &e->orch->highlights[i];
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
    TesseraDefId ent = tessera_register_entity_def(e, &(TesseraEntityDef){ .scale = 1.0f });
    CHECK(tile != 0 && ent != 0);

    TesseraTilePlacement tiles[9];
    int ti = 0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            tiles[ti] = (TesseraTilePlacement){ .coord = {x, y}, .tile_def = tile,
                                                .id = 501 + (TesseraTileId)ti };
            ti++;
        }
    TesseraEntityPlacement ents[1] = {
        { .id = 42, .def = ent, .coord = { -1, 0 } } };

    TesseraState st = {
        .tiles = tiles, .tile_count = 9,
        .entities = ents, .entity_count = 1,
        .camera = { .focus = {0, 0}, .distance = 6.0f,
                    .yaw = 0.0f, .pitch = 1.0f, .fov = 0.9f },
    };

    /* ---- baseline: scene without highlights, settled + captured ---- */
    tessera_set_state(e, &st);
    advance(e, buf, 1);
    settle(e, buf);
    memcpy(base, buf, sizeof base);

    /* ---- add: a pulsing green outline on the entity ---- */
    TesseraHighlightPlacement hls[1] = {
        { .target_id = 42, .kind = TESSERA_HIGHLIGHT_ENTITY,
          .style = TESSERA_HIGHLIGHT_OUTLINE,
          .color = { 0.2f, 1.0f, 0.3f, 1.0f }, .thickness = 3.0f,
          .pulse_s = 0.5f, .pulse_min = 0.3f, .pulse_max = 1.0f },
    };
    st.highlights = hls; st.highlight_count = 1;
    st.epoch++;
    tessera_set_state(e, &st);
    advance(e, buf, 2);                       /* promote + one fade step */

    TsHighlightInst* v = find_hl(e, TESSERA_HIGHLIGHT_ENTITY, 42);
    CHECK(v != NULL);
    CHECK(v && v->alpha < 0.999f && !v->removing);        /* fading in */
    CHECK(v && v->style == TESSERA_HIGHLIGHT_OUTLINE);

    settle(e, buf);
    v = find_hl(e, TESSERA_HIGHLIGHT_ENTITY, 42);
    CHECK(v && fabsf(v->alpha - 1.0f) < 1e-3f);           /* fully in */

    /* the frame with the outline must differ from the settled baseline */
    advance(e, buf, 1);
    CHECK(memcmp(base, buf, sizeof base) != 0);

    /* ---- pulse: the built item's intensity breathes across ticks, yet the
     * engine still reports idle (the pulse never blocks completion) ---- */
    CHECK(tessera_is_idle(e));
    TsHighlightItem* items = NULL;
    size_t ni = ts_orch_build_highlights(e->orch, &e->frame_arena, &items);
    CHECK(ni == 1);
    float i0 = ni ? items[0].intensity : 0.0f;
    advance(e, buf, 8);                       /* ~0.13s into the 0.5s pulse */
    CHECK(tessera_is_idle(e));
    ni = ts_orch_build_highlights(e->orch, &e->frame_arena, &items);
    CHECK(ni == 1);
    float i1 = ni ? items[0].intensity : -1.0f;
    advance(e, buf, 5);                       /* third sample dodges symmetry */
    ni = ts_orch_build_highlights(e->orch, &e->frame_arena, &items);
    CHECK(ni == 1);
    float i2 = ni ? items[0].intensity : -1.0f;
    CHECK(fabsf(i1 - i0) > 0.01f || fabsf(i2 - i0) > 0.01f);  /* pulsed */

    /* ---- the highlight keeps tracking the entity through a move: the mask
     * re-renders the live drawlist, so mid-move frames must differ from the
     * settled highlighted frame captured above ---- */
    static unsigned char hlframe[W * H * 4];
    memcpy(hlframe, buf, sizeof hlframe);
    ents[0].coord = (TesseraCoord){ 1, 0 };
    st.epoch++;
    tessera_set_state(e, &st);
    advance(e, buf, 4);                                   /* mid-move */
    CHECK(!tessera_is_idle(e));
    CHECK(memcmp(hlframe, buf, sizeof hlframe) != 0);
    settle(e, buf);

    /* ---- color change: crossfade retargets from the current color ---- */
    hls[0].color[0] = 1.0f; hls[0].color[1] = 0.1f; hls[0].color[2] = 0.1f; /* -> red */
    hls[0].pulse_s = 0.0f; hls[0].pulse_max = 0.0f;       /* steady from here on */
    st.epoch++;
    tessera_set_state(e, &st);
    advance(e, buf, 4);                       /* mid crossfade */
    v = find_hl(e, TESSERA_HIGHLIGHT_ENTITY, 42);
    CHECK(v && !tessera_is_idle(e));
    CHECK(v && fabsf(v->from_color[1] - 1.0f) < 1e-3f);   /* from old green */
    CHECK(v && fabsf(v->to_color[0] - 1.0f) < 1e-3f);     /* toward red */
    float mid_r = v ? v->color[0] : 0.0f;
    float mid_g = v ? v->color[1] : 0.0f;
    CHECK(mid_r > 0.2f && mid_r < 0.999f);                /* between the two */

    /* re-push ANOTHER color mid-flight: must retarget from the interpolated
     * value, not snap back to either endpoint */
    hls[0].color[0] = 0.1f; hls[0].color[1] = 0.1f; hls[0].color[2] = 1.0f; /* -> blue */
    st.epoch++;
    tessera_set_state(e, &st);
    advance(e, buf, 1);                       /* promote */
    v = find_hl(e, TESSERA_HIGHLIGHT_ENTITY, 42);
    CHECK(v && fabsf(v->from_color[0] - mid_r) < 0.15f);  /* from ~the mid color */
    CHECK(v && fabsf(v->from_color[1] - mid_g) < 0.15f);
    settle(e, buf);
    v = find_hl(e, TESSERA_HIGHLIGHT_ENTITY, 42);
    CHECK(v && fabsf(v->color[2] - 1.0f) < 1e-3f);        /* arrived at blue */

    /* an unchanged re-push of a settled highlight must NOT hold the engine busy */
    st.epoch++;
    tessera_set_state(e, &st);
    advance(e, buf, 1);
    CHECK(tessera_is_idle(e));

    /* ---- glow style + a tile target, coexisting with depth-of-field:
     * both post passes must run in one frame ---- */
    tessera_set_focus(e, &(TesseraFocus){ .enabled = true, .focus_distance = 6.0f,
                                          .focus_range = 0.5f, .blur_strength = 6.0f });
    settle(e, buf);
    advance(e, buf, 1);
    static unsigned char dofbase[W * H * 4];
    memcpy(dofbase, buf, sizeof dofbase);                 /* DoF + blue outline */

    TesseraHighlightPlacement hls2[2];
    hls2[0] = hls[0];
    hls2[1] = (TesseraHighlightPlacement){
        .target_id = 501, .kind = TESSERA_HIGHLIGHT_TILE,
        .style = TESSERA_HIGHLIGHT_GLOW,
        .color = { 1.0f, 0.6f, 0.1f, 0.9f }, .thickness = 8.0f };
    st.highlights = hls2; st.highlight_count = 2;
    st.epoch++;
    tessera_set_state(e, &st);
    settle(e, buf);
    v = find_hl(e, TESSERA_HIGHLIGHT_TILE, 501);
    CHECK(v && v->style == TESSERA_HIGHLIGHT_GLOW);
    advance(e, buf, 1);
    CHECK(memcmp(dofbase, buf, sizeof dofbase) != 0);     /* glow visible over DoF */
    tessera_set_focus(e, NULL);

    /* ---- remove: fade out then cull; frame returns to the baseline ---- */
    st.highlights = NULL; st.highlight_count = 0;
    st.epoch++;
    tessera_set_state(e, &st);
    advance(e, buf, 2);
    v = find_hl(e, TESSERA_HIGHLIGHT_ENTITY, 42);
    CHECK(v && v->removing);                  /* fading out, not yet culled */
    settle(e, buf);
    CHECK(e->orch->highlight_count == 0);

    /* re-settle at the entity's new tile for a fresh clean baseline compare:
     * the baseline was captured before the move, so re-push the original
     * coord and let everything settle back */
    ents[0].coord = (TesseraCoord){ -1, 0 };
    st.epoch++;
    tessera_set_state(e, &st);
    settle(e, buf);
    advance(e, buf, 1);
    CHECK(memcmp(base, buf, sizeof base) == 0);   /* pixel-identical again */

    tessera_destroy(e);
    printf(g_fail ? "test_highlights: %d FAILED\n" : "test_highlights: OK\n", g_fail);
    return g_fail ? 1 : 0;
}
