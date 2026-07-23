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
#include "state.h"
#include "anim/anim.h"
#include "stb_image.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define DICE_DEFAULT_THROW_S 1.15f
#define DICE_DEFAULT_SLIDE_S 0.45f   /* reposition-only tween (no throw) */
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

    /* Spare cell: the body colour used by the bevels / caps / rim. Approximate it
     * by averaging the border pixels of the face sprites (usually the plate's
     * background), so the bevels match the faces instead of a generic grey. */
    if (spare) {
        unsigned long ar = 0, ag = 0, ab = 0, npx = 0;
        for (uint32_t f = 0; f < nfaces; ++f) {
            int col = (int)f % cols, row = (int)f / cols;
            int x0 = col * DICE_CELL_PX + DICE_CELL_PAD;
            int y0 = row * DICE_CELL_PX + DICE_CELL_PAD;
            for (int x = 0; x < inner; x += 4)
                for (int e = 0; e < 2; ++e) {
                    int yy = e ? inner - 1 : 0;
                    uint8_t* o = &atlas[((y0 + yy) * aw + (x0 + x)) * 4];
                    if (o[3] > 10) { ar += o[0]; ag += o[1]; ab += o[2]; npx++; }
                }
            for (int y = 0; y < inner; y += 4)
                for (int e = 0; e < 2; ++e) {
                    int xx = e ? inner - 1 : 0;
                    uint8_t* o = &atlas[((y0 + y) * aw + (x0 + xx)) * 4];
                    if (o[3] > 10) { ar += o[0]; ag += o[1]; ab += o[2]; npx++; }
                }
        }
        uint8_t br = 205, bg = 205, bb = 205;
        if (npx > 0) { br = (uint8_t)(ar/npx); bg = (uint8_t)(ag/npx); bb = (uint8_t)(ab/npx); }
        int col = (int)nfaces % cols, row = (int)nfaces / cols;
        int x0 = col * DICE_CELL_PX + DICE_CELL_PAD;
        int y0 = row * DICE_CELL_PX + DICE_CELL_PAD;
        for (int y = 0; y < inner; ++y)
            for (int x = 0; x < inner; ++x) {
                uint8_t* o = &atlas[((y0 + y) * aw + (x0 + x)) * 4];
                o[0] = br; o[1] = bg; o[2] = bb; o[3] = 255;
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
 *
 *  The convex solids are CHAMFERED: each textured face is inset toward its
 *  centroid and the gap left along every edge and corner is filled with small
 *  body-coloured bevel facets, so the die has softened edges/corners instead of
 *  razor-sharp ones.
 * ======================================================================= */

/* How far each face vertex is pulled toward its face centroid (fraction). The
 * freed strip along the edges/corners becomes the bevel. */
#define DICE_BEVEL 0.10f
#define DICE_MAXV  20   /* most vertices of any supported solid (dodeca) */
#define DICE_MAXFV 5    /* most vertices per face (dodeca pentagon)      */

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

/* Build a chamfered convex solid from a vertex table + face index lists (each
 * face `fs` vertices; vertices are projected to the sphere of radius R). Every
 * textured face is inset toward its centroid; the freed strips are filled with
 * bevel facets sampling `bevel_uv` (plain body colour): one quad per edge and
 * one fan per original vertex. `up_hints[f]` (or NULL) sets each sprite's up
 * direction; `square` maps quad faces edge-to-edge (cube) instead of onto the
 * inscribed circle. rest[f] = face-up orientation (caller may override). */
static void add_chamfered_solid(DiceMesh* m, float R, const float (*Vr)[3], int nverts,
                                const int* faces, int nfaces, int fs,
                                const vec4* uv, const vec4 bevel_uv, versor* rest,
                                const float (*up_hints)[3], bool square) {
    vec3 NV[DICE_MAXV];
    for (int i = 0; i < nverts; ++i) {
        vec3 p = { Vr[i][0], Vr[i][1], Vr[i][2] };
        glm_vec3_normalize(p); glm_vec3_scale(p, R, NV[i]);
    }
    int  ring[DICE_MAXV][DICE_MAXFV];    /* per-face vertex indices, ring-ordered */
    vec3 iv[DICE_MAXV][DICE_MAXFV];      /* per-face inset positions              */
    vec3 fn[DICE_MAXV];                  /* per-face outward normals              */
    float bcu, bcw; map_uv(bevel_uv, 0.5f, 0.5f, &bcu, &bcw);

    /* ---- textured inset faces ---- */
    for (int f = 0; f < nfaces; ++f) {
        vec3 c = {0,0,0};
        for (int k = 0; k < fs; ++k) glm_vec3_add(c, NV[faces[f*fs+k]], c);
        glm_vec3_scale(c, 1.0f/(float)fs, c);
        vec3 n; glm_vec3_normalize_to(c, n); glm_vec3_copy(n, fn[f]);

        vec3 right, up;
        if (up_hints) {
            vec3 hint = { up_hints[f][0], up_hints[f][1], up_hints[f][2] };
            float d = glm_vec3_dot(hint, n);
            up[0]=hint[0]-d*n[0]; up[1]=hint[1]-d*n[1]; up[2]=hint[2]-d*n[2];
            glm_vec3_normalize(up);
            glm_vec3_cross(up, n, right); glm_vec3_normalize(right);
        } else {
            face_uv_frame(n, right, up);
        }

        /* ring order by angle around the centroid */
        int idx[DICE_MAXFV]; float ang[DICE_MAXFV];
        for (int k = 0; k < fs; ++k) {
            idx[k] = faces[f*fs+k];
            vec3 rel; glm_vec3_sub(NV[idx[k]], c, rel);
            ang[k] = atan2f(glm_vec3_dot(rel, up), glm_vec3_dot(rel, right));
        }
        for (int a = 1; a < fs; ++a) {
            int ki = idx[a]; float ka = ang[a]; int b = a;
            while (b > 0 && ang[b-1] > ka) { ang[b]=ang[b-1]; idx[b]=idx[b-1]; --b; }
            ang[b] = ka; idx[b] = ki;
        }
        for (int k = 0; k < fs; ++k) {
            ring[f][k] = idx[k];
            glm_vec3_lerp(NV[idx[k]], c, DICE_BEVEL, iv[f][k]);   /* inset */
        }

        /* UV extents (per-axis half-extent for square, else circumradius) */
        float eu = 1e-6f, ev = 1e-6f, er = 1e-6f;
        for (int k = 0; k < fs; ++k) {
            vec3 rel; glm_vec3_sub(iv[f][k], c, rel);
            float ru = fabsf(glm_vec3_dot(rel, right)), rv = fabsf(glm_vec3_dot(rel, up));
            float rr = glm_vec3_norm(rel);
            if (ru > eu) eu = ru; if (rv > ev) ev = rv; if (rr > er) er = rr;
        }
        float cu, cw; map_uv(uv[f], 0.5f, 0.5f, &cu, &cw);
        uint32_t ci = dm_vert(m, c, n, cu, cw);
        uint32_t vi[DICE_MAXFV];
        for (int k = 0; k < fs; ++k) {
            vec3 rel; glm_vec3_sub(iv[f][k], c, rel);
            float lu, lw;
            if (square) { lu = 0.5f + 0.5f*glm_vec3_dot(rel,right)/eu;
                          lw = 0.5f - 0.5f*glm_vec3_dot(rel,up)/ev; }
            else        { lu = 0.5f + 0.5f*glm_vec3_dot(rel,right)/er;
                          lw = 0.5f - 0.5f*glm_vec3_dot(rel,up)/er; }
            float u, w; map_uv(uv[f], lu, lw, &u, &w);
            vi[k] = dm_vert(m, iv[f][k], n, u, w);
        }
        for (int k = 0; k < fs; ++k) dm_tri(m, ci, vi[k], vi[(k+1)%fs], n);
        face_rest(n, rest[f]);
    }

    /* ---- edge bevels: one quad per shared edge (processed once, f<g) ---- */
    for (int f = 0; f < nfaces; ++f)
        for (int k = 0; k < fs; ++k) {
            int a = ring[f][k], b = ring[f][(k+1)%fs];
            int g = -1, ga = -1, gb = -1;
            for (int gg = 0; gg < nfaces && g < 0; ++gg) {
                if (gg == f) continue;
                int pa = -1, pb = -1;
                for (int kk = 0; kk < fs; ++kk) {
                    if (ring[gg][kk] == a) pa = kk;
                    if (ring[gg][kk] == b) pb = kk;
                }
                if (pa >= 0 && pb >= 0) { g = gg; ga = pa; gb = pb; }
            }
            if (g < 0 || g < f) continue;
            vec3 n; glm_vec3_add(fn[f], fn[g], n); glm_vec3_normalize(n);
            uint32_t q0 = dm_vert(m, iv[f][k],           n, bcu, bcw);
            uint32_t q1 = dm_vert(m, iv[f][(k+1)%fs],    n, bcu, bcw);
            uint32_t q2 = dm_vert(m, iv[g][gb],          n, bcu, bcw);
            uint32_t q3 = dm_vert(m, iv[g][ga],          n, bcu, bcw);
            dm_tri(m, q0, q1, q2, n); dm_tri(m, q0, q2, q3, n);
        }

    /* ---- corner bevels: one fan per original vertex ---- */
    for (int v = 0; v < nverts; ++v) {
        int fl[16], pl[16], cnt = 0;
        for (int f = 0; f < nfaces && cnt < 16; ++f)
            for (int k = 0; k < fs; ++k)
                if (ring[f][k] == v) { fl[cnt]=f; pl[cnt]=k; cnt++; break; }
        if (cnt < 3) continue;
        vec3 vn; glm_vec3_normalize_to(NV[v], vn);
        vec3 t0; { vec3 up = {0,1,0}; if (fabsf(glm_vec3_dot(vn,up))>0.9f){up[0]=1;up[1]=0;up[2]=0;}
                   glm_vec3_cross(up, vn, t0); glm_vec3_normalize(t0); }
        vec3 t1; glm_vec3_cross(vn, t0, t1);
        float ang[16];
        for (int j = 0; j < cnt; ++j) {
            vec3 rel; glm_vec3_sub(iv[fl[j]][pl[j]], NV[v], rel);
            ang[j] = atan2f(glm_vec3_dot(rel,t1), glm_vec3_dot(rel,t0));
        }
        for (int a = 1; a < cnt; ++a) {
            int ff=fl[a], pp=pl[a]; float ka=ang[a]; int b=a;
            while (b>0 && ang[b-1]>ka){ ang[b]=ang[b-1]; fl[b]=fl[b-1]; pl[b]=pl[b-1]; --b; }
            ang[b]=ka; fl[b]=ff; pl[b]=pp;
        }
        vec3 cc = {0,0,0};
        for (int j = 0; j < cnt; ++j) glm_vec3_add(cc, iv[fl[j]][pl[j]], cc);
        glm_vec3_scale(cc, 1.0f/(float)cnt, cc);
        uint32_t ci = dm_vert(m, cc, vn, bcu, bcw);
        uint32_t cv[16];
        for (int j = 0; j < cnt; ++j) cv[j] = dm_vert(m, iv[fl[j]][pl[j]], vn, bcu, bcw);
        for (int j = 0; j < cnt; ++j) dm_tri(m, ci, cv[j], cv[(j+1)%cnt], vn);
    }
}

/* Cube (d6): six square faces (sprite edge-to-edge), chamfered. Face order
 * +Y,-Y,+X,-X,+Z,-Z matches the sprite indices. */
static void build_cube(DiceMesh* m, float R, const vec4* uv, const vec4 bevel_uv,
                       versor* rest) {
    static const float V[8][3] = {
        {-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},{-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}
    };
    static const int F[6*4] = {
        3,2,6,7,  0,1,5,4,  1,2,6,5,  0,3,7,4,  4,5,6,7,  0,1,2,3
    };
    add_chamfered_solid(m, R, V, 8, F, 6, 4, uv, bevel_uv, rest, NULL, true);
}

/* Tetrahedron (d4): four triangular faces, chamfered and read at the apex. */
static void build_tetra(DiceMesh* m, float R, const vec4* uv, const vec4 bevel_uv,
                        versor* rest) {
    static const float V[4][3] = { {1,1,1},{1,-1,-1},{-1,1,-1},{-1,-1,1} };
    static const int F[4*3] = { 1,2,3, 0,3,2, 0,1,3, 0,2,1 };
    vec3 NV[4];
    for (int i = 0; i < 4; ++i) {
        vec3 p = { V[i][0], V[i][1], V[i][2] };
        glm_vec3_normalize(p); glm_vec3_scale(p, R, NV[i]);
    }
    /* per-face in-plane "up" toward the apex (first listed vertex) */
    float hints[4][3]; vec3 n[4], up[4];
    for (int f = 0; f < 4; ++f) {
        int ia=F[f*3+0], ib=F[f*3+1], ic=F[f*3+2];
        vec3 c; glm_vec3_add(NV[ia],NV[ib],c); glm_vec3_add(c,NV[ic],c);
        glm_vec3_scale(c, 1.0f/3.0f, c);
        glm_vec3_normalize_to(c, n[f]);
        vec3 mid; glm_vec3_add(NV[ib],NV[ic],mid); glm_vec3_scale(mid,0.5f,mid);
        vec3 u; glm_vec3_sub(NV[ia], mid, u);
        float d = glm_vec3_dot(u,n[f]); vec3 tmp; glm_vec3_scale(n[f],d,tmp);
        glm_vec3_sub(u, tmp, u); glm_vec3_normalize(u);
        glm_vec3_copy(u, up[f]);
        hints[f][0]=u[0]; hints[f][1]=u[1]; hints[f][2]=u[2];
    }
    add_chamfered_solid(m, R, V, 4, F, 4, 3, uv, bevel_uv, rest, hints, false);
    for (int f = 0; f < 4; ++f) tetra_rest(n[f], up[f], rest[f]);   /* read-at-apex */
}

/* Octahedron (d8): eight triangular faces, chamfered. */
static void build_octa(DiceMesh* m, float R, const vec4* uv, const vec4 bevel_uv,
                       versor* rest) {
    static const float V[6][3] = {
        {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}
    };
    static const int F[8*3] = {
        0,2,4, 0,4,3, 0,3,5, 0,5,2,
        1,4,2, 1,3,4, 1,5,3, 1,2,5
    };
    add_chamfered_solid(m, R, V, 6, F, 8, 3, uv, bevel_uv, rest, NULL, false);
}

/* Icosahedron (d20): twenty triangular faces (classic icosphere connectivity). */
static void build_icosa(DiceMesh* m, float R, const vec4* uv, const vec4 bevel_uv,
                        versor* rest) {
    const float t = 1.61803399f;   /* golden ratio */
    float V[12][3] = {
        {-1, t, 0},{ 1, t, 0},{-1,-t, 0},{ 1,-t, 0},
        { 0,-1, t},{ 0, 1, t},{ 0,-1,-t},{ 0, 1,-t},
        { t, 0,-1},{ t, 0, 1},{-t, 0,-1},{-t, 0, 1}
    };
    static const int F[20*3] = {
        0,11,5,  0,5,1,   0,1,7,   0,7,10,  0,10,11,
        1,5,9,   5,11,4,  11,10,2, 10,7,6,  7,1,8,
        3,9,4,   3,4,2,   3,2,6,   3,6,8,   3,8,9,
        4,9,5,   2,4,11,  6,2,10,  8,6,7,   9,8,1
    };
    add_chamfered_solid(m, R, (const float(*)[3])V, 12, F, 20, 3, uv, bevel_uv, rest, NULL, false);
}

/* Dodecahedron (d12): twelve pentagonal faces, chamfered. */
static void build_dodeca(DiceMesh* m, float R, const vec4* uv, const vec4 bevel_uv,
                         versor* rest) {
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
    add_chamfered_solid(m, R, (const float(*)[3])V, 20, F, 12, 5, uv, bevel_uv, rest, NULL, false);
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
 * with a beveled (chamfered) edge into a thin plain rim. */
static void build_coin(DiceMesh* m, float R, const vec4* uv, const vec4 bevel_uv,
                       versor* rest) {
    const int S = DICE_DISC_SEG;
    float bw = R * DICE_BEVEL * 1.4f;             /* chamfer band width */
    float rd = R - bw;                            /* textured disc radius */
    float hy = R * 0.18f;                         /* half thickness */
    float ry = hy - bw; if (ry < 0.02f * R) ry = 0.02f * R;   /* rim half-height */
    float bcu, bcw; map_uv(bevel_uv, 0.5f, 0.5f, &bcu, &bcw);

    for (int face = 0; face < 2; ++face) {
        float sgn = face == 0 ? 1.0f : -1.0f;
        vec3 n = { 0, sgn, 0 };
        const vec4* r = &uv[face];
        /* textured disc (radius rd) */
        float cu, cw; map_uv(*r, 0.5f, 0.5f, &cu, &cw);
        vec3 ctr = { 0, sgn * hy, 0 };
        uint32_t c = dm_vert(m, ctr, n, cu, cw);
        uint32_t prev = 0;
        for (int k = 0; k <= S; ++k) {
            float a = 6.2831853f * (float)(k % S) / (float)S;
            float cx = cosf(a), cz = sinf(a);
            vec3 p = { rd * cx, sgn * hy, rd * cz };
            float lu = 0.5f + 0.5f * cx, lw = 0.5f - 0.5f * cz;
            float u, w; map_uv(*r, lu, lw, &u, &w);
            uint32_t v = dm_vert(m, p, n, u, w);
            if (k == 0) { prev = v; continue; }
            dm_tri(m, c, prev, v, n);
            prev = v;
        }
        face_rest(n, rest[face]);
        /* chamfer ring: disc edge (rd, sgn·hy) → rim edge (R, sgn·ry), plain */
        for (int k = 0; k < S; ++k) {
            float a0 = 6.2831853f * (float)k / (float)S;
            float a1 = 6.2831853f * (float)(k + 1) / (float)S;
            float am = 0.5f * (a0 + a1);
            vec3 nn = { cosf(am) * 0.7f, sgn * 0.7f, sinf(am) * 0.7f };
            vec3 p0 = { rd*cosf(a0), sgn*hy, rd*sinf(a0) };
            vec3 p1 = { rd*cosf(a1), sgn*hy, rd*sinf(a1) };
            vec3 p2 = { R*cosf(a1),  sgn*ry, R*sinf(a1) };
            vec3 p3 = { R*cosf(a0),  sgn*ry, R*sinf(a0) };
            uint32_t q0=dm_vert(m,p0,nn,bcu,bcw), q1=dm_vert(m,p1,nn,bcu,bcw),
                     q2=dm_vert(m,p2,nn,bcu,bcw), q3=dm_vert(m,p3,nn,bcu,bcw);
            dm_tri(m, q0, q1, q2, nn); dm_tri(m, q0, q2, q3, nn);
        }
    }
    /* vertical rim (plain) */
    for (int k = 0; k < S; ++k) {
        float a0 = 6.2831853f * (float)k / (float)S;
        float a1 = 6.2831853f * (float)(k + 1) / (float)S;
        vec3 n = { cosf(0.5f*(a0+a1)), 0, sinf(0.5f*(a0+a1)) };
        vec3 p0 = { R*cosf(a0), -ry, R*sinf(a0) };
        vec3 p1 = { R*cosf(a1), -ry, R*sinf(a1) };
        vec3 p2 = { R*cosf(a1),  ry, R*sinf(a1) };
        vec3 p3 = { R*cosf(a0),  ry, R*sinf(a0) };
        uint32_t a=dm_vert(m,p0,n,bcu,bcw), b=dm_vert(m,p1,n,bcu,bcw),
                 cc=dm_vert(m,p2,n,bcu,bcw), d=dm_vert(m,p3,n,bcu,bcw);
        dm_tri(m, a, b, cc, n); dm_tri(m, a, cc, d, n);
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

    /* Pick the model. Every shape now reserves one spare atlas cell for the
     * plain body colour used by the chamfer bevels (and the coin rim / barrel
     * caps). */
    bool is_coin   = (N == 2);
    bool is_tetra  = (N == 4);
    bool is_cube   = (N == 6);
    bool is_octa   = (N == 8);
    bool is_dodeca = (N == 12);
    bool is_icosa  = (N == 20);

    TsTexture atlas; vec4* uv = NULL; int cols = 1;
    if (!build_atlas(gpu, arena, log, def, true, &atlas, &uv, &cols, err, err_sz))
        return false;

    versor* rest = TS_ARENA_ARR(arena, versor, N);
    if (!rest) { ts_gpu_free_texture(gpu, &atlas); snprintf(err, err_sz, "dice: rest alloc failed"); return false; }
    vec4 bevel_uv; glm_vec4_copy(uv[N], bevel_uv);   /* spare cell = body colour */

    DiceMesh dm = {0};
    if (is_coin)        build_coin(&dm, R, uv, bevel_uv, rest);
    else if (is_tetra)  build_tetra(&dm, R, uv, bevel_uv, rest);
    else if (is_cube)   build_cube(&dm, R, uv, bevel_uv, rest);
    else if (is_octa)   build_octa(&dm, R, uv, bevel_uv, rest);
    else if (is_dodeca) build_dodeca(&dm, R, uv, bevel_uv, rest);
    else if (is_icosa)  build_icosa(&dm, R, uv, bevel_uv, rest);
    else {
        /* barrel fallback: square-ish side faces, clamped for extreme counts */
        float H = 2.0f * R * sinf(3.14159265f / (float)N);
        H = ts_clampf(H, 0.5f * R, 1.5f * R);
        build_prism(&dm, N, R, H, uv, bevel_uv, rest);
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
    /* reposition-only slide: when a die's placement changes in position alone
     * (same id/def/face/seed/throw_s) we skip the throw and simply tween the
     * position from where it is now to the new rest spot with an easeInOut
     * curve, holding its rest orientation. */
    bool    sliding;
    vec3    slide_from;
    TsTween slide_tw;
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

void ts_dice_add(TsDice* d, TesseraEngine* e, const TsDiceThrow* spec) {
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

/* Retarget a live die to a new position without re-throwing it: tween from its
 * current on-screen pose to the new rest spot with an easeInOut curve, holding
 * the (unchanged) rest orientation. Used when only the placement position moved. */
static void dice_slide_to(DiceInst* it, const float pos[3], float slide_s) {
    it->removing = false;
    it->resting = false;
    it->sliding = true;
    glm_vec3_copy(it->pos, it->slide_from);        /* start where it is now */
    glm_vec3_copy((float*)pos, it->rest_pos);      /* new destination        */
    it->alpha = it->to_alpha = it->from_alpha = 1.0f;
    it->scale = it->to_scale = it->from_scale = 1.0f;
    ts_tween_start(&it->slide_tw, slide_s > 0.0f ? slide_s : DICE_DEFAULT_SLIDE_S,
                   0.0f, TS_EASE_IN_OUT_CUBIC);
}

void ts_dice_remove(TsDice* d, TesseraDiceId id, float fade_s) {
    if (!d) return;
    DiceInst* it = dice_find(d, id);
    if (!it || it->removing) return;
    it->removing = true;
    it->resting = false;
    it->sliding = false;
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

        /* Reposition-only slide: tween pos → rest_pos (easeInOut), hold the rest
         * orientation, no throw physics. A removal clears `sliding` so this is
         * skipped and the normal fade path runs. */
        if (it->sliding && !it->removing) {
            ts_tween_advance(&it->slide_tw, dt);
            ts_tween_vec3(&it->slide_tw, it->slide_from, it->rest_pos, it->pos);
            glm_quat_copy(it->rest_rot, it->rot);
            it->alpha = 1.0f; it->scale = 1.0f;
            if (ts_tween_done(&it->slide_tw)) { it->sliding = false; it->resting = true; }
            ++i;
            continue;
        }

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

/* ==========================================================================
 *  State diff: throw newly-appeared dice, remove vanished ones
 * ======================================================================= */
static const TesseraDicePlacement* find_dice_placement(const TsSnapshot* s,
                                                        TesseraDiceId id) {
    if (!s) return NULL;
    for (size_t i = 0; i < s->dice_count; ++i)
        if (s->dice[i].id == id) return &s->dice[i];
    return NULL;
}

/* True when two placements describe the same throw (so we must NOT re-throw a
 * die that is already settling/settled on an unchanged placement). */
static bool dice_placement_same(const TesseraDicePlacement* a,
                                const TesseraDicePlacement* b) {
    if (!a || !b) return false;
    return a->def == b->def && a->face == b->face && a->seed == b->seed &&
           a->throw_s == b->throw_s &&
           a->position[0] == b->position[0] &&
           a->position[1] == b->position[1] &&
           a->position[2] == b->position[2];
}

/* True when two placements are identical except (possibly) their position — same
 * die, same number/face, same throw. When this holds but the position differs,
 * the die is simply slid to the new spot instead of being re-thrown. */
static bool dice_placement_same_except_pos(const TesseraDicePlacement* a,
                                           const TesseraDicePlacement* b) {
    if (!a || !b) return false;
    return a->def == b->def && a->face == b->face && a->seed == b->seed &&
           a->throw_s == b->throw_s;
}

void ts_dice_on_promote(TesseraEngine* e, const TsSnapshot* prev,
                        const TsSnapshot* next, float remove_s) {
    if (!e) return;
    size_t nd = next ? next->dice_count : 0;
    /* Lazily create the live-set only if there is (or was) something to do. */
    if (!e->dice) {
        if (nd == 0) return;
        e->dice = ts_dice_create();
        if (!e->dice) return;
    }
    TsDice* d = e->dice;

    /* Throw dice that are new, or whose placement changed since prev. */
    for (size_t i = 0; i < nd; ++i) {
        const TesseraDicePlacement* p = &next->dice[i];
        if (p->id == 0) continue;
        const TesseraDicePlacement* pp = find_dice_placement(prev, p->id);
        DiceInst* inst = dice_find(d, p->id);
        bool live = inst != NULL;
        if (live && pp && dice_placement_same(pp, p)) continue;  /* unchanged */
        /* Only the position moved (same number/throw): slide there, don't re-throw. */
        if (live && pp && dice_placement_same_except_pos(pp, p)) {
            dice_slide_to(inst, p->position, p->throw_s);
            continue;
        }
        TsDiceThrow t = { .id = p->id, .def = p->def, .face = p->face,
                          .seed = p->seed, .throw_s = p->throw_s };
        t.position[0] = p->position[0];
        t.position[1] = p->position[1];
        t.position[2] = p->position[2];
        ts_dice_add(d, e, &t);
    }

    /* Remove live dice absent from the next state. */
    for (size_t i = 0; i < d->count; ++i) {
        DiceInst* it = &d->items[i];
        if (!it->alive || it->removing) continue;
        if (!find_dice_placement(next, it->id))
            ts_dice_remove(d, it->id, remove_s);
    }
}

bool ts_dice_all_idle(const TsDice* d) {
    if (!d) return true;
    for (size_t i = 0; i < d->count; ++i) {
        const DiceInst* it = &d->items[i];
        if (!it->alive) continue;
        if (it->removing) return false;
        if (it->sliding) return false;
        if (!ts_tween_done(&it->throw_tw)) return false;
        if (!ts_tween_done(&it->fade_tw)) return false;
    }
    return true;
}

uint32_t ts_dice_count(const TsDice* d) { return d ? (uint32_t)d->count : 0; }

/* Ray (o + t*dir, dir normalized) vs sphere (centre c, radius r). Nearest t>=0.
 * Mirrors the ray_sphere in pick.c; kept local so dice stays self-contained. */
static bool dice_ray_sphere(const float o[3], const float dir[3],
                            const float c[3], float r, float* t_out) {
    vec3 m; glm_vec3_sub((float*)o, (float*)c, m);
    float b = glm_vec3_dot(m, (float*)dir);
    float cc = glm_vec3_dot(m, m) - r * r;
    if (cc > 0.0f && b > 0.0f) return false;   /* outside and pointing away */
    float disc = b * b - cc;
    if (disc < 0.0f) return false;
    float t = -b - sqrtf(disc);
    if (t < 0.0f) t = 0.0f;                     /* origin inside the sphere */
    *t_out = t;
    return true;
}

bool ts_dice_raycast(const TsDice* d, TesseraEngine* e, const float o[3],
                     const float dir[3], TesseraDiceId* out_id, float* out_dist) {
    if (!d || !e) return false;
    float best = 1e30f; bool hit = false; TesseraDiceId best_id = 0;
    for (size_t i = 0; i < d->count; ++i) {
        const DiceInst* it = &d->items[i];
        if (!it->alive || it->removing) continue;   /* despawning: not selectable */
        TsDef* def = ts_registry_get(&e->registry, it->def, TS_DEF_DICE);
        if (!def || !def->as.dice.valid) continue;
        float r = 0.5f * def->as.dice.size * it->scale;
        if (r <= 0.0f) continue;
        float t;
        if (dice_ray_sphere(o, dir, it->pos, r, &t) && t < best) {
            best = t; best_id = it->id; hit = true;
        }
    }
    if (hit) { if (out_id) *out_id = best_id; if (out_dist) *out_dist = best; }
    return hit;
}

bool ts_dice_pos(const TsDice* d, TesseraDiceId id, float out[3]) {
    if (!d) return false;
    for (size_t i = 0; i < d->count; ++i) {
        const DiceInst* it = &d->items[i];
        if (it->id == id && it->alive && !it->removing) {
            out[0] = it->pos[0]; out[1] = it->pos[1]; out[2] = it->pos[2];
            return true;
        }
    }
    return false;
}

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
