/* test_fuzz.c — robustness of the public FFI boundary (M9 task 2 & 5).
 *
 * Creates a real GPU engine (Metal works headless) and feeds it a battery of
 * malformed / hostile inputs: NULL state, huge counts against NULL arrays, bad
 * def ids, NaN/Inf coords & camera params, negative sizes, empty states, a
 * single-tile board, a huge stack of entities on one tile, a retarget storm of
 * rapid set_state interleaved with tick, and register calls with NULL defs.
 *
 * The engine must never crash and must always report gracefully:
 *   - every batch is followed by a few ticks + one capture to prove the
 *     pipeline stays alive;
 *   - tessera_last_error must return a valid non-null string throughout.
 * The test passes if it runs to completion without crashing. */
#include "tessera.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

static int g_error_calls = 0;
static void log_fn(void* ud, int level, const char* msg) {
    (void)ud; (void)msg;
    if (level >= TESSERA_LOG_ERROR) g_error_calls++;
}

static const char* g_png = "/tmp/tessera_fuzz.png";

/* Run a handful of ticks then one capture; assert last_error stays valid and
 * the engine is still alive (capture may legitimately succeed or fail, but it
 * must not crash). */
static void pump(TesseraEngine* e, const char* label) {
    printf("[batch] %s\n", label); fflush(stdout);
    for (int i = 0; i < 4; ++i) tessera_tick(e, 1.0 / 60.0);
    tessera_capture_png(e, 128, 128, g_png);
    const char* err = tessera_last_error(e);
    CHECK(err != NULL);
    if (err == NULL) printf("  (last_error NULL after %s)\n", label);
}

int main(void) {
    TesseraConfig cfg = {
        .width = 256, .height = 256, .pixel_density = 1.0f, .log = log_fn,
    };
    TesseraEngine* e = tessera_create(&cfg);
    CHECK(e != NULL);
    if (!e) { printf("tessera_create returned NULL\n"); return 1; }
    CHECK(tessera_last_error(e) != NULL);
    bool have_gpu = strcmp(tessera_backend_name(e), "none") != 0;
    if (!have_gpu) printf("WARN: no GPU backend (%s); FFI paths still exercised\n",
                          tessera_last_error(e));

    /* Valid defs to reference in some batches. */
    TesseraTileDef tdef = { .thickness = 0.25f, .tint = {1, 1, 1, 1} };
    TesseraDefId tile_id = tessera_register_tile_def(e, &tdef);
    TesseraEntityDef edef = { .scale = 1.0f };
    TesseraDefId ent_id = tessera_register_entity_def(e, &edef);
    CHECK(tile_id != 0);
    CHECK(ent_id != 0);

    /* ---- batch: NULL state ------------------------------------------------ */
    tessera_set_state(e, NULL);
    pump(e, "null-state");

    /* ---- batch: huge counts against NULL arrays --------------------------- */
    /* The engine treats a NULL array as empty regardless of the reported count,
     * so this must be handled without reading the bogus pointers. */
    {
        TesseraState s;
        memset(&s, 0, sizeof s);
        s.tiles = NULL;    s.tile_count   = (size_t)-1;
        s.entities = NULL; s.entity_count = (size_t)-1;
        s.effects = NULL;  s.effect_count = (size_t)0xFFFFFFFFu;
        s.camera = (TesseraCamera){ .distance = 8, .yaw = 0.5f, .pitch = 0.6f, .fov = 0.9f };
        tessera_set_state(e, &s);
        pump(e, "huge-counts-null-arrays");
    }

    /* ---- batch: valid array but a huge count on a DIFFERENT (null) array --- */
    {
        TesseraTilePlacement tiles[1] = { { .coord = {0, 0}, .tile_def = tile_id } };
        TesseraState s;
        memset(&s, 0, sizeof s);
        s.tiles = tiles; s.tile_count = 1;
        s.entities = NULL; s.entity_count = (size_t)-1;  /* null => ignored */
        s.camera = (TesseraCamera){ .distance = 8, .yaw = 0.5f, .pitch = 0.6f, .fov = 0.9f };
        tessera_set_state(e, &s);
        pump(e, "valid-tiles-huge-null-entities");
    }

    /* ---- batch: bad / zero / huge def ids in placements ------------------- */
    {
        TesseraTilePlacement tiles[3] = {
            { .coord = {0, 0}, .tile_def = 0 },              /* 0 = hole */
            { .coord = {1, 0}, .tile_def = 999999u },        /* unknown  */
            { .coord = {2, 0}, .tile_def = 0xFFFFFFFFu },    /* huge     */
        };
        TesseraEntityPlacement ents[3] = {
            { .id = 1, .def = 0,           .coord = {0, 0} },
            { .id = 2, .def = 123456u,     .coord = {1, 0} },
            { .id = 3, .def = 0xFFFFFFFFu, .coord = {2, 0} },
        };
        TesseraEffectPlacement fx[2] = {
            { .id = 10, .def = 0,      .coord = {0, 0} },
            { .id = 11, .def = 42424u, .coord = {1, 0}, .attach_entity_id = 999 },
        };
        TesseraState s = {
            .tiles = tiles, .tile_count = 3,
            .entities = ents, .entity_count = 3,
            .effects = fx, .effect_count = 2,
            .camera = { .distance = 8, .yaw = 0.5f, .pitch = 0.6f, .fov = 0.9f },
        };
        tessera_set_state(e, &s);
        pump(e, "bad-def-ids");
    }

    /* ---- batch: NaN / Inf coords & camera params -------------------------- */
    {
        /* coords are integers (can't be NaN), but push extreme values, and make
         * every camera float NaN/Inf. */
        TesseraTilePlacement tiles[2] = {
            { .coord = {INT32_MIN, INT32_MAX}, .tile_def = tile_id },
            { .coord = {0, 0}, .tile_def = tile_id },
        };
        TesseraEntityPlacement ents[1] = {
            { .id = 1, .def = ent_id, .coord = {INT32_MAX, INT32_MIN}, .facing = 65535 },
        };
        float nan = NAN, inf = INFINITY;
        /* Every camera float is NaN/Inf, INCLUDING yaw. A prior bug hung the
         * camera-glide shortest-arc normalization on an infinite yaw delta;
         * apply_camera + advance_camera now sanitize non-finite params
         * (src/engine.c), so this must complete rather than spin. Keep yaw = Inf
         * here as a regression guard. */
        TesseraState s = {
            .tiles = tiles, .tile_count = 2,
            .entities = ents, .entity_count = 1,
            .camera = { .focus = {INT32_MAX, INT32_MIN},
                        .distance = inf, .yaw = inf, .pitch = -inf, .fov = nan },
        };
        tessera_set_state(e, &s);
        pump(e, "nan-inf");

        /* also exercise -Inf yaw explicitly */
        s.camera.yaw = -inf; s.epoch = 999;
        tessera_set_state(e, &s);
        pump(e, "neg-inf-yaw");

        /* Also feed NaN/Inf through set_light / set_quality / timing. */
        TesseraLight bad_light = {
            .dir = {nan, inf, -inf}, .color = {nan, nan, nan},
            .intensity = inf, .ambient = {-inf, nan, inf} };
        tessera_set_light(e, &bad_light);
        TesseraQuality bad_q = { .shadows = (TesseraShadowMode)999, .msaa = -7,
                                 .render_scale = nan };
        tessera_set_quality(e, &bad_q);
        TesseraTiming bad_t = { .move_s = nan, .add_s = inf, .remove_s = -inf,
                                .tile_s = nan, .reflow_s = inf, .camera_s = -1.0f,
                                .speed_multiplier = nan };
        tessera_set_timing(e, &bad_t);
        pump(e, "nan-inf-light-quality-timing");

        /* Restore sane timing/quality for the remaining batches. */
        TesseraTiming ok_t = { .move_s = 0.3f, .add_s = 0.3f, .remove_s = 0.25f,
                               .tile_s = 0.3f, .reflow_s = 0.3f, .camera_s = 0.5f,
                               .speed_multiplier = 1.0f };
        tessera_set_timing(e, &ok_t);
        TesseraQuality ok_q = { .shadows = TESSERA_SHADOW_BLOB, .msaa = 1,
                                .render_scale = 1.0f };
        tessera_set_quality(e, &ok_q);
    }

    /* ---- batch: negative sizes -------------------------------------------- */
    tessera_resize(e, -100, -100, -1.0f);
    tessera_resize(e, 0, 0, 0.0f);
    CHECK(tessera_capture_png(e, -1, -1, g_png) == false);
    CHECK(tessera_capture_png(e, 0, 128, g_png) == false);
    CHECK(tessera_capture_png(e, 128, 0, g_png) == false);
    CHECK(tessera_capture_png(e, 128, 128, NULL) == false);
    tessera_resize(e, 256, 256, 1.0f);   /* restore */
    pump(e, "negative-sizes");

    /* ---- batch: empty state ----------------------------------------------- */
    {
        TesseraState s;
        memset(&s, 0, sizeof s);
        s.camera = (TesseraCamera){ .distance = 8, .yaw = 0.5f, .pitch = 0.6f, .fov = 0.9f };
        tessera_set_state(e, &s);
        pump(e, "empty-state");
    }

    /* ---- batch: single-tile board ----------------------------------------- */
    {
        TesseraTilePlacement tiles[1] = { { .coord = {0, 0}, .tile_def = tile_id } };
        TesseraEntityPlacement ents[1] = { { .id = 1, .def = ent_id, .coord = {0, 0} } };
        TesseraState s = {
            .tiles = tiles, .tile_count = 1,
            .entities = ents, .entity_count = 1,
            .camera = { .distance = 5, .yaw = 0.5f, .pitch = 0.7f, .fov = 0.9f },
        };
        tessera_set_state(e, &s);
        pump(e, "single-tile");
    }

    /* ---- batch: huge stack of many entities on one tile ------------------- */
    {
        enum { STACK = 500 };
        TesseraTilePlacement tiles[1] = { { .coord = {0, 0}, .tile_def = tile_id } };
        TesseraEntityPlacement* ents = (TesseraEntityPlacement*)malloc(STACK * sizeof *ents);
        CHECK(ents != NULL);
        for (int i = 0; i < STACK; ++i)
            ents[i] = (TesseraEntityPlacement){
                .id = (TesseraEntityId)(1000 + i), .def = ent_id, .coord = {0, 0} };
        TesseraState s = {
            .tiles = tiles, .tile_count = 1,
            .entities = ents, .entity_count = STACK,
            .camera = { .distance = 10, .yaw = 0.5f, .pitch = 0.8f, .fov = 0.9f },
        };
        tessera_set_state(e, &s);
        pump(e, "huge-stack");
        free(ents);
    }

    /* ---- batch: retarget storm (rapid set_state interleaved with tick) ---- */
    {
        TesseraTilePlacement tiles[2] = {
            { .coord = {0, 0}, .tile_def = tile_id },
            { .coord = {1, 0}, .tile_def = tile_id },
        };
        for (int i = 0; i < 300; ++i) {
            TesseraEntityPlacement ents[1] = {
                { .id = 1, .def = ent_id, .coord = {(i & 1), 0}, .facing = (uint16_t)i },
            };
            TesseraState s = {
                .tiles = tiles, .tile_count = 2,
                .entities = ents, .entity_count = 1,
                .camera = { .distance = 8, .yaw = 0.01f * i, .pitch = 0.6f, .fov = 0.9f },
                .epoch = (uint64_t)i,
            };
            tessera_set_state(e, &s);           /* retarget before settle */
            if ((i & 3) == 0) tessera_tick(e, 1.0 / 120.0);
        }
        pump(e, "retarget-storm");
        CHECK(tessera_last_error(e) != NULL);
    }

    /* ---- batch: state-blob / replay deserialization fuzz ------------------ */
    {
        /* a valid blob must reconstruct and go straight through set_state */
        TesseraTilePlacement tiles[1] = { { .coord = {0, 0}, .tile_def = tile_id } };
        TesseraEntityPlacement ents[1] = { { .id = 5, .def = ent_id, .coord = {0, 0} } };
        TesseraState s = {
            .tiles = tiles, .tile_count = 1,
            .entities = ents, .entity_count = 1,
            .camera = { .distance = 6, .yaw = 0.4f, .pitch = 0.7f, .fov = 0.9f },
        };
        size_t n = tessera_state_serialize(&s, NULL, 0);
        CHECK(n > 0);
        uint8_t* blob = (uint8_t*)malloc(n);
        CHECK(blob != NULL);
        CHECK(tessera_state_serialize(&s, blob, n) == n);
        TesseraState* d = tessera_state_deserialize(blob, n);
        CHECK(d != NULL);
        if (d) { tessera_set_state(e, d); tessera_state_free(d); }
        pump(e, "deserialized-set-state");

        /* truncations, bit flips and raw garbage must be rejected cleanly */
        CHECK(tessera_state_deserialize(NULL, n) == NULL);
        for (size_t len = 0; len < n; ++len)
            CHECK(tessera_state_deserialize(blob, len) == NULL);
        uint32_t rng = 0xBEEFu;
        uint8_t junk[256];
        for (int it = 0; it < 200; ++it) {
            size_t len = 1 + (rng % sizeof junk);
            for (size_t i = 0; i < len; ++i) {
                rng = rng * 1664525u + 1013904223u;
                junk[i] = (uint8_t)(rng >> 24);
            }
            TesseraState* g = tessera_state_deserialize(junk, len);
            if (g) tessera_state_free(g);       /* garbage may not parse; never crash */
            CHECK(tessera_replay_open(junk, len) == NULL);
        }
        for (size_t i = 0; i < n; ++i) {        /* single-bit flips over the blob */
            blob[i] ^= (uint8_t)(1u << (i % 8));
            TesseraState* g = tessera_state_deserialize(blob, n);
            if (g) tessera_state_free(g);
            blob[i] ^= (uint8_t)(1u << (i % 8));
        }
        free(blob);
        pump(e, "deserialize-fuzz");
    }

    /* ---- batch: register calls with NULL defs ----------------------------- */
    CHECK(tessera_register_atlas(e, NULL) == 0);
    CHECK(tessera_register_tile_def(e, NULL) == 0);
    CHECK(tessera_register_entity_def(e, NULL) == 0);
    CHECK(tessera_register_effect_def(e, NULL) == 0);
    CHECK(tessera_last_error(e) != NULL);
    /* NULL-engine calls must be no-crash no-ops too. */
    tessera_set_state(NULL, NULL);
    tessera_tick(NULL, 0.016);
    tessera_resize(NULL, 10, 10, 1.0f);
    CHECK(tessera_register_tile_def(NULL, &tdef) == 0);
    CHECK(tessera_capture_png(NULL, 64, 64, g_png) == false);
    CHECK(tessera_last_error(NULL) != NULL);   /* must return a string, not crash */
    CHECK(tessera_is_idle(NULL) == true);
    pump(e, "null-defs");

    /* Final liveness check: a normal state still renders after the abuse. */
    {
        TesseraTilePlacement tiles[1] = { { .coord = {0, 0}, .tile_def = tile_id } };
        TesseraState s = { .tiles = tiles, .tile_count = 1,
                           .camera = { .distance = 6, .yaw = 0.4f, .pitch = 0.7f, .fov = 0.9f } };
        tessera_set_state(e, &s);
        for (int i = 0; i < 30 && !tessera_is_idle(e); ++i) tessera_tick(e, 1.0 / 60.0);
        pump(e, "final-liveness");
    }

    tessera_destroy(e);

    printf("engine error-log callbacks observed: %d\n", g_error_calls);
    if (g_fail == 0) { printf("all fuzz checks passed (no crash)\n"); return 0; }
    printf("%d checks failed\n", g_fail);
    return 1;
}
