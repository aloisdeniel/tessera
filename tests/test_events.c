/* test_events.c — typed engine event stream (sound/haptics/FX sync).
 *
 * Runs a scripted scene (entity spawn, dice throw + multi-step move, card
 * deal + flip, removals) headless and drains tessera_poll_events, asserting
 * the expected sequence: DICE_CONTACTs precede DICE_SETTLED, DICE_SETTLED and
 * the final ENTITY_HOP_LANDED precede that op's OP_COMPLETED, waypoints
 * arrive in path order with the right coords, timestamps never decrease, the
 * callback mirrors the ring, and overflow drops the oldest events while
 * growing the dropped counter. Extends the tests/test_operations.c pattern.
 */
#include "tessera.h"
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

/* ---- accumulated event log (drained after each settle) ---- */
#define EV_MAX 1024
static TesseraEvent g_ev[EV_MAX];
static uint32_t     g_ev_n = 0;

static void drain(TesseraEngine* e) {
    TesseraEvent buf[64];
    uint32_t n;
    while ((n = tessera_poll_events(e, buf, 64)) > 0)
        for (uint32_t i = 0; i < n && g_ev_n < EV_MAX; ++i)
            g_ev[g_ev_n++] = buf[i];
}

/* First index of `type` in the log at or after `from` (-1 if absent). */
static int ev_find(uint32_t type, uint32_t from) {
    for (uint32_t i = from; i < g_ev_n; ++i)
        if (g_ev[i].type == type) return (int)i;
    return -1;
}

static int ev_count(uint32_t type, uint32_t from) {
    int c = 0;
    for (uint32_t i = from; i < g_ev_n; ++i)
        if (g_ev[i].type == type) c++;
    return c;
}

/* ---- event callback: mirrors the ring (count + last type seen) ---- */
static uint32_t g_cb_count = 0;
static uint32_t g_cb_last_type = 0;
static void ev_cb(const TesseraEvent* ev, void* user) {
    g_cb_count++;
    g_cb_last_type = ev->type;
    if (user) *(int*)user += 1;
}

/* Minimal 32-bit uncompressed TGA (stb_image decodes it) — see test_dice.c. */
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

static TesseraDefId make_die(TesseraEngine* e, uint32_t n) {
    const int P = 16;
    unsigned char px[16 * 16 * 4];
    TesseraDiceFace* faces = (TesseraDiceFace*)calloc(n, sizeof *faces);
    unsigned char** blobs = (unsigned char**)calloc(n, sizeof *blobs);
    for (uint32_t f = 0; f < n; ++f) {
        for (int i = 0; i < P * P; ++i) {
            px[i*4+0] = (unsigned char)(20 + (f * 37) % 200);
            px[i*4+1] = (unsigned char)(30 + (f * 71) % 200);
            px[i*4+2] = (unsigned char)(40 + (f * 91) % 200);
            px[i*4+3] = 255;
        }
        int len = 0;
        blobs[f] = tga32(px, P, P, &len);
        faces[f].sprite.data = blobs[f];
        faces[f].sprite.size = (size_t)len;
    }
    TesseraDiceDef dd = {0};
    dd.faces = faces; dd.face_count = n; dd.size = 1.0f;
    TesseraDefId id = tessera_register_dice_def(e, &dd);
    for (uint32_t f = 0; f < n; ++f) free(blobs[f]);
    free(blobs); free(faces);
    return id;
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

    int cb_user = 0;
    tessera_set_event_callback(e, ev_cb, &cb_user);

    TesseraDefId unit = tessera_register_entity_def(e, &(TesseraEntityDef){ .scale = 1.0f });
    CHECK(unit != 0);
    TesseraDefId die = make_die(e, 6);
    CHECK(die != 0);
    TesseraCardDef cd = { .width = 1.0f, .height = 1.5f };
    TesseraDefId card = tessera_register_card_def(e, &cd);
    CHECK(card != 0);

    TesseraCamera cam = { .focus = {1, 1}, .distance = 14.0f, .pitch = 0.9f, .fov = 0.9f };

    CHECK(tessera_poll_events(e, NULL, 8) == 0);    /* invalid args are safe */
    CHECK(tessera_events_dropped(e) == 0);

    /* ---- op1: seed (snap — no entity events, just the op settle) ---- */
    TesseraEntityPlacement seed = { .id = 1, .def = unit, .coord = {0, 0} };
    TesseraState s0 = { .entities = &seed, .entity_count = 1, .camera = cam, .epoch = 1 };
    TesseraOpId op1 = tessera_set_state(e, &s0);
    settle(e, buf);
    drain(e);
    CHECK(g_ev_n >= 1);
    CHECK(g_ev[g_ev_n - 1].type == TESSERA_EVENT_OP_COMPLETED);
    CHECK(g_ev[g_ev_n - 1].subject == TESSERA_EVENT_SUBJECT_OPERATION);
    CHECK(g_ev[g_ev_n - 1].subject_id == op1);
    CHECK(ev_count(TESSERA_EVENT_ENTITY_SPAWNED, 0) == 0);   /* seed snaps */
    CHECK(ev_count(TESSERA_EVENT_DICE_CONTACT, 0) == 0);

    /* ---- op2: spawn a second entity + move the camera ---- */
    uint32_t mark = g_ev_n;
    TesseraEntityPlacement two[2] = {
        { .id = 1, .def = unit, .coord = {0, 0} },
        { .id = 2, .def = unit, .coord = {2, 0} },
    };
    TesseraCamera cam2 = cam; cam2.yaw = 0.8f;
    TesseraState s1 = { .entities = two, .entity_count = 2, .camera = cam2, .epoch = 2 };
    TesseraOpId op2 = tessera_set_state(e, &s1);
    settle(e, buf);
    drain(e);
    {
        int sp = ev_find(TESSERA_EVENT_ENTITY_SPAWNED, mark);
        int ca = ev_find(TESSERA_EVENT_CAMERA_ARRIVED, mark);
        int oc = ev_find(TESSERA_EVENT_OP_COMPLETED, mark);
        CHECK(sp >= 0 && ca >= 0 && oc >= 0);
        CHECK(sp < oc && ca < oc);                       /* settle comes last */
        CHECK(g_ev[sp].subject == TESSERA_EVENT_SUBJECT_ENTITY);
        CHECK(g_ev[sp].subject_id == 2);
        CHECK(g_ev[sp].coord.x == 2 && g_ev[sp].coord.y == 0);
        CHECK(g_ev[oc].subject_id == op2);
    }

    /* ---- op3: multi-step walk + a dice throw, in one operation ---- */
    mark = g_ev_n;
    TesseraCoord path[2] = { {2, 2}, {0, 2} };
    TesseraEntityPlacement walk[2] = {
        { .id = 1, .def = unit, .coord = {0, 0} },
        { .id = 2, .def = unit, .coord = {0, 2}, .path = path, .path_count = 2 },
    };
    TesseraDicePlacement dice = { .id = 7, .def = die, .face = 3,
                                  .position = {1.0f, 0.0f, 1.0f}, .seed = 1 };
    TesseraState s2 = { .entities = walk, .entity_count = 2, .camera = cam2, .epoch = 3,
                        .dice = &dice, .dice_count = 1 };
    TesseraOpId op3 = tessera_set_state(e, &s2);
    settle(e, buf);
    drain(e);
    {
        /* dice: at least one contact, all before the settle; payloads sane */
        int ds = ev_find(TESSERA_EVENT_DICE_SETTLED, mark);
        CHECK(ds >= 0);
        CHECK(ev_count(TESSERA_EVENT_DICE_SETTLED, mark) == 1);
        CHECK(g_ev[ds].subject == TESSERA_EVENT_SUBJECT_DICE);
        CHECK(g_ev[ds].subject_id == 7);
        CHECK(g_ev[ds].value == 3.0f);                       /* the face */
        CHECK(g_ev[ds].coord.x == 1 && g_ev[ds].coord.y == 1);
        int ncontact = ev_count(TESSERA_EVENT_DICE_CONTACT, mark);
        CHECK(ncontact >= 1);
        for (uint32_t i = mark; i < g_ev_n; ++i)
            if (g_ev[i].type == TESSERA_EVENT_DICE_CONTACT) {
                CHECK((int)i < ds);                          /* contact < settled */
                CHECK(g_ev[i].value > 0.0f);                 /* impact speed */
                CHECK(g_ev[i].subject_id == 7);
            }

        /* walk: waypoint (2,2) handed off first, hops land in path order */
        int wp = ev_find(TESSERA_EVENT_ENTITY_WAYPOINT_REACHED, mark);
        CHECK(wp >= 0);
        CHECK(ev_count(TESSERA_EVENT_ENTITY_WAYPOINT_REACHED, mark) == 1);
        CHECK(g_ev[wp].subject == TESSERA_EVENT_SUBJECT_ENTITY);
        CHECK(g_ev[wp].subject_id == 2);
        CHECK(g_ev[wp].coord.x == 2 && g_ev[wp].coord.y == 2);
        CHECK(g_ev[wp].value == 1.0f);                       /* first handoff */
        int h1 = ev_find(TESSERA_EVENT_ENTITY_HOP_LANDED, mark);
        CHECK(h1 >= 0);
        CHECK(ev_count(TESSERA_EVENT_ENTITY_HOP_LANDED, mark) == 2);
        int h2 = ev_find(TESSERA_EVENT_ENTITY_HOP_LANDED, (uint32_t)h1 + 1);
        CHECK(h2 >= 0);
        CHECK(g_ev[h1].coord.x == 2 && g_ev[h1].coord.y == 2);  /* waypoint hop */
        CHECK(g_ev[h2].coord.x == 0 && g_ev[h2].coord.y == 2);  /* destination */
        CHECK(wp <= h1 && h1 < h2);

        /* the op settles after every transition event it produced */
        int oc = ev_find(TESSERA_EVENT_OP_COMPLETED, mark);
        CHECK(oc >= 0);
        CHECK(g_ev[oc].subject_id == op3);
        CHECK(oc > ds && oc > h2);
        CHECK((uint32_t)oc == g_ev_n - 1);
    }

    /* ---- op4: removal completes -> ENTITY_REMOVED before the settle ---- */
    mark = g_ev_n;
    TesseraEntityPlacement one = { .id = 1, .def = unit, .coord = {0, 0} };
    TesseraState s3 = { .entities = &one, .entity_count = 1, .camera = cam2, .epoch = 4 };
    TesseraOpId op4 = tessera_set_state(e, &s3);
    settle(e, buf);
    drain(e);
    {
        int rm = ev_find(TESSERA_EVENT_ENTITY_REMOVED, mark);
        int oc = ev_find(TESSERA_EVENT_OP_COMPLETED, mark);
        CHECK(rm >= 0 && oc >= 0 && rm < oc);
        CHECK(g_ev[rm].subject_id == 2);
        CHECK(g_ev[oc].subject_id == op4);
    }

    /* ---- op5/op6/op7: deal a card off a pile, then flip it ---- */
    mark = g_ev_n;
    TesseraCardDrawPlacement pile = { .id = 100, .def = card,
                                      .position = {4.0f, 0.0f, 4.0f}, .count = 5 };
    TesseraState s4 = { .entities = &one, .entity_count = 1, .camera = cam2, .epoch = 5,
                        .card_draws = &pile, .card_draw_count = 1 };
    tessera_set_state(e, &s4);
    settle(e, buf);
    drain(e);
    CHECK(ev_count(TESSERA_EVENT_CARD_DEALT, mark) == 0);

    mark = g_ev_n;
    TesseraCardPlacement dealt = { .id = 200, .def = card,
                                   .position = {6.0f, 0.0f, 4.0f},
                                   .source_draw = 100 };
    TesseraCardDrawPlacement pile4 = pile; pile4.count = 4;
    TesseraState s5 = { .entities = &one, .entity_count = 1, .camera = cam2, .epoch = 6,
                        .cards = &dealt, .card_count = 1,
                        .card_draws = &pile4, .card_draw_count = 1 };
    tessera_set_state(e, &s5);
    settle(e, buf);
    drain(e);
    {
        int cd = ev_find(TESSERA_EVENT_CARD_DEALT, mark);
        int oc = ev_find(TESSERA_EVENT_OP_COMPLETED, mark);
        CHECK(cd >= 0 && oc >= 0 && cd < oc);
        CHECK(g_ev[cd].subject == TESSERA_EVENT_SUBJECT_CARD);
        CHECK(g_ev[cd].subject_id == 200);
        CHECK(g_ev[cd].coord.x == 4 && g_ev[cd].coord.y == 4);  /* off the pile */
    }

    mark = g_ev_n;
    TesseraCardPlacement flipped = dealt; flipped.hidden = true;
    TesseraState s6 = { .entities = &one, .entity_count = 1, .camera = cam2, .epoch = 7,
                        .cards = &flipped, .card_count = 1,
                        .card_draws = &pile4, .card_draw_count = 1 };
    tessera_set_state(e, &s6);
    settle(e, buf);
    drain(e);
    {
        int cf = ev_find(TESSERA_EVENT_CARD_FLIPPED, mark);
        CHECK(cf >= 0);
        CHECK(g_ev[cf].subject == TESSERA_EVENT_SUBJECT_CARD);
        CHECK(g_ev[cf].subject_id == 200);
        CHECK(g_ev[cf].value == 1.0f);                       /* now hidden */
    }

    /* ---- global invariants: timestamps never decrease; the callback saw
     *      exactly the ring's events, ending on the same one ---- */
    for (uint32_t i = 1; i < g_ev_n; ++i)
        CHECK(g_ev[i].time >= g_ev[i - 1].time);
    for (uint32_t i = 0; i < g_ev_n; ++i)
        CHECK(g_ev[i].reserved == 0);
    CHECK(g_cb_count == g_ev_n);
    CHECK(cb_user == (int)g_ev_n);
    CHECK(g_cb_last_type == g_ev[g_ev_n - 1].type);
    CHECK(tessera_events_dropped(e) == 0);

    /* ---- overflow: 300 unpolled op settles overflow the 256-slot ring,
     *      dropping the OLDEST and counting the drops ---- */
    tessera_set_event_callback(e, NULL, NULL);
    uint32_t before_cb = g_cb_count;
    TesseraOpId first_op = 0, last_op = 0;
    for (int k = 0; k < 300; ++k) {
        TesseraState sr = { .entities = &one, .entity_count = 1, .camera = cam2,
                            .epoch = 100 + (uint64_t)k };
        TesseraOpId op = tessera_set_state(e, &sr);
        if (k == 0) first_op = op;
        last_op = op;
        settle(e, buf);
    }
    CHECK(g_cb_count == before_cb);                  /* callback was cleared */
    CHECK(tessera_events_dropped(e) == 300 - 256);
    {
        static TesseraEvent big[512];
        uint32_t n = tessera_poll_events(e, big, 512);
        CHECK(n == 256);
        CHECK(big[0].type == TESSERA_EVENT_OP_COMPLETED);
        CHECK(big[0].subject_id == first_op + (300 - 256));   /* oldest dropped */
        CHECK(big[n - 1].subject_id == last_op);
        CHECK(tessera_poll_events(e, big, 512) == 0);         /* fully drained */
    }

    tessera_destroy(e);
    printf(g_fail ? "test_events: %d FAIL\n" : "test_events: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
