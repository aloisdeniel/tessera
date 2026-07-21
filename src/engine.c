/* engine.c — per-frame tick + the render pass. */
#include "engine.h"
#include "state.h"
#include "orchestration/orch.h"
#include "anim/skeleton.h"
#include "fx/fx.h"
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

    TsDrawItem* items = NULL;
    size_t count = ts_scene_build_drawlist(e, &e->frame_arena, &items);
    if (count == 0 || !g->mesh_pipeline) return;

    TsFrameUniform fu;
    fill_frame_uniform(e, &fu);

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

    /* M7 blob shadows, M6 particles: translucent passes after opaque geometry. */
    record_blobs(e, cmd, pass, &fu);
    ts_fx_record(e, cmd, pass, &fu);
}

/* Apply a state camera (grid focus) onto the live orbit camera. The first pose
 * snaps; subsequent changes glide over timing.camera_s (M7). */

/* Return v if finite, else the fallback — guards against NaN/Inf reaching the
 * camera math from malformed FFI state (see advance_camera). */
static float finite_or(float v, float fallback) {
    return isfinite(v) ? v : fallback;
}

static void apply_camera(TesseraEngine* e, const TesseraCamera* c) {
    float cyaw   = finite_or(c->yaw, e->camera.yaw);
    float cpitch = finite_or(c->pitch, e->camera.pitch);
    vec3 focus; ts_grid_to_world_f(c->focus.x, c->focus.y, focus);
    float fov  = (isfinite(c->fov) && c->fov > 0.0f) ? c->fov : e->camera.fov;
    float dist = (isfinite(c->distance) && c->distance > 0.1f) ? c->distance : e->camera.distance;

    if (!e->cam_have) {
        ts_camera_set(&e->camera, focus, dist, cyaw, cpitch, fov);
        e->cam_have = true;
        e->cam_active = false;
        return;
    }
    /* set up a glide from the current pose to the requested one */
    e->cam_from = e->camera;
    e->cam_to = e->camera;
    glm_vec3_copy(focus, e->cam_to.focus);
    e->cam_to.distance = dist;
    e->cam_to.yaw = cyaw;
    e->cam_to.pitch = ts_clampf(cpitch, -1.45f, 1.45f);
    e->cam_to.fov = fov;
    float dur = e->timing.camera_s > 0.0f ? e->timing.camera_s : 0.5f;
    ts_tween_start(&e->cam_tween, dur, 0.0f, TS_EASE_IN_OUT_CUBIC);
    e->cam_active = true;
}

static void advance_camera(TesseraEngine* e, float dt) {
    if (!e->cam_active) return;
    ts_tween_advance(&e->cam_tween, dt);
    float t = ts_tween_value01(&e->cam_tween);
    vec3 f;
    glm_vec3_lerp(e->cam_from.focus, e->cam_to.focus, t, f);
    /* shortest-arc yaw so a spin doesn't take the long way round. remainderf
     * maps into [-pi,pi] in O(1) — an unbounded while-loop would hang on a
     * non-finite delta (apply_camera also sanitizes, this is defense in depth). */
    float dyaw = e->cam_to.yaw - e->cam_from.yaw;
    dyaw = isfinite(dyaw) ? remainderf(dyaw, 2.0f * GLM_PIf) : 0.0f;
    float yaw   = e->cam_from.yaw + dyaw * t;
    float dist  = ts_lerpf(e->cam_from.distance, e->cam_to.distance, t);
    float pitch = ts_lerpf(e->cam_from.pitch, e->cam_to.pitch, t);
    float fov   = ts_lerpf(e->cam_from.fov, e->cam_to.fov, t);
    ts_camera_set(&e->camera, f, dist, yaw, pitch, fov);
    if (ts_tween_done(&e->cam_tween)) e->cam_active = false;
}

void ts_engine_tick(TesseraEngine* e, double dt) {
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
            apply_camera(e, &next->camera);
        }
    }

    float mult = e->timing.speed_multiplier > 0.0f ? e->timing.speed_multiplier : 1.0f;
    if (e->orch) ts_orch_advance(e->orch, (float)dt * mult);
    if (e->fx) ts_fx_advance(e, (float)dt * mult);
    advance_camera(e, (float)dt * mult);

    ts_engine_render(e);
}
