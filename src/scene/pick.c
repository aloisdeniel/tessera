/* pick.c — screen-space ray picking against the live scene.
 *
 * Unprojects a pixel through the current camera into a world ray, then tests it
 * against every live tile (default AABB) and entity (fixed bounding sphere),
 * reporting the nearest hit of each. See tessera_pick() in the public header. */
#include "engine.h"
#include "orchestration/orch.h"
#include "scene/scene.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Default tile box: full tile footprint, standard thickness, top at y=0. */
#define TS_PICK_TILE_THICKNESS 0.25f
/* One bounding sphere for every entity, sitting on the ground (y=0). */
#define TS_PICK_ENTITY_RADIUS  (0.45f * TS_TILE_SIZE)
/* Bounding radii used when fitting the camera to a set of targets. Tiles use the
 * circumscribed circle of the square footprint; entities a rough standing box. */
#define TS_FIT_TILE_RADIUS     (0.70711f * TS_TILE_SIZE)
#define TS_FIT_ENTITY_RADIUS   (0.9f * TS_TILE_SIZE)

/* Slab test: ray (o + t*d) vs AABB [mn,mx]. Returns nearest t>=0 in *t_out. */
static bool ray_aabb(const vec3 o, const vec3 d, const vec3 mn, const vec3 mx, float* t_out) {
    float tmin = 0.0f, tmax = 1e30f;
    for (int i = 0; i < 3; ++i) {
        if (fabsf(d[i]) < 1e-8f) {
            if (o[i] < mn[i] || o[i] > mx[i]) return false;   /* parallel & outside */
            continue;
        }
        float inv = 1.0f / d[i];
        float t1 = (mn[i] - o[i]) * inv;
        float t2 = (mx[i] - o[i]) * inv;
        if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; }
        if (t1 > tmin) tmin = t1;
        if (t2 < tmax) tmax = t2;
        if (tmin > tmax) return false;
    }
    *t_out = tmin;
    return true;
}

/* Ray (o + t*d, d normalized) vs sphere (centre c, radius r). Nearest t>=0. */
static bool ray_sphere(const vec3 o, const vec3 d, const vec3 c, float r, float* t_out) {
    vec3 m; glm_vec3_sub((float*)o, (float*)c, m);
    float b = glm_vec3_dot(m, (float*)d);
    float cc = glm_vec3_dot(m, m) - r * r;
    if (cc > 0.0f && b > 0.0f) return false;   /* outside and pointing away */
    float disc = b * b - cc;
    if (disc < 0.0f) return false;             /* misses the sphere */
    float t = -b - sqrtf(disc);
    if (t < 0.0f) t = 0.0f;                     /* origin inside the sphere */
    *t_out = t;
    return true;
}

/* Logical window size — the space SDL mouse/touch events use, not the pixel
 * drawable, so hosts can feed input straight through on hi-DPI displays (aspect
 * is identical either way). Returns false on a zero-sized viewport. */
static bool pick_logical_size(TesseraEngine* e, float* W, float* H) {
    int lw = 0, lh = 0;
    if (e->gpu.window) SDL_GetWindowSize(e->gpu.window, &lw, &lh);
    *W = lw > 0 ? (float)lw : (float)e->gpu.width;
    *H = lh > 0 ? (float)lh : (float)e->gpu.height;
    return *W > 0.0f && *H > 0.0f;
}

/* As above, plus a camera refresh so the mapping matches the pose on screen. */
static bool pick_viewport(TesseraEngine* e, float* W, float* H) {
    if (!pick_logical_size(e, W, H)) return false;
    e->camera.dirty = true;
    ts_camera_update(&e->camera, *W / *H);
    return true;
}

bool ts_engine_pick(TesseraEngine* e, float sx, float sy, TesseraPick* out) {
    memset(out, 0, sizeof *out);
    if (!e) return false;

    float W, H;
    if (!pick_viewport(e, &W, &H)) return false;

    /* Unproject the near/far points of the pixel through inverse(view_proj).
     * Pixel origin is top-left; NDC y points up, depth range is 0..1 (SDL_GPU). */
    mat4 inv;
    glm_mat4_inv(e->camera.view_proj, inv);
    float ndc_x = 2.0f * sx / W - 1.0f;
    float ndc_y = 1.0f - 2.0f * sy / H;

    vec4 near_clip = { ndc_x, ndc_y, 0.0f, 1.0f };
    vec4 far_clip  = { ndc_x, ndc_y, 1.0f, 1.0f };
    vec4 wn, wf;
    glm_mat4_mulv(inv, near_clip, wn);
    glm_mat4_mulv(inv, far_clip, wf);
    if (fabsf(wn[3]) < 1e-8f || fabsf(wf[3]) < 1e-8f) return false;
    glm_vec4_scale(wn, 1.0f / wn[3], wn);
    glm_vec4_scale(wf, 1.0f / wf[3], wf);

    vec3 o = { wn[0], wn[1], wn[2] };
    vec3 dir;
    glm_vec3_sub((vec3){ wf[0], wf[1], wf[2] }, o, dir);
    if (glm_vec3_norm(dir) < 1e-8f) return false;
    glm_vec3_normalize(dir);

    glm_vec3_copy(o, out->ray_origin);
    glm_vec3_copy(dir, out->ray_dir);

    if (!e->orch) return false;
    struct TsOrch* orch = e->orch;

    /* nearest tile via its default AABB */
    float best_tile = 1e30f;
    for (size_t i = 0; i < orch->tile_count; ++i) {
        const TsTileInst* t = &orch->tiles[i];
        vec3 c; ts_grid_to_world(t->coord.x, t->coord.y, c);
        vec3 mn = { c[0] - 0.5f * TS_TILE_SIZE, -TS_PICK_TILE_THICKNESS, c[2] - 0.5f * TS_TILE_SIZE };
        vec3 mx = { c[0] + 0.5f * TS_TILE_SIZE, 0.0f,                    c[2] + 0.5f * TS_TILE_SIZE };
        float td;
        if (ray_aabb(o, dir, mn, mx, &td) && td < best_tile) {
            best_tile = td;
            out->hit_tile = true;
            out->tile = t->coord;
            out->tile_distance = td;
        }
    }

    /* nearest entity via its fixed bounding sphere */
    float best_ent = 1e30f;
    for (size_t i = 0; i < orch->entity_count; ++i) {
        const TsEntityInst* inst = &orch->entities[i];
        if (inst->removing) continue;   /* despawning: not selectable */
        vec3 centre = { inst->pos[0], inst->pos[1] + TS_PICK_ENTITY_RADIUS, inst->pos[2] };
        float ed;
        if (ray_sphere(o, dir, centre, TS_PICK_ENTITY_RADIUS, &ed) && ed < best_ent) {
            best_ent = ed;
            out->hit_entity = true;
            out->entity = inst->id;
            out->entity_distance = ed;
        }
    }

    /* world point of the nearest hit overall */
    if (out->hit_tile || out->hit_entity) {
        float nearest = out->hit_tile ? out->tile_distance : 1e30f;
        if (out->hit_entity && out->entity_distance < nearest) nearest = out->entity_distance;
        out->point[0] = o[0] + dir[0] * nearest;
        out->point[1] = o[1] + dir[1] * nearest;
        out->point[2] = o[2] + dir[2] * nearest;
        return true;
    }
    return false;
}

/* ------------------------------------------------------- inverse (scene->screen) */
/* Project a world point through the current view_proj into logical window
 * coordinates. Exact inverse of the unprojection in ts_engine_pick, so a pick
 * and this projection round-trip. */
bool ts_engine_world_to_screen(TesseraEngine* e, const vec3 world, TesseraScreenPos* out) {
    memset(out, 0, sizeof *out);
    if (!e) return false;

    float W, H;
    if (!pick_viewport(e, &W, &H)) return false;

    glm_vec3_copy((float*)world, out->world);

    vec4 clip;
    glm_mat4_mulv(e->camera.view_proj, (vec4){ world[0], world[1], world[2], 1.0f }, clip);

    /* w<=0 => on or behind the near plane: no meaningful pixel. Reported (found)
     * but not onscreen, with x/y left at 0. */
    if (clip[3] <= 1e-6f) return true;

    float ndc_x = clip[0] / clip[3];
    float ndc_y = clip[1] / clip[3];
    float ndc_z = clip[2] / clip[3];

    out->x = (ndc_x + 1.0f) * 0.5f * W;   /* inverse of ndc_x = 2*sx/W - 1 */
    out->y = (1.0f - ndc_y) * 0.5f * H;   /* inverse of ndc_y = 1 - 2*sy/H */
    out->depth = ndc_z;
    out->onscreen = ndc_x >= -1.0f && ndc_x <= 1.0f &&
                    ndc_y >= -1.0f && ndc_y <= 1.0f &&
                    ndc_z >=  0.0f && ndc_z <= 1.0f;
    return true;
}

bool ts_engine_entity_screen_position(TesseraEngine* e, TesseraEntityId id,
                                      TesseraScreenPos* out) {
    memset(out, 0, sizeof *out);
    if (!e || !e->orch) return false;
    vec3 p;
    if (!ts_orch_entity_pos(e->orch, id, p)) return false;
    return ts_engine_world_to_screen(e, p, out);
}

bool ts_engine_tile_screen_position(TesseraEngine* e, TesseraTileId id,
                                    TesseraScreenPos* out) {
    memset(out, 0, sizeof *out);
    if (!e || !e->orch) return false;
    vec3 p;
    if (!ts_orch_tile_pos(e->orch, id, p)) return false;
    return ts_engine_world_to_screen(e, p, out);
}

/* ------------------------------------------------------------- fit camera */
/* True if, at orbit distance `d`, every target sphere projects inside the
 * viewport with `pad` fractional margin. Works on a COPY of the camera so the
 * live pose is untouched. Monotonic in `d`: as the camera pulls back, targets
 * shrink toward the focus (which projects to screen centre), so once they fit
 * they keep fitting — the search below relies on this. */
static bool fit_ok(const TsCamera* base, float d, float aspect,
                   const vec3* ctr, const float* rad, size_t n, float pad) {
    TsCamera c = *base;
    c.distance = d;
    c.dirty = true;
    ts_camera_update(&c, aspect);

    /* screen-aligned basis to expand each sphere along the view right/up axes */
    vec3 fwd; glm_vec3_sub(c.focus, c.eye, fwd);
    if (glm_vec3_norm(fwd) < 1e-6f) return false;
    glm_vec3_normalize(fwd);
    vec3 wup = { 0.0f, 1.0f, 0.0f };
    vec3 right; glm_vec3_cross(fwd, wup, right);
    if (glm_vec3_norm(right) < 1e-6f) { right[0] = 1.0f; right[1] = 0.0f; right[2] = 0.0f; }
    glm_vec3_normalize(right);
    vec3 up; glm_vec3_cross(right, fwd, up); glm_vec3_normalize(up);

    float lim = 1.0f - pad;
    for (size_t i = 0; i < n; ++i) {
        vec3 pr, pu;
        glm_vec3_scale(right, rad[i], pr);
        glm_vec3_scale(up,    rad[i], pu);
        vec3 samples[5];
        glm_vec3_copy((float*)ctr[i], samples[0]);
        glm_vec3_add((float*)ctr[i], pr, samples[1]);
        glm_vec3_sub((float*)ctr[i], pr, samples[2]);
        glm_vec3_add((float*)ctr[i], pu, samples[3]);
        glm_vec3_sub((float*)ctr[i], pu, samples[4]);
        for (int k = 0; k < 5; ++k) {
            vec4 clip;
            glm_mat4_mulv(c.view_proj,
                          (vec4){ samples[k][0], samples[k][1], samples[k][2], 1.0f }, clip);
            if (clip[3] <= 1e-6f) return false;           /* behind the camera */
            if (fabsf(clip[0] / clip[3]) > lim ||
                fabsf(clip[1] / clip[3]) > lim) return false;
        }
    }
    return true;
}

bool ts_engine_fit_distance(TesseraEngine* e,
                            const TesseraTileId* tiles, size_t tile_count,
                            const TesseraEntityId* entities, size_t entity_count,
                            float padding, float* out_distance) {
    if (!e || !out_distance || !e->orch) return false;
    *out_distance = 0.0f;

    float W, H;
    if (!pick_logical_size(e, &W, &H)) return false;
    float aspect = W / H;

    size_t cap = tile_count + entity_count;
    if (cap == 0) return false;

    vec3*  ctr = (vec3*)malloc(cap * sizeof(vec3));
    float* rad = (float*)malloc(cap * sizeof(float));
    if (!ctr || !rad) { free(ctr); free(rad); return false; }

    size_t n = 0;
    for (size_t i = 0; i < tile_count; ++i) {
        vec3 p;
        if (!ts_orch_tile_pos(e->orch, tiles[i], p)) continue;
        glm_vec3_copy(p, ctr[n]);
        rad[n] = TS_FIT_TILE_RADIUS;
        ++n;
    }
    for (size_t i = 0; i < entity_count; ++i) {
        vec3 p;
        if (!ts_orch_entity_pos(e->orch, entities[i], p)) continue;
        p[1] += TS_FIT_ENTITY_RADIUS;   /* raise to the body centre */
        glm_vec3_copy(p, ctr[n]);
        rad[n] = TS_FIT_ENTITY_RADIUS;
        ++n;
    }
    if (n == 0) { free(ctr); free(rad); return false; }

    float pad = padding < 0.0f ? 0.0f : (padding > 0.9f ? 0.9f : padding);

    /* Bisect for the smallest fitting distance (fit_ok is monotonic in d). */
    float lo = fmaxf(0.1f, e->camera.znear);
    float hi = 1.0e5f;
    if (fit_ok(&e->camera, lo, aspect, ctr, rad, n, pad)) {
        *out_distance = lo;
    } else if (!fit_ok(&e->camera, hi, aspect, ctr, rad, n, pad)) {
        *out_distance = hi;             /* never fits (targets behind camera): best effort */
    } else {
        for (int it = 0; it < 48; ++it) {
            float mid = 0.5f * (lo + hi);
            if (fit_ok(&e->camera, mid, aspect, ctr, rad, n, pad)) hi = mid;
            else lo = mid;
        }
        *out_distance = hi;
    }

    free(ctr);
    free(rad);
    return true;
}
