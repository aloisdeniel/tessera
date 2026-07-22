/* test_cards.c — cards, piles, and hands (state-driven).
 *
 * Covers: card-def registration (atlas validation), the visible<->hidden
 * crossfade on a flip, a hand overriding its cards' positions with a fan, a
 * pile's thickness tracking its card count, and add/remove settling. Runs
 * headless (render_rgba advances the animation offscreen). Reaches into the
 * orchestrator (via the internal header the test lib exposes) to assert the
 * animated state a public query can't observe. */
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

/* Register a solid-colour 16x16 atlas. */
static TesseraDefId make_atlas(TesseraEngine* e, unsigned char r, unsigned char g,
                               unsigned char b) {
    const int P = 16;
    unsigned char px[16 * 16 * 4];
    for (int i = 0; i < P * P; ++i) {
        px[i*4+0] = r; px[i*4+1] = g; px[i*4+2] = b; px[i*4+3] = 255;
    }
    int len = 0; unsigned char* tga = tga32(px, P, P, &len);
    TesseraBytes by = { .data = tga, .size = (size_t)len };
    TesseraDefId id = tessera_register_atlas(e, &by);
    free(tga);
    return id;
}

static void log_fn(void* ud, int level, const char* msg) {
    (void)ud;
    if (level >= TESSERA_LOG_ERROR) printf("[engine] %s\n", msg);
}

/* Find a live card/pile instance by id + kind in the orchestrator. */
static TsCardInst* find_inst(TesseraEngine* e, uint64_t id, bool is_draw) {
    if (!e->orch) return NULL;
    for (size_t i = 0; i < e->orch->card_count; ++i)
        if (e->orch->cards[i].id == id && e->orch->cards[i].is_draw == is_draw)
            return &e->orch->cards[i];
    return NULL;
}

static void advance(TesseraEngine* e, unsigned char* buf, int n) {
    for (int i = 0; i < n; ++i) tessera_render_rgba(e, 1.0 / 60.0, 64, 64, buf, 64*64*4);
}
static void settle(TesseraEngine* e, unsigned char* buf) {
    int steps = 0;
    while (!tessera_is_idle(e) && steps < 400) { advance(e, buf, 1); steps++; }
    CHECK(steps < 400);
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

    TesseraDefId visible = make_atlas(e, 230, 60, 60);
    TesseraDefId hidden  = make_atlas(e, 40, 50, 70);
    TesseraDefId back    = make_atlas(e, 120, 30, 30);
    CHECK(visible && hidden && back);

    /* invalid atlas id must be rejected */
    TesseraCardDef bad = {0};
    bad.visible_atlas = 999999;
    CHECK(tessera_register_card_def(e, &bad) == 0);

    TesseraCardDef cd = {0};
    cd.visible_atlas = visible; cd.hidden_atlas = hidden; cd.back_atlas = back;
    TesseraDefId cdef = tessera_register_card_def(e, &cd);
    CHECK(cdef != 0);

    /* ---- state: 1 free card (hidden), a hand of 2, a pile of 5 ---- */
    TesseraHandPlacement hand = {0};
    hand.id = 100;
    hand.position[0] = 0.0f; hand.position[1] = 1.0f; hand.position[2] = 3.0f;

    TesseraCardPlacement cards[3];
    memset(cards, 0, sizeof cards);
    cards[0].id = 1; cards[0].def = cdef;
    cards[0].position[0] = -3.0f; cards[0].position[1] = 0.0f; cards[0].position[2] = 0.0f;
    cards[0].hidden = true;
    /* hand cards: give them a bogus own-position to prove the hand overrides it */
    for (int i = 0; i < 2; ++i) {
        cards[1 + i].id = (TesseraCardId)(10 + i);
        cards[1 + i].def = cdef;
        cards[1 + i].position[0] = 99.0f; cards[1 + i].position[1] = 99.0f;
        cards[1 + i].position[2] = 99.0f;
        cards[1 + i].hand = 100;
        cards[1 + i].hand_slot = (uint32_t)i;
    }

    TesseraCardDrawPlacement draw = {0};
    draw.id = 200; draw.def = cdef;
    draw.position[0] = 3.0f; draw.count = 5; draw.top_hidden = true;

    TesseraState st = {0};
    st.cards = cards; st.card_count = 3;
    st.card_draws = &draw; st.card_draw_count = 1;
    st.hands = &hand; st.hand_count = 1;
    st.camera.distance = 12.0f; st.camera.pitch = 0.7f; st.camera.fov = 0.9f;
    tessera_set_state(e, &st);
    advance(e, buf, 1);            /* promote */
    settle(e, buf);

    /* all four instances live */
    TsCardInst* c1 = find_inst(e, 1, false);
    TsCardInst* h0 = find_inst(e, 10, false);
    TsCardInst* h1 = find_inst(e, 11, false);
    TsCardInst* dpile = find_inst(e, 200, true);
    CHECK(c1 && h0 && h1 && dpile);

    /* the hidden card settled with mix == 1 (fully concealed) */
    CHECK(c1 && fabsf(c1->mix - 1.0f) < 0.01f);

    /* the hand overrode its cards: NOT at (99,99,99), and near the hand anchor */
    if (h0 && h1) {
        CHECK(h0->pos[0] < 50.0f && h0->pos[1] < 50.0f && h0->pos[2] < 50.0f);
        float dx = h0->pos[0] - hand.position[0];
        float dz = h0->pos[2] - hand.position[2];
        CHECK(sqrtf(dx*dx + dz*dz) < 4.0f);
        /* the two fanned cards sit at different spots */
        float sx = h0->pos[0] - h1->pos[0];
        CHECK(fabsf(sx) > 0.05f);
    }

    /* pile thickness reflects 5 cards (thicker than a single card) */
    float thick5 = dpile ? dpile->thick : 0.0f;
    CHECK(dpile && thick5 > 0.03f);
    CHECK(dpile && dpile->count == 5);

    /* ---- flip the free card face up: the front must crossfade to visible ---- */
    cards[0].hidden = false;
    tessera_set_state(e, &st);
    advance(e, buf, 8);                   /* let the (eased) crossfade get going */
    c1 = find_inst(e, 1, false);
    CHECK(c1 && c1->mix > 0.0f && c1->mix < 0.99f);   /* mid crossfade */

    /* re-push an UNCHANGED state mid-flip: the crossfade must keep running, not
     * freeze at its current blended value (regression for the interrupted-flip
     * bug). Nudge an unrelated field so the promotion definitely happens. */
    st.epoch++;
    tessera_set_state(e, &st);
    advance(e, buf, 1);
    settle(e, buf);
    c1 = find_inst(e, 1, false);
    CHECK(c1 && c1->mix < 0.01f);         /* settled fully visible (did not freeze) */

    /* ---- grow the pile 5 -> 30: thickness must increase ---- */
    draw.count = 30;
    tessera_set_state(e, &st);
    settle(e, buf);
    dpile = find_inst(e, 200, true);
    CHECK(dpile && dpile->count == 30);
    CHECK(dpile && dpile->thick > thick5 + 0.001f);

    /* ---- remove everything: all card instances cull ---- */
    st.cards = NULL; st.card_count = 0;
    st.card_draws = NULL; st.card_draw_count = 0;
    st.hands = NULL; st.hand_count = 0;
    tessera_set_state(e, &st);
    settle(e, buf);
    CHECK(e->orch->card_count == 0);

    tessera_destroy(e);
    printf(g_fail ? "test_cards: %d FAILED\n" : "test_cards: OK\n", g_fail);
    return g_fail ? 1 : 0;
}
