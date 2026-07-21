/* camera.c — orbit camera producing view/proj/viewProj. */
#include "scene/scene.h"

void ts_camera_init(TsCamera* c) {
    glm_vec3_zero(c->focus);
    c->distance = 8.0f;
    c->yaw = 0.6f;
    c->pitch = 0.7f;
    c->fov = glm_rad(50.0f);
    c->znear = 0.1f;
    c->zfar = 200.0f;
    c->dirty = true;
}

void ts_camera_set(TsCamera* c, vec3 focus, float distance, float yaw,
                   float pitch, float fov) {
    glm_vec3_copy(focus, c->focus);
    c->distance = distance > 0.1f ? distance : 0.1f;
    c->yaw = yaw;
    c->pitch = ts_clampf(pitch, -1.45f, 1.45f);
    if (fov > 0.0f) c->fov = fov;
    c->dirty = true;
}

void ts_camera_orbit(TsCamera* c, float dyaw, float dpitch, float dzoom) {
    c->yaw += dyaw;
    c->pitch = ts_clampf(c->pitch + dpitch, -1.45f, 1.45f);
    c->distance = ts_clampf(c->distance + dzoom, 1.0f, 100.0f);
    c->dirty = true;
}

void ts_camera_update(TsCamera* c, float aspect) {
    if (!c->dirty) return;
    if (aspect <= 0.0f) aspect = 16.0f / 9.0f;
    vec3 eye;
    ts_orbit_eye(c->focus, c->distance, c->yaw, c->pitch, eye);
    glm_vec3_copy(eye, c->eye);
    ts_look_at(eye, c->focus, (vec3){0.0f, 1.0f, 0.0f}, c->view);
    if (c->ortho) {
        /* Match the perspective framing at the focus distance: the ortho half
         * height equals what the fov subtends there, so switching modes keeps
         * the board roughly the same on-screen size. */
        float half_h = c->distance * tanf(c->fov * 0.5f);
        float half_w = half_h * aspect;
        glm_ortho_rh_zo(-half_w, half_w, -half_h, half_h, c->znear, c->zfar, c->proj);
    } else {
        ts_perspective(c->fov, aspect, c->znear, c->zfar, c->proj);
    }
    glm_mat4_mul(c->proj, c->view, c->view_proj);
    c->dirty = false;
}
