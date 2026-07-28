/* test_serialize.c — state serialization, save/undo & replay.
 *
 * Covers: two-call sizing; a MAXIMAL state (tiles with ids, entities with
 * multi-step move paths, effects, cards with paths/hands/source piles, card
 * draws, hands, dice, overlays, labels, highlights, a non-default camera and
 * an epoch) surviving serialize -> deserialize -> serialize BYTE-IDENTICALLY;
 * header validation (magic / version / total size) and graceful rejection of
 * garbage — every truncation length, bit flips, corrupted counts — without
 * crashing; the replay container (append / serialize / open / count / get,
 * with per-record blobs byte-identical to direct serialization); and a render
 * proof: pushing the DESERIALIZED state into a second engine produces the
 * same headless capture as the original (leaning on the cross-engine
 * determinism test_golden.c already established). */
#include "tessera.h"
#include "stb_image.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

#define IMG_W 192
#define IMG_H 192

static void log_fn(void* ud, int level, const char* msg) {
    (void)ud;
    if (level >= TESSERA_LOG_ERROR) printf("[engine] %s\n", msg);
}

/* ---- the maximal state -------------------------------------------------- */
/* Def ids come from the engine's slotmap and are deterministic for a fixed
 * registration order — every engine that registers the same defs in the same
 * order hands out the same ids. The pure-data tests use placeholder ids; the
 * render test probes the real ones (and asserts they repeat). */
typedef struct { TesseraDefId tile, entity, card, dice; } DefIds;
#define PLACEHOLDER_IDS ((DefIds){ 1, 2, 3, 4 })

static TesseraCoord g_epath[3] = { {0, 0}, {1, 0}, {1, 1} };
static float g_cpath[6] = { 0.0f, 0.0f, 0.0f, 2.0f, 0.9f, -1.5f };

typedef struct {
    TesseraTilePlacement tiles[4];
    TesseraEntityPlacement ents[2];
    TesseraEffectPlacement fx[1];
    TesseraCardPlacement cards[2];
    TesseraCardDrawPlacement draws[1];
    TesseraHandPlacement hands[1];
    TesseraDicePlacement dice[1];
    TesseraOverlayPlacement overlays[2];
    TesseraLabelPlacement labels[1];
    TesseraHighlightPlacement hls[2];
    TesseraPointLightPlacement plights[2];
    TesseraWorldModelPlacement wmodels[1];
} MaxArrays;

static void build_maximal(MaxArrays* a, TesseraState* st, DefIds ids) {
    memset(a, 0, sizeof *a);
    memset(st, 0, sizeof *st);

    int ti = 0;
    for (int y = 0; y <= 1; ++y)
        for (int x = 0; x <= 1; ++x)
            a->tiles[ti] = (TesseraTilePlacement){
                .coord = {x, y}, .tile_def = ids.tile,
                .variant = (uint32_t)ti, .id = (TesseraTileId)(100 + ti) }, ti++;

    a->ents[0] = (TesseraEntityPlacement){
        .id = 1, .def = ids.entity, .coord = {1, 1}, .facing = 2, .anim = 0,
        .path = g_epath, .path_count = 3 };
    a->ents[1] = (TesseraEntityPlacement){
        .id = 2, .def = ids.entity, .coord = {0, 1}, .facing = 1 };

    a->fx[0] = (TesseraEffectPlacement){
        .id = 900, .def = 0, .coord = {0, 0}, .attach_entity_id = 1,
        .attach_card_id = 20 };

    a->cards[0] = (TesseraCardPlacement){
        .id = 20, .def = ids.card, .position = {2.0f, 0.9f, -1.5f},
        .orientation = {0, 0, 0, 1}, .hidden = true,
        .source_draw = 40, .path = g_cpath, .path_count = 2 };
    a->cards[1] = (TesseraCardPlacement){
        .id = 21, .def = ids.card, .hand = 50, .hand_slot = 1 };

    a->draws[0] = (TesseraCardDrawPlacement){
        .id = 40, .def = ids.card, .position = {-2.0f, 0.0f, 0.0f},
        .orientation = {0, 0, 0, 1}, .count = 12, .top_hidden = true };

    a->hands[0] = (TesseraHandPlacement){
        .id = 50, .position = {0.0f, 1.4f, 3.0f}, .orientation = {0, 0, 0, 1},
        .spread_deg = 40.0f, .radius = 2.5f, .card_spacing = 0.3f };

    a->dice[0] = (TesseraDicePlacement){
        .id = 60, .def = ids.dice, .face = 3,
        .position = {0.5f, 0.0f, 2.0f}, .seed = 1234, .throw_s = 0.6f };

    a->overlays[0] = (TesseraOverlayPlacement){
        .coord = {0, 0}, .shape = TESSERA_OVERLAY_RING,
        .tint = {1.0f, 0.8f, 0.1f, 0.9f}, .pulse_s = 1.2f,
        .pulse_alpha_min = 0.4f, .pulse_alpha_max = 1.0f,
        .pulse_scale_min = 0.9f, .pulse_scale_max = 1.05f };
    a->overlays[1] = (TesseraOverlayPlacement){
        .coord = {1, 0}, .shape = TESSERA_OVERLAY_DISC,
        .tint = {0.1f, 0.9f, 0.2f, 0.7f} };

    a->labels[0] = (TesseraLabelPlacement){
        .id = 70, .font = 0 /* no font registered; label is carried, not drawn */,
        .anchor = TESSERA_LABEL_ANCHOR_ENTITY, .anchor_id = 1,
        .position = {0.0f, 1.2f, 0.0f}, .size = 0.5f,
        .color = {1, 1, 1, 1}, .billboard = true };
    snprintf(a->labels[0].text, sizeof a->labels[0].text, "HP 12/20 · über");

    a->hls[0] = (TesseraHighlightPlacement){
        .target_id = 1, .kind = TESSERA_HIGHLIGHT_ENTITY,
        .style = TESSERA_HIGHLIGHT_OUTLINE, .color = {1.0f, 0.85f, 0.1f, 1.0f},
        .thickness = 3.0f, .pulse_s = 1.0f, .pulse_min = 0.5f, .pulse_max = 1.0f };
    a->hls[1] = (TesseraHighlightPlacement){
        .target_id = 101, .kind = TESSERA_HIGHLIGHT_TILE,
        .style = TESSERA_HIGHLIGHT_GLOW, .color = {0.2f, 0.8f, 1.0f, 1.0f} };

    a->plights[0] = (TesseraPointLightPlacement){
        .id = 80, .position = {1.0f, 1.5f, -2.0f}, .color = {1.0f, 0.6f, 0.2f},
        .intensity = 2.0f, .radius = 5.0f };
    a->plights[1] = (TesseraPointLightPlacement){
        .id = 81, .position = {-1.0f, 0.8f, 2.0f}, .color = {0.3f, 0.5f, 1.0f},
        .intensity = 1.2f };
    a->wmodels[0] = (TesseraWorldModelPlacement){
        .id = 90, .def = ids.entity, .position = {3.0f, -0.5f, 1.0f},
        .orientation = {0, 0.7071f, 0, 0.7071f}, .scale = 2.5f };

    st->tiles = a->tiles;         st->tile_count = 4;
    st->entities = a->ents;       st->entity_count = 2;
    st->effects = a->fx;          st->effect_count = 1;
    st->cards = a->cards;         st->card_count = 2;
    st->card_draws = a->draws;    st->card_draw_count = 1;
    st->hands = a->hands;         st->hand_count = 1;
    st->dice = a->dice;           st->dice_count = 1;
    st->overlays = a->overlays;   st->overlay_count = 2;
    st->labels = a->labels;       st->label_count = 1;
    st->highlights = a->hls;      st->highlight_count = 2;
    st->point_lights = a->plights; st->point_light_count = 2;
    st->world_models = a->wmodels; st->world_model_count = 1;
    st->camera = (TesseraCamera){
        .mode = TESSERA_CAMERA_ORBIT, .focus = {0.5f, 0.5f},
        .distance = 8.0f, .yaw = 0.6f, .pitch = 0.8f, .fov = 0.9f,
        .position = {1, 2, 3}, .orientation = {0, 0, 0, 1}, .target = {4, 5, 6},
        .target_id = 7, .focus_card_id = 8, .fit_padding = 0.1f };
    st->epoch = 0xC0FFEEuLL;
}

/* ---- pure-data round trip ----------------------------------------------- */
static void test_roundtrip(void) {
    MaxArrays a;
    TesseraState st;
    build_maximal(&a, &st, PLACEHOLDER_IDS);

    /* two-call sizing */
    size_t n = tessera_state_serialize(&st, NULL, 0);
    CHECK(n > 24);
    uint8_t* blob = (uint8_t*)malloc(n);
    CHECK(tessera_state_serialize(&st, blob, n) == n);
    /* an undersized buffer still reports the required size */
    CHECK(tessera_state_serialize(&st, blob, 10) == n);
    CHECK(tessera_state_serialize(&st, blob, n) == n);   /* rewrite after abuse */

    /* header: magic/version/total LE, epoch as the sequence number */
    CHECK(blob[0] == 'T' && blob[1] == 'S' && blob[2] == 'S' && blob[3] == 'T');
    uint64_t total = 0, epoch = 0;
    for (int i = 0; i < 8; ++i) total |= (uint64_t)blob[8 + i] << (8 * i);
    for (int i = 0; i < 8; ++i) epoch |= (uint64_t)blob[16 + i] << (8 * i);
    CHECK(total == (uint64_t)n);
    CHECK(epoch == 0xC0FFEEuLL);

    TesseraState* d = tessera_state_deserialize(blob, n);
    CHECK(d != NULL);
    if (d) {
        CHECK(d->epoch == 0xC0FFEEuLL);
        CHECK(d->tile_count == 4 && d->tiles[3].id == 103);
        CHECK(d->entity_count == 2);
        CHECK(d->entities[0].path_count == 3 && d->entities[0].path != NULL);
        CHECK(d->entities[0].path != g_epath);   /* owned copy */
        CHECK(d->entities[0].path[2].x == 1 && d->entities[0].path[2].y == 1);
        CHECK(d->entities[1].path == NULL && d->entities[1].path_count == 0);
        CHECK(d->effect_count == 1 && d->effects[0].attach_entity_id == 1
              && d->effects[0].attach_card_id == 20);
        CHECK(d->card_count == 2 && d->cards[0].hidden == true);
        CHECK(d->cards[0].path_count == 2 && d->cards[0].path != NULL);
        CHECK(fabsf(d->cards[0].path[4] - 0.9f) < 1e-6f);
        CHECK(d->cards[1].hand == 50 && d->cards[1].hand_slot == 1);
        CHECK(d->card_draw_count == 1 && d->card_draws[0].count == 12
              && d->card_draws[0].top_hidden == true);
        CHECK(d->hand_count == 1 && fabsf(d->hands[0].spread_deg - 40.0f) < 1e-6f);
        CHECK(d->dice_count == 1 && d->dice[0].face == 3 && d->dice[0].seed == 1234);
        CHECK(d->overlay_count == 2
              && d->overlays[0].shape == TESSERA_OVERLAY_RING
              && fabsf(d->overlays[0].pulse_scale_max - 1.05f) < 1e-6f);
        CHECK(d->label_count == 1 && strcmp(d->labels[0].text, a.labels[0].text) == 0
              && d->labels[0].billboard == true && d->labels[0].anchor_id == 1);
        CHECK(d->highlight_count == 2 && d->highlights[1].kind == TESSERA_HIGHLIGHT_TILE
              && d->highlights[1].style == TESSERA_HIGHLIGHT_GLOW);
        CHECK(d->point_light_count == 2 && d->point_lights[0].id == 80
              && fabsf(d->point_lights[0].radius - 5.0f) < 1e-6f
              && fabsf(d->point_lights[1].intensity - 1.2f) < 1e-6f);
        CHECK(d->world_model_count == 1 && d->world_models[0].id == 90
              && fabsf(d->world_models[0].scale - 2.5f) < 1e-6f
              && fabsf(d->world_models[0].orientation[1] - 0.7071f) < 1e-6f);
        CHECK(d->camera.mode == TESSERA_CAMERA_ORBIT
              && fabsf(d->camera.focus.x - 0.5f) < 1e-6f
              && d->camera.target_id == 7 && d->camera.focus_card_id == 8);

        /* serialize the reconstruction: byte-identical */
        size_t n2 = tessera_state_serialize(d, NULL, 0);
        CHECK(n2 == n);
        uint8_t* blob2 = (uint8_t*)malloc(n2);
        CHECK(tessera_state_serialize(d, blob2, n2) == n2);
        CHECK(memcmp(blob, blob2, n) == 0);
        free(blob2);
        tessera_state_free(d);
    }

    /* an empty state round-trips too */
    TesseraState empty;
    memset(&empty, 0, sizeof empty);
    size_t en = tessera_state_serialize(&empty, NULL, 0);
    CHECK(en > 0);
    uint8_t* eb = (uint8_t*)malloc(en);
    tessera_state_serialize(&empty, eb, en);
    TesseraState* ed = tessera_state_deserialize(eb, en);
    CHECK(ed != NULL);
    if (ed) {
        CHECK(ed->tile_count == 0 && ed->tiles == NULL);
        CHECK(ed->highlight_count == 0 && ed->highlights == NULL);
        size_t en2 = tessera_state_serialize(ed, NULL, 0);
        uint8_t* eb2 = (uint8_t*)malloc(en2);
        tessera_state_serialize(ed, eb2, en2);
        CHECK(en2 == en && memcmp(eb, eb2, en) == 0);
        free(eb2);
        tessera_state_free(ed);
    }
    free(eb);

    /* NULL arrays with huge counts serialize as empty (snapshot semantics) */
    TesseraState hostile;
    memset(&hostile, 0, sizeof hostile);
    hostile.tile_count = (size_t)-1;
    hostile.entity_count = (size_t)-1;
    CHECK(tessera_state_serialize(&hostile, NULL, 0) == en);

    CHECK(tessera_state_serialize(NULL, NULL, 0) == 0);
    tessera_state_free(NULL);   /* no-op */
    free(blob);
}

/* ---- hostile-input rejection -------------------------------------------- */
static void test_garbage(void) {
    MaxArrays a;
    TesseraState st;
    build_maximal(&a, &st, PLACEHOLDER_IDS);
    size_t n = tessera_state_serialize(&st, NULL, 0);
    uint8_t* blob = (uint8_t*)malloc(n);
    tessera_state_serialize(&st, blob, n);

    CHECK(tessera_state_deserialize(NULL, n) == NULL);
    CHECK(tessera_state_deserialize(blob, 0) == NULL);

    /* every truncation must be rejected (total-size / bounds checks) */
    for (size_t len = 1; len < n; ++len)
        CHECK(tessera_state_deserialize(blob, len) == NULL);

    uint8_t* mut = (uint8_t*)malloc(n);

    /* bad magic / bad version */
    memcpy(mut, blob, n);
    mut[0] ^= 0xFF;
    CHECK(tessera_state_deserialize(mut, n) == NULL);
    memcpy(mut, blob, n);
    mut[4] = 0x7F;
    CHECK(tessera_state_deserialize(mut, n) == NULL);

    /* corrupt the tile count into something huge */
    memcpy(mut, blob, n);
    mut[24 + 88] = 0xFF; mut[24 + 88 + 7] = 0xFF;
    CHECK(tessera_state_deserialize(mut, n) == NULL);

    /* single-bit flips anywhere must never crash (NULL or a valid state) */
    for (size_t i = 0; i < n; ++i) {
        memcpy(mut, blob, n);
        mut[i] ^= (uint8_t)(1u << (i % 8));
        TesseraState* d = tessera_state_deserialize(mut, n);
        if (d) tessera_state_free(d);
    }

    /* deterministic pseudo-random garbage buffers */
    uint32_t rng = 0x12345678u;
    for (int it = 0; it < 64; ++it) {
        size_t len = 1 + (rng % 512);
        for (size_t i = 0; i < len; ++i) {
            rng = rng * 1664525u + 1013904223u;
            mut[i % n] = (uint8_t)(rng >> 24);
        }
        TesseraState* d = tessera_state_deserialize(mut, len < n ? len : n);
        if (d) tessera_state_free(d);
        CHECK(tessera_replay_open(mut, len < n ? len : n) == NULL);
    }

    free(mut);
    free(blob);
}

/* ---- replay container ---------------------------------------------------- */
static void test_replay(void) {
    MaxArrays a;
    TesseraState st;
    build_maximal(&a, &st, PLACEHOLDER_IDS);

    TesseraReplay* rec = tessera_replay_create();
    CHECK(rec != NULL);
    CHECK(tessera_replay_count(rec) == 0);
    CHECK(tessera_replay_append(rec, 0, NULL) == false);
    CHECK(tessera_replay_append(NULL, 0, &st) == false);

    /* three evolving states */
    uint64_t stamps[3] = { 0, 700, 1500 };
    size_t sizes[3];
    uint8_t* blobs[3];
    for (int i = 0; i < 3; ++i) {
        st.epoch = (uint64_t)(i + 1);
        a.ents[0].coord.x = i;
        sizes[i] = tessera_state_serialize(&st, NULL, 0);
        blobs[i] = (uint8_t*)malloc(sizes[i]);
        tessera_state_serialize(&st, blobs[i], sizes[i]);
        CHECK(tessera_replay_append(rec, stamps[i], &st));
    }
    CHECK(tessera_replay_count(rec) == 3);

    size_t cn = tessera_replay_serialize(rec, NULL, 0);
    CHECK(cn > 24);
    uint8_t* cont = (uint8_t*)malloc(cn);
    CHECK(tessera_replay_serialize(rec, cont, cn) == cn);
    CHECK(cont[0] == 'T' && cont[1] == 'S' && cont[2] == 'R' && cont[3] == 'P');
    tessera_replay_free(rec);

    TesseraReplay* play = tessera_replay_open(cont, cn);
    CHECK(play != NULL);
    if (play) {
        CHECK(tessera_replay_count(play) == 3);
        CHECK(tessera_replay_get(play, 3, NULL) == NULL);   /* out of range */
        for (uint32_t i = 0; i < 3; ++i) {
            uint64_t ts = 999999;
            TesseraState* d = tessera_replay_get(play, i, &ts);
            CHECK(d != NULL && ts == stamps[i]);
            if (d) {
                CHECK(d->epoch == (uint64_t)(i + 1));
                CHECK(d->entities[0].coord.x == (int32_t)i);
                /* record blob byte-identical to direct serialization */
                size_t dn = tessera_state_serialize(d, NULL, 0);
                CHECK(dn == sizes[i]);
                uint8_t* db = (uint8_t*)malloc(dn);
                tessera_state_serialize(d, db, dn);
                CHECK(memcmp(db, blobs[i], dn) == 0);
                free(db);
                tessera_state_free(d);
            }
        }
        tessera_replay_free(play);
    }

    /* container truncations / corruption must be rejected */
    for (size_t len = 0; len < cn; len += 7)
        CHECK(tessera_replay_open(cont, len) == NULL);
    cont[0] ^= 0xFF;
    CHECK(tessera_replay_open(cont, cn) == NULL);
    cont[0] ^= 0xFF;
    CHECK(tessera_replay_open(NULL, cn) == NULL);
    tessera_replay_free(NULL);   /* no-op */
    CHECK(tessera_replay_count(NULL) == 0);
    CHECK(tessera_replay_get(NULL, 0, NULL) == NULL);
    CHECK(tessera_replay_serialize(NULL, NULL, 0) == 0);

    free(cont);
    for (int i = 0; i < 3; ++i) free(blobs[i]);
}

/* ---- render equivalence --------------------------------------------------- */
/* Minimal 32-bit uncompressed TGA (same helper as test_dice.c). */
static unsigned char* tga32(const unsigned char* rgba, int w, int h, int* len) {
    int sz = 18 + w * h * 4;
    unsigned char* d = (unsigned char*)malloc((size_t)sz);
    memset(d, 0, 18);
    d[2] = 2; d[12] = w & 0xFF; d[13] = (w >> 8) & 0xFF;
    d[14] = h & 0xFF; d[15] = (h >> 8) & 0xFF; d[16] = 32; d[17] = 0x28;
    for (int i = 0; i < w * h; ++i) {
        d[18+i*4+0] = rgba[i*4+2]; d[18+i*4+1] = rgba[i*4+1];
        d[18+i*4+2] = rgba[i*4+0]; d[18+i*4+3] = rgba[i*4+3];
    }
    *len = sz; return d;
}

/* Register the fixed def set; ids depend only on registration order, so every
 * engine doing this yields the same DefIds. */
static bool register_defs(TesseraEngine* e, DefIds* out) {
    TesseraTileDef tdef = { .thickness = 0.25f, .tint = {0.45f, 0.65f, 0.4f, 1.0f} };
    out->tile = tessera_register_tile_def(e, &tdef);
    TesseraEntityDef edef = { .scale = 1.0f };
    out->entity = tessera_register_entity_def(e, &edef);
    TesseraCardDef cdef = { .tint = {1, 1, 1, 1} };
    out->card = tessera_register_card_def(e, &cdef);

    const int P = 16;
    unsigned char px[16 * 16 * 4];
    TesseraDiceFace faces[6];
    unsigned char* sprites[6];
    for (int f = 0; f < 6; ++f) {
        for (int i = 0; i < P * P; ++i) {
            px[i*4+0] = (unsigned char)(30 + f * 30);
            px[i*4+1] = (unsigned char)(200 - f * 25);
            px[i*4+2] = 90; px[i*4+3] = 255;
        }
        int len = 0;
        sprites[f] = tga32(px, P, P, &len);
        faces[f] = (TesseraDiceFace){ .sprite = { .data = sprites[f], .size = (size_t)len } };
    }
    TesseraDiceDef dd = { .faces = faces, .face_count = 6, .size = 0.8f };
    out->dice = tessera_register_dice_def(e, &dd);
    for (int f = 0; f < 6; ++f) free(sprites[f]);
    return out->tile && out->entity && out->card && out->dice;
}

static bool render_state(const TesseraState* st, const DefIds* expect,
                         const char* png_path) {
    TesseraConfig cfg = { .width = IMG_W, .height = IMG_H, .pixel_density = 1.0f,
                          .log = log_fn };
    TesseraEngine* e = tessera_create(&cfg);
    if (!e) return false;
    if (strcmp(tessera_backend_name(e), "none") == 0) {
        tessera_destroy(e);
        return false;
    }
    DefIds ids;
    if (!register_defs(e, &ids)) { tessera_destroy(e); return false; }
    /* the id sequence must repeat across engines for the state to resolve */
    CHECK(ids.tile == expect->tile && ids.entity == expect->entity
          && ids.card == expect->card && ids.dice == expect->dice);

    tessera_set_state(e, st);
    const double dt = 1.0 / 60.0;
    for (int i = 0; i < 900 && !tessera_is_idle(e); ++i) tessera_tick(e, dt);
    for (int i = 0; i < 10; ++i) tessera_tick(e, dt);

    bool ok = tessera_capture_png(e, IMG_W, IMG_H, png_path);
    tessera_destroy(e);
    return ok;
}

static unsigned char* load_png(const char* path, int* w, int* h) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); return NULL; }
    unsigned char* buf = (unsigned char*)malloc((size_t)len);
    size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (got != (size_t)len) { free(buf); return NULL; }
    int comp = 0;
    unsigned char* px = stbi_load_from_memory(buf, (int)len, w, h, &comp, 4);
    free(buf);
    return px;
}

static void test_render_equivalence(void) {
    /* probe the deterministic def ids with a throwaway engine */
    DefIds ids;
    {
        TesseraConfig cfg = { .width = IMG_W, .height = IMG_H,
                              .pixel_density = 1.0f, .log = log_fn };
        TesseraEngine* probe = tessera_create(&cfg);
        if (!probe || strcmp(tessera_backend_name(probe), "none") == 0
            || !register_defs(probe, &ids)) {
            printf("SKIP: render equivalence (no GPU backend)\n");
            if (probe) tessera_destroy(probe);
            return;
        }
        tessera_destroy(probe);
    }

    MaxArrays a;
    TesseraState st;
    build_maximal(&a, &st, ids);
    /* steady visuals: drop the free-running pulses so both settled captures
     * sample the identical animation clock state */
    a.overlays[0].pulse_s = 0;
    a.hls[0].pulse_s = 0;

    size_t n = tessera_state_serialize(&st, NULL, 0);
    uint8_t* blob = (uint8_t*)malloc(n);
    tessera_state_serialize(&st, blob, n);
    TesseraState* d = tessera_state_deserialize(blob, n);
    CHECK(d != NULL);
    free(blob);
    if (!d) return;

    const char* pa = "/tmp/tessera_ser_a.png";
    const char* pb = "/tmp/tessera_ser_b.png";
    if (!render_state(&st, &ids, pa) || !render_state(d, &ids, pb)) {
        printf("SKIP: render equivalence (no GPU backend)\n");
        tessera_state_free(d);
        return;
    }
    tessera_state_free(d);

    int wa = 0, ha = 0, wb = 0, hb = 0;
    unsigned char* ia = load_png(pa, &wa, &ha);
    unsigned char* ib = load_png(pb, &wb, &hb);
    CHECK(ia && ib && wa == IMG_W && ha == IMG_H && wb == wa && hb == ha);
    if (ia && ib && wa == wb && ha == hb) {
        CHECK(memcmp(ia, ib, (size_t)wa * ha * 4) == 0);
        /* prove the image is non-trivial (the scene actually drew) */
        int diff = 0;
        for (int i = 4; i < wa * ha * 4; i += 997)
            if (ia[i] != ia[0]) { diff = 1; break; }
        CHECK(diff == 1);
    }
    stbi_image_free(ia);
    stbi_image_free(ib);
    printf("render equivalence: captures byte-identical\n");
}

int main(void) {
    test_roundtrip();
    test_garbage();
    test_replay();
    test_render_equivalence();

    if (g_fail == 0) { printf("all serialization checks passed\n"); return 0; }
    printf("%d checks failed\n", g_fail);
    return 1;
}
