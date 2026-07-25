/* test_labels.c — world-anchored 3D text labels (state-driven).
 *
 * Covers: the snapshot round-trip of the new labels array (bounded text deep-
 * copied into the single bump-packed block, 16B-aligned), fade-in on add /
 * fade-out + cull on remove (diff keyed by id), the text-change crossfade
 * (old string fades out under the new one), the color crossfade retargeting
 * from the *current interpolated* color on a mid-flight re-push, an entity-
 * anchored label tracking the LIVE interpolated position mid-move, the idle
 * discipline (unchanged re-push snaps), and a render proof: a frame with a
 * label differs from the settled baseline and returns byte-identical after
 * removal. Needs a TrueType font; resolved from $TESSERA_FONT or the usual
 * system locations (skips with a message when none is found). */
#include "tessera.h"
#include "engine.h"
#include "orchestration/orch.h"
#include "state.h"
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

static const char* find_font(void) {
    static const char* candidates[] = {
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
        "/System/Library/Fonts/Helvetica.ttc",
        "/Library/Fonts/Arial.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    };
    const char* env = getenv("TESSERA_FONT");
    if (env && env[0]) return env;
    for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; ++i) {
        FILE* f = fopen(candidates[i], "rb");
        if (f) { fclose(f); return candidates[i]; }
    }
    return NULL;
}

static TsLabelInst* find_label(TesseraEngine* e, TesseraLabelId id) {
    if (!e->orch) return NULL;
    for (size_t i = 0; i < e->orch->label_count; ++i)
        if (e->orch->labels[i].id == id) return &e->orch->labels[i];
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

/* ---- snapshot round-trip (no GPU needed) ---- */
static void test_snapshot_roundtrip(TesseraDefId font) {
    TesseraLabelPlacement lbl = {
        .id = 7, .font = font, .anchor = TESSERA_LABEL_ANCHOR_ENTITY,
        .anchor_id = 42, .position = { 1.0f, 2.0f, 3.0f }, .size = 0.7f,
        .color = { 0.2f, 0.4f, 0.6f, 0.8f }, .billboard = true };
    snprintf(lbl.text, sizeof lbl.text, "HP 12/20 · über");
    TesseraState st = { .labels = &lbl, .label_count = 1 };

    TsSnapshot* s = ts_snapshot_copy(&st);
    CHECK(s && s->label_count == 1 && s->labels != NULL);
    if (s && s->labels) {
        CHECK(s->labels != &lbl);                       /* deep copy */
        CHECK(((uintptr_t)s->labels % _Alignof(TesseraLabelPlacement)) == 0);
        CHECK(s->labels[0].id == 7 && s->labels[0].anchor_id == 42);
        CHECK(strcmp(s->labels[0].text, lbl.text) == 0);
        CHECK(s->labels[0].billboard == true);
        CHECK(fabsf(s->labels[0].color[3] - 0.8f) < 1e-6f);
    }
    ts_snapshot_free(s);

    /* an unterminated text must come back NUL-terminated */
    memset(lbl.text, 'x', sizeof lbl.text);
    s = ts_snapshot_copy(&st);
    CHECK(s && s->labels && s->labels[0].text[TESSERA_LABEL_TEXT_CAP - 1] == 0);
    ts_snapshot_free(s);
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
    const char* font_path = find_font();
    if (!font_path) {
        printf("SKIP: no TrueType font found (set TESSERA_FONT)\n");
        tessera_destroy(e);
        return 0;
    }
    TesseraDefId font = tessera_register_font(
        e, &(TesseraBytes){ .path = font_path }, 48.0f);
    CHECK(font != 0);

    test_snapshot_roundtrip(font);

    static unsigned char buf[W * H * 4];
    static unsigned char base[W * H * 4];

    TesseraTileDef tdef = { .thickness = 0.25f, .tint = {0.35f, 0.4f, 0.5f, 1.0f} };
    TesseraDefId tile = tessera_register_tile_def(e, &tdef);
    TesseraDefId ent = tessera_register_entity_def(e, &(TesseraEntityDef){ .scale = 1.0f });
    CHECK(tile != 0 && ent != 0);

    TesseraTilePlacement tiles[9];
    int ti = 0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            tiles[ti++] = (TesseraTilePlacement){ .coord = {x, y}, .tile_def = tile };
    TesseraEntityPlacement ents[1] = {
        { .id = 42, .def = ent, .coord = { -1, 0 } } };

    TesseraState st = {
        .tiles = tiles, .tile_count = 9,
        .entities = ents, .entity_count = 1,
        .camera = { .focus = {0, 0}, .distance = 6.0f,
                    .yaw = 0.0f, .pitch = 1.0f, .fov = 0.9f },
    };

    /* ---- baseline: board without labels, settled + captured ---- */
    tessera_set_state(e, &st);
    advance(e, buf, 1);
    settle(e, buf);
    memcpy(base, buf, sizeof base);

    /* ---- add: a billboard world label + an entity-anchored label ---- */
    TesseraLabelPlacement lbls[2] = {
        { .id = 1, .font = font, .anchor = TESSERA_LABEL_ANCHOR_WORLD,
          .position = { 0.0f, 1.2f, 0.0f }, .size = 1.0f,
          .color = { 1.0f, 1.0f, 1.0f, 1.0f }, .billboard = true },
        { .id = 2, .font = font, .anchor = TESSERA_LABEL_ANCHOR_ENTITY,
          .anchor_id = 42, .position = { 0.0f, 1.5f, 0.0f }, .size = 0.6f,
          .color = { 1.0f, 0.9f, 0.2f, 1.0f }, .billboard = true },
    };
    snprintf(lbls[0].text, sizeof lbls[0].text, "SCORE 10");
    snprintf(lbls[1].text, sizeof lbls[1].text, "HP 5");
    st.labels = lbls; st.label_count = 2;
    st.epoch++;
    tessera_set_state(e, &st);
    advance(e, buf, 2);                       /* promote + one fade step */

    TsLabelInst* a = find_label(e, 1);
    TsLabelInst* b = find_label(e, 2);
    CHECK(a && b);
    CHECK(a && a->alpha < 0.999f && !a->removing);      /* fading in */
    CHECK(b && strcmp(b->text, "HP 5") == 0);
    settle(e, buf);
    a = find_label(e, 1);
    CHECK(a && fabsf(a->alpha - 1.0f) < 1e-3f);

    /* the frame with labels must differ from the settled baseline */
    advance(e, buf, 1);
    CHECK(memcmp(base, buf, sizeof base) != 0);

    /* ---- entity-anchored label tracks the LIVE interpolated position ---- */
    ents[0].coord = (TesseraCoord){ 1, 0 };             /* move the entity */
    st.epoch++;
    tessera_set_state(e, &st);
    advance(e, buf, 4);                                 /* mid-move */
    CHECK(!tessera_is_idle(e));
    {
        vec3 live;
        CHECK(ts_orch_entity_pos(e->orch, 42, live));
        TsLabelItem* items = NULL;
        size_t ni = ts_orch_build_labels(e->orch, e, &e->frame_arena, &items);
        CHECK(ni == 2);
        bool found = false;
        for (size_t i = 0; i < ni; ++i) {
            if (items[i].text && strcmp(items[i].text, "HP 5") == 0) {
                found = true;
                CHECK(fabsf(items[i].pos[0] - live[0]) < 1e-4f);       /* glued */
                CHECK(fabsf(items[i].pos[1] - (live[1] + 1.5f)) < 1e-4f);
                CHECK(fabsf(items[i].pos[2] - live[2]) < 1e-4f);
                /* mid-move the entity is between tiles, not at either end */
                CHECK(live[0] > -0.999f && live[0] < 0.999f);
            }
        }
        CHECK(found);
    }
    settle(e, buf);

    /* ---- text change: crossfade (old string under the new one) ---- */
    snprintf(lbls[0].text, sizeof lbls[0].text, "SCORE 25");
    st.epoch++;
    tessera_set_state(e, &st);
    advance(e, buf, 3);                       /* promote + part of the fade */
    a = find_label(e, 1);
    CHECK(a && !tessera_is_idle(e));
    CHECK(a && strcmp(a->text, "SCORE 25") == 0);
    CHECK(a && strcmp(a->prev_text, "SCORE 10") == 0);
    CHECK(a && a->text_mix > 0.001f && a->text_mix < 0.999f);
    {
        TsLabelItem* items = NULL;
        size_t ni = ts_orch_build_labels(e->orch, e, &e->frame_arena, &items);
        bool both = false;
        for (size_t i = 0; i < ni; ++i)
            if (items[i].prev_text && strcmp(items[i].prev_text, "SCORE 10") == 0 &&
                strcmp(items[i].text, "SCORE 25") == 0) both = true;
        CHECK(both);                           /* both runs drawn mid-fade */
    }
    settle(e, buf);
    a = find_label(e, 1);
    CHECK(a && fabsf(a->text_mix - 1.0f) < 1e-3f);

    /* ---- color change: crossfade retargets from the current color ---- */
    lbls[1].color[0] = 0.1f; lbls[1].color[1] = 0.2f; lbls[1].color[2] = 1.0f; /* -> blue */
    st.epoch++;
    tessera_set_state(e, &st);
    advance(e, buf, 4);                       /* mid crossfade */
    b = find_label(e, 2);
    CHECK(b && !tessera_is_idle(e));
    CHECK(b && fabsf(b->from_color[0] - 1.0f) < 1e-3f); /* from old warm color */
    CHECK(b && fabsf(b->to_color[2] - 1.0f) < 1e-3f);   /* toward blue */
    float mid_r = b ? b->color[0] : 0.0f;
    CHECK(mid_r > 0.11f && mid_r < 0.999f);             /* between the two */

    /* re-push ANOTHER color mid-flight: must retarget from the interpolated
     * value, not snap back to either endpoint */
    lbls[1].color[0] = 0.9f; lbls[1].color[1] = 0.1f; lbls[1].color[2] = 0.1f; /* -> red */
    st.epoch++;
    tessera_set_state(e, &st);
    advance(e, buf, 1);                       /* promote */
    b = find_label(e, 2);
    CHECK(b && fabsf(b->from_color[0] - mid_r) < 0.15f);
    settle(e, buf);
    b = find_label(e, 2);
    CHECK(b && fabsf(b->color[0] - 0.9f) < 1e-3f);      /* arrived at red */

    /* an unchanged re-push of settled labels must NOT hold the engine busy */
    st.epoch++;
    tessera_set_state(e, &st);
    advance(e, buf, 1);
    CHECK(tessera_is_idle(e));

    /* ---- remove: fade out then cull; frame returns to the baseline ---- */
    ents[0].coord = (TesseraCoord){ -1, 0 };  /* entity back where it started */
    st.epoch++;
    tessera_set_state(e, &st);
    settle(e, buf);
    st.labels = NULL; st.label_count = 0;
    st.epoch++;
    tessera_set_state(e, &st);
    advance(e, buf, 2);
    a = find_label(e, 1);
    CHECK(a && a->removing);                  /* fading out, not yet culled */
    settle(e, buf);
    CHECK(e->orch->label_count == 0);
    advance(e, buf, 1);
    CHECK(memcmp(base, buf, sizeof base) == 0);   /* pixel-identical again */

    tessera_destroy(e);
    printf(g_fail ? "test_labels: %d FAILED\n" : "test_labels: OK\n", g_fail);
    return g_fail ? 1 : 0;
}
