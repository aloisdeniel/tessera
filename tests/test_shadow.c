/* test_shadow.c — directional shadow mapping (TESSERA_SHADOW_MAP).
 *
 * Same capture-based approach as test_golden.c: a fixed scene is rendered
 * headless under the three shadow modes and the PNGs are compared. Checked-in
 * reference images would be backend/driver-sensitive, so the assertions are
 * relational instead:
 *   - all three modes render and produce non-trivial images;
 *   - MAP differs from NONE and from BLOB (the depth-map pass draws something
 *     the other modes do not);
 *   - MAP is deterministic (two captures are byte-identical);
 *   - MAP only darkens: mean brightness(MAP) < mean brightness(NONE), which
 *    proves real occlusion shadows landed on the board. */
#include "tessera.h"
#include "stb_image.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

#define IMG_W 256
#define IMG_H 256

static void log_fn(void* ud, int level, const char* msg) {
    (void)ud;
    if (level >= TESSERA_LOG_ERROR) printf("[engine ERROR] %s\n", msg);
}

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

/* Fixed board: 5x5 tiles, two tall entities, low sun so shadows stretch. */
static bool render_scene(TesseraShadowMode mode, const char* png_path) {
    TesseraConfig cfg = {
        .width = IMG_W, .height = IMG_H, .pixel_density = 1.0f,
        .log = log_fn,
    };
    TesseraEngine* e = tessera_create(&cfg);
    if (!e) { printf("tessera_create returned NULL\n"); return false; }
    if (strcmp(tessera_backend_name(e), "none") == 0) {
        printf("no GPU backend: %s\n", tessera_last_error(e));
        tessera_destroy(e);
        return false;
    }

    TesseraQuality q = { .shadows = mode, .msaa = 1, .render_scale = 1.0f };
    tessera_set_quality(e, &q);

    TesseraLight light = {
        .dir = {-0.55f, -0.7f, -0.35f},
        .color = {1.0f, 0.98f, 0.92f}, .intensity = 1.0f,
        .ambient = {0.28f, 0.30f, 0.36f},
    };
    tessera_set_light(e, &light);

    TesseraTileDef grass = { .thickness = 0.25f, .tint = {0.45f, 0.7f, 0.4f, 1.0f} };
    TesseraDefId grass_id = tessera_register_tile_def(e, &grass);
    TesseraEntityDef unit = { .scale = 1.0f };
    TesseraDefId unit_id = tessera_register_entity_def(e, &unit);

    /* One card on the board so the card depth-pass pipeline is exercised. */
    TesseraDefId atlas = make_atlas(e, 230, 60, 60);
    TesseraCardDef cd = { .visible_atlas = atlas, .hidden_atlas = atlas,
                          .back_atlas = atlas };
    TesseraDefId cdef = tessera_register_card_def(e, &cd);

    TesseraTilePlacement tiles[25];
    int ti = 0;
    for (int y = -2; y <= 2; ++y)
        for (int x = -2; x <= 2; ++x)
            tiles[ti++] = (TesseraTilePlacement){ .coord = {x, y}, .tile_def = grass_id };

    TesseraEntityPlacement ents[2] = {
        { .id = 1, .def = unit_id, .coord = {0, 0}, .facing = 0 },
        { .id = 2, .def = unit_id, .coord = {1, -1}, .facing = 1 },
    };

    TesseraCardPlacement card = { .id = 1, .def = cdef,
                                  .position = {-1.5f, 0.35f, 1.5f} };

    TesseraState st = {
        .tiles = tiles, .tile_count = 25,
        .entities = ents, .entity_count = 2,
        .cards = &card, .card_count = cdef ? 1 : 0,
        .camera = { .focus = {0, 0}, .distance = 7.0f,
                    .yaw = 0.7f, .pitch = 0.75f, .fov = 0.9f },
    };
    tessera_set_state(e, &st);

    const double dt = 1.0 / 60.0;
    for (int i = 0; i < 600 && !tessera_is_idle(e); ++i) tessera_tick(e, dt);
    for (int i = 0; i < 10; ++i) tessera_tick(e, dt);

    bool ok = tessera_capture_png(e, IMG_W, IMG_H, png_path);
    if (!ok) printf("capture failed: %s\n", tessera_last_error(e));
    tessera_destroy(e);
    return ok;
}

/* stb_image here is built with STBI_NO_STDIO; decode from memory. */
static unsigned char* load_png(const char* path, int* w, int* h) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); return NULL; }
    unsigned char* buf = (unsigned char*)malloc((size_t)len);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (got != (size_t)len) { free(buf); return NULL; }
    int comp = 0;
    unsigned char* px = stbi_load_from_memory(buf, (int)len, w, h, &comp, 4);
    free(buf);
    return px;
}

static double mean_brightness(const unsigned char* px, int w, int h) {
    double sum = 0.0;
    for (int i = 0; i < w * h; ++i)
        sum += (px[i * 4] + px[i * 4 + 1] + px[i * 4 + 2]) / 3.0;
    return sum / (double)(w * h);
}

static bool non_trivial(const unsigned char* px, int w, int h) {
    for (int i = 1; i < w * h; ++i)
        if (px[i * 4] != px[0] || px[i * 4 + 1] != px[1] || px[i * 4 + 2] != px[2])
            return true;
    return false;
}

int main(void) {
    const char* p_none = "/tmp/tessera_shadow_none.png";
    const char* p_blob = "/tmp/tessera_shadow_blob.png";
    const char* p_map  = "/tmp/tessera_shadow_map.png";
    const char* p_map2 = "/tmp/tessera_shadow_map2.png";

    if (!render_scene(TESSERA_SHADOW_NONE, p_none) ||
        !render_scene(TESSERA_SHADOW_BLOB, p_blob) ||
        !render_scene(TESSERA_SHADOW_MAP,  p_map)  ||
        !render_scene(TESSERA_SHADOW_MAP,  p_map2)) {
        printf("FAIL: could not render scene headless\n");
        return 1;
    }

    int w0, h0, w1, h1, w2, h2, w3, h3;
    unsigned char* none = load_png(p_none, &w0, &h0);
    unsigned char* blob = load_png(p_blob, &w1, &h1);
    unsigned char* map  = load_png(p_map,  &w2, &h2);
    unsigned char* map2 = load_png(p_map2, &w3, &h3);
    CHECK(none && blob && map && map2);
    if (!none || !blob || !map || !map2) return 1;
    CHECK(w0 == IMG_W && h0 == IMG_H);
    CHECK(w2 == IMG_W && h2 == IMG_H);

    size_t bytes = (size_t)IMG_W * IMG_H * 4;

    /* All modes drew something. */
    CHECK(non_trivial(none, IMG_W, IMG_H));
    CHECK(non_trivial(blob, IMG_W, IMG_H));
    CHECK(non_trivial(map,  IMG_W, IMG_H));

    /* The depth map changes the image vs both other modes. */
    CHECK(memcmp(map, none, bytes) != 0);
    CHECK(memcmp(map, blob, bytes) != 0);
    /* Blob decals still render in BLOB mode. */
    CHECK(memcmp(blob, none, bytes) != 0);

    /* Deterministic. */
    CHECK(memcmp(map, map2, bytes) == 0);

    /* Shadows only darken: MAP must be dimmer than the unshadowed render. */
    double m_none = mean_brightness(none, IMG_W, IMG_H);
    double m_map  = mean_brightness(map,  IMG_W, IMG_H);
    printf("mean brightness: none=%.3f map=%.3f\n", m_none, m_map);
    CHECK(m_map < m_none);

    free(none); free(blob); free(map); free(map2);

    if (g_fail) { printf("%d failure(s)\n", g_fail); return 1; }
    printf("test_shadow OK\n");
    return 0;
}
