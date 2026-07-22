/*
 * dice.c — procedural dice: model + atlas generation, throw simulation, drawlist.
 *
 * Model generation packs the N face sprites into one grid atlas (with gutters
 * and a spare cell for the plain caps) and builds a convex mesh whose faces
 * carry baked UVs into those cells — a cube for 6 faces, a two-sided token for
 * 2, an N-gonal barrel otherwise. Each face also gets a *rest orientation*: the
 * rotation that turns its outward normal to +Y, so the die can settle face-up.
 *
 * A thrown die spawns airborne and tumbles along a precomputed trajectory whose
 * spin winds down to exactly the target face's rest orientation, so it always
 * lands on the requested face. Removal fades + shrinks the die out.
 */
#include "dice/dice.h"

#include "engine.h"
#include "registry.h"
#include "anim/anim.h"
#include "stb_image.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define DICE_DEFAULT_THROW_S 1.15f
#define DICE_CELL_PX         256      /* atlas cell resolution                 */
#define DICE_CELL_PAD        10       /* gutter around each cell (px), no bleed */
#define DICE_DISC_SEG        48       /* segments for coin discs / prism caps  */

/* ==========================================================================
 *  Image helpers
 * ======================================================================= */

/* Resolve TesseraBytes (path or in-memory) to a malloc'd buffer, like the
 * registry does for atlas/gltf inputs. */
static void* dice_read_bytes(const TesseraBytes* b, size_t* out_size) {
    if (b->data && b->size) {
        void* copy = malloc(b->size);
        if (!copy) return NULL;
        memcpy(copy, b->data, b->size);
        *out_size = b->size;
        return copy;
    }
    if (b->path) {
        FILE* f = fopen(b->path, "rb");
        if (!f) return NULL;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n <= 0) { fclose(f); return NULL; }
        void* buf = malloc((size_t)n);
        size_t rd = buf ? fread(buf, 1, (size_t)n, f) : 0;
        fclose(f);
        if (!buf || rd != (size_t)n) { free(buf); return NULL; }
        *out_size = (size_t)n;
        return buf;
    }
    return NULL;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* Bilinear-resample RGBA src (sw*sh) into a (tw*th) block at (x0,y0) of the
 * dst RGBA image (dw*dh). */
static void blit_resampled(uint8_t* dst, int dw, int dh, int x0, int y0,
                           int tw, int th, const uint8_t* src, int sw, int sh) {
    for (int y = 0; y < th; ++y) {
        float fy = ((y + 0.5f) / th) * sh - 0.5f;
        int y1 = (int)floorf(fy); float wy = fy - (float)y1;
        int ya = clampi(y1, 0, sh - 1), yb = clampi(y1 + 1, 0, sh - 1);
        int dy = y0 + y; if (dy < 0 || dy >= dh) continue;
        for (int x = 0; x < tw; ++x) {
            float fx = ((x + 0.5f) / tw) * sw - 0.5f;
            int x1 = (int)floorf(fx); float wx = fx - (float)x1;
            int xa = clampi(x1, 0, sw - 1), xb = clampi(x1 + 1, 0, sw - 1);
            int dxp = x0 + x; if (dxp < 0 || dxp >= dw) continue;
            uint8_t* out = &dst[(dy * dw + dxp) * 4];
            for (int c = 0; c < 4; ++c) {
                float p00 = src[(ya * sw + xa) * 4 + c];
                float p10 = src[(ya * sw + xb) * 4 + c];
                float p01 = src[(yb * sw + xa) * 4 + c];
                float p11 = src[(yb * sw + xb) * 4 + c];
                float top = p00 + (p10 - p00) * wx;
                float bot = p01 + (p11 - p01) * wx;
                out[c] = (uint8_t)(top + (bot - top) * wy + 0.5f);
            }
        }
    }
}

/* UV rect (inset by half a texel) covering cell `index` of a cols*rows grid. */
static void cell_uv(uint32_t index, int cols, int atlas_w, int atlas_h, vec4 out) {
    int col = (int)index % cols, row = (int)index / cols;
    int x0 = col * DICE_CELL_PX + DICE_CELL_PAD;
    int y0 = row * DICE_CELL_PX + DICE_CELL_PAD;
    int inner = DICE_CELL_PX - 2 * DICE_CELL_PAD;
    out[0] = (x0 + 0.5f) / atlas_w;
    out[1] = (y0 + 0.5f) / atlas_h;
    out[2] = (x0 + inner - 0.5f) / atlas_w;
    out[3] = (y0 + inner - 0.5f) / atlas_h;
}

/* Decode the N face sprites, pack them into a grid atlas (+ one spare cell for
 * caps when `spare`), upload it, and return the per-cell UV rects (cells+1 when
 * spare). Returns false on failure. */
static bool build_atlas(TsGpu* gpu, TsArena* arena, const TsLog* log,
                        const TesseraDiceDef* def, bool spare,
                        TsTexture* out_tex, vec4** out_uv, int* out_cols,
                        char* err, size_t err_sz) {
    uint32_t nfaces = (uint32_t)def->face_count;
    uint32_t cells = nfaces + (spare ? 1u : 0u);
    int cols = (int)ceilf(sqrtf((float)cells));
    if (cols < 1) cols = 1;
    int rows = (int)((cells + cols - 1) / cols);
    int aw = cols * DICE_CELL_PX, ah = rows * DICE_CELL_PX;

    uint8_t* atlas = (uint8_t*)calloc((size_t)aw * ah * 4, 1);
    if (!atlas) { snprintf(err, err_sz, "dice: atlas alloc failed"); return false; }

    int inner = DICE_CELL_PX - 2 * DICE_CELL_PAD;
    for (uint32_t f = 0; f < nfaces; ++f) {
        size_t sz = 0;
        void* bytes = dice_read_bytes(&def->faces[f].sprite, &sz);
        int sw = 0, sh = 0, comp = 0;
        stbi_uc* px = bytes ? stbi_load_from_memory((const stbi_uc*)bytes, (int)sz,
                                                    &sw, &sh, &comp, 4) : NULL;
        free(bytes);
        int col = (int)f % cols, row = (int)f / cols;
        int x0 = col * DICE_CELL_PX + DICE_CELL_PAD;
        int y0 = row * DICE_CELL_PX + DICE_CELL_PAD;
        if (!px) {
            /* Missing/undecodable sprite: fill the cell with a flat mid-grey so
             * the die still renders (a face with no art). */
            TS_LOGW(log, "dice: face %u sprite decode failed; using blank", f);
            for (int y = 0; y < inner; ++y)
                for (int x = 0; x < inner; ++x) {
                    uint8_t* o = &atlas[((y0 + y) * aw + (x0 + x)) * 4];
                    o[0] = o[1] = o[2] = 200; o[3] = 255;
                }
            continue;
        }
        blit_resampled(atlas, aw, ah, x0, y0, inner, inner, px, sw, sh);
        stbi_image_free(px);
    }

    /* Spare cell (caps): opaque light-grey plastic body colour. */
    if (spare) {
        int col = (int)nfaces % cols, row = (int)nfaces / cols;
        int x0 = col * DICE_CELL_PX + DICE_CELL_PAD;
        int y0 = row * DICE_CELL_PX + DICE_CELL_PAD;
        for (int y = 0; y < inner; ++y)
            for (int x = 0; x < inner; ++x) {
                uint8_t* o = &atlas[((y0 + y) * aw + (x0 + x)) * 4];
                o[0] = o[1] = o[2] = 205; o[3] = 255;
            }
    }

    bool ok = ts_gpu_upload_texture(gpu, atlas, (uint32_t)aw, (uint32_t)ah, out_tex);
    free(atlas);
    if (!ok) { snprintf(err, err_sz, "dice: atlas GPU upload failed"); return false; }

    vec4* uv = TS_ARENA_ARR(arena, vec4, cells);
    if (!uv) { ts_gpu_free_texture(gpu, out_tex); snprintf(err, err_sz, "dice: uv alloc failed"); return false; }
    for (uint32_t c = 0; c < cells; ++c) cell_uv(c, cols, aw, ah, uv[c]);
    *out_uv = uv; *out_cols = cols;
    return true;
}

/* ==========================================================================
 *  Mesh accumulator (auto-oriented winding against a desired outward normal)
 * ======================================================================= */
typedef struct {
    TesseraVertex* v; uint32_t vc, vcap;
    uint32_t*      i; uint32_t ic, icap;
} DiceMesh;

static uint32_t dm_vert(DiceMesh* m, vec3 p, vec3 n, float u, float w) {
    if (m->vc == m->vcap) {
        m->vcap = m->vcap ? m->vcap * 2 : 64;
        m->v = (TesseraVertex*)realloc(m->v, m->vcap * sizeof(TesseraVertex));
    }
    uint32_t idx = m->vc++;
    TesseraVertex* vv = &m->v[idx];
    vv->pos[0] = p[0]; vv->pos[1] = p[1]; vv->pos[2] = p[2];
    vv->normal[0] = n[0]; vv->normal[1] = n[1]; vv->normal[2] = n[2];
    vv->uv[0] = u; vv->uv[1] = w;
    return idx;
}
static void dm_idx3(DiceMesh* m, uint32_t a, uint32_t b, uint32_t c) {
    if (m->ic + 3 > m->icap) {
        m->icap = m->icap ? m->icap * 2 : 128;
        m->i = (uint32_t*)realloc(m->i, m->icap * sizeof(uint32_t));
    }
    m->i[m->ic++] = a; m->i[m->ic++] = b; m->i[m->ic++] = c;
}
/* Emit triangle (a,b,c) wound so its front face points along `desired`. */
static void dm_tri(DiceMesh* m, uint32_t a, uint32_t b, uint32_t c, vec3 desired) {
    vec3 e1, e2, fn;
    glm_vec3_sub(m->v[b].pos, m->v[a].pos, e1);
    glm_vec3_sub(m->v[c].pos, m->v[a].pos, e2);
    glm_vec3_cross(e1, e2, fn);
    if (glm_vec3_dot(fn, desired) >= 0.0f) dm_idx3(m, a, b, c);
    else                                   dm_idx3(m, a, c, b);
}

/* Map a face-local UV (lu,lw) in [0,1] into atlas cell rect `r`. */
static void map_uv(const vec4 r, float lu, float lw, float* u, float* w) {
    *u = r[0] + lu * (r[2] - r[0]);
    *w = r[1] + lw * (r[3] - r[1]);
}

/* Add a planar quad (corners CCW-ish; winding auto-fixed) with face normal `n`,
 * local UVs at the four corners mapped through atlas rect `r`. */
static void add_quad(DiceMesh* m, vec3 c0, vec3 c1, vec3 c2, vec3 c3, vec3 n,
                     const vec4 r, const float luv[4][2]) {
    float u, w;
    map_uv(r, luv[0][0], luv[0][1], &u, &w); uint32_t a = dm_vert(m, c0, n, u, w);
    map_uv(r, luv[1][0], luv[1][1], &u, &w); uint32_t b = dm_vert(m, c1, n, u, w);
    map_uv(r, luv[2][0], luv[2][1], &u, &w); uint32_t c = dm_vert(m, c2, n, u, w);
    map_uv(r, luv[3][0], luv[3][1], &u, &w); uint32_t d = dm_vert(m, c3, n, u, w);
    dm_tri(m, a, b, c, n);
    dm_tri(m, a, c, d, n);
}

/* Rest orientation for a face: the rotation that turns `normal` to +Y. */
static void face_rest(vec3 normal, versor out) {
    vec3 up = {0.0f, 1.0f, 0.0f};
    vec3 n; glm_vec3_normalize_to(normal, n);
    glm_quat_from_vecs(n, up, out);
    glm_quat_normalize(out);
}

/* Sprite tangent frame for a face with outward normal `n`: `right` and `up` span
 * the face so the sprite reads upright (its +v points toward world +Y, projected
 * onto the face) and NON-mirrored when viewed from outside. Handedness: the
 * texture u-axis is `right`, the texture v-axis is `-up` (image v grows down), so
 * u×v = right×(-up) = -n — the orientation-preserving choice a viewer outside
 * (looking down -n) sees un-flipped. Using a per-face ad-hoc basis instead is
 * what mirrored the -Y/-X/+Z cube faces. */
static void face_uv_frame(vec3 n, vec3 right, vec3 up) {
    vec3 hint = {0.0f, 1.0f, 0.0f};
    if (fabsf(glm_vec3_dot(n, hint)) > 0.98f) { hint[0]=0; hint[1]=0; hint[2]=1; }
    float d = glm_vec3_dot(hint, n);
    up[0] = hint[0] - d*n[0]; up[1] = hint[1] - d*n[1]; up[2] = hint[2] - d*n[2];
    glm_vec3_normalize(up);
    glm_vec3_cross(up, n, right);   /* right × up = +n (viewer side) */
    glm_vec3_normalize(right);
}

/* ==========================================================================
 *  Geometry
 *    coin (2), cube (6), Platonic solids (4/8/12/20), barrel prism (other N)
 * ======================================================================= */

/* A square face centred at `n*R`, spanning ±R along an upright, non-mirrored
 * tangent frame; the sprite fills the face edge-to-edge. */
static void add_square_face(DiceMesh* m, vec3 n, float R, const vec4 uvr, versor rest) {
    vec3 right, up; face_uv_frame(n, right, up);
    vec3 t; glm_vec3_copy(right, t);      /* texture u-axis */
    vec3 s; glm_vec3_negate_to(up, s);    /* texture v-axis (down) */
    static const float SQ[4][2] = {{0,0},{1,0},{1,1},{0,1}};
    vec3 cor[4];
    for (int k = 0; k < 4; ++k) {
        float du = (SQ[k][0]*2.0f - 1.0f) * R;
        float dv = (SQ[k][1]*2.0f - 1.0f) * R;
        cor[k][0] = n[0]*R + t[0]*du + s[0]*dv;
        cor[k][1] = n[1]*R + t[1]*du + s[1]*dv;
        cor[k][2] = n[2]*R + t[2]*du + s[2]*dv;
    }
    add_quad(m, cor[0], cor[1], cor[2], cor[3], n, uvr, SQ);
    face_rest(n, rest);
}

/* A flat convex-polygon face from `nv` coplanar vertices (any order). The
 * outward normal is taken from the centroid direction (valid for our
 * origin-centred solids). The square sprite is mapped centred on the face's
 * circumscribed circle, so it is centred and fills the face; the polygon is
 * triangulated as a fan. */
static void add_polygon_face(DiceMesh* m, const vec3* v, int nv, const vec4 uvr,
                             versor rest) {
    vec3 c = {0,0,0};
    for (int i = 0; i < nv; ++i) glm_vec3_add(c, (float*)v[i], c);
    glm_vec3_scale(c, 1.0f/(float)nv, c);
    vec3 n; glm_vec3_normalize_to(c, n);
    vec3 right, up; face_uv_frame(n, right, up);

    float rad = 0.0f;
    for (int i = 0; i < nv; ++i) {
        vec3 d; glm_vec3_sub((float*)v[i], c, d);
        float l = glm_vec3_norm(d); if (l > rad) rad = l;
    }
    if (rad < 1e-6f) rad = 1.0f;

    /* order the vertices into a ring by angle around the centroid */
    int order[16]; float ang[16];
    for (int i = 0; i < nv; ++i) {
        vec3 d; glm_vec3_sub((float*)v[i], c, d);
        ang[i] = atan2f(glm_vec3_dot(d, up), glm_vec3_dot(d, right));
        order[i] = i;
    }
    for (int a = 1; a < nv; ++a) {
        int key = order[a]; float ka = ang[key]; int b = a;
        while (b > 0 && ang[order[b-1]] > ka) { order[b] = order[b-1]; --b; }
        order[b] = key;
    }

    float cu, cw; map_uv(uvr, 0.5f, 0.5f, &cu, &cw);
    uint32_t ci = dm_vert(m, c, n, cu, cw);
    uint32_t vi[16];
    for (int k = 0; k < nv; ++k) {
        const float* p = v[order[k]];
        vec3 d; glm_vec3_sub((float*)p, c, d);
        float lu = 0.5f + 0.5f * glm_vec3_dot(d, right) / rad;
        float lw = 0.5f - 0.5f * glm_vec3_dot(d, up) / rad;
        float u, w; map_uv(uvr, lu, lw, &u, &w);
        vi[k] = dm_vert(m, (float*)p, n, u, w);
    }
    for (int k = 0; k < nv; ++k)
        dm_tri(m, ci, vi[k], vi[(k+1)%nv], n);
    face_rest(n, rest);
}

/* Emit `nfaces` faces of `face_size` vertices each from a shared vertex table,
 * with every vertex projected onto the sphere of radius R (regular solid). */
static void emit_solid(DiceMesh* m, float R, const float (*V)[3], int nverts,
                       const int* faces, int nfaces, int face_size,
                       const vec4* uv, versor* rest) {
    vec3* NV = (vec3*)malloc((size_t)nverts * sizeof(vec3));
    for (int i = 0; i < nverts; ++i) {
        vec3 p = { V[i][0], V[i][1], V[i][2] };
        glm_vec3_normalize(p);
        glm_vec3_scale(p, R, NV[i]);
    }
    for (int f = 0; f < nfaces; ++f) {
        vec3 fv[16];
        for (int k = 0; k < face_size; ++k)
            glm_vec3_copy(NV[faces[f*face_size + k]], fv[k]);
        add_polygon_face(m, fv, face_size, uv[f], rest[f]);
    }
    free(NV);
}

/* Cube (d6): six square faces, sprite filling each edge-to-edge. */
static void build_cube(DiceMesh* m, float R, const vec4* uv, versor* rest) {
    static const float FN[6][3] = {
        {0,1,0},{0,-1,0},{1,0,0},{-1,0,0},{0,0,1},{0,0,-1}
    };
    for (int f = 0; f < 6; ++f) {
        vec3 n = { FN[f][0], FN[f][1], FN[f][2] };
        add_square_face(m, n, R, uv[f], rest[f]);
    }
}

/* d4 "read-at-apex" rest: rotate the model so the chosen face sits in a natural
 * resting posture — the die balanced on the opposite face, this face tilted up
 * toward the viewer with its apex vertex at the top so the numeral reads there,
 * exactly how a real d4 is read. `n`/`up` are the face's model-space normal and
 * its in-plane direction toward the apex; they map to the world normal (elevated
 * by the tetra's rest angle, arccos(1/3) above horizontal) and world up. */
static void tetra_rest(vec3 n, vec3 up, versor out) {
    vec3 right; glm_vec3_cross(up, n, right); glm_vec3_normalize(right);
    const float s = 1.0f/3.0f, c = 0.94280904f;   /* sin/cos of the rest tilt */
    vec3 Nw = {0, s, c}, Uw = {0, c, -s};
    vec3 Tw; glm_vec3_cross(Uw, Nw, Tw); glm_vec3_normalize(Tw);
    mat4 M, W; glm_mat4_identity(M); glm_mat4_identity(W);
    for (int k = 0; k < 3; ++k) {
        M[0][k] = right[k]; M[1][k] = up[k]; M[2][k] = n[k];   /* model basis  */
        W[0][k] = Tw[k];    W[1][k] = Uw[k]; W[2][k] = Nw[k];  /* world target */
    }
    mat4 Mt, Q; glm_mat4_transpose_to(M, Mt); glm_mat4_mul(W, Mt, Q);
    glm_mat4_quat(Q, out); glm_quat_normalize(out);
}

/* One triangular d4 face. Unlike the other solids the sprite "up" is baked
 * toward `apex` (its first vertex), so tetra_rest can present that vertex at the
 * top with the numeral upright. */
static void add_tetra_face(DiceMesh* m, vec3 apex, vec3 b1, vec3 b2,
                           const vec4 uvr, versor rest) {
    vec3 v[3]; glm_vec3_copy(apex, v[0]); glm_vec3_copy(b1, v[1]); glm_vec3_copy(b2, v[2]);
    vec3 cen; glm_vec3_add(v[0], v[1], cen); glm_vec3_add(cen, v[2], cen);
    glm_vec3_scale(cen, 1.0f/3.0f, cen);
    vec3 n; glm_vec3_normalize_to(cen, n);
    vec3 mid; glm_vec3_add(b1, b2, mid); glm_vec3_scale(mid, 0.5f, mid);
    vec3 up; glm_vec3_sub(apex, mid, up);
    float d = glm_vec3_dot(up, n); vec3 tmp; glm_vec3_scale(n, d, tmp);
    glm_vec3_sub(up, tmp, up); glm_vec3_normalize(up);
    vec3 right; glm_vec3_cross(up, n, right); glm_vec3_normalize(right);

    float rad = 0.0f;
    for (int i = 0; i < 3; ++i) {
        vec3 dd; glm_vec3_sub(v[i], cen, dd);
        float l = glm_vec3_norm(dd); if (l > rad) rad = l;
    }
    if (rad < 1e-6f) rad = 1.0f;

    float cu, cw; map_uv(uvr, 0.5f, 0.5f, &cu, &cw);
    uint32_t ci = dm_vert(m, cen, n, cu, cw);
    uint32_t vi[3];
    for (int k = 0; k < 3; ++k) {
        vec3 rel; glm_vec3_sub(v[k], cen, rel);
        float lu = 0.5f + 0.5f * glm_vec3_dot(rel, right) / rad;
        float lw = 0.5f - 0.5f * glm_vec3_dot(rel, up) / rad;
        float u, w; map_uv(uvr, lu, lw, &u, &w);
        vi[k] = dm_vert(m, v[k], n, u, w);
    }
    for (int k = 0; k < 3; ++k) dm_tri(m, ci, vi[k], vi[(k+1)%3], n);
    tetra_rest(n, up, rest);
}

/* Tetrahedron (d4): four triangular faces, read at the apex. */
static void build_tetra(DiceMesh* m, float R, const vec4* uv, versor* rest) {
    static const float Vr[4][3] = { {1,1,1},{1,-1,-1},{-1,1,-1},{-1,-1,1} };
    static const int F[4][3] = { {1,2,3},{0,3,2},{0,1,3},{0,2,1} };
    vec3 V[4];
    for (int i = 0; i < 4; ++i) {
        vec3 p = { Vr[i][0], Vr[i][1], Vr[i][2] };
        glm_vec3_normalize(p); glm_vec3_scale(p, R, V[i]);
    }
    for (int f = 0; f < 4; ++f)   /* apex = the face's first vertex */
        add_tetra_face(m, V[F[f][0]], V[F[f][1]], V[F[f][2]], uv[f], rest[f]);
}

/* Octahedron (d8): eight triangular faces. */
static void build_octa(DiceMesh* m, float R, const vec4* uv, versor* rest) {
    static const float V[6][3] = {
        {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}
    };
    static const int F[8*3] = {
        0,2,4, 0,4,3, 0,3,5, 0,5,2,
        1,4,2, 1,3,4, 1,5,3, 1,2,5
    };
    emit_solid(m, R, V, 6, F, 8, 3, uv, rest);
}

/* Icosahedron (d20): twenty triangular faces (classic icosphere connectivity). */
static void build_icosa(DiceMesh* m, float R, const vec4* uv, versor* rest) {
    const float t = 1.61803399f;   /* golden ratio */
    static float V[12][3];
    const float src[12][3] = {
        {-1, t, 0},{ 1, t, 0},{-1,-t, 0},{ 1,-t, 0},
        { 0,-1, t},{ 0, 1, t},{ 0,-1,-t},{ 0, 1,-t},
        { t, 0,-1},{ t, 0, 1},{-t, 0,-1},{-t, 0, 1}
    };
    memcpy(V, src, sizeof V); (void)t;
    static const int F[20*3] = {
        0,11,5,  0,5,1,   0,1,7,   0,7,10,  0,10,11,
        1,5,9,   5,11,4,  11,10,2, 10,7,6,  7,1,8,
        3,9,4,   3,4,2,   3,2,6,   3,6,8,   3,8,9,
        4,9,5,   2,4,11,  6,2,10,  8,6,7,   9,8,1
    };
    emit_solid(m, R, V, 12, F, 20, 3, uv, rest);
}

/* Dodecahedron (d12): twelve pentagonal faces. */
static void build_dodeca(DiceMesh* m, float R, const vec4* uv, versor* rest) {
    const float b = 0.61803399f;   /* 1/phi */
    const float c = 1.61803399f;   /* phi   */
    float V[20][3] = {
        { 1, 1, 1},{ 1, 1,-1},{ 1,-1, 1},{ 1,-1,-1},
        {-1, 1, 1},{-1, 1,-1},{-1,-1, 1},{-1,-1,-1},
        { 0, b, c},{ 0, b,-c},{ 0,-b, c},{ 0,-b,-c},
        { b, c, 0},{ b,-c, 0},{-b, c, 0},{-b,-c, 0},
        { c, 0, b},{ c, 0,-b},{-c, 0, b},{-c, 0,-b}
    };
    static const int F[12*5] = {
         0, 8,10, 2,16,   0,16,17, 1,12,   0,12,14, 4, 8,
         8, 4,18, 6,10,  10, 6,15,13, 2,  16, 2,13, 3,17,
        17, 3,11, 9, 1,  12, 1, 9, 5,14,  14, 5,19,18, 4,
        18,19, 7,15, 6,  13,15, 7,11, 3,   9,11, 7,19, 5
    };
    emit_solid(m, R, (const float(*)[3])V, 20, F, 12, 5, uv, rest);
}

/* Barrel prism: N rectangular side faces (sprites 0..N-1), two plain caps that
 * sample the spare cell `cap_uv`. Axis along Z. */
static void build_prism(DiceMesh* m, uint32_t N, float R, float H,
                        const vec4* uv, const vec4 cap_uv, versor* rest) {
    float hz = H * 0.5f;
    for (uint32_t k = 0; k < N; ++k) {
        float a0 = 6.2831853f * (float)k / (float)N;
        float a1 = 6.2831853f * (float)(k + 1) / (float)N;
        float am = 0.5f * (a0 + a1);
        vec3 n = { cosf(am), sinf(am), 0.0f };
        vec3 p0 = { R*cosf(a0), R*sinf(a0), -hz };
        vec3 p1 = { R*cosf(a1), R*sinf(a1), -hz };
        vec3 p2 = { R*cosf(a1), R*sinf(a1),  hz };
        vec3 p3 = { R*cosf(a0), R*sinf(a0),  hz };
        static const float LUV[4][2] = {{0,0},{1,0},{1,1},{0,1}};
        add_quad(m, p0, p1, p2, p3, n, uv[k], LUV);
        face_rest(n, rest[k]);
    }
    /* two N-gon caps (plain). */
    for (int cap = 0; cap < 2; ++cap) {
        float z = cap == 0 ? hz : -hz;
        vec3 n = { 0, 0, cap == 0 ? 1.0f : -1.0f };
        float cu = 0.5f * (cap_uv[0] + cap_uv[2]);
        float cw = 0.5f * (cap_uv[1] + cap_uv[3]);
        vec3 ctr = { 0, 0, z };
        uint32_t c = dm_vert(m, ctr, n, cu, cw);
        uint32_t first = 0, prev = 0;
        for (uint32_t k = 0; k <= N; ++k) {
            float a = 6.2831853f * (float)(k % N) / (float)N;
            vec3 p = { R*cosf(a), R*sinf(a), z };
            uint32_t v = dm_vert(m, p, n, cu, cw);
            if (k == 0) { first = v; prev = v; continue; }
            dm_tri(m, c, prev, v, n);
            prev = v;
        }
        (void)first;
    }
}

/* Two-sided coin/token: top disc = sprite 0 (+Y), bottom disc = sprite 1 (-Y),
 * thin rim samples the spare cell. */
static void build_coin(DiceMesh* m, float R, const vec4* uv, const vec4 cap_uv,
                       versor* rest) {
    float hy = R * 0.16f;
    const int S = DICE_DISC_SEG;
    for (int face = 0; face < 2; ++face) {
        float y = face == 0 ? hy : -hy;
        vec3 n = { 0, face == 0 ? 1.0f : -1.0f, 0 };
        const vec4* r = &uv[face];
        float cu, cw; map_uv(*r, 0.5f, 0.5f, &cu, &cw);
        vec3 ctr = { 0, y, 0 };
        uint32_t c = dm_vert(m, ctr, n, cu, cw);
        uint32_t prev = 0;
        for (int k = 0; k <= S; ++k) {
            float a = 6.2831853f * (float)(k % S) / (float)S;
            float cx = cosf(a), cz = sinf(a);
            vec3 p = { R * cx, y, R * cz };
            /* disc UV: sprite centred, radius fills the cell */
            float lu = 0.5f + 0.5f * cx;
            float lw = 0.5f - 0.5f * cz;
            float u, w; map_uv(*r, lu, lw, &u, &w);
            uint32_t v = dm_vert(m, p, n, u, w);
            if (k == 0) { prev = v; continue; }
            dm_tri(m, c, prev, v, n);
            prev = v;
        }
        face_rest(n, rest[face]);
    }
    /* rim (plain) */
    float cu = 0.5f * (cap_uv[0] + cap_uv[2]);
    float cw = 0.5f * (cap_uv[1] + cap_uv[3]);
    for (int k = 0; k < S; ++k) {
        float a0 = 6.2831853f * (float)k / (float)S;
        float a1 = 6.2831853f * (float)(k + 1) / (float)S;
        vec3 n = { cosf(0.5f*(a0+a1)), 0, sinf(0.5f*(a0+a1)) };
        vec3 p0 = { R*cosf(a0), -hy, R*sinf(a0) };
        vec3 p1 = { R*cosf(a1), -hy, R*sinf(a1) };
        vec3 p2 = { R*cosf(a1),  hy, R*sinf(a1) };
        vec3 p3 = { R*cosf(a0),  hy, R*sinf(a0) };
        uint32_t a = dm_vert(m, p0, n, cu, cw);
        uint32_t b = dm_vert(m, p1, n, cu, cw);
        uint32_t cc = dm_vert(m, p2, n, cu, cw);
        uint32_t d = dm_vert(m, p3, n, cu, cw);
        dm_tri(m, a, b, cc, n);
        dm_tri(m, a, cc, d, n);
    }
}

/* ==========================================================================
 *  Model build / free
 * ======================================================================= */
bool ts_dice_build_model(TsGpu* gpu, TsArena* arena, const TsLog* log,
                         const TesseraDiceDef* def, TsDiceModel* out,
                         char* err, size_t err_sz) {
    memset(out, 0, sizeof *out);
    if (!def || !def->faces || def->face_count < 2) {
        snprintf(err, err_sz, "dice: need >= 2 faces");
        return false;
    }
    if (def->face_count > TS_DICE_MAX_FACES) {
        snprintf(err, err_sz, "dice: too many faces (max %u)", TS_DICE_MAX_FACES);
        return false;
    }
    uint32_t N = (uint32_t)def->face_count;
    float size = def->size > 0.0f ? def->size : 1.0f;
    float R = size * 0.5f;

    /* Pick the model. The five Platonic solids (and the cube) texture every
     * face, so they need no spare cap cell; the coin (rim) and the barrel
     * fallback (end caps) do. */
    bool is_coin   = (N == 2);
    bool is_tetra  = (N == 4);
    bool is_cube   = (N == 6);
    bool is_octa   = (N == 8);
    bool is_dodeca = (N == 12);
    bool is_icosa  = (N == 20);
    bool is_platonic = is_tetra || is_cube || is_octa || is_dodeca || is_icosa;
    bool is_prism  = !(is_coin || is_platonic);
    bool spare     = is_coin || is_prism;

    TsTexture atlas; vec4* uv = NULL; int cols = 1;
    if (!build_atlas(gpu, arena, log, def, spare, &atlas, &uv, &cols, err, err_sz))
        return false;

    versor* rest = TS_ARENA_ARR(arena, versor, N);
    if (!rest) { ts_gpu_free_texture(gpu, &atlas); snprintf(err, err_sz, "dice: rest alloc failed"); return false; }
    vec4 cap_uv;
    if (spare) glm_vec4_copy(uv[N], cap_uv);
    else       glm_vec4_copy(uv[0], cap_uv);

    DiceMesh dm = {0};
    if (is_coin)        build_coin(&dm, R, uv, cap_uv, rest);
    else if (is_tetra)  build_tetra(&dm, R, uv, rest);
    else if (is_cube)   build_cube(&dm, R, uv, rest);
    else if (is_octa)   build_octa(&dm, R, uv, rest);
    else if (is_dodeca) build_dodeca(&dm, R, uv, rest);
    else if (is_icosa)  build_icosa(&dm, R, uv, rest);
    else {
        /* barrel fallback: square-ish side faces, clamped for extreme counts */
        float H = 2.0f * R * sinf(3.14159265f / (float)N);
        H = ts_clampf(H, 0.5f * R, 1.5f * R);
        build_prism(&dm, N, R, H, uv, cap_uv, rest);
    }

    bool ok = ts_gpu_upload_mesh(gpu, dm.v, dm.vc, dm.i, dm.ic, &out->mesh);
    free(dm.v); free(dm.i);
    if (!ok) { ts_gpu_free_texture(gpu, &atlas); snprintf(err, err_sz, "dice: mesh upload failed"); return false; }

    out->atlas = atlas;
    out->rest = rest;
    out->face_count = N;
    out->size = size;
    bool tint_zero = def->tint[0]==0 && def->tint[1]==0 && def->tint[2]==0 && def->tint[3]==0;
    if (tint_zero) glm_vec4_one(out->tint);
    else           glm_vec4_copy((float*)def->tint, out->tint);
    out->valid = true;
    return true;
}

void ts_dice_free_model(TsGpu* gpu, TsDiceModel* m) {
    if (!m || !m->valid) return;
    ts_gpu_free_mesh(gpu, &m->mesh);
    ts_gpu_free_texture(gpu, &m->atlas);
    m->valid = false;
}

/* ==========================================================================
 *  Live dice: throw simulation
 * ======================================================================= */

/* Precomputed tumble trajectories, selected by seed. Offsets are in units of
 * the die's `size`; the spin winds down to exactly the rest orientation. The
 * horizontal (x,z) launch offset is large so the die is clearly thrown *in* from
 * a distance and slides across to its resting spot, rather than dropping nearly
 * straight down. */
static const struct {
    vec3  axis;         /* spin axis                                  */
    float turns;        /* whole tumbles before settling              */
    vec3  launch_off;   /* airborne spawn offset from rest (× size)   */
    vec3  launch_euler; /* initial orientation (radians, xyz)         */
    float arc;          /* unused                                     */
} DICE_TRAJ[8] = {
    {{ 1.0f, 0.2f, 0.3f}, 3, {  3.4f, 3.2f, -2.6f}, {0.9f, 0.4f, 1.2f}, 0.0f},
    {{ 0.2f, 0.4f, 1.0f}, 4, { -3.8f, 3.6f,  2.2f}, {1.5f, 1.1f, 0.2f}, 0.0f},
    {{ 0.6f, 0.0f, 0.8f}, 3, {  2.6f, 2.9f,  3.6f}, {0.3f, 1.9f, 0.7f}, 0.0f},
    {{ 1.0f, 0.1f, 0.0f}, 5, { -3.6f, 4.0f, -3.0f}, {2.1f, 0.6f, 1.4f}, 0.0f},
    {{ 0.3f, 0.9f, 0.3f}, 4, {  3.8f, 3.4f,  2.0f}, {0.7f, 2.4f, 0.9f}, 0.0f},
    {{ 0.0f, 0.3f, 1.0f}, 3, { -2.4f, 3.0f, -3.6f}, {1.2f, 0.2f, 2.0f}, 0.0f},
    {{ 0.8f, 0.5f, 0.2f}, 4, {  3.0f, 3.7f,  3.0f}, {1.8f, 1.5f, 0.5f}, 0.0f},
    {{ 0.4f, 0.0f, 1.0f}, 5, { -3.6f, 4.2f, -2.4f}, {0.5f, 2.7f, 1.1f}, 0.0f},
};
#define DICE_TRAJ_N ((uint32_t)(sizeof(DICE_TRAJ)/sizeof(DICE_TRAJ[0])))

/* The vertical axis is integrated as real physics: gravity accelerates the die
 * down, and each time it reaches the rest height its velocity reverses and is
 * scaled by the restitution `e` (energy lost to the "felt"), so it bounces lower
 * and quicker until it settles. */
#define DICE_RESTITUTION 0.42f
/* Impact wobble: each ground contact injects angular velocity (∝ impact speed)
 * into a damped torsional spring that returns to zero, so the die visibly jolts
 * on every touch and the jolt settles out before it comes to rest. */
#define DICE_WOB_KICK  0.22f   /* impulse per unit impact speed (rad/s)     */
#define DICE_WOB_KICKMAX 3.2f  /* clamp so a fast first hit stays sane       */
#define DICE_WOB_STIFF 120.0f  /* spring back toward zero offset             */
#define DICE_WOB_DAMP  13.0f   /* underdamped → a quick decaying jiggle      */

/* Impact skid: each contact also nudges the horizontal path sideways (mostly
 * perpendicular to travel, alternating, plus a little forward) — a damped spring
 * that returns to zero so the die still settles at its requested spot. */
#define DICE_SKID_KICK 0.16f   /* impulse per unit impact speed (world/s)    */
#define DICE_SKID_KICKMAX 2.2f
#define DICE_SKID_STIFF 70.0f  /* softer than the wobble → a slower skip      */
#define DICE_SKID_DAMP  11.0f

/* Geometric sum of the bounce timeline for infinite restitution bounces:
 * 1 + 2·(e + e^2 + ...) = 1 + 2e/(1-e). Used to size gravity to `throw_s`. */
static float dice_bounce_timeline(void) {
    float e = DICE_RESTITUTION;
    return 1.0f + 2.0f * e / (1.0f - e);
}

/* Fraction of the tumble / slide speed kept after each ground contact — surface
 * friction. Low so an impact cuts the rotation hard and visibly. */
#define DICE_SPIN_FRICTION 0.4f

/* Progress [0,1] whose RATE is piecewise-constant and drops by DICE_SPIN_FRICTION
 * at every ground contact: the die tumbles/slides at full speed through the air,
 * then each bounce abruptly slows it (you see the spin lurch slower on impact),
 * approaching a stop as the bounces die out. The steps sit on the analytic bounce
 * schedule (same gravity/throw_s the vertical physics uses, so they line up with
 * the visible bounces) and the whole thing is normalised to reach exactly 1 at
 * p=1 — so the spin still lands on the target face. */
static float dice_spin_progress(float p) {
    const float e = DICE_RESTITUTION, fr = DICE_SPIN_FRICTION;
    const int M = 8;
    float kk = 1.0f + 2.0f * e / (1.0f - e);
    float b[10]; b[0] = 0.0f;
    float acc = 0.0f, ei = e;
    for (int m = 1; m <= M; ++m) {         /* contact m at fraction (1+2·Σe^i)/kk */
        b[m] = (1.0f + 2.0f * acc) / kk;
        if (b[m] > 1.0f) b[m] = 1.0f;
        acc += ei; ei *= e;
    }
    b[M + 1] = 1.0f;
    float norm = 0.0f, w = 1.0f;           /* segment s runs at speed fr^s */
    for (int s = 0; s <= M; ++s) { norm += w * (b[s + 1] - b[s]); w *= fr; }
    if (norm < 1e-6f) return ts_clampf(p, 0.0f, 1.0f);
    p = ts_clampf(p, 0.0f, 1.0f);
    float prog = 0.0f; w = 1.0f;
    for (int s = 0; s <= M; ++s) {
        float lo = b[s], hi = b[s + 1];
        if (p <= lo) break;
        prog += w * ((p < hi ? p : hi) - lo);
        if (p < hi) break;
        w *= fr;
    }
    return prog / norm;
}

typedef struct {
    TesseraDiceId id;
    TesseraDefId  def;
    uint32_t      face;
    /* endpoints */
    vec3   rest_pos, launch_pos;
    versor rest_rot, launch_rot;
    vec3   spin_axis;
    float  spin_total;   /* radians of extra whole-turn spin */
    float  fall_h;       /* launch height above rest (world units)          */
    float  cy, vy, grav; /* integrated vertical: height above rest, velocity, gravity */
    vec3   wob, wobv;    /* impact wobble: a damped-spring angular offset (rad)
                          * kicked on each ground contact, decaying to zero    */
    float  skid[2], skidv[2]; /* impact skid: damped-spring horizontal (x,z) offset,
                               * kicked on each contact, decaying to zero      */
    int    nbounce;      /* ground-contact count (alternates skid direction)  */
    TsTween throw_tw;    /* tumble progress (LINEAR; curves applied per-axis) */
    /* spawn / despawn */
    float  from_alpha, to_alpha, from_scale, to_scale;
    TsTween fade_tw;
    bool   removing, resting, alive;
    /* current pose */
    vec3   pos; versor rot; float alpha, scale;
} DiceInst;

struct TsDice {
    DiceInst* items; size_t count, cap;
};

TsDice* ts_dice_create(void) { return (TsDice*)calloc(1, sizeof(TsDice)); }
void ts_dice_destroy(TsDice* d) { if (d) { free(d->items); free(d); } }

static DiceInst* dice_find(TsDice* d, TesseraDiceId id) {
    for (size_t i = 0; i < d->count; ++i)
        if (d->items[i].id == id && d->items[i].alive) return &d->items[i];
    return NULL;
}
static DiceInst* dice_alloc(TsDice* d) {
    if (d->count == d->cap) {
        size_t nc = d->cap ? d->cap * 2 : 8;
        d->items = (DiceInst*)realloc(d->items, nc * sizeof(DiceInst));
        d->cap = nc;
    }
    DiceInst* it = &d->items[d->count++];
    memset(it, 0, sizeof *it);
    return it;
}

void ts_dice_add(TsDice* d, TesseraEngine* e, const TesseraDiceThrow* spec) {
    if (!d || !e || !spec) return;
    TsDef* def = ts_registry_get(&e->registry, spec->def, TS_DEF_DICE);
    if (!def || !def->as.dice.valid) return;
    const TsDiceModel* model = &def->as.dice;

    DiceInst* it = dice_find(d, spec->id);
    if (!it) { it = dice_alloc(d); it->id = spec->id; }
    it->def = spec->def;
    it->alive = true;
    it->removing = false;
    it->resting = false;
    it->face = spec->face < model->face_count ? spec->face : model->face_count - 1;

    glm_vec3_copy((float*)spec->position, it->rest_pos);
    glm_quat_copy((float*)model->rest[it->face], it->rest_rot);

    const uint32_t t = spec->seed % DICE_TRAJ_N;
    float size = model->size;
    it->launch_pos[0] = it->rest_pos[0] + DICE_TRAJ[t].launch_off[0] * size;
    it->launch_pos[1] = it->rest_pos[1] + DICE_TRAJ[t].launch_off[1] * size;
    it->launch_pos[2] = it->rest_pos[2] + DICE_TRAJ[t].launch_off[2] * size;
    /* build the launch orientation from euler angles as a quaternion */
    {
        mat4 rm; glm_euler_xyz((float*)DICE_TRAJ[t].launch_euler, rm);
        glm_mat4_quat(rm, it->launch_rot);
        glm_quat_normalize(it->launch_rot);
    }
    glm_vec3_normalize_to((float*)DICE_TRAJ[t].axis, it->spin_axis);
    it->spin_total = DICE_TRAJ[t].turns * 6.2831853f;

    float throw_s = spec->throw_s > 0.0f ? spec->throw_s : DICE_DEFAULT_THROW_S;
    ts_tween_start(&it->throw_tw, throw_s, 0.0f, TS_EASE_LINEAR);

    /* Seed the vertical physics: drop from the launch height, with gravity chosen
     * so the fall + its restitution bounces damp out right around `throw_s`
     * (T = sqrt(2H/g)·(1 + 2·Σe^k), solved for g). */
    it->fall_h = it->launch_pos[1] - it->rest_pos[1];
    it->cy = it->fall_h;
    it->vy = 0.0f;
    glm_vec3_zero(it->wob);
    glm_vec3_zero(it->wobv);
    it->skid[0] = it->skid[1] = it->skidv[0] = it->skidv[1] = 0.0f;
    it->nbounce = 0;
    float kk = dice_bounce_timeline();
    it->grav = (it->fall_h > 1e-4f) ? 2.0f * it->fall_h * kk * kk / (throw_s * throw_s) : 0.0f;

    it->from_alpha = 0.0f; it->to_alpha = 1.0f;
    it->from_scale = 0.6f; it->to_scale = 1.0f;
    float fade_s = throw_s * 0.35f; if (fade_s > 0.28f) fade_s = 0.28f;
    ts_tween_start(&it->fade_tw, fade_s, 0.0f, TS_EASE_OUT_CUBIC);

    /* start at the launch pose so frame 0 is already airborne */
    glm_vec3_copy(it->launch_pos, it->pos);
    glm_quat_copy(it->launch_rot, it->rot);
    it->alpha = 0.0f; it->scale = it->from_scale;
}

void ts_dice_remove(TsDice* d, TesseraDiceId id, float fade_s) {
    if (!d) return;
    DiceInst* it = dice_find(d, id);
    if (!it || it->removing) return;
    it->removing = true;
    it->resting = false;
    it->from_alpha = it->alpha; it->to_alpha = 0.0f;
    it->from_scale = it->scale; it->to_scale = 0.5f;
    ts_tween_start(&it->fade_tw, fade_s > 0.0f ? fade_s : 0.25f, 0.0f, TS_EASE_IN_CUBIC);
}

void ts_dice_clear(TsDice* d, float fade_s) {
    if (!d) return;
    for (size_t i = 0; i < d->count; ++i)
        if (d->items[i].alive && !d->items[i].removing)
            ts_dice_remove(d, d->items[i].id, fade_s);
}

void ts_dice_advance(TsDice* d, float dt) {
    if (!d) return;
    for (size_t i = 0; i < d->count;) {
        DiceInst* it = &d->items[i];
        ts_tween_advance(&it->throw_tw, dt);
        ts_tween_advance(&it->fade_tw, dt);

        float p = ts_tween_value01(&it->throw_tw);          /* linear 0..1 */

        /* Vertical: integrate gravity + restitution bounce. Velocity reverses and
         * loses energy each time the die reaches the rest height, so it keeps
         * bumping (lower/quicker) while the horizontal motion and spin run their
         * course. Each contact also kicks the impact wobble, and the wobble
         * damped-spring is integrated in the same sub-steps. Sub-step so a large
         * frame dt can't tunnel through the ground; once the throw clock is done
         * the residual is negligible — settle everything. */
        if (it->removing || ts_tween_done(&it->throw_tw)) {
            it->cy = 0.0f; it->vy = 0.0f;
            glm_vec3_zero(it->wob); glm_vec3_zero(it->wobv);
            it->skid[0] = it->skid[1] = it->skidv[0] = it->skidv[1] = 0.0f;
        } else if (it->grav > 0.0f) {
            /* horizontal axis to topple about on impact (perpendicular to spin) */
            vec3 up = {0, 1, 0}, kax;
            glm_vec3_cross(it->spin_axis, up, kax);
            if (glm_vec3_norm(kax) < 0.1f) { kax[0]=1; kax[1]=0; kax[2]=0; }
            glm_vec3_normalize(kax);
            /* horizontal travel direction (x,z) and its perpendicular, for the skid */
            float tvx = it->rest_pos[0] - it->launch_pos[0];
            float tvz = it->rest_pos[2] - it->launch_pos[2];
            float tl = sqrtf(tvx*tvx + tvz*tvz);
            if (tl < 1e-4f) { tvx = 1.0f; tvz = 0.0f; tl = 1.0f; }
            tvx /= tl; tvz /= tl;
            float perpx = -tvz, perpz = tvx;   /* left of travel */

            float rem = dt;
            const float H = 1.0f / 240.0f;
            while (rem > 1e-6f) {
                float h = rem > H ? H : rem;
                it->vy -= it->grav * h;
                it->cy += it->vy * h;
                if (it->cy < 0.0f) {
                    it->cy = 0.0f;
                    float impact = fabsf(it->vy);
                    it->vy = -it->vy * DICE_RESTITUTION;    /* bounce */
                    it->nbounce++;

                    /* spin jolt (wobble) */
                    float k = impact * DICE_WOB_KICK;
                    if (k > DICE_WOB_KICKMAX) k = DICE_WOB_KICKMAX;
                    float dir = (it->wobv[0]*kax[0] + it->wobv[1]*kax[1] + it->wobv[2]*kax[2]) >= 0.0f ? -1.0f : 1.0f;
                    for (int c = 0; c < 3; ++c) it->wobv[c] += kax[c] * k * dir;

                    /* horizontal skid: sideways (alternating) + a little forward */
                    float sk = impact * DICE_SKID_KICK;
                    if (sk > DICE_SKID_KICKMAX) sk = DICE_SKID_KICKMAX;
                    float side = (it->nbounce & 1) ? 1.0f : -1.0f;
                    it->skidv[0] += (perpx * side + tvx * 0.35f) * sk;
                    it->skidv[1] += (perpz * side + tvz * 0.35f) * sk;
                }
                /* damped springs back toward zero offset (wobble + skid) */
                for (int c = 0; c < 3; ++c) {
                    it->wobv[c] += (-DICE_WOB_STIFF * it->wob[c] - DICE_WOB_DAMP * it->wobv[c]) * h;
                    it->wob[c]  += it->wobv[c] * h;
                }
                for (int c = 0; c < 2; ++c) {
                    it->skidv[c] += (-DICE_SKID_STIFF * it->skid[c] - DICE_SKID_DAMP * it->skidv[c]) * h;
                    it->skid[c]  += it->skidv[c] * h;
                }
                rem -= h;
            }
        }

        /* Horizontal slide and spin move at full speed through the air and are
         * cut sharply at each ground contact (stepped velocity), so the impacts
         * visibly slow the tumble; the progress still reaches 1 at the end, so
         * the spin lands exactly on the target face. The skid offset adds a small
         * per-impact deflection on top (decays to zero, so the rest spot holds). */
        float prog = dice_spin_progress(p);

        it->pos[0] = ts_lerpf(it->launch_pos[0], it->rest_pos[0], prog) + it->skid[0];
        it->pos[2] = ts_lerpf(it->launch_pos[2], it->rest_pos[2], prog) + it->skid[1];
        it->pos[1] = it->rest_pos[1] + it->cy;

        float ang = it->spin_total * (1.0f - prog);
        versor spin; glm_quatv(spin, ang, it->spin_axis);
        versor base; glm_quat_slerp(it->launch_rot, it->rest_rot, prog, base);
        versor tumble; glm_quat_mul(spin, base, tumble);

        /* impact wobble (world-frame offset from the touch), decays to identity */
        float wl = glm_vec3_norm(it->wob);
        if (wl > 1e-5f) {
            versor wq; vec3 wax = { it->wob[0]/wl, it->wob[1]/wl, it->wob[2]/wl };
            glm_quatv(wq, wl, wax);
            glm_quat_mul(wq, tumble, it->rot);
        } else {
            glm_quat_copy(tumble, it->rot);
        }
        glm_quat_normalize(it->rot);

        float f = ts_tween_value01(&it->fade_tw);
        it->alpha = ts_lerpf(it->from_alpha, it->to_alpha, f);
        it->scale = ts_lerpf(it->from_scale, it->to_scale, f);

        if (!it->removing && ts_tween_done(&it->throw_tw)) it->resting = true;

        if (it->removing && ts_tween_done(&it->fade_tw)) {
            d->items[i] = d->items[d->count - 1];
            d->count--;
            continue;
        }
        ++i;
    }
}

bool ts_dice_all_idle(const TsDice* d) {
    if (!d) return true;
    for (size_t i = 0; i < d->count; ++i) {
        const DiceInst* it = &d->items[i];
        if (!it->alive) continue;
        if (it->removing) return false;
        if (!ts_tween_done(&it->throw_tw)) return false;
        if (!ts_tween_done(&it->fade_tw)) return false;
    }
    return true;
}

uint32_t ts_dice_count(const TsDice* d) { return d ? (uint32_t)d->count : 0; }

bool ts_dice_face(const TsDice* d, TesseraDiceId id, uint32_t* out_face) {
    if (!d) return false;
    for (size_t i = 0; i < d->count; ++i)
        if (d->items[i].id == id && d->items[i].alive) {
            if (out_face) *out_face = d->items[i].face;
            return true;
        }
    return false;
}

/* ==========================================================================
 *  Drawlist
 * ======================================================================= */
size_t ts_dice_drawitem_count(const TsDice* d) { return d ? d->count : 0; }

size_t ts_dice_build_drawlist(TsDice* d, TesseraEngine* e, struct TsDrawItem* dst) {
    if (!d) return 0;
    size_t w = 0;
    for (size_t i = 0; i < d->count; ++i) {
        DiceInst* it = &d->items[i];
        if (!it->alive || it->scale <= 0.001f || it->alpha <= 0.003f) continue;
        TsDef* def = ts_registry_get(&e->registry, it->def, TS_DEF_DICE);
        if (!def || !def->as.dice.valid) continue;
        const TsDiceModel* model = &def->as.dice;

        TsDrawItem* di = &dst[w++];
        memset(di, 0, sizeof *di);
        di->mesh = &model->mesh;
        di->texture = model->atlas.texture;
        vec3 s = { it->scale, it->scale, it->scale };
        ts_trs(it->pos, it->rot, s, di->model);
        di->tint[0] = model->tint[0];
        di->tint[1] = model->tint[1];
        di->tint[2] = model->tint[2];
        di->tint[3] = model->tint[3] * it->alpha;
        di->uv_rect[0] = 0.0f; di->uv_rect[1] = 0.0f;
        di->uv_rect[2] = 1.0f; di->uv_rect[3] = 1.0f;
        di->skinned = false;
    }
    return w;
}
