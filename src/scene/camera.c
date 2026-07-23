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
    glm_vec3_copy((vec3){0.0f, 1.0f, 0.0f}, c->look_up);
    c->has_look = false;
    c->dirty = true;
}

void ts_camera_set(TsCamera* c, vec3 focus, float distance, float yaw,
                   float pitch, float fov) {
    glm_vec3_copy(focus, c->focus);
    c->distance = distance > 0.1f ? distance : 0.1f;
    c->yaw = yaw;
    c->pitch = ts_clampf(pitch, -1.45f, 1.45f);
    if (fov > 0.0f) c->fov = fov;
    c->has_look = false;   /* orbit control resumes */
    c->dirty = true;
}

void ts_camera_orbit(TsCamera* c, float dyaw, float dpitch, float dzoom) {
    c->yaw += dyaw;
    c->pitch = ts_clampf(c->pitch + dpitch, -1.45f, 1.45f);
    c->distance = ts_clampf(c->distance + dzoom, 1.0f, 100.0f);
    c->has_look = false;   /* orbit control resumes */
    c->dirty = true;
}

/* Build proj from the current distance/fov/ortho into c->proj. */
static void camera_build_proj(TsCamera* c, float aspect) {
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
}

void ts_camera_set_look(TsCamera* c, const TsCamPose* p, float aspect) {
    if (aspect <= 0.0f) aspect = 16.0f / 9.0f;

    /* forward (eye -> target); |eye-target| mirrors the orbit `distance`. */
    vec3 fwd; glm_vec3_sub((float*)p->target, (float*)p->eye, fwd);
    float flen = glm_vec3_norm(fwd);
    c->distance = flen > 0.1f ? flen : 0.1f;
    if (flen < 1e-6f) { fwd[0] = 0.0f; fwd[1] = 0.0f; fwd[2] = -1.0f; flen = 1.0f; }
    glm_vec3_scale(fwd, 1.0f / flen, fwd);

    /* up: normalize, then guard degenerate (|fwd × up| ~ 0) by falling back to
     * +Z then +X. */
    vec3 up; glm_vec3_copy((float*)p->up, up);
    if (glm_vec3_norm(up) < 1e-6f) glm_vec3_copy((vec3){0.0f, 1.0f, 0.0f}, up);
    else glm_vec3_normalize(up);
    vec3 cr; glm_vec3_cross(fwd, up, cr);
    if (glm_vec3_norm(cr) < 1e-5f) {
        glm_vec3_copy((vec3){0.0f, 0.0f, 1.0f}, up);
        glm_vec3_cross(fwd, up, cr);
        if (glm_vec3_norm(cr) < 1e-5f) glm_vec3_copy((vec3){1.0f, 0.0f, 0.0f}, up);
    }

    if (p->fov > 0.0f) c->fov = p->fov;
    glm_vec3_copy((float*)p->eye, c->eye);
    glm_vec3_copy((float*)p->target, c->focus);
    glm_vec3_copy(up, c->look_up);

    /* back-solve orbit angles from the eye relative to the focus (+Y-up model)
     * so the orbit-derived paths (fit distance, DOF framing) stay consistent. */
    float dy = (c->eye[1] - c->focus[1]) / c->distance;
    c->pitch = asinf(ts_clampf(dy, -1.0f, 1.0f));
    c->yaw = atan2f(c->eye[0] - c->focus[0], c->eye[2] - c->focus[2]);

    c->has_look = true;
    ts_look_at(c->eye, c->focus, c->look_up, c->view);
    camera_build_proj(c, aspect);
    glm_mat4_mul(c->proj, c->view, c->view_proj);
    c->dirty = false;
}

void ts_camera_update(TsCamera* c, float aspect) {
    if (!c->dirty) return;
    if (aspect <= 0.0f) aspect = 16.0f / 9.0f;
    if (c->has_look) {
        /* A look pose is authoritative: keep the exact eye/up, only rebuild
         * view + proj (e.g. for a new aspect after a resize). */
        ts_look_at(c->eye, c->focus, c->look_up, c->view);
    } else {
        vec3 eye;
        ts_orbit_eye(c->focus, c->distance, c->yaw, c->pitch, eye);
        glm_vec3_copy(eye, c->eye);
        ts_look_at(eye, c->focus, (vec3){0.0f, 1.0f, 0.0f}, c->view);
    }
    camera_build_proj(c, aspect);
    glm_mat4_mul(c->proj, c->view, c->view_proj);
    c->dirty = false;
}
