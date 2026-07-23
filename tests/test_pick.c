/* test_pick.c — screen-ray picking against the live scene (tessera_pick).
 * Needs a GPU engine (Metal headless works); no window shown. */
#include "tessera.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

static void settle(TesseraEngine* e, double secs) {
    const double step = 1.0 / 120.0;
    for (double t = 0; t < secs; t += step) tessera_tick(e, step);
}

int main(void) {
    const int W = 1280, H = 720;
    TesseraConfig cfg = { .width = W, .height = H, .pixel_density = 1.0f };
    TesseraEngine* e = tessera_create(&cfg);
    if (!e || strlen(tessera_last_error(e))) {
        fprintf(stderr, "engine init failed: %s\n", e ? tessera_last_error(e) : "null");
        return 77;  /* skip: no usable GPU in this environment */
    }

    TesseraDefId tile = tessera_register_tile_def(e, &(TesseraTileDef){ .thickness = 0.25f, .tint = {0.5f,0.6f,0.4f,1} });
    TesseraDefId unit = tessera_register_entity_def(e, &(TesseraEntityDef){ .scale = 1.0f });

    /* 5x5 board centred on origin; one entity standing on (0,0). Each tile gets
     * a unique instance id so it can be projected back to screen. */
    TesseraTilePlacement tiles[25]; size_t nt = 0;
    TesseraTileId tile00_id = 0;
    TesseraTileId corners[4]; int nc = 0;
    for (int z = -2; z <= 2; ++z) for (int x = -2; x <= 2; ++x) {
        TesseraTileId tid = (TesseraTileId)(nt + 1);   /* nonzero */
        if (x == 0 && z == 0) tile00_id = tid;
        if ((x == -2 || x == 2) && (z == -2 || z == 2)) corners[nc++] = tid;
        tiles[nt++] = (TesseraTilePlacement){ .coord = {x, z}, .tile_def = tile, .id = tid };
    }
    TesseraEntityPlacement ents[1] = { { .id = 42, .def = unit, .coord = {0, 0} } };
    /* look straight-ish down at the origin so the centre pixel passes through (0,0) */
    TesseraCamera cam = { .focus = {0, 0}, .distance = 10.0f, .yaw = 0.0f, .pitch = 1.2f, .fov = 0.9f };
    TesseraState s = { .tiles = tiles, .tile_count = nt, .entities = ents, .entity_count = 1,
                       .camera = cam, .epoch = 1 };
    tessera_set_state(e, &s);
    settle(e, 1.0);

    /* centre pixel -> ray through the focus (0,0): must hit the entity and tile (0,0) */
    TesseraPick p;
    bool any = tessera_pick(e, W / 2.0f, H / 2.0f, &p);
    CHECK(any);
    CHECK(p.hit_entity);
    CHECK(p.entity == 42);
    CHECK(p.hit_tile);
    CHECK(p.tile.x == 0 && p.tile.y == 0);
    /* entity sphere is in front of the tile it stands on */
    CHECK(p.entity_distance <= p.tile_distance + 1e-3f);
    /* ray direction is normalized and points generally downward from above */
    float dl = sqrtf(p.ray_dir[0]*p.ray_dir[0] + p.ray_dir[1]*p.ray_dir[1] + p.ray_dir[2]*p.ray_dir[2]);
    CHECK(fabsf(dl - 1.0f) < 1e-3f);
    CHECK(p.ray_dir[1] < 0.0f);
    /* nearest hit point sits near the origin column */
    CHECK(fabsf(p.point[0]) < 0.6f && fabsf(p.point[2]) < 0.6f);

    /* a ray into empty sky above the board should miss everything */
    TesseraPick miss;
    bool hit_sky = tessera_pick(e, W / 2.0f, 2.0f, &miss);  /* very top of the screen */
    /* not asserting a guaranteed miss (depends on framing), but the ray fields
     * must always be populated */
    (void)hit_sky;
    float ml = sqrtf(miss.ray_dir[0]*miss.ray_dir[0] + miss.ray_dir[1]*miss.ray_dir[1] + miss.ray_dir[2]*miss.ray_dir[2]);
    CHECK(fabsf(ml - 1.0f) < 1e-3f);

    /* off-centre pixel toward +x should pick a tile with x > 0 (or none), never x < -2 */
    TesseraPick side;
    if (tessera_pick(e, W * 0.72f, H / 2.0f, &side) && side.hit_tile)
        CHECK(side.tile.x >= 0);

    /* ---- inverse of picking: entity/tile -> screen ---- */
    /* Entity 42 and tile (0,0) both sit on the camera focus (world origin), so
     * they must project back onto the centre pixel that picked them. */
    TesseraScreenPos esp;
    CHECK(tessera_entity_screen_position(e, 42, &esp));
    CHECK(esp.onscreen);
    CHECK(fabsf(esp.x - W / 2.0f) < 2.0f);
    CHECK(fabsf(esp.y - H / 2.0f) < 2.0f);
    CHECK(esp.depth >= 0.0f && esp.depth <= 1.0f);

    TesseraScreenPos tsp;
    CHECK(tessera_tile_screen_position(e, tile00_id, &tsp));
    CHECK(tsp.onscreen);
    CHECK(fabsf(tsp.x - W / 2.0f) < 2.0f);
    CHECK(fabsf(tsp.y - H / 2.0f) < 2.0f);

    /* an off-centre tile projects off-centre, and a pick at that pixel returns
     * to the same tile (full round-trip through both directions) */
    TesseraScreenPos sidep;
    TesseraTileId side_id = tiles[nt - 1].id;   /* tile (2,2) */
    CHECK(tessera_tile_screen_position(e, side_id, &sidep));
    if (sidep.onscreen) {
        TesseraPick rp;
        if (tessera_pick(e, sidep.x, sidep.y, &rp) && rp.hit_tile)
            CHECK(rp.tile.x == 2 && rp.tile.y == 2);
    }

    /* unknown / zero ids report not-found; ray/projection helpers are null-safe */
    TesseraScreenPos nf;
    CHECK(tessera_entity_screen_position(e, 9999, &nf) == false);
    CHECK(tessera_tile_screen_position(e, 0, &nf) == false);
    CHECK(tessera_world_to_screen(e, (float[3]){0, 0, 0}, &nf) == true && nf.onscreen);
    CHECK(tessera_entity_screen_position(NULL, 42, &nf) == false);
    CHECK(tessera_world_to_screen(e, NULL, &nf) == false);

    /* ---- fit camera distance to a set of targets ---- */
    /* The camera starts at distance 10 with the 5x5 board comfortably framed.
     * Fitting tightly to the four corners must (a) succeed, (b) return a finite
     * positive distance, and (c) actually keep all four corners on screen when
     * applied. It should also be tighter (smaller) than the roomy start. */
    float fit_d = 0.0f;
    CHECK(tessera_camera_fit_distance(e, corners, 4, NULL, 0, 0.06f, &fit_d));
    CHECK(fit_d > 0.0f && fit_d < 1.0e5f);
    CHECK(fit_d < 10.0f);

    TesseraCamera fitcam = cam; fitcam.distance = fit_d;
    TesseraState s2 = s; s2.camera = fitcam; s2.epoch = 2;
    tessera_set_state(e, &s2);
    settle(e, 1.0);
    for (int i = 0; i < 4; ++i) {
        TesseraScreenPos csp;
        CHECK(tessera_tile_screen_position(e, corners[i], &csp));
        CHECK(csp.onscreen);
        CHECK(csp.x >= 0.0f && csp.x <= W && csp.y >= 0.0f && csp.y <= H);
    }
    /* fitting a strict superset (every tile) can only need >= the corner fit */
    TesseraTileId all[25];
    for (int i = 0; i < 25; ++i) all[i] = (TesseraTileId)(i + 1);
    float fit_all = 0.0f;
    CHECK(tessera_camera_fit_distance(e, all, 25, NULL, 0, 0.06f, &fit_all));
    CHECK(fit_all >= fit_d - 1e-2f);

    /* a larger padding must frame no tighter than a smaller one (more border =>
     * pull back at least as far). Exercises the padding knob deterministically;
     * aspect sensitivity itself needs a real window resize (see chess example). */
    float fit_pad = 0.0f;
    CHECK(tessera_camera_fit_distance(e, corners, 4, NULL, 0, 0.25f, &fit_pad));
    CHECK(fit_pad >= fit_d - 1e-2f);

    /* empty / unresolved target lists report failure */
    float fit_none = 123.0f;
    CHECK(tessera_camera_fit_distance(e, NULL, 0, NULL, 0, 0.06f, &fit_none) == false);
    TesseraTileId bogus[1] = { 99999 };
    CHECK(tessera_camera_fit_distance(e, bogus, 1, NULL, 0, 0.06f, &fit_none) == false);

    /* null-arg safety */
    CHECK(tessera_pick(NULL, 0, 0, &p) == false);
    CHECK(tessera_pick(e, 0, 0, NULL) == false);
    CHECK(tessera_camera_fit_distance(NULL, corners, 4, NULL, 0, 0, &fit_d) == false);
    CHECK(tessera_camera_fit_distance(e, corners, 4, NULL, 0, 0, NULL) == false);

    /* ---- dice picking + reverse pick ---- */
    /* Register a plain 6-face die (blank sprites decode to a grey cube — geometry
     * is all the pick needs) and drop it on the camera focus (world origin). */
    {
        TesseraDiceFace dfaces[6];
        memset(dfaces, 0, sizeof dfaces);
        TesseraDiceDef ddef = { .faces = dfaces, .face_count = 6, .size = 1.2f };
        TesseraDefId die = tessera_register_dice_def(e, &ddef);
        CHECK(die != 0);

        TesseraDicePlacement dp = { .id = 7, .def = die, .face = 0,
                                    .position = {0, 0.6f, 0}, .seed = 1, .throw_s = 0.3f };
        TesseraState sd = s;                 /* original board + camera (distance 10) */
        sd.dice = &dp; sd.dice_count = 1; sd.epoch = 3;
        tessera_set_state(e, &sd);
        settle(e, 1.5);                      /* let the throw settle to the rest pose */
        CHECK(tessera_dice_all_idle(e));

        /* centre pixel passes through the focus, so it hits the die sitting there */
        TesseraPick dpick;
        CHECK(tessera_pick(e, W / 2.0f, H / 2.0f, &dpick));
        CHECK(dpick.hit_dice);
        CHECK(dpick.dice == 7);
        CHECK(dpick.dice_distance > 0.0f);

        /* reverse pick: the die sits on the focus column, so it projects onto the
         * centre in x; its centre is raised (y=0.6) so y sits a little above the
         * mid-line — the exact-position invariant is the round-trip just below. */
        TesseraScreenPos dsp;
        CHECK(tessera_dice_screen_position(e, 7, &dsp));
        CHECK(dsp.onscreen);
        CHECK(fabsf(dsp.x - W / 2.0f) < 4.0f);
        CHECK(dsp.y >= 0.0f && dsp.y <= H);

        /* full round-trip: a pick at the projected pixel returns the same die */
        TesseraPick drt;
        CHECK(tessera_pick(e, dsp.x, dsp.y, &drt) && drt.hit_dice && drt.dice == 7);

        /* unknown die id reports not-found */
        CHECK(tessera_dice_screen_position(e, 999, &dsp) == false);
        CHECK(tessera_card_screen_position(e, 999, &dsp) == false);
    }

    tessera_destroy(e);
    if (g_fail == 0) { printf("all pick tests passed\n"); return 0; }
    printf("%d pick checks failed\n", g_fail);
    return 1;
}
