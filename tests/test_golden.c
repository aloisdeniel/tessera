/* test_golden.c — rendering determinism / golden test (M9).
 *
 * NOTE ON APPROACH: a checked-in reference-image comparison is inherently
 * backend/driver-sensitive (GPU vendor, driver version, MSAA resolve, sRGB
 * handling all shift exact pixel values), so a fixed PNG committed to the repo
 * would produce false failures across machines. Instead this test verifies
 * DETERMINISM: the same fixed scene, rendered by two independent engines that
 * settle identically, must yield byte-identical images. That catches
 * nondeterministic rendering regressions (uninitialized state, frame-order
 * dependence, RNG leakage) without pinning to a specific backend. It also
 * asserts the image is non-trivial (not a single flat color), proving the
 * pipeline actually drew the scene.
 *
 * Runs headless via tessera_capture_png. GPU (Metal) works offscreen. */
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

/* Build the same fixed scene into an engine and settle it, then capture. The
 * scene is a 3x3 tile board with two entities and a fixed camera. */
static bool render_scene(const char* png_path) {
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

    /* Deterministic timing (values don't matter once settled, but keep fixed). */
    TesseraTiming timing = {
        .move_s = 0.3f, .add_s = 0.3f, .remove_s = 0.25f,
        .tile_s = 0.3f, .reflow_s = 0.3f, .camera_s = 0.5f,
        .speed_multiplier = 1.0f,
    };
    tessera_set_timing(e, &timing);

    TesseraTileDef grass = { .thickness = 0.25f, .tint = {0.4f, 0.7f, 0.35f, 1.0f} };
    TesseraDefId grass_id = tessera_register_tile_def(e, &grass);

    TesseraEntityDef unit = { .scale = 1.0f };
    TesseraDefId unit_id = tessera_register_entity_def(e, &unit);

    TesseraTilePlacement tiles[9];
    int ti = 0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            tiles[ti++] = (TesseraTilePlacement){ .coord = {x, y}, .tile_def = grass_id };

    TesseraEntityPlacement ents[2] = {
        { .id = 1, .def = unit_id, .coord = {-1, -1}, .facing = 0 },
        { .id = 2, .def = unit_id, .coord = { 1,  1}, .facing = 2 },
    };

    TesseraState st = {
        .tiles = tiles, .tile_count = 9,
        .entities = ents, .entity_count = 2,
        .camera = { .focus = {0, 0}, .distance = 7.0f,
                    .yaw = 0.6f, .pitch = 0.8f, .fov = 0.9f },
    };
    tessera_set_state(e, &st);

    /* Settle: fixed dt, promote + run transitions to completion. */
    const double dt = 1.0 / 60.0;
    for (int i = 0; i < 600 && !tessera_is_idle(e); ++i) tessera_tick(e, dt);
    /* A few extra fixed ticks so both engines land in the identical idle state. */
    for (int i = 0; i < 10; ++i) tessera_tick(e, dt);

    bool ok = tessera_capture_png(e, IMG_W, IMG_H, png_path);
    if (!ok) printf("capture failed: %s\n", tessera_last_error(e));
    tessera_destroy(e);
    return ok;
}

/* The third-party stb_image build defines STBI_NO_STDIO, so the file-path
 * stbi_load() is not compiled. Read the PNG bytes ourselves and decode from
 * memory (stbi_load_from_memory is always available). Forces 4 (RGBA) channels. */
static unsigned char* load_png(const char* path, int* w, int* h, int* comp) {
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
    unsigned char* px = stbi_load_from_memory(buf, (int)len, w, h, comp, 4);
    free(buf);
    return px;
}

int main(void) {
    const char* pa = "/tmp/tessera_golden_a.png";
    const char* pb = "/tmp/tessera_golden_b.png";

    if (!render_scene(pa) || !render_scene(pb)) {
        printf("SKIP/FAIL: could not render scene headless\n");
        /* Treat inability to render as a hard failure on this (Metal) target. */
        return 1;
    }

    int wa, ha, na, wb, hb, nb;
    unsigned char* a = load_png(pa, &wa, &ha, &na);
    unsigned char* b = load_png(pb, &wb, &hb, &nb);
    CHECK(a != NULL);
    CHECK(b != NULL);
    if (!a || !b) { free(a); free(b); printf("%d checks failed\n", g_fail); return 1; }

    CHECK(wa == IMG_W && ha == IMG_H);
    CHECK(wb == IMG_W && hb == IMG_H);
    CHECK(wa == wb && ha == hb);

    /* Determinism: two independent renders must be pixel-identical. Allow a
     * tiny tolerance for robustness, but count any differing pixels. */
    size_t n = (size_t)IMG_W * IMG_H * 4;
    size_t diff_pixels = 0;
    int max_diff = 0;
    if (a && b && wa == wb && ha == hb) {
        for (size_t i = 0; i < n; i += 4) {
            int d = 0;
            for (int c = 0; c < 4; ++c) {
                int dc = abs((int)a[i + c] - (int)b[i + c]);
                if (dc > d) d = dc;
            }
            if (d > 0) diff_pixels++;
            if (d > max_diff) max_diff = d;
        }
    }
    printf("determinism: %zu differing pixels (max channel diff %d)\n",
           diff_pixels, max_diff);
    CHECK(diff_pixels == 0);      /* deterministic render => byte-identical */
    CHECK(max_diff <= 1);         /* tiny tolerance guard */

    /* Non-trivial: the image must not be a single flat color. */
    size_t distinct_from_first = 0;
    for (size_t i = 4; i < n; i += 4) {
        if (a[i] != a[0] || a[i + 1] != a[1] ||
            a[i + 2] != a[2] || a[i + 3] != a[3]) distinct_from_first++;
    }
    printf("non-trivial: %zu of %d pixels differ from pixel 0\n",
           distinct_from_first, IMG_W * IMG_H);
    CHECK(distinct_from_first > (size_t)(IMG_W * IMG_H) / 100);  /* >1% varied */

    free(a);
    free(b);

    if (g_fail == 0) { printf("golden/determinism test passed\n"); return 0; }
    printf("%d checks failed\n", g_fail);
    return 1;
}
