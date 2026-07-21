/* examples/showcase — a guided tour of every Tessera feature.
 *
 *   SPACE : advance to the next predefined state (transitions animate live)
 *   C     : glide the camera to focus the next entity (loops around)
 *   R     : reset the camera to the state's overview pose
 *   S     : cycle shadow quality (blob -> none -> blob)   [M7]
 *   P     : toggle perspective / isometric projection
 *   F     : toggle depth-of-field (focal blur)
 *   [ ]   : move the focal plane nearer / farther
 *   - =   : narrow / widen the sharp focus range
 *   click : ray-pick the tile / entity under the cursor and print it
 *   arrows: orbit the camera manually
 *   ESC   : quit
 *
 * What it exercises:
 *   M3/M4  tiles rising/sinking, entities hopping, stacking + reflow, spawn/despawn
 *   M5     skinned, animated "bar" units (idle bend clip; bends harder while moving)
 *   M6     particle effects — a burst when effects appear, an aura riding a unit
 *   M7     cel/flat lighting, blob shadows, animated camera focus/zoom transitions
 */
#include "tessera.h"
#include "../common/glb_gen.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TILES 512
#define MAX_ENTS  32
#define MAX_FX     8

static TesseraTilePlacement   g_tiles[MAX_TILES];
static TesseraEntityPlacement g_ents[MAX_ENTS];
static TesseraEffectPlacement g_fx[MAX_FX];
static size_t g_tcount, g_ecount, g_fcount;
static TesseraCamera g_cam;

static TesseraDefId d_grass, d_stone, d_water, d_cube, d_bar, d_burst, d_aura;

static size_t board_rect(int x0, int x1, int z0, int z1, int water_row,
                         int hole_x, int hole_z) {
    size_t n = 0;
    for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x) {
            if (x == hole_x && z == hole_z) continue;
            TesseraDefId d = (z == water_row) ? d_water
                           : (((x + z) & 1) ? d_stone : d_grass);
            g_tiles[n++] = (TesseraTilePlacement){ .coord = {x, z}, .tile_def = d };
        }
    return n;
}

/* Alternate unit def so half the units are skinned bars, half static cubes. */
static void put_ent(size_t i, TesseraEntityId id, int x, int z) {
    TesseraDefId def = (id & 1) ? d_bar : d_cube;
    g_ents[i] = (TesseraEntityPlacement){ .id = id, .def = def, .coord = {x, z}, .anim = 0 };
}

#define NUM_STATES 4
static void build_state(int idx) {
    g_cam = (TesseraCamera){ .focus = {0, 0}, .distance = 16.0f,
                             .yaw = 0.7f, .pitch = 0.62f, .fov = 0.85f };
    g_fcount = 0;
    switch (idx) {
    case 0: /* full 9x9 board; six units spread out; a spark burst at the centre */
        g_tcount = board_rect(-4, 4, -4, 4, -4, 99, 99);
        put_ent(0, 1, -4, -3); put_ent(1, 2, 4, -3);
        put_ent(2, 3, -4,  3); put_ent(3, 4, 4,  3);
        put_ent(4, 5, -2,  0); put_ent(5, 6, 2,  0);
        g_ecount = 6;
        g_fx[g_fcount++] = (TesseraEffectPlacement){ .id = 100, .def = d_burst, .coord = {0, 0} };
        break;
    case 1: /* everyone converges onto the centre (stacking + reflow); aura on unit 1 */
        g_tcount = board_rect(-4, 4, -4, 4, 0, 99, 99);
        put_ent(0, 1, 0, 0); put_ent(1, 2, 0, 0); put_ent(2, 3, 0, 0);
        put_ent(3, 4, 0, 0); put_ent(4, 5, 0, 0); put_ent(5, 6, 0, 0);
        g_ecount = 6;
        g_fx[g_fcount++] = (TesseraEffectPlacement){ .id = 101, .def = d_aura,
                                                     .coord = {0, 0}, .attach_entity_id = 1 };
        break;
    case 2: /* board shrinks to 5x5 with a hole; two units despawn (poof) */
        g_tcount = board_rect(-2, 2, -2, 2, -2, 2, 2);
        put_ent(0, 1, -2, -1); put_ent(1, 2, 2, -1);
        put_ent(2, 3, -1,  1); put_ent(3, 4, 1,  1);
        g_ecount = 4;                       /* ids 5,6 removed -> despawn */
        break;
    case 3: /* board grows to 11x7; units respawn and fan out; two bursts */
        g_tcount = board_rect(-5, 5, -3, 3, 3, 99, 99);
        put_ent(0, 1, -5, -3); put_ent(1, 2, 5, -3);
        put_ent(2, 3, -5,  3); put_ent(3, 4, 5,  3);
        put_ent(4, 5,  0, -3); put_ent(5, 6, 0,  3);   /* ids 5,6 spawn back */
        g_ecount = 6;
        g_fx[g_fcount++] = (TesseraEffectPlacement){ .id = 102, .def = d_burst, .coord = {0, -3} };
        g_fx[g_fcount++] = (TesseraEffectPlacement){ .id = 103, .def = d_burst, .coord = {0,  3} };
        break;
    }
}

static void push_current(TesseraEngine* e) {
    TesseraState s = {
        .tiles = g_tiles, .tile_count = g_tcount,
        .entities = g_ents, .entity_count = g_ecount,
        .effects = g_fx, .effect_count = g_fcount,
        .camera = g_cam, .epoch = (uint64_t)SDL_GetTicks(),
    };
    tessera_set_state(e, &s);
}

static void logfn(void* ud, int level, const char* msg) {
    (void)ud; (void)level; fprintf(stderr, "  %s\n", msg);
}
static void settle(TesseraEngine* e, double secs) {
    const double step = 1.0 / 120.0;
    for (double t = 0; t < secs; t += step) tessera_tick(e, step);
}

/* Register every def used by the tour. */
static void register_defs(TesseraEngine* e) {
    d_grass = tessera_register_tile_def(e, &(TesseraTileDef){ .thickness = 0.25f, .tint = {0.45f, 0.72f, 0.40f, 1} });
    d_stone = tessera_register_tile_def(e, &(TesseraTileDef){ .thickness = 0.25f, .tint = {0.62f, 0.62f, 0.66f, 1} });
    d_water = tessera_register_tile_def(e, &(TesseraTileDef){ .thickness = 0.16f, .tint = {0.30f, 0.55f, 0.85f, 1} });
    d_cube  = tessera_register_entity_def(e, &(TesseraEntityDef){ .scale = 1.0f });

    /* skinned, animated bar (clip 0 = "bend"; reused as the move clip) */
    size_t glb_size = 0;
    uint8_t* glb = ts_example_build_bar_glb(&glb_size);
    d_bar = tessera_register_entity_def(e, &(TesseraEntityDef){
        .gltf = { .data = glb, .size = glb_size, .debug_name = "bar" },
        .scale = 0.9f, .move_anim = 0 });
    free(glb);

    /* additive spark burst (on_add) + soft smoke (on_remove) */
    TesseraParticleSpec spark = {
        .mode = TESSERA_EMIT_BURST, .count = 90, .lifetime_s = 0.8f, .lifetime_var = 0.25f,
        .speed = 3.2f, .speed_var = 1.0f, .spread_deg = 55.0f, .gravity = -3.5f,
        .size_start = 0.22f, .size_end = 0.02f,
        .color_start = {1.0f, 0.85f, 0.35f, 1.0f}, .color_end = {1.0f, 0.25f, 0.05f, 0.0f},
        .blend = TESSERA_BLEND_ADD };
    TesseraParticleSpec smoke = {
        .mode = TESSERA_EMIT_BURST, .count = 40, .lifetime_s = 1.0f, .lifetime_var = 0.3f,
        .speed = 1.4f, .speed_var = 0.5f, .spread_deg = 40.0f, .gravity = 0.6f,
        .size_start = 0.15f, .size_end = 0.55f,
        .color_start = {0.85f, 0.85f, 0.9f, 0.7f}, .color_end = {0.5f, 0.5f, 0.55f, 0.0f},
        .blend = TESSERA_BLEND_ALPHA };
    d_burst = tessera_register_effect_def(e, &(TesseraEffectDef){ .on_add = spark, .on_remove = smoke });

    TesseraParticleSpec aura = {
        .mode = TESSERA_EMIT_CONTINUOUS, .count = 45, .lifetime_s = 1.0f, .lifetime_var = 0.2f,
        .speed = 0.8f, .speed_var = 0.3f, .spread_deg = 25.0f, .gravity = 1.4f,
        .size_start = 0.10f, .size_end = 0.01f,
        .color_start = {0.4f, 0.9f, 1.0f, 1.0f}, .color_end = {0.2f, 0.5f, 1.0f, 0.0f},
        .blend = TESSERA_BLEND_ADD, .duration_s = 8.0f };
    d_aura = tessera_register_effect_def(e, &(TesseraEffectDef){ .on_add = aura, .on_remove = smoke });
}

/* Headless verification: drive the states + features and write PNGs. */
static int run_demo(TesseraEngine* e, const char* dir) {
    char p[512];
    build_state(0); push_current(e); settle(e, 0.18);      /* catch the spark burst */
    snprintf(p, sizeof p, "%s/showcase_0_burst.png", dir);
    tessera_capture_png(e, 1280, 720, p); printf("wrote %s\n", p);
    settle(e, 1.6);
    snprintf(p, sizeof p, "%s/showcase_0_spread.png", dir);
    tessera_capture_png(e, 1280, 720, p); printf("wrote %s\n", p);

    build_state(1); push_current(e); settle(e, 0.9);        /* converge + aura + skinning */
    snprintf(p, sizeof p, "%s/showcase_1_aura.png", dir);
    tessera_capture_png(e, 1280, 720, p); printf("wrote %s\n", p);
    settle(e, 1.2);
    snprintf(p, sizeof p, "%s/showcase_1_stacked.png", dir);
    tessera_capture_png(e, 1280, 720, p); printf("wrote %s\n", p);

    build_state(2); push_current(e); settle(e, 1.6);        /* shrink + despawn poof */
    snprintf(p, sizeof p, "%s/showcase_2_shrunk.png", dir);
    tessera_capture_png(e, 1280, 720, p); printf("wrote %s\n", p);

    /* camera focus glide onto the first unit */
    g_cam.focus = (TesseraCoordF){ (float)g_ents[0].coord.x, (float)g_ents[0].coord.y };
    g_cam.distance = 7.0f; push_current(e);
    settle(e, 0.35);
    snprintf(p, sizeof p, "%s/showcase_3_cam_gliding.png", dir);
    tessera_capture_png(e, 1280, 720, p); printf("wrote %s (idle=%d)\n", p, tessera_is_idle(e));
    settle(e, 1.0);
    snprintf(p, sizeof p, "%s/showcase_4_cam_focused.png", dir);
    tessera_capture_png(e, 1280, 720, p); printf("wrote %s (idle=%d)\n", p, tessera_is_idle(e));

    /* depth-of-field: overview board, focal plane on the mid distance */
    build_state(3); push_current(e); settle(e, 1.4);
    tessera_set_focus(e, &(TesseraFocus){ .enabled = true, .focus_distance = 16.0f,
                                          .focus_range = 2.5f, .blur_strength = 14.0f });
    settle(e, 0.05);
    snprintf(p, sizeof p, "%s/showcase_5_dof.png", dir);
    tessera_capture_png(e, 1280, 720, p); printf("wrote %s\n", p);

    /* isometric projection (DoF off) */
    tessera_set_focus(e, NULL);
    tessera_set_projection(e, TESSERA_PROJECTION_ISOMETRIC);
    settle(e, 0.05);
    snprintf(p, sizeof p, "%s/showcase_6_isometric.png", dir);
    tessera_capture_png(e, 1280, 720, p); printf("wrote %s\n", p);
    tessera_set_projection(e, TESSERA_PROJECTION_PERSPECTIVE);
    return 0;
}

int main(int argc, char** argv) {
    const char* demo_dir = NULL;
    for (int i = 1; i < argc; ++i)
        if (!strcmp(argv[i], "--demo") && i + 1 < argc) demo_dir = argv[++i];

    TesseraConfig cfg = { .width = 1280, .height = 720, .pixel_density = 1.0f,
                          .debug = false, .log = logfn };
    TesseraEngine* e = tessera_create(&cfg);
    if (!e || strlen(tessera_last_error(e))) {
        fprintf(stderr, "init failed: %s\n", e ? tessera_last_error(e) : "null");
        return 1;
    }

    TesseraLight light = { .dir = {-0.5f, -1.0f, -0.35f}, .color = {1.0f, 0.96f, 0.88f},
                           .intensity = 1.1f, .ambient = {0.26f, 0.30f, 0.40f} };
    tessera_set_light(e, &light);

    TesseraTiming t = { .move_s = 0.45f, .add_s = 0.4f, .remove_s = 0.35f,
                        .tile_s = 0.4f, .reflow_s = 0.4f, .camera_s = 0.8f,
                        .speed_multiplier = 1.0f };
    tessera_set_timing(e, &t);

    register_defs(e);

    if (demo_dir) {
        int rc = run_demo(e, demo_dir);
        tessera_destroy(e);
        return rc;
    }

    int state_idx = 0, cam_ent = -1, shadow_mode = TESSERA_SHADOW_BLOB;
    bool iso = false;
    TesseraFocus focus = { .enabled = false, .focus_distance = 12.0f,
                           .focus_range = 2.5f, .blur_strength = 14.0f };
    build_state(state_idx);
    push_current(e);

    printf("SPACE=state  C=focus  R=overview  S=shadows  P=projection  "
           "F=DoF  []=focal dist  -=/+=range  click=pick  ESC=quit\n");

    Uint64 prev = SDL_GetPerformanceCounter();
    double freq = (double)SDL_GetPerformanceFrequency();
    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) running = false;
            else if (ev.type == SDL_EVENT_WINDOW_RESIZED)
                tessera_resize(e, ev.window.data1, ev.window.data2, 1.0f);
            else if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                     ev.button.button == SDL_BUTTON_LEFT) {
                TesseraPick pick;
                bool any = tessera_pick(e, ev.button.x, ev.button.y, &pick);
                if (!any) {
                    printf("pick (%.0f,%.0f): nothing\n", ev.button.x, ev.button.y);
                } else {
                    if (pick.hit_entity)
                        printf("pick: entity %llu @ dist %.2f\n",
                               (unsigned long long)pick.entity, pick.entity_distance);
                    if (pick.hit_tile)
                        printf("pick: tile (%d,%d) @ dist %.2f\n",
                               pick.tile.x, pick.tile.y, pick.tile_distance);
                }
            }
            else if (ev.type == SDL_EVENT_KEY_DOWN) {
                switch (ev.key.key) {
                case SDLK_ESCAPE: running = false; break;
                case SDLK_SPACE:
                    state_idx = (state_idx + 1) % NUM_STATES;
                    cam_ent = -1;
                    build_state(state_idx);
                    push_current(e);
                    printf("state %d  (%zu tiles, %zu entities, %zu effects)\n",
                           state_idx, g_tcount, g_ecount, g_fcount);
                    break;
                case SDLK_C:
                    if (g_ecount > 0) {
                        cam_ent = (cam_ent + 1) % (int)g_ecount;
                        g_cam.focus = (TesseraCoordF){ (float)g_ents[cam_ent].coord.x,
                                                       (float)g_ents[cam_ent].coord.y };
                        g_cam.distance = 8.0f;
                        push_current(e);
                    }
                    break;
                case SDLK_R:
                    cam_ent = -1;
                    g_cam.focus = (TesseraCoordF){0, 0};
                    g_cam.distance = 16.0f;
                    push_current(e);
                    break;
                case SDLK_S: {
                    shadow_mode = (shadow_mode == TESSERA_SHADOW_BLOB)
                                  ? TESSERA_SHADOW_NONE : TESSERA_SHADOW_BLOB;
                    TesseraQuality q = { .shadows = (TesseraShadowMode)shadow_mode,
                                         .msaa = 1, .render_scale = 1.0f };
                    tessera_set_quality(e, &q);
                    printf("shadows: %s\n", shadow_mode == TESSERA_SHADOW_BLOB ? "blob" : "off");
                    break;
                }
                case SDLK_P:
                    iso = !iso;
                    tessera_set_projection(e, iso ? TESSERA_PROJECTION_ISOMETRIC
                                                  : TESSERA_PROJECTION_PERSPECTIVE);
                    printf("projection: %s\n", iso ? "isometric" : "perspective");
                    break;
                case SDLK_F:
                    focus.enabled = !focus.enabled;
                    tessera_set_focus(e, &focus);
                    printf("depth-of-field: %s (dist %.1f, range %.1f)\n",
                           focus.enabled ? "on" : "off", focus.focus_distance, focus.focus_range);
                    break;
                case SDLK_LEFTBRACKET:
                    focus.focus_distance = focus.focus_distance > 2.0f ? focus.focus_distance - 1.0f : 1.0f;
                    if (focus.enabled) { tessera_set_focus(e, &focus); printf("focal dist %.1f\n", focus.focus_distance); }
                    break;
                case SDLK_RIGHTBRACKET:
                    focus.focus_distance += 1.0f;
                    if (focus.enabled) { tessera_set_focus(e, &focus); printf("focal dist %.1f\n", focus.focus_distance); }
                    break;
                case SDLK_MINUS:
                    focus.focus_range = focus.focus_range > 0.75f ? focus.focus_range - 0.5f : 0.5f;
                    if (focus.enabled) { tessera_set_focus(e, &focus); printf("focus range %.1f\n", focus.focus_range); }
                    break;
                case SDLK_EQUALS:
                    focus.focus_range += 0.5f;
                    if (focus.enabled) { tessera_set_focus(e, &focus); printf("focus range %.1f\n", focus.focus_range); }
                    break;
                case SDLK_LEFT:  tessera__debug_orbit(e, -0.12f, 0, 0); break;
                case SDLK_RIGHT: tessera__debug_orbit(e,  0.12f, 0, 0); break;
                case SDLK_UP:    tessera__debug_orbit(e, 0, -0.08f, 0); break;
                case SDLK_DOWN:  tessera__debug_orbit(e, 0,  0.08f, 0); break;
                default: break;
                }
            }
        }

        Uint64 now = SDL_GetPerformanceCounter();
        double dt = (double)(now - prev) / freq;
        prev = now;
        tessera_tick(e, dt);
    }

    tessera_destroy(e);
    return 0;
}
