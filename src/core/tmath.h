/*
 * tmath.h — math conventions for Tessera, thin helpers over cglm.
 *
 * CONVENTIONS (assert-worthy, do not change without care):
 *   - Right-handed world space, +Y up.
 *   - Column-major mat4 (cglm default), multiply as M * v.
 *   - Compose transforms as T * R * S (translate * rotate * scale).
 *   - Perspective built with cglm's *_rh_zo (right-handed, 0..1 depth) to
 *     match SDL_GPU's 0..1 NDC depth range.
 *   - Quaternions are cglm `versor` {x,y,z,w}.
 *   - Square grid: world = tile_size * (x, 0, y).
 */
#ifndef TESSERA_TMATH_H
#define TESSERA_TMATH_H

/* Expose all clip-space variants (we use the RH / zero-to-one depth ones to
 * match SDL_GPU's 0..1 NDC depth range regardless of cglm's default config). */
#define CGLM_CLIPSPACE_INCLUDE_ALL
#include <cglm/cglm.h>
#include <math.h>
#include "tessera.h"

#ifndef TS_TILE_SIZE
#define TS_TILE_SIZE 1.0f
#endif

static inline float ts_clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
static inline float ts_lerpf(float a, float b, float t) { return a + (b - a) * t; }

/* Compose a TRS matrix into `out`. */
static inline void ts_trs(vec3 t, versor r, vec3 s, mat4 out) {
    mat4 rot;
    glm_quat_mat4(r, rot);
    glm_mat4_identity(out);
    glm_translate(out, t);
    glm_mat4_mul(out, rot, out);
    glm_scale(out, s);
}

/* Right-handed perspective with 0..1 depth (SDL_GPU / Metal / Vulkan). */
static inline void ts_perspective(float fovy, float aspect, float znear,
                                  float zfar, mat4 out) {
    glm_perspective_rh_zo(fovy, aspect, znear, zfar, out);
}

/* Orbit camera eye position from focus + spherical params. */
static inline void ts_orbit_eye(vec3 focus, float distance, float yaw,
                                float pitch, vec3 out_eye) {
    float cp = cosf(pitch);
    out_eye[0] = focus[0] + distance * cp * sinf(yaw);
    out_eye[1] = focus[1] + distance * sinf(pitch);
    out_eye[2] = focus[2] + distance * cp * cosf(yaw);
}

static inline void ts_look_at(vec3 eye, vec3 center, vec3 up, mat4 out) {
    glm_lookat_rh(eye, center, up, out);
}

/* Grid coordinate -> world position (top surface at y=0, square grid). */
static inline void ts_grid_to_world(int32_t x, int32_t y, vec3 out) {
    out[0] = TS_TILE_SIZE * (float)x;
    out[1] = 0.0f;
    out[2] = TS_TILE_SIZE * (float)y;
}

/* Continuous grid position -> world position. Whole numbers land on tile
 * centres; (0.5,0.5) is the corner between tiles (0,0) and (1,1). */
static inline void ts_grid_to_world_f(float x, float y, vec3 out) {
    out[0] = TS_TILE_SIZE * x;
    out[1] = 0.0f;
    out[2] = TS_TILE_SIZE * y;
}

/* Facing quadrant (0..3) -> Y-axis rotation quaternion. */
static inline void ts_facing_quat(uint16_t facing, versor out) {
    float angle = (float)(facing & 3u) * (GLM_PI_2f);
    glm_quatv(out, angle, (vec3){0.0f, 1.0f, 0.0f});
}

#endif /* TESSERA_TMATH_H */
