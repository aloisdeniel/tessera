/* glb_gen.h — generate a minimal skinned GLB in memory (shared by examples).
 *
 * Produces a two-joint "bending bar": the base stays planted (joint 0) while the
 * top curves (joint 1), driven by one looping "bend" animation clip. Handy for
 * exercising the M5 skinning path without shipping a binary asset.
 *
 * Header-only; include from one .c per example. Returns malloc'd bytes; the
 * caller frees them after tessera_register_entity_def() copies what it needs.
 */
#ifndef TESSERA_EXAMPLE_GLB_GEN_H
#define TESSERA_EXAMPLE_GLB_GEN_H

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint8_t* d; size_t len, cap; } TsGlbBuf;
static void tsglb_put(TsGlbBuf* b, const void* p, size_t n) {
    if (b->len + n > b->cap) { b->cap = (b->len + n) * 2 + 64; b->d = realloc(b->d, b->cap); }
    memcpy(b->d + b->len, p, n); b->len += n;
}
static void tsglb_pad4(TsGlbBuf* b, uint8_t fill) { while (b->len & 3) tsglb_put(b, &fill, 1); }

/* Build the bending-bar skinned GLB. *out_size receives the byte count. */
static uint8_t* ts_example_build_bar_glb(size_t* out_size) {
    enum { RINGS = 9 };
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

    float ibm[2][16] = {
        {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1},
        {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,-1,0,1},
    };
    const int K = 5;
    float times[5] = {0.0f, 0.5f, 1.0f, 1.5f, 2.0f};
    float degs[5]  = {0.0f, 35.0f, 0.0f, -35.0f, 0.0f};
    float rot[5][4];
    for (int i = 0; i < K; ++i) {
        float a = degs[i] * (float)M_PI / 180.0f * 0.5f;
        rot[i][0]=0; rot[i][1]=0; rot[i][2]=sinf(a); rot[i][3]=cosf(a);
    }

    TsGlbBuf bin = {0};
    size_t o_pos = bin.len; tsglb_put(&bin, pos, sizeof(float)*3*vc);
    size_t o_nrm = bin.len; tsglb_put(&bin, nrm, sizeof(float)*3*vc);
    size_t o_uv  = bin.len; tsglb_put(&bin, uv,  sizeof(float)*2*vc);
    size_t o_jnt = bin.len; tsglb_put(&bin, jnt, sizeof(uint8_t)*4*vc); tsglb_pad4(&bin, 0);
    size_t o_wgt = bin.len; tsglb_put(&bin, wgt, sizeof(float)*4*vc);
    size_t o_idx = bin.len; tsglb_put(&bin, idx, sizeof(uint16_t)*ic); tsglb_pad4(&bin, 0);
    size_t o_ibm = bin.len; tsglb_put(&bin, ibm, sizeof(float)*16*2);
    size_t o_tim = bin.len; tsglb_put(&bin, times, sizeof(float)*K);
    size_t o_rot = bin.len; tsglb_put(&bin, rot, sizeof(float)*4*K);

    char json[4096];
    int n = snprintf(json, sizeof json,
      "{\"asset\":{\"version\":\"2.0\"},"
      "\"scene\":0,\"scenes\":[{\"nodes\":[0,1]}],"
      "\"nodes\":[{\"mesh\":0,\"skin\":0},{\"children\":[2],\"translation\":[0,0,0]},"
        "{\"translation\":[0,1,0]}],"
      "\"meshes\":[{\"primitives\":[{\"attributes\":{"
        "\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2,\"JOINTS_0\":3,\"WEIGHTS_0\":4},\"indices\":5}]}],"
      "\"skins\":[{\"joints\":[1,2],\"inverseBindMatrices\":6}],"
      "\"animations\":[{\"name\":\"bend\",\"channels\":[{\"sampler\":0,\"target\":{\"node\":2,\"path\":\"rotation\"}}],"
        "\"samplers\":[{\"input\":7,\"output\":8,\"interpolation\":\"LINEAR\"}]}],"
      "\"buffers\":[{\"byteLength\":%zu}],"
      "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},"
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},"
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},"
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},"
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu}],"
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
      o_ibm, (size_t)128, o_tim, sizeof(float)*K, o_rot, sizeof(float)*4*K,
      vc, vc, vc, vc, vc, ic, K, K);

    TsGlbBuf glb = {0};
    uint32_t magic = 0x46546C67, ver = 2;
    TsGlbBuf jchunk = {0}; tsglb_put(&jchunk, json, (size_t)n); tsglb_pad4(&jchunk, ' ');
    TsGlbBuf bchunk = {0}; tsglb_put(&bchunk, bin.d, bin.len);  tsglb_pad4(&bchunk, 0);
    uint32_t total = 12 + 8 + (uint32_t)jchunk.len + 8 + (uint32_t)bchunk.len;

    tsglb_put(&glb, &magic, 4); tsglb_put(&glb, &ver, 4); tsglb_put(&glb, &total, 4);
    uint32_t jl = (uint32_t)jchunk.len, jt = 0x4E4F534A;
    tsglb_put(&glb, &jl, 4); tsglb_put(&glb, &jt, 4); tsglb_put(&glb, jchunk.d, jchunk.len);
    uint32_t bl = (uint32_t)bchunk.len, bt = 0x004E4942;
    tsglb_put(&glb, &bl, 4); tsglb_put(&glb, &bt, 4); tsglb_put(&glb, bchunk.d, bchunk.len);

    free(bin.d); free(jchunk.d); free(bchunk.d);
    *out_size = glb.len;
    return glb.d;
}

#endif /* TESSERA_EXAMPLE_GLB_GEN_H */
