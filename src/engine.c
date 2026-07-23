/* engine.c — per-frame tick + the render pass. */
#include "engine.h"
#include "state.h"
#include "orchestration/orch.h"
#include "anim/skeleton.h"
#include "fx/fx.h"
#include "dice/dice.h"
#include "card/card.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void ts_engine_set_error(TesseraEngine* e, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->error, sizeof e->error, fmt, ap);
    va_end(ap);
    TS_LOGE(&e->log, "%s", e->error);
}

static void fill_frame_uniform(TesseraEngine* e, TsFrameUniform* u) {
    glm_mat4_copy(e->camera.view_proj, u->view_proj);
    glm_vec4(e->light.dir, 0.0f, u->light_dir);
    u->ambient[0] = e->light.ambient[0];
    u->ambient[1] = e->light.ambient[1];
    u->ambient[2] = e->light.ambient[2];
    u->ambient[3] = 1.0f;
    u->light_color[0] = e->light.color[0];
    u->light_color[1] = e->light.color[1];
    u->light_color[2] = e->light.color[2];
    u->light_color[3] = e->light.intensity;
    u->camera_pos[0] = e->camera.eye[0];
    u->camera_pos[1] = e->camera.eye[1];
    u->camera_pos[2] = e->camera.eye[2];
    u->camera_pos[3] = 1.0f;
}

/* Draw a blob shadow decal under each live entity (M7). Called after the opaque
 * mesh pass so geometry depth occludes shadows correctly. */
static void record_blobs(TesseraEngine* e, SDL_GPUCommandBuffer* cmd,
                         SDL_GPURenderPass* pass, const TsFrameUniform* fu) {
    TsGpu* g = &e->gpu;
    if (!g->blob_pipeline || !e->orch) return;
    if (e->quality.shadows != TESSERA_SHADOW_BLOB &&
        e->quality.shadows != TESSERA_SHADOW_MAP) return;   /* MAP falls back to blob */

    TsBlob* blobs = NULL;
    size_t nb = ts_orch_build_blobs(e->orch, e, &e->frame_arena, &blobs);
    if (nb == 0) return;

    const TsMesh* quad = &e->registry.quad_mesh;
    if (!quad->vbo) return;

    SDL_BindGPUGraphicsPipeline(pass, g->blob_pipeline);
    SDL_PushGPUVertexUniformData(cmd, 0, fu, sizeof *fu);
    SDL_GPUBufferBinding vb = { .buffer = quad->vbo, .offset = 0 };
    SDL_GPUBufferBinding ib = { .buffer = quad->ibo, .offset = 0 };
    SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
    SDL_BindGPUIndexBuffer(pass, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);

    for (size_t i = 0; i < nb; ++i) {
        const TsBlob* b = &blobs[i];
        TsObjectUniform ou;
        mat4 m; glm_mat4_identity(m);
        glm_translate(m, (vec3){ b->center[0], b->center[1], b->center[2] });
        glm_scale(m, (vec3){ b->radius * 2.0f, 1.0f, b->radius * 2.0f });
        glm_mat4_copy(m, ou.model);
        glm_vec4_copy((vec4){ 0.0f, 0.0f, 0.0f, b->alpha }, ou.tint);
        glm_vec4_copy((vec4){ 0.0f, 0.0f, 1.0f, 1.0f }, ou.uv_rect);
        SDL_PushGPUVertexUniformData(cmd, 1, &ou, sizeof ou);
        SDL_DrawGPUIndexedPrimitives(pass, quad->index_count, 1, 0, 0, 0);
    }
}

/* Draw the live cards / piles with the dedicated card pipeline (3 samplers +
 * crossfade). Runs after the opaque mesh pass, depth-tested and depth-writing so
 * cards occlude one another correctly; alpha-blended for fades/flips. */
static void record_cards(TesseraEngine* e, SDL_GPUCommandBuffer* cmd,
                         SDL_GPURenderPass* pass, const TsFrameUniform* fu) {
    TsGpu* g = &e->gpu;
    if (!g->card_pipeline || !e->orch) return;

    TsCardDrawItem* cards = NULL;
    size_t nc = ts_orch_build_cards(e->orch, e, &e->frame_arena, &cards);
    if (nc == 0) return;

    SDL_BindGPUGraphicsPipeline(pass, g->card_pipeline);
    SDL_PushGPUVertexUniformData(cmd, 0, fu, sizeof *fu);
    SDL_PushGPUFragmentUniformData(cmd, 0, fu, sizeof *fu);

    for (size_t i = 0; i < nc; ++i) {
        const TsCardDrawItem* c = &cards[i];
        if (!c->mesh || !c->mesh->vbo) continue;

        TsCardObjectUniform ou;
        glm_mat4_copy((vec4*)c->model, ou.model);
        glm_vec4_copy((float*)c->tint, ou.tint);
        glm_vec4_copy((float*)c->uv_visible, ou.uv_visible);
        glm_vec4_copy((float*)c->uv_hidden, ou.uv_hidden);
        glm_vec4_copy((float*)c->uv_back, ou.uv_back);
        glm_vec4_copy((vec4){ c->mix, 0.0f, 0.0f, 0.0f }, ou.params);
        SDL_PushGPUVertexUniformData(cmd, 1, &ou, sizeof ou);
        SDL_PushGPUFragmentUniformData(cmd, 1, &ou, sizeof ou);

        SDL_GPUBufferBinding vb = { .buffer = c->mesh->vbo, .offset = 0 };
        SDL_GPUBufferBinding ib = { .buffer = c->mesh->ibo, .offset = 0 };
        SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
        SDL_BindGPUIndexBuffer(pass, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        SDL_GPUTexture* white = e->registry.white.texture;
        SDL_GPUTextureSamplerBinding tsb[3] = {
            { .texture = c->tex_visible ? c->tex_visible : white, .sampler = g->linear_sampler },
            { .texture = c->tex_hidden  ? c->tex_hidden  : white, .sampler = g->linear_sampler },
            { .texture = c->tex_back    ? c->tex_back    : white, .sampler = g->linear_sampler },
        };
        SDL_BindGPUFragmentSamplers(pass, 0, tsb, 3);
        SDL_DrawGPUIndexedPrimitives(pass, c->mesh->index_count, 1, 0, 0, 0);
    }
}

/* Depth-of-field is active only when enabled with a real blur and a pipeline. */
bool ts_engine_dof_active(const TesseraEngine* e) {
    return e->focus.enabled && e->gpu.dof_pipeline && e->focus.blur_strength > 0.0f;
}

void ts_engine_resolve_dof(const TesseraEngine* e, TsDofParams* p) {
    p->znear = e->camera.znear;
    p->zfar = e->camera.zfar;
    p->focus_dist = e->focus.focus_distance > 0.0f ? e->focus.focus_distance : e->camera.distance;
    p->focus_range = e->focus.focus_range > 0.0f ? e->focus.focus_range : 1.0f;
    p->blur_px = e->focus.blur_strength;
    p->ortho = e->camera.ortho;
}

void ts_engine_render(TesseraEngine* e) {
    TsGpu* g = &e->gpu;
    if (!g->device || !g->window) return;

    float aspect = (g->height > 0) ? (float)g->width / (float)g->height : 1.7778f;
    ts_camera_update(&e->camera, aspect);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(g->device);
    if (!cmd) { ts_engine_set_error(e, "AcquireGPUCommandBuffer: %s", SDL_GetError()); return; }

    SDL_GPUTexture* swap = NULL;
    Uint32 sw = 0, sh = 0;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, g->window, &swap, &sw, &sh) || !swap) {
        /* Window minimized or swapchain unavailable — nothing to draw. */
        SDL_SubmitGPUCommandBuffer(cmd);
        return;
    }
    if ((int)sw != g->width || (int)sh != g->height) {
        g->width = (int)sw; g->height = (int)sh;
        e->camera.dirty = true;
    }
    ts_gpu_ensure_depth(g, sw, sh);

    /* Build + upload particle geometry before the render pass (copy pass). */
    ts_fx_prepare(e, cmd);

    bool dof = ts_engine_dof_active(e) && ts_gpu_ensure_scene_target(g, sw, sh);

    SDL_GPUColorTargetInfo color = {
        .texture = dof ? g->scene_color : swap,
        .clear_color = (SDL_FColor){0.08f, 0.10f, 0.14f, 1.0f},
        .load_op = SDL_GPU_LOADOP_CLEAR,
        .store_op = SDL_GPU_STOREOP_STORE,
    };
    SDL_GPUDepthStencilTargetInfo depth = {
        .texture = g->depth_texture,
        .clear_depth = 1.0f,
        .load_op = SDL_GPU_LOADOP_CLEAR,
        /* keep depth when a post pass will sample it */
        .store_op = dof ? SDL_GPU_STOREOP_STORE : SDL_GPU_STOREOP_DONT_CARE,
        .stencil_load_op = SDL_GPU_LOADOP_DONT_CARE,
        .stencil_store_op = SDL_GPU_STOREOP_DONT_CARE,
    };

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &color, 1, &depth);
    ts_engine_record_draws(e, cmd, pass, (uint32_t)g->width, (uint32_t)g->height);
    SDL_EndGPURenderPass(pass);

    if (dof) {
        TsDofParams p; ts_engine_resolve_dof(e, &p);
        ts_gpu_dof_post(g, cmd, g->scene_color, swap, g->depth_texture, sw, sh, &p);
    }

    SDL_SubmitGPUCommandBuffer(cmd);
    e->have_rendered = true;
}

void ts_engine_record_draws(TesseraEngine* e, SDL_GPUCommandBuffer* cmd,
                            SDL_GPURenderPass* pass, uint32_t vp_w, uint32_t vp_h) {
    TsGpu* g = &e->gpu;
    float aspect = (vp_h > 0) ? (float)vp_w / (float)vp_h : 1.7778f;
    e->camera.dirty = true;
    ts_camera_update(&e->camera, aspect);

    TsFrameUniform fu;
    fill_frame_uniform(e, &fu);

    /* Opaque mesh + dice geometry. A scene with only cards (no tiles/entities)
     * still needs the card/decal/particle passes below, so don't early-return on
     * an empty mesh list — just skip the mesh loop. */
    TsDrawItem* items = NULL;
    size_t count = g->mesh_pipeline ? ts_scene_build_drawlist(e, &e->frame_arena, &items) : 0;

    /* Bind the pipeline lazily so mixed static/skinned scenes only switch when
     * needed. bound: 0=none, 1=static mesh, 2=skinned. */
    int bound = 0;
    for (size_t i = 0; i < count; ++i) {
        const TsDrawItem* it = &items[i];
        if (!it->mesh || !it->mesh->vbo) continue;

        /* A skinned-format mesh must use the skinned pipeline; if we can't
         * (no palette this frame, or the pipeline failed to build) skip it
         * rather than feed a skinned vertex layout to the static pipeline. */
        if (it->skinned && (!it->joints || !g->skinned_pipeline)) continue;

        bool use_skin = it->skinned;
        int want = use_skin ? 2 : 1;
        if (want != bound) {
            SDL_BindGPUGraphicsPipeline(pass, want == 2 ? g->skinned_pipeline : g->mesh_pipeline);
            SDL_PushGPUVertexUniformData(cmd, 0, &fu, sizeof fu);
            SDL_PushGPUFragmentUniformData(cmd, 0, &fu, sizeof fu);
            bound = want;
        }

        TsObjectUniform ou;
        glm_mat4_copy((vec4*)it->model, ou.model);
        glm_vec4_copy((float*)it->tint, ou.tint);
        glm_vec4_copy((float*)it->uv_rect, ou.uv_rect);
        SDL_PushGPUVertexUniformData(cmd, 1, &ou, sizeof ou);

        if (use_skin) {
            /* Full 64-matrix palette, identity-padded (shader declares [64]). */
            mat4 palette[TS_MAX_JOINTS];
            uint32_t jc = it->joint_count < TS_MAX_JOINTS ? it->joint_count : TS_MAX_JOINTS;
            for (uint32_t j = 0; j < jc; ++j) glm_mat4_copy((vec4*)it->joints[j], palette[j]);
            for (uint32_t j = jc; j < TS_MAX_JOINTS; ++j) glm_mat4_identity(palette[j]);
            SDL_PushGPUVertexUniformData(cmd, 2, palette, sizeof palette);
        }

        SDL_GPUBufferBinding vb = { .buffer = it->mesh->vbo, .offset = 0 };
        SDL_GPUBufferBinding ib = { .buffer = it->mesh->ibo, .offset = 0 };
        SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
        SDL_BindGPUIndexBuffer(pass, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        SDL_GPUTextureSamplerBinding tsb = {
            .texture = it->texture ? it->texture : e->registry.white.texture,
            .sampler = g->linear_sampler };
        SDL_BindGPUFragmentSamplers(pass, 0, &tsb, 1);
        SDL_DrawGPUIndexedPrimitives(pass, it->mesh->index_count, 1, 0, 0, 0);
    }

    /* Cards use their own pipeline; draw them (depth-writing) before the
     * translucent decal/particle passes. */
    record_cards(e, cmd, pass, &fu);

    /* M7 blob shadows, M6 particles: translucent passes after opaque geometry. */
    record_blobs(e, cmd, pass, &fu);
    ts_fx_record(e, cmd, pass, &fu);
}

/* Apply a state camera onto the live camera. The camera is described by a
 * tagged mode (orbit / manual / target / focus-a-live-object); each tick the
 * engine resolves that spec into a goal pose against the LIVE scene and tweens
 * the camera pose toward it, so follow modes track a moving object (M7+). */

/* Return v if finite, else the fallback — guards against NaN/Inf reaching the
 * camera math from malformed FFI state (see resolve_camera_goal). */
static float finite_or(float v, float fallback) {
    return isfinite(v) ? v : fallback;
}

/* Aspect ratio the render path uses (drawable w/h), falling back to 16:9. */
static float current_aspect(const TesseraEngine* e) {
    return (e->gpu.height > 0) ? (float)e->gpu.width / (float)e->gpu.height
                               : 16.0f / 9.0f;
}

/* Rotate a local axis by an (already-valid) quaternion into world space. */
static void quat_axis(versor q, const vec3 local, vec3 out) {
    glm_quat_rotatev(q, (float*)local, out);
}

/* Normalize an orientation quaternion (xyzw); identity when all-zero/degenerate. */
static void norm_orientation(const float src[4], versor out) {
    versor q = { finite_or(src[0], 0.0f), finite_or(src[1], 0.0f),
                 finite_or(src[2], 0.0f), finite_or(src[3], 0.0f) };
    float n = glm_quat_norm(q);
    if (n < 1e-6f) { glm_quat_identity(out); return; }
    glm_vec4_scale(q, 1.0f / n, out);
}

/* Find a hand placement by id in the last-promoted target snapshot. */
static const TesseraHandPlacement* find_target_hand(TesseraEngine* e, TesseraHandId id) {
    if (!e->state || !e->state->target || id == 0) return NULL;
    const TsSnapshot* t = e->state->target;
    for (size_t i = 0; i < t->hand_count; ++i)
        if (t->hands[i].id == id) return &t->hands[i];
    return NULL;
}

/* FOCUS_CARD framing: place `out` in front of card (C,Q,w,h) at a distance that
 * fills the frame with `k`/`aspect` margin. */
static void frame_card(const vec3 C, versor Q, float w, float h,
                       float k, float aspect, TsCamPose* out) {
    vec3 n; quat_axis(Q, (vec3){0.0f, 1.0f,  0.0f}, n); /* front normal (local +Y) */
    vec3 u; quat_axis(Q, (vec3){0.0f, 0.0f, -1.0f}, u); /* card up = art top (local -Z) */
    float d = fmaxf((h * 0.5f) / k, (w * 0.5f) / (k * aspect));
    if (d < 0.2f) d = 0.2f;
    vec3 off; glm_vec3_scale(n, d, off);
    glm_vec3_add((float*)C, off, out->eye);
    glm_vec3_copy((float*)C, out->target);
    glm_vec3_copy(u, out->up);
}

/* Resolve the goal pose for `cam` against the live scene. Returns false if the
 * mode references an object that is not live yet (caller keeps current pose). */
static bool resolve_camera_goal(TesseraEngine* e, const TesseraCamera* cam,
                                float aspect, TsCamPose* out) {
    if (aspect <= 0.0f) aspect = 16.0f / 9.0f;
    float fov = (isfinite(cam->fov) && cam->fov > 0.0f) ? cam->fov : e->camera.fov;
    out->fov = fov;
    glm_vec3_copy((vec3){0.0f, 1.0f, 0.0f}, out->up);

    float t   = tanf(fov * 0.5f);
    float pad = ts_clampf(cam->fit_padding > 0.0f ? cam->fit_padding : 0.08f, 0.0f, 0.9f);
    float k   = t * (1.0f - pad);
    if (k < 1e-4f) k = 1e-4f;

    switch (cam->mode) {
    case TESSERA_CAMERA_MANUAL: {
        versor q; norm_orientation(cam->orientation, q);
        vec3 eye = { finite_or(cam->position[0], 0.0f), finite_or(cam->position[1], 0.0f),
                     finite_or(cam->position[2], 0.0f) };
        vec3 fwd; quat_axis(q, (vec3){0.0f, 0.0f, -1.0f}, fwd);
        vec3 up;  quat_axis(q, (vec3){0.0f, 1.0f,  0.0f}, up);
        glm_vec3_copy(eye, out->eye);
        glm_vec3_add(eye, fwd, out->target);
        glm_vec3_copy(up, out->up);
        return true;
    }
    case TESSERA_CAMERA_TARGET: {
        out->eye[0] = finite_or(cam->position[0], 0.0f);
        out->eye[1] = finite_or(cam->position[1], 0.0f);
        out->eye[2] = finite_or(cam->position[2], 0.0f);
        out->target[0] = finite_or(cam->target[0], 0.0f);
        out->target[1] = finite_or(cam->target[1], 0.0f);
        out->target[2] = finite_or(cam->target[2], 0.0f);
        return true;
    }
    case TESSERA_CAMERA_FOCUS_TILE:
    case TESSERA_CAMERA_FOCUS_ENTITY:
    case TESSERA_CAMERA_FOCUS_DICE:
    case TESSERA_CAMERA_FOCUS_DRAW: {
        vec3 P; bool ok = false;
        switch (cam->mode) {
        case TESSERA_CAMERA_FOCUS_TILE:   ok = e->orch && ts_orch_tile_pos(e->orch, cam->target_id, P); break;
        case TESSERA_CAMERA_FOCUS_ENTITY: ok = e->orch && ts_orch_entity_pos(e->orch, cam->target_id, P); break;
        case TESSERA_CAMERA_FOCUS_DICE:   ok = e->dice && ts_dice_pos(e->dice, cam->target_id, P); break;
        case TESSERA_CAMERA_FOCUS_DRAW:   ok = e->orch && ts_orch_draw_pos(e->orch, cam->target_id, P); break;
        default: break;
        }
        if (!ok) return false;
        float dist  = (isfinite(cam->distance) && cam->distance > 0.1f) ? cam->distance : e->camera.distance;
        float yaw   = finite_or(cam->yaw, e->camera.yaw);
        float pitch = finite_or(cam->pitch, e->camera.pitch);
        ts_orbit_eye(P, dist, yaw, pitch, out->eye);
        glm_vec3_copy(P, out->target);
        return true;
    }
    case TESSERA_CAMERA_FOCUS_CARD: {
        vec3 C; versor Q; float w, h;
        if (!e->orch || !ts_orch_card_transform(e->orch, e, cam->target_id, C, Q, &w, &h))
            return false;
        frame_card(C, Q, w, h, k, aspect, out);
        return true;
    }
    case TESSERA_CAMERA_FOCUS_HAND: {
        const TesseraHandPlacement* hp = find_target_hand(e, cam->target_id);
        if (!hp) return false;
        /* A specific card in this hand requested and live -> frame it fullscreen. */
        if (cam->focus_card_id != 0 && e->orch &&
            ts_orch_card_hand(e->orch, cam->focus_card_id) == cam->target_id) {
            vec3 C; versor Q; float w, h;
            if (ts_orch_card_transform(e->orch, e, cam->focus_card_id, C, Q, &w, &h)) {
                frame_card(C, Q, w, h, k, aspect, out);
                return true;
            }
        }
        /* Frame the whole hand. */
        vec3 H = { finite_or(hp->position[0], 0.0f), finite_or(hp->position[1], 0.0f),
                   finite_or(hp->position[2], 0.0f) };
        versor HQ; norm_orientation(hp->orientation, HQ);
        vec3 n; quat_axis(HQ, (vec3){0.0f, 0.0f, 1.0f}, n);
        vec3 u; quat_axis(HQ, (vec3){0.0f, 1.0f, 0.0f}, u);
        vec3 r; quat_axis(HQ, (vec3){1.0f, 0.0f, 0.0f}, r);
        float half_w, half_h;
        if (!e->orch || !ts_orch_hand_extent(e->orch, e, cam->target_id, r, u, H, &half_w, &half_h))
            return false;
        float d = fmaxf(half_h / k, half_w / (k * aspect));
        if (d < 0.4f) d = 0.4f;
        vec3 off; glm_vec3_scale(n, d, off);
        glm_vec3_add(H, off, out->eye);
        glm_vec3_copy(H, out->target);
        glm_vec3_copy(u, out->up);
        return true;
    }
    case TESSERA_CAMERA_ORBIT:
    default: {
        vec3 P; ts_grid_to_world_f(finite_or(cam->focus.x, 0.0f), finite_or(cam->focus.y, 0.0f), P);
        float dist  = (isfinite(cam->distance) && cam->distance > 0.1f) ? cam->distance : e->camera.distance;
        float yaw   = finite_or(cam->yaw, e->camera.yaw);
        float pitch = finite_or(cam->pitch, e->camera.pitch);
        ts_orbit_eye(P, dist, yaw, pitch, out->eye);
        glm_vec3_copy(P, out->target);
        return true;
    }
    }
}

/* Bit-exact float compare so NaN "keep current" sentinels match themselves. */
static bool cam_feq(float a, float b) {
    uint32_t ua, ub;
    memcpy(&ua, &a, sizeof ua);
    memcpy(&ub, &b, sizeof ub);
    return ua == ub;
}

/* Two camera specs are the same goal when every field that feeds
 * resolve_camera_goal matches (bit-exact, so sentinels compare equal). */
static bool camera_spec_eq(const TesseraCamera* a, const TesseraCamera* b) {
    if (a->mode != b->mode) return false;
    if (a->target_id != b->target_id || a->focus_card_id != b->focus_card_id) return false;
    if (!cam_feq(a->focus.x, b->focus.x) || !cam_feq(a->focus.y, b->focus.y)) return false;
    if (!cam_feq(a->distance, b->distance)) return false;
    if (!cam_feq(a->yaw, b->yaw) || !cam_feq(a->pitch, b->pitch)) return false;
    if (!cam_feq(a->fov, b->fov) || !cam_feq(a->fit_padding, b->fit_padding)) return false;
    for (int i = 0; i < 3; ++i) if (!cam_feq(a->position[i], b->position[i])) return false;
    for (int i = 0; i < 4; ++i) if (!cam_feq(a->orientation[i], b->orientation[i])) return false;
    for (int i = 0; i < 3; ++i) if (!cam_feq(a->target[i], b->target[i])) return false;
    return true;
}

static void apply_camera(TesseraEngine* e, const TesseraCamera* c) {
    /* Re-emitting the identical camera spec (common when one logical move plays
     * as several scenes) must NOT restart the transition: a fresh tween would
     * snap the eye back to the object's *old* position and then chase it, so a
     * follow mode visibly trails its moving target. Detect the no-change case
     * and keep the current tween / idle-follow running instead. */
    bool same = e->cam_spec_have && camera_spec_eq(c, &e->cam_spec);

    /* Remember the promoted spec so advance_camera can re-resolve the goal each
     * tick (follow modes track a moving object). */
    e->cam_spec = *c;
    e->cam_spec_have = true;

    float aspect = current_aspect(e);
    TsCamPose G;
    if (!resolve_camera_goal(e, c, aspect, &G)) {
        /* Target not live yet: keep whatever pose we already have (no jump). */
        return;
    }
    if (!e->cam_have) {
        ts_camera_set_look(&e->camera, &G, aspect);
        e->cam_cur = G;
        e->cam_to = G;
        e->cam_have = true;
        e->cam_active = false;
        return;
    }
    if (same) {
        /* Unchanged goal: leave the in-flight tween (or idle-follow) alone so the
         * camera stays locked onto the live target. advance_camera re-resolves G
         * every tick, so tracking continues seamlessly across the scene swap. */
        e->cam_to = G;
        return;
    }
    /* Tween from the current live pose to the new goal over timing.camera_s. */
    e->cam_from = e->cam_cur;
    e->cam_to = G;
    float dur = e->timing.camera_s > 0.0f ? e->timing.camera_s : 0.5f;
    ts_tween_start(&e->cam_tween, dur, 0.0f, TS_EASE_IN_OUT_CUBIC);
    e->cam_active = true;
}

static void advance_camera(TesseraEngine* e, float dt) {
    if (!e->cam_spec_have) return;

    float aspect = current_aspect(e);
    TsCamPose G;
    bool resolved = resolve_camera_goal(e, &e->cam_spec, aspect, &G);

    if (e->cam_active) {
        ts_tween_advance(&e->cam_tween, dt);
        float t = ts_tween_value01(&e->cam_tween);
        if (resolved) {
            /* Interpolate pose toward the LIVE goal (re-resolved each tick). */
            TsCamPose p;
            glm_vec3_lerp(e->cam_from.eye, G.eye, t, p.eye);
            glm_vec3_lerp(e->cam_from.target, G.target, t, p.target);
            vec3 up; glm_vec3_lerp(e->cam_from.up, G.up, t, up);
            if (glm_vec3_norm(up) < 1e-6f) glm_vec3_copy((vec3){0.0f, 1.0f, 0.0f}, up);
            else glm_vec3_normalize(up);
            glm_vec3_copy(up, p.up);
            p.fov = ts_lerpf(e->cam_from.fov, G.fov, t);
            ts_camera_set_look(&e->camera, &p, aspect);
            e->cam_cur = p;
        }
        /* Unresolved: hold the last pose but keep advancing the clock so the
         * tween can still complete. */
        if (ts_tween_done(&e->cam_tween)) e->cam_active = false;
    } else if (resolved) {
        /* Idle-follow: lock the camera onto the live goal every tick — this is
         * what makes follow modes track after the tween ends. */
        ts_camera_set_look(&e->camera, &G, aspect);
        e->cam_cur = G;
    }
    /* Idle + unresolved: hold the last pose (do nothing). */
}

void ts_engine_tick(TesseraEngine* e, double dt) {
    ts_engine_advance(e, dt);
    ts_engine_render(e);
}

static void ts_engine_settle_operation(TesseraEngine* e);

void ts_engine_advance(TesseraEngine* e, double dt) {
    if (dt < 0) dt = 0;
    e->clock += dt;
    ts_arena_reset(&e->frame_arena);

    /* Promote any pending snapshot (thread-safe swap), then diff into the
     * orchestrator to (re)start transitions. */
    if (e->state) {
        TsSnapshot *prev = NULL, *next = NULL;
        bool promoted = false;
        SDL_LockMutex(e->state_mutex);
        promoted = ts_state_promote(e->state, &prev, &next);
        SDL_UnlockMutex(e->state_mutex);
        if (promoted && next) {
            if (!e->orch) e->orch = ts_orch_create();
            if (e->orch) ts_orch_on_promote(e->orch, e, prev, next, &e->timing);
            if (!e->fx) e->fx = ts_fx_create();
            ts_fx_on_promote(e, prev, next);
            ts_dice_on_promote(e, prev, next, e->timing.remove_s);
            apply_camera(e, &next->camera);
            /* This snapshot's op is now in flight; it completes when the
             * transition it set up next goes idle (below). A promote that
             * supersedes an earlier in-flight op just adopts the newer id —
             * monotonicity means completing it also completes the older. */
            e->op_inflight = next->op_id;
            e->op_has_inflight = true;
        }
    }

    float mult = e->timing.speed_multiplier > 0.0f ? e->timing.speed_multiplier : 1.0f;
    if (e->orch) ts_orch_advance(e->orch, (float)dt * mult);
    if (e->fx) ts_fx_advance(e, (float)dt * mult);
    if (e->dice) ts_dice_advance(e->dice, (float)dt * mult);
    advance_camera(e, (float)dt * mult);

    ts_engine_settle_operation(e);
}

/* A promoted transition completes the tick it first has no animation left. Fire
 * at most one completion per tick; the callback runs outside the state mutex so
 * it may re-enter the engine's read paths (but not set_state). */
static void ts_engine_settle_operation(TesseraEngine* e) {
    if (!e->op_has_inflight) return;
    bool idle = !e->cam_active
             && (!e->fx   || ts_fx_is_idle(e->fx))
             && (!e->dice || ts_dice_all_idle(e->dice))
             && (!e->orch || ts_orch_is_idle(e->orch));
    if (!idle) return;

    TesseraOpId done = e->op_inflight;
    e->op_has_inflight = false;
    SDL_LockMutex(e->state_mutex);
    if (done > e->op_completed) e->op_completed = done;
    SDL_UnlockMutex(e->state_mutex);
    if (e->op_cb) e->op_cb(done, e->op_cb_user);
}
