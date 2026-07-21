/*
 * scene.h — camera, grid, and the multi-entity tile layout solver.
 */
#ifndef TESSERA_SCENE_H
#define TESSERA_SCENE_H

#include "core/core.h"
#include "core/tmath.h"
#include "tessera.h"

/* ---- camera (scene/camera.c) ---- */
typedef struct {
    vec3  focus;
    float distance, yaw, pitch;
    float fov;         /* vertical, radians */
    float znear, zfar;
    bool  ortho;       /* true = orthographic (isometric look), false = perspective */
    /* cached outputs */
    mat4  view, proj, view_proj;
    vec3  eye;         /* world-space eye position (recomputed on update) */
    bool  dirty;
} TsCamera;

void ts_camera_init(TsCamera* c);
void ts_camera_set(TsCamera* c, vec3 focus, float distance, float yaw, float pitch, float fov);
/* Recompute view/proj if dirty. aspect = width/height. */
void ts_camera_update(TsCamera* c, float aspect);
void ts_camera_orbit(TsCamera* c, float dyaw, float dpitch, float dzoom);

/* ---- layout solver (scene/layout.c) ---- */
/* Deterministic sub-slot layout for n entities sharing one tile.
 * Fills up to `n` local (x,z) offsets within the tile footprint and a single
 * uniform scale so they fit. Slot i corresponds to the i-th entity in an
 * id-sorted group (contract relied on by M4 reflow). */
#define TS_MAX_SLOTS 32
typedef struct {
    vec2  offset[TS_MAX_SLOTS];  /* local x,z within tile (units of tile size) */
    float scale;                 /* uniform scale applied to each occupant     */
    uint32_t count;
} TsLayout;

void ts_layout_solve(uint32_t n, TsLayout* out);

#endif /* TESSERA_SCENE_H */
