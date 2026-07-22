/* test_dice.c — procedural dice: model generation, throw settling, removal.
 *
 * Exercises all three generated geometries (coin=2, cube=6, barrel=N), the
 * throw simulation (a die must settle on exactly the requested face), and the
 * add / remove / clear lifecycle. Runs headless: render_rgba advances the
 * animation offscreen with no window present, so it is ASan-friendly. */
#include "tessera.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

/* Minimal 32-bit uncompressed TGA (stb_image decodes it) so the test needs no
 * image-write dependency. */
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

/* Register a dice def with `n` faces, each a flat 16x16 sprite. */
static TesseraDefId make_die(TesseraEngine* e, uint32_t n, float size) {
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
    dd.faces = faces; dd.face_count = n; dd.size = size;
    TesseraDefId id = tessera_register_dice_def(e, &dd);
    for (uint32_t f = 0; f < n; ++f) free(blobs[f]);
    free(blobs); free(faces);
    return id;
}

static void log_fn(void* ud, int level, const char* msg) {
    (void)ud;
    if (level >= TESSERA_LOG_ERROR) printf("[engine] %s\n", msg);
}

int main(void) {
    TesseraConfig cfg = { .width = 64, .height = 64, .pixel_density = 1.0f, .log = log_fn };
    TesseraEngine* e = tessera_create(&cfg);
    if (!e) { printf("create NULL\n"); return 1; }
    if (strcmp(tessera_backend_name(e), "none") == 0) {
        printf("SKIP: no GPU backend (%s)\n", tessera_last_error(e));
        tessera_destroy(e);
        return 0;   /* headless CI without a GPU: skip, don't fail */
    }

    /* invalid: fewer than two faces must be rejected */
    CHECK(make_die(e, 1, 1.0f) == 0);

    /* one of each geometry: coin(2), tetra(4), cube(6), octa(8), dodeca(12),
     * icosa(20), plus a barrel(10) for the fallback path */
    #define ND 7
    uint32_t counts[ND] = { 2, 4, 6, 8, 12, 20, 10 };
    TesseraDefId defs[ND];
    for (int i = 0; i < ND; ++i) {
        defs[i] = make_die(e, counts[i], 0.9f);
        CHECK(defs[i] != 0);
        CHECK(tessera_dice_def_face_count(e, defs[i]) == counts[i]);
    }

    /* throw one of each, landing on a chosen face (< its face count) — via state */
    uint32_t want[ND] = { 1, 3, 4, 6, 11, 19, 7 };
    TesseraDicePlacement dice[ND];
    memset(dice, 0, sizeof dice);
    for (int i = 0; i < ND; ++i) {
        dice[i].id = (TesseraDiceId)(i + 1);
        dice[i].def = defs[i];
        dice[i].face = want[i];
        dice[i].position[0] = (float)(i * 2 - 6);
        dice[i].position[1] = 0.5f;
        dice[i].seed = (uint32_t)(i * 17 + 3);
        dice[i].throw_s = 1.0f;
    }
    TesseraState st = {0};
    st.dice = dice; st.dice_count = ND;
    st.camera.distance = 14.0f; st.camera.pitch = 0.8f; st.camera.fov = 0.9f;
    tessera_set_state(e, &st);

    unsigned char buf[64 * 64 * 4];
    /* first frame promotes the state and throws every die */
    CHECK(tessera_render_rgba(e, 1.0 / 60.0, 64, 64, buf, sizeof buf));
    CHECK(tessera_dice_count(e) == ND);
    CHECK(!tessera_dice_all_idle(e));

    /* the target face is fixed at throw time, before settling */
    uint32_t f0 = 999;
    CHECK(tessera_dice_face(e, 2, &f0) && f0 == want[1]);

    /* advance until settled (cap the iterations) */
    int steps = 0;
    while (!tessera_dice_all_idle(e) && steps < 600) {
        CHECK(tessera_render_rgba(e, 1.0 / 60.0, 64, 64, buf, sizeof buf));
        steps++;
    }
    CHECK(tessera_dice_all_idle(e));
    CHECK(steps > 0 && steps < 600);

    /* every die reports its requested face */
    for (int i = 0; i < ND; ++i) {
        uint32_t f = 999;
        CHECK(tessera_dice_face(e, (TesseraDiceId)(i + 1), &f));
        CHECK(f == want[i]);
    }

    /* remove one: drop it from the state — it fades then culls */
    TesseraDicePlacement fewer[ND - 1];
    int w = 0;
    for (int i = 0; i < ND; ++i) if (dice[i].id != 3) fewer[w++] = dice[i];
    st.dice = fewer; st.dice_count = ND - 1;
    tessera_set_state(e, &st);
    for (int i = 0; i < 120 && tessera_dice_count(e) == ND; ++i)
        tessera_render_rgba(e, 1.0 / 60.0, 64, 64, buf, sizeof buf);
    CHECK(tessera_dice_count(e) == ND - 1);

    /* clear the rest: state with no dice */
    st.dice = NULL; st.dice_count = 0;
    tessera_set_state(e, &st);
    for (int i = 0; i < 200 && tessera_dice_count(e) > 0; ++i)
        tessera_render_rgba(e, 1.0 / 60.0, 64, 64, buf, sizeof buf);
    CHECK(tessera_dice_count(e) == 0);
    CHECK(tessera_dice_all_idle(e));

    /* re-throw with a fresh id after everything cleared */
    TesseraDicePlacement again = { .id = 42, .def = defs[0], .face = 0,
                                   .position = {0, 0.5f, 0}, .seed = 5, .throw_s = 0.5f };
    st.dice = &again; st.dice_count = 1;
    tessera_set_state(e, &st);
    tessera_render_rgba(e, 1.0 / 60.0, 64, 64, buf, sizeof buf);
    CHECK(tessera_dice_count(e) == 1);

    tessera_destroy(e);
    printf(g_fail ? "test_dice: %d FAILED\n" : "test_dice: OK\n", g_fail);
    return g_fail ? 1 : 0;
}
