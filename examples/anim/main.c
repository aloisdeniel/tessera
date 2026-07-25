/* examples/anim — skeletal animation (M5).
 *
 * Generates a minimal skinned GLB in memory (a two-joint bending bar with one
 * "bend" animation clip), registers it as an entity def, and captures the pose
 * at several times to prove GPU skinning + the clip sampler work end to end.
 *
 *   anim_rest.png   — clip at t=0 (upright)
 *   anim_bendA.png  — clip mid-swing one way
 *   anim_bendB.png  — clip mid-swing the other way
 *   anim_walkmove.png — the bar plays its move clip while hopping to a new tile
 *
 * Run:  example_anim <out_dir>
 */
#include "tessera.h"
#include "../common/glb_gen.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if 0 /* generator now lives in ../common/glb_gen.h (ts_example_build_bar_glb) */
static uint8_t* build_bar_glb(size_t* out_size) {
    enum { RINGS = 9 };                 /* 8 vertical segments */
    const float H = 2.0f, HALF = 0.22f;
    float pos[RINGS * 4][3], nrm[RINGS * 4][3], uv[RINGS * 4][2], wgt[RINGS * 4][4];
    uint8_t jnt[RINGS * 4][4];
    uint16_t idx[8 * 4 * 6];
    int vc = 0, ic = 0;

    const float cx[4] = {-HALF, HALF, HALF, -HALF};
    const float cz[4] = {-HALF, -HALF, HALF, HALF};
    for (int k = 0; k < RINGS; ++k) {
        float y = H * (float)k / (float)(RINGS - 1);
        float w1 = (y - 0.5f) / 1.0f; if (w1 < 0) w1 = 0; if (w1 > 1) w1 = 1;
        for (int s = 0; s < 4; ++s) {
            pos[vc][0] = cx[s]; pos[vc][1] = y; pos[vc][2] = cz[s];
            float nl = sqrtf(cx[s]*cx[s] + cz[s]*cz[s]);
            nrm[vc][0] = cx[s]/nl; nrm[vc][1] = 0; nrm[vc][2] = cz[s]/nl;
            uv[vc][0] = s / 4.0f; uv[vc][1] = y / H;
            jnt[vc][0] = 0; jnt[vc][1] = 1; jnt[vc][2] = 0; jnt[vc][3] = 0;
            wgt[vc][0] = 1.0f - w1; wgt[vc][1] = w1; wgt[vc][2] = 0; wgt[vc][3] = 0;
            ++vc;
        }
    }
    for (int k = 0; k < RINGS - 1; ++k)
        for (int s = 0; s < 4; ++s) {
            int a = k*4 + s, b = k*4 + (s+1)%4, c = (k+1)*4 + (s+1)%4, d = (k+1)*4 + s;
            idx[ic++]=a; idx[ic++]=b; idx[ic++]=c;
            idx[ic++]=a; idx[ic++]=c; idx[ic++]=d;
        }

    /* inverse-bind: joint0 identity; joint1 translate(0,-1,0) */
    float ibm[2][16] = {
        {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1},
        {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,-1,0,1},
    };
    /* animation: joint1 rotation about Z, 0 -> +35 -> 0 -> -35 -> 0 over 2s */
    const int K = 5;
    float times[5] = {0.0f, 0.5f, 1.0f, 1.5f, 2.0f};
    float degs[5]  = {0.0f, 35.0f, 0.0f, -35.0f, 0.0f};
    float rot[5][4];
    for (int i = 0; i < K; ++i) {
        float a = degs[i] * (float)M_PI / 180.0f * 0.5f;
        rot[i][0]=0; rot[i][1]=0; rot[i][2]=sinf(a); rot[i][3]=cosf(a);
    }

    /* pack the binary buffer */
    Buf bin = {0};
    size_t o_pos = bin.len; buf_put(&bin, pos, sizeof(float)*3*vc);
    size_t o_nrm = bin.len; buf_put(&bin, nrm, sizeof(float)*3*vc);
    size_t o_uv  = bin.len; buf_put(&bin, uv,  sizeof(float)*2*vc);
    size_t o_jnt = bin.len; buf_put(&bin, jnt, sizeof(uint8_t)*4*vc); buf_pad4(&bin, 0);
    size_t o_wgt = bin.len; buf_put(&bin, wgt, sizeof(float)*4*vc);
    size_t o_idx = bin.len; buf_put(&bin, idx, sizeof(uint16_t)*ic); buf_pad4(&bin, 0);
    size_t o_ibm = bin.len; buf_put(&bin, ibm, sizeof(float)*16*2);
    size_t o_tim = bin.len; buf_put(&bin, times, sizeof(float)*K);
    size_t o_rot = bin.len; buf_put(&bin, rot, sizeof(float)*4*K);

    char json[4096];
    int n = snprintf(json, sizeof json,
      "{\"asset\":{\"version\":\"2.0\"},"
      "\"scene\":0,\"scenes\":[{\"nodes\":[0,1]}],"
      "\"nodes\":["
        "{\"mesh\":0,\"skin\":0},"
        "{\"children\":[2],\"translation\":[0,0,0]},"
        "{\"translation\":[0,1,0]}],"
      "\"meshes\":[{\"primitives\":[{\"attributes\":{"
        "\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2,\"JOINTS_0\":3,\"WEIGHTS_0\":4},"
        "\"indices\":5}]}],"
      "\"skins\":[{\"joints\":[1,2],\"inverseBindMatrices\":6}],"
      "\"animations\":[{\"name\":\"bend\",\"channels\":[{\"sampler\":0,\"target\":{\"node\":2,\"path\":\"rotation\"}}],"
        "\"samplers\":[{\"input\":7,\"output\":8,\"interpolation\":\"LINEAR\"}]}],"
      "\"buffers\":[{\"byteLength\":%zu}],"
      "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},"   /* 0 pos */
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},"   /* 1 nrm */
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},"   /* 2 uv  */
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},"   /* 3 jnt */
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},"   /* 4 wgt */
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},"   /* 5 idx */
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},"   /* 6 ibm */
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},"   /* 7 tim */
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu}],"  /* 8 rot */
      "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":%d,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":%d,\"type\":\"VEC3\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":%d,\"type\":\"VEC2\"},"
        "{\"bufferView\":3,\"componentType\":5121,\"count\":%d,\"type\":\"VEC4\"},"
        "{\"bufferView\":4,\"componentType\":5126,\"count\":%d,\"type\":\"VEC4\"},"
        "{\"bufferView\":5,\"componentType\":5123,\"count\":%d,\"type\":\"SCALAR\"},"
        "{\"bufferView\":6,\"componentType\":5126,\"count\":2,\"type\":\"MAT4\"},"
        "{\"bufferView\":7,\"componentType\":5126,\"count\":%d,\"type\":\"SCALAR\"},"
        "{\"bufferView\":8,\"componentType\":5126,\"count\":%d,\"type\":\"VEC4\"}]}",
      bin.len,
      o_pos, sizeof(float)*3*vc, o_nrm, sizeof(float)*3*vc, o_uv, sizeof(float)*2*vc,
      o_jnt, (size_t)(4*vc), o_wgt, sizeof(float)*4*vc, o_idx, sizeof(uint16_t)*ic,
      o_ibm, (size_t)(128), o_tim, sizeof(float)*K, o_rot, sizeof(float)*4*K,
      vc, vc, vc, vc, vc, ic, K, K);

    /* assemble GLB: header + JSON chunk + BIN chunk */
    Buf glb = {0};
    uint32_t magic = 0x46546C67, ver = 2, zero = 0;
    Buf jchunk = {0}; buf_put(&jchunk, json, (size_t)n); buf_pad4(&jchunk, ' ');
    Buf bchunk = {0}; buf_put(&bchunk, bin.d, bin.len);  buf_pad4(&bchunk, 0);
    uint32_t total = 12 + 8 + (uint32_t)jchunk.len + 8 + (uint32_t)bchunk.len;

    buf_put(&glb, &magic, 4); buf_put(&glb, &ver, 4); buf_put(&glb, &total, 4);
    uint32_t jl = (uint32_t)jchunk.len, jt = 0x4E4F534A; /* JSON */
    buf_put(&glb, &jl, 4); buf_put(&glb, &jt, 4); buf_put(&glb, jchunk.d, jchunk.len);
    uint32_t bl = (uint32_t)bchunk.len, bt = 0x004E4942; /* BIN\0 */
    buf_put(&glb, &bl, 4); buf_put(&glb, &bt, 4); buf_put(&glb, bchunk.d, bchunk.len);
    (void)zero;

    free(bin.d); free(jchunk.d); free(bchunk.d);
    *out_size = glb.len;
    return glb.d;
}
#endif

/* ------------------------------------------------------------- example */
static void logfn(void* ud, int level, const char* msg) {
    (void)ud; (void)level; fprintf(stderr, "  %s\n", msg);
}
static void settle(TesseraEngine* e, double secs) {
    const double step = 1.0 / 120.0;
    for (double t = 0; t < secs; t += step) tessera_tick(e, step);
}

int main(int argc, char** argv) {
    const char* dir = (argc > 1) ? argv[1] : ".";
    char path[512];

    TesseraConfig cfg = { .width = 1280, .height = 720, .pixel_density = 1.0f, .log = logfn };
    TesseraEngine* e = tessera_create(&cfg);
    if (!e || strlen(tessera_last_error(e))) {
        fprintf(stderr, "init failed: %s\n", e ? tessera_last_error(e) : "null");
        return 1;
    }

    /* optional second arg: shadow mode (none/blob/map) for the captures */
    if (argc > 2) {
        TesseraQuality q = { .shadows = TESSERA_SHADOW_BLOB, .msaa = 1, .render_scale = 1.0f };
        if (!strcmp(argv[2], "none")) q.shadows = TESSERA_SHADOW_NONE;
        if (!strcmp(argv[2], "map"))  q.shadows = TESSERA_SHADOW_MAP;
        tessera_set_quality(e, &q);
    }

    size_t glb_size = 0;
    uint8_t* glb = ts_example_build_bar_glb(&glb_size);
    printf("generated skinned GLB: %zu bytes\n", glb_size);

    TesseraTileDef floor = { .thickness = 0.25f, .tint = {0.55f, 0.57f, 0.62f, 1} };
    TesseraDefId d_floor = tessera_register_tile_def(e, &floor);
    TesseraEntityDef bar = {
        .gltf = { .data = glb, .size = glb_size, .debug_name = "bar" },
        .scale = 1.0f, .move_anim = 0,   /* clip 0 = "bend"; reuse as the move clip */
    };
    TesseraDefId d_bar = tessera_register_entity_def(e, &bar);
    free(glb);

    printf("bar anim clips: %u (name0=%s)\n",
           tessera_entity_def_anim_count(e, d_bar),
           tessera_entity_def_anim_name(e, d_bar, 0));

    TesseraTilePlacement tiles[25]; size_t nt = 0;
    for (int z = -2; z <= 2; ++z) for (int x = -2; x <= 2; ++x)
        tiles[nt++] = (TesseraTilePlacement){ .coord = {x, z}, .tile_def = d_floor };

    TesseraEntityPlacement ents[] = { { .id = 1, .def = d_bar, .coord = {0, 0}, .anim = 0 } };
    TesseraCamera cam = { .focus = {0, 0}, .distance = 7.0f, .yaw = 0.5f, .pitch = 0.35f, .fov = 0.9f };
    TesseraState s = { .tiles = tiles, .tile_count = nt, .entities = ents, .entity_count = 1,
                       .camera = cam, .epoch = 1 };
    tessera_set_state(e, &s);

    settle(e, 0.02);
    snprintf(path, sizeof path, "%s/anim_rest.png", dir);
    printf("rest:  %s\n", tessera_capture_png(e, 1280, 720, path) ? "OK" : "FAIL");

    settle(e, 0.5);   /* clip t~0.5 -> +35 deg bend */
    snprintf(path, sizeof path, "%s/anim_bendA.png", dir);
    printf("bendA: %s\n", tessera_capture_png(e, 1280, 720, path) ? "OK" : "FAIL");

    settle(e, 1.0);   /* clip t~1.5 -> -35 deg bend */
    snprintf(path, sizeof path, "%s/anim_bendB.png", dir);
    printf("bendB: %s\n", tessera_capture_png(e, 1280, 720, path) ? "OK" : "FAIL");

    tessera_destroy(e);
    return 0;
}
