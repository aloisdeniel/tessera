/* chess_gen.h — procedurally generate the six chess piece models as static GLBs.
 *
 * Each piece is a lathe (surface of revolution) of a hand-authored side profile,
 * capped top and bottom, plus a couple of extra boxes for the knight's head and
 * the king's cross. The mesh carries POSITION + NORMAL + indices and a single
 * material whose baseColorFactor tints the whole piece (white vs. black army) —
 * Tessera reads that factor and applies it as the entity tint.
 *
 * Header-only; include from one .c. Returns malloc'd GLB bytes; the caller frees
 * them after tessera_register_entity_def() copies what it needs.
 *
 * All models are authored feet-at-y=0, +Y up, and fit inside a 1x1 tile.
 */
#ifndef TESSERA_CHESS_GEN_H
#define TESSERA_CHESS_GEN_H

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Piece kinds (also used by the game logic). 0 = empty square. */
enum { CHESS_PAWN = 1, CHESS_KNIGHT, CHESS_BISHOP, CHESS_ROOK, CHESS_QUEEN, CHESS_KING };

/* --- mesh accumulator ---------------------------------------------------- */
#define CHESS_MAXV 8192
#define CHESS_MAXI 24576
typedef struct {
    float    pos[CHESS_MAXV][3];
    float    nrm[CHESS_MAXV][3];
    uint16_t idx[CHESS_MAXI];
    int      vc, ic;
} ChessMesh;

static int cm_vert(ChessMesh* m, float x, float y, float z,
                   float nx, float ny, float nz) {
    int i = m->vc++;
    m->pos[i][0] = x; m->pos[i][1] = y; m->pos[i][2] = z;
    float l = sqrtf(nx*nx + ny*ny + nz*nz); if (l < 1e-6f) l = 1.0f;
    m->nrm[i][0] = nx/l; m->nrm[i][1] = ny/l; m->nrm[i][2] = nz/l;
    return i;
}
static void cm_tri(ChessMesh* m, int a, int b, int c) {
    m->idx[m->ic++] = (uint16_t)a; m->idx[m->ic++] = (uint16_t)b; m->idx[m->ic++] = (uint16_t)c;
}
static void cm_quad(ChessMesh* m, int a, int b, int c, int d) {
    cm_tri(m, a, b, c); cm_tri(m, a, c, d);
}

/* --- lathe a (y,r) profile around the Y axis ----------------------------- */
typedef struct { float y, r; } ChessPP;

#define CHESS_SEG 28
static void cm_lathe(ChessMesh* m, const ChessPP* p, int n) {
    const int S = CHESS_SEG;
    int ring[64];   /* first vertex index of each ring, per profile point */
    for (int i = 0; i < n; ++i) {
        /* smooth 2D outward normal from neighbouring profile points */
        float y0 = p[i > 0 ? i - 1 : i].y,     r0 = p[i > 0 ? i - 1 : i].r;
        float y1 = p[i < n - 1 ? i + 1 : i].y, r1 = p[i < n - 1 ? i + 1 : i].r;
        float dy = y1 - y0, dr = r1 - r0;
        float nr = dy, ny = -dr;                 /* rotate tangent +90 deg */
        float nl = sqrtf(nr*nr + ny*ny); if (nl < 1e-6f) { nr = 1; ny = 0; nl = 1; }
        nr /= nl; ny /= nl;
        ring[i] = m->vc;
        for (int s = 0; s < S; ++s) {
            float a = 6.2831853f * (float)s / (float)S;
            float c = cosf(a), sn = sinf(a);
            cm_vert(m, p[i].r * c, p[i].y, p[i].r * sn,
                       nr * c, ny, nr * sn);
        }
    }
    for (int i = 0; i < n - 1; ++i) {
        if (p[i].r < 1e-5f && p[i + 1].r < 1e-5f) continue;
        for (int s = 0; s < S; ++s) {
            int s2 = (s + 1) % S;
            cm_quad(m, ring[i] + s, ring[i] + s2, ring[i + 1] + s2, ring[i + 1] + s);
        }
    }
    /* bottom cap (fan) */
    if (p[0].r > 1e-5f) {
        int c = cm_vert(m, 0, p[0].y, 0, 0, -1, 0);
        for (int s = 0; s < S; ++s) {
            int s2 = (s + 1) % S;
            int a = cm_vert(m, p[0].r * cosf(6.2831853f * s / S), p[0].y,
                               p[0].r * sinf(6.2831853f * s / S), 0, -1, 0);
            int b = cm_vert(m, p[0].r * cosf(6.2831853f * s2 / S), p[0].y,
                               p[0].r * sinf(6.2831853f * s2 / S), 0, -1, 0);
            cm_tri(m, c, b, a);
        }
    }
    /* top cap (fan) */
    if (p[n - 1].r > 1e-5f) {
        float ty = p[n - 1].y, tr = p[n - 1].r;
        int c = cm_vert(m, 0, ty, 0, 0, 1, 0);
        for (int s = 0; s < S; ++s) {
            int s2 = (s + 1) % S;
            int a = cm_vert(m, tr * cosf(6.2831853f * s / S), ty,
                               tr * sinf(6.2831853f * s / S), 0, 1, 0);
            int b = cm_vert(m, tr * cosf(6.2831853f * s2 / S), ty,
                               tr * sinf(6.2831853f * s2 / S), 0, 1, 0);
            cm_tri(m, c, a, b);
        }
    }
}

/* --- an axis-aligned box, optionally leaned forward about the X axis ------ */
static void cm_box(ChessMesh* m, float cx, float cy, float cz,
                   float hx, float hy, float hz, float lean) {
    float ca = cosf(lean), sa = sinf(lean);
    /* 8 corners in local space, then lean about X (rotates the y/z plane) */
    static const int sg[8][3] = {
        {-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},
        {-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}
    };
    int v[8];
    for (int i = 0; i < 8; ++i) {
        float lx = sg[i][0]*hx, ly = sg[i][1]*hy, lz = sg[i][2]*hz;
        float ry = ly*ca - lz*sa, rz = ly*sa + lz*ca;
        v[i] = cm_vert(m, cx + lx, cy + ry, cz + rz, 0, 1, 0); /* normal fixed below */
    }
    /* faces (CCW outward): -Z,+Z,-Y,+Y,-X,+X */
    static const int fc[6][4] = {
        {0,3,2,1},{4,5,6,7},{0,1,5,4},{3,7,6,2},{0,4,7,3},{1,2,6,5}
    };
    float fn[6][3] = {
        {0,0,-1},{0,0,1},{0,-1,0},{0,1,0},{-1,0,0},{1,0,0}
    };
    for (int f = 0; f < 6; ++f) {
        /* rotate the face normal by the lean too */
        float ny = fn[f][1]*ca - fn[f][2]*sa, nz = fn[f][1]*sa + fn[f][2]*ca;
        int q[4];
        for (int k = 0; k < 4; ++k) {
            int src = v[fc[f][k]];
            q[k] = cm_vert(m, m->pos[src][0], m->pos[src][1], m->pos[src][2],
                              fn[f][0], ny, nz);
        }
        cm_quad(m, q[0], q[1], q[2], q[3]);
    }
}

/* --- profiles ------------------------------------------------------------ */
static void chess_build_mesh(ChessMesh* m, int kind, int face_dir) {
    m->vc = 0; m->ic = 0;
    switch (kind) {
    case CHESS_PAWN: {
        ChessPP p[] = {
            {0.00,0.30},{0.05,0.30},{0.08,0.22},{0.11,0.14},{0.13,0.11},
            {0.25,0.10},{0.29,0.18},{0.32,0.13},{0.34,0.11},
            {0.37,0.115},{0.40,0.15},{0.47,0.16},{0.54,0.135},{0.585,0.06},{0.60,0.0}
        };
        cm_lathe(m, p, (int)(sizeof p / sizeof p[0]));
        break;
    }
    case CHESS_ROOK: {
        ChessPP p[] = {
            {0.00,0.33},{0.05,0.33},{0.08,0.25},{0.12,0.18},{0.20,0.165},
            {0.42,0.165},{0.46,0.19},{0.49,0.235},{0.60,0.235},{0.615,0.25},{0.62,0.25}
        };
        cm_lathe(m, p, (int)(sizeof p / sizeof p[0]));
        break;
    }
    case CHESS_KNIGHT: {
        ChessPP p[] = {
            {0.00,0.33},{0.05,0.33},{0.08,0.25},{0.12,0.18},{0.28,0.165},
            {0.32,0.20},{0.35,0.175}
        };
        cm_lathe(m, p, (int)(sizeof p / sizeof p[0]));
        /* neck + head lean toward the opponent (face_dir = +1 or -1 along z) */
        float d = (float)face_dir;
        cm_box(m, 0.0f, 0.46f, 0.02f*d, 0.12f, 0.16f, 0.10f, 0.35f*d);
        cm_box(m, 0.0f, 0.60f, 0.15f*d, 0.11f, 0.085f, 0.19f, 0.75f*d); /* muzzle */
        cm_box(m, 0.0f, 0.66f, -0.06f*d, 0.09f, 0.10f, 0.06f, 0.2f*d);  /* ears */
        break;
    }
    case CHESS_BISHOP: {
        ChessPP p[] = {
            {0.00,0.31},{0.05,0.31},{0.08,0.23},{0.11,0.15},{0.28,0.115},
            {0.32,0.19},{0.35,0.135},{0.37,0.115},
            {0.44,0.155},{0.52,0.165},{0.59,0.135},{0.63,0.085},
            {0.645,0.11},{0.66,0.065},
            {0.68,0.05},{0.72,0.06},{0.75,0.0}
        };
        cm_lathe(m, p, (int)(sizeof p / sizeof p[0]));
        break;
    }
    case CHESS_QUEEN: {
        ChessPP p[] = {
            {0.00,0.35},{0.05,0.35},{0.08,0.27},{0.12,0.17},{0.32,0.125},
            {0.36,0.21},{0.39,0.145},{0.41,0.125},
            {0.50,0.175},{0.58,0.165},{0.65,0.13},{0.69,0.165},
            {0.735,0.205},{0.745,0.12},
            {0.76,0.09},{0.80,0.115},{0.85,0.0}
        };
        cm_lathe(m, p, (int)(sizeof p / sizeof p[0]));
        break;
    }
    case CHESS_KING: {
        ChessPP p[] = {
            {0.00,0.36},{0.05,0.36},{0.08,0.28},{0.12,0.18},{0.34,0.135},
            {0.38,0.22},{0.41,0.15},{0.43,0.13},
            {0.52,0.185},{0.61,0.175},{0.68,0.14},{0.72,0.17},
            {0.765,0.205},{0.775,0.13},
            {0.79,0.10},{0.83,0.12},{0.86,0.10}
        };
        cm_lathe(m, p, (int)(sizeof p / sizeof p[0]));
        /* the crown cross */
        cm_box(m, 0.0f, 0.955f, 0.0f, 0.028f, 0.085f, 0.028f, 0.0f);
        cm_box(m, 0.0f, 0.945f, 0.0f, 0.075f, 0.028f, 0.028f, 0.0f);
        break;
    }
    default: {
        ChessPP p[] = {{0.0,0.3},{0.5,0.3}};
        cm_lathe(m, p, 2);
        break;
    }
    }
}

/* --- GLB serialisation --------------------------------------------------- */
typedef struct { uint8_t* d; size_t len, cap; } ChessBuf;
static void cb_put(ChessBuf* b, const void* p, size_t n) {
    if (b->len + n > b->cap) { b->cap = (b->len + n) * 2 + 64; b->d = realloc(b->d, b->cap); }
    memcpy(b->d + b->len, p, n); b->len += n;
}
static void cb_pad4(ChessBuf* b, uint8_t fill) { while (b->len & 3) cb_put(b, &fill, 1); }

/* Build a piece GLB. kind in CHESS_*, rgba is the material base color,
 * face_dir orients the knight (+1 / -1 along +z). */
static uint8_t* chess_build_piece_glb(int kind, int face_dir, const float rgba[4],
                                      size_t* out_size) {
    ChessMesh* m = (ChessMesh*)malloc(sizeof *m);
    chess_build_mesh(m, kind, face_dir);
    int vc = m->vc, ic = m->ic;

    ChessBuf bin = {0};
    size_t o_pos = bin.len; cb_put(&bin, m->pos, sizeof(float) * 3 * vc);
    size_t o_nrm = bin.len; cb_put(&bin, m->nrm, sizeof(float) * 3 * vc);
    size_t o_idx = bin.len; cb_put(&bin, m->idx, sizeof(uint16_t) * ic); cb_pad4(&bin, 0);

    char json[2048];
    int n = snprintf(json, sizeof json,
      "{\"asset\":{\"version\":\"2.0\"},"
      "\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
      "\"nodes\":[{\"mesh\":0}],"
      "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorFactor\":[%.4f,%.4f,%.4f,%.4f],"
        "\"metallicFactor\":0.05,\"roughnessFactor\":0.65}}],"
      "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1},"
        "\"indices\":2,\"material\":0}]}],"
      "\"buffers\":[{\"byteLength\":%zu}],"
      "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},"
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},"
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu}],"
      "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":%d,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":%d,\"type\":\"VEC3\"},"
        "{\"bufferView\":2,\"componentType\":5123,\"count\":%d,\"type\":\"SCALAR\"}]}",
      rgba[0], rgba[1], rgba[2], rgba[3],
      bin.len,
      o_pos, sizeof(float) * 3 * vc, o_nrm, sizeof(float) * 3 * vc,
      o_idx, sizeof(uint16_t) * ic,
      vc, vc, ic);

    ChessBuf glb = {0};
    uint32_t magic = 0x46546C67, ver = 2;
    ChessBuf jchunk = {0}; cb_put(&jchunk, json, (size_t)n); cb_pad4(&jchunk, ' ');
    ChessBuf bchunk = {0}; cb_put(&bchunk, bin.d, bin.len);  cb_pad4(&bchunk, 0);
    uint32_t total = 12 + 8 + (uint32_t)jchunk.len + 8 + (uint32_t)bchunk.len;

    cb_put(&glb, &magic, 4); cb_put(&glb, &ver, 4); cb_put(&glb, &total, 4);
    uint32_t jl = (uint32_t)jchunk.len, jt = 0x4E4F534A;
    cb_put(&glb, &jl, 4); cb_put(&glb, &jt, 4); cb_put(&glb, jchunk.d, jchunk.len);
    uint32_t bl = (uint32_t)bchunk.len, bt = 0x004E4942;
    cb_put(&glb, &bl, 4); cb_put(&glb, &bt, 4); cb_put(&glb, bchunk.d, bchunk.len);

    free(bin.d); free(jchunk.d); free(bchunk.d); free(m);
    *out_size = glb.len;
    return glb.d;
}

#endif /* TESSERA_CHESS_GEN_H */
