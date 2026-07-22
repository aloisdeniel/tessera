/*
 * card.c — card slab model generation.
 *
 * Builds a thin rounded rectangle (front at +Y, back at -Y, an extruded rim in
 * between) with a per-vertex face code so one draw call can texture the front,
 * back and edge differently. UVs on the front/back span 0..1 across the slab so
 * the card pipeline can remap them into each face's atlas rect. Triangle winding
 * is auto-oriented against a desired outward normal (mesh pipeline is
 * CULLMODE_BACK / CCW-front), the same trick the dice generator uses.
 */
#include "card/card.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Defaults: a slab a little smaller than 2x3 tiles (tile size = 1.0). */
#define CARD_DEF_W      1.84f
#define CARD_DEF_H      2.76f
#define CARD_DEF_THICK  0.03f
#define CARD_DEF_RADIUS 0.12f
#define CARD_CORNER_SEG 6      /* arc segments per rounded corner */

/* ------------------------------------------------------------ mesh accum */
typedef struct {
    TsCardVertex* v; uint32_t vc, vcap;
    uint32_t*     i; uint32_t ic, icap;
} CardMesh;

static uint32_t cm_vert(CardMesh* m, vec3 p, vec3 n, float u, float w, float face) {
    if (m->vc == m->vcap) {
        m->vcap = m->vcap ? m->vcap * 2 : 64;
        m->v = (TsCardVertex*)realloc(m->v, m->vcap * sizeof(TsCardVertex));
    }
    uint32_t idx = m->vc++;
    TsCardVertex* vv = &m->v[idx];
    vv->pos[0] = p[0]; vv->pos[1] = p[1]; vv->pos[2] = p[2];
    vv->normal[0] = n[0]; vv->normal[1] = n[1]; vv->normal[2] = n[2];
    vv->uv[0] = u; vv->uv[1] = w;
    vv->face = face;
    return idx;
}
static void cm_idx3(CardMesh* m, uint32_t a, uint32_t b, uint32_t c) {
    if (m->ic + 3 > m->icap) {
        m->icap = m->icap ? m->icap * 2 : 128;
        m->i = (uint32_t*)realloc(m->i, m->icap * sizeof(uint32_t));
    }
    m->i[m->ic++] = a; m->i[m->ic++] = b; m->i[m->ic++] = c;
}
/* Emit triangle (a,b,c) wound so its front face points along `desired`. */
static void cm_tri(CardMesh* m, uint32_t a, uint32_t b, uint32_t c, vec3 desired) {
    vec3 e1, e2, fn;
    glm_vec3_sub(m->v[b].pos, m->v[a].pos, e1);
    glm_vec3_sub(m->v[c].pos, m->v[a].pos, e2);
    glm_vec3_cross(e1, e2, fn);
    if (glm_vec3_dot(fn, desired) >= 0.0f) cm_idx3(m, a, b, c);
    else                                   cm_idx3(m, a, c, b);
}

/* An all-zero rect (host left it unset) means the whole texture. */
static void default_uv(TesseraRect* r) {
    if (r->u0 == 0.0f && r->v0 == 0.0f && r->u1 == 0.0f && r->v1 == 0.0f) {
        r->u0 = 0.0f; r->v0 = 0.0f; r->u1 = 1.0f; r->v1 = 1.0f;
    }
}

/* ------------------------------------------------------------ geometry */
/* Rounded-rect perimeter in the XZ plane, CCW, `n` points into out (x,z). */
static int rounded_perimeter(float hw, float hh, float r, float out[][2], int cap) {
    /* corner centres, swept 0..90 / 90..180 / 180..270 / 270..360 */
    const float cx[4] = {  hw - r, -(hw - r), -(hw - r),  hw - r };
    const float cz[4] = {  hh - r,  hh - r,  -(hh - r), -(hh - r) };
    int seg = CARD_CORNER_SEG;
    int p = 0;
    for (int c = 0; c < 4; ++c) {
        float a0 = (float)c * (GLM_PI_2f);
        for (int j = 0; j < seg; ++j) {
            if (p >= cap) return p;
            float a = a0 + (GLM_PI_2f) * (float)j / (float)seg;
            out[p][0] = cx[c] + r * cosf(a);
            out[p][1] = cz[c] + r * sinf(a);
            ++p;
        }
    }
    return p;
}

bool ts_card_build_model(TsGpu* gpu, const TsLog* log, const TesseraCardDef* def,
                         TsCardModel* out, char* err, size_t err_sz) {
    (void)log;
    memset(out, 0, sizeof *out);
    if (!def) { snprintf(err, err_sz, "card: null def"); return false; }

    float width  = def->width  > 0.0f ? def->width  : CARD_DEF_W;
    float height = def->height > 0.0f ? def->height : CARD_DEF_H;
    float thick  = def->thickness > 0.0f ? def->thickness : CARD_DEF_THICK;
    float hw = width * 0.5f, hh = height * 0.5f, t2 = thick * 0.5f;
    float rmax = (hw < hh ? hw : hh) * 0.95f;
    float r = def->corner_radius > 0.0f ? def->corner_radius : CARD_DEF_RADIUS;
    r = ts_clampf(r, 0.02f, rmax);

    float perim[4 * CARD_CORNER_SEG][2];
    int P = rounded_perimeter(hw, hh, r, perim, 4 * CARD_CORNER_SEG);
    if (P < 3) { snprintf(err, err_sz, "card: degenerate perimeter"); return false; }

    CardMesh cm = {0};

    /* ---- front face (+Y, face 0): fan from centre ---- */
    {
        vec3 nY = {0, 1, 0};
        uint32_t ctr = cm_vert(&cm, (vec3){0, t2, 0}, nY, 0.5f, 0.5f, TS_CARD_FACE_FRONT);
        uint32_t* ring = (uint32_t*)malloc((size_t)P * sizeof(uint32_t));
        for (int k = 0; k < P; ++k) {
            float px = perim[k][0], pz = perim[k][1];
            /* Art top is model -Z: when a card is stood up to face +Z (hands),
             * -Z maps to +Y (up), so the art reads upright. */
            float u = 0.5f + px / width, w = 0.5f + pz / height;
            ring[k] = cm_vert(&cm, (vec3){px, t2, pz}, nY, u, w, TS_CARD_FACE_FRONT);
        }
        for (int k = 0; k < P; ++k)
            cm_tri(&cm, ctr, ring[k], ring[(k + 1) % P], nY);
        free(ring);
    }
    /* ---- back face (-Y, face 1): fan from centre, u mirrored ---- */
    {
        vec3 nY = {0, -1, 0};
        uint32_t ctr = cm_vert(&cm, (vec3){0, -t2, 0}, nY, 0.5f, 0.5f, TS_CARD_FACE_BACK);
        uint32_t* ring = (uint32_t*)malloc((size_t)P * sizeof(uint32_t));
        for (int k = 0; k < P; ++k) {
            float px = perim[k][0], pz = perim[k][1];
            float u = 0.5f - px / width, w = 0.5f + pz / height;   /* mirror u (seen from -Y) */
            ring[k] = cm_vert(&cm, (vec3){px, -t2, pz}, nY, u, w, TS_CARD_FACE_BACK);
        }
        for (int k = 0; k < P; ++k)
            cm_tri(&cm, ctr, ring[k], ring[(k + 1) % P], nY);
        free(ring);
    }
    /* ---- edge rim (face 2): extruded perimeter, outward radial normal ---- */
    for (int k = 0; k < P; ++k) {
        int k1 = (k + 1) % P;
        float ax = perim[k][0],  az = perim[k][1];
        float bx = perim[k1][0], bz = perim[k1][1];
        vec3 nrm = { ax + bx, 0.0f, az + bz };
        if (glm_vec3_norm(nrm) < 1e-5f) { nrm[0] = ax; nrm[2] = az; }
        glm_vec3_normalize(nrm);
        uint32_t q0 = cm_vert(&cm, (vec3){ax, t2, az},  nrm, 0.5f, 0.0f, TS_CARD_FACE_EDGE);
        uint32_t q1 = cm_vert(&cm, (vec3){bx, t2, bz},  nrm, 0.5f, 0.0f, TS_CARD_FACE_EDGE);
        uint32_t q2 = cm_vert(&cm, (vec3){bx, -t2, bz}, nrm, 0.5f, 1.0f, TS_CARD_FACE_EDGE);
        uint32_t q3 = cm_vert(&cm, (vec3){ax, -t2, az}, nrm, 0.5f, 1.0f, TS_CARD_FACE_EDGE);
        cm_tri(&cm, q0, q1, q2, nrm);
        cm_tri(&cm, q0, q2, q3, nrm);
    }

    bool ok = ts_gpu_upload_mesh_raw(gpu, cm.v, cm.vc, (uint32_t)sizeof(TsCardVertex),
                                     cm.i, cm.ic, &out->mesh);
    free(cm.v); free(cm.i);
    if (!ok) { snprintf(err, err_sz, "card: mesh upload failed"); return false; }

    out->width = width; out->height = height; out->thickness = thick; out->corner_radius = r;
    out->visible_atlas = def->visible_atlas; out->visible_uv = def->visible_uv;
    out->hidden_atlas  = def->hidden_atlas;  out->hidden_uv  = def->hidden_uv;
    out->back_atlas    = def->back_atlas;     out->back_uv    = def->back_uv;
    /* An all-zero (unset) rect means "use the whole texture". */
    default_uv(&out->visible_uv);
    default_uv(&out->hidden_uv);
    default_uv(&out->back_uv);
    bool tint_zero = def->tint[0] == 0 && def->tint[1] == 0 && def->tint[2] == 0 && def->tint[3] == 0;
    if (tint_zero) glm_vec4_one(out->tint);
    else           glm_vec4_copy((float*)def->tint, out->tint);
    out->valid = true;
    return true;
}

void ts_card_free_model(TsGpu* gpu, TsCardModel* m) {
    if (!m || !m->valid) return;
    ts_gpu_free_mesh(gpu, &m->mesh);
    m->valid = false;
}
