/* engine.c — per-frame tick + the render pass. */
#include "engine.h"
#include "state.h"
#include "orchestration/orch.h"
#include "anim/skeleton.h"
#include "fx/fx.h"
#include "dice/dice.h"
#include "card/card.h"
#include <math.h>
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

/* ---- typed engine event stream ---------------------------------------- */
/* Push one event into the poll ring (dropping the oldest on overflow) and
 * stage it for the end-of-tick callback flush. Tick thread only; the ring
 * bookkeeping is mutex-guarded because tessera_poll_events is any-thread. */
void ts_engine_emit_event(TesseraEngine* e, uint32_t type, uint32_t subject,
                          uint64_t subject_id, TesseraCoord coord, float value) {
    TesseraEvent ev = {
        .time = e->clock,
        .subject_id = subject_id,
        .type = type,
        .subject = subject,
        .coord = coord,
        .value = value,
        .reserved = 0,
    };
    SDL_LockMutex(e->state_mutex);
    if (e->ev_len == TS_EVENT_CAP) {          /* full: drop the oldest */
        e->ev_head = (e->ev_head + 1) % TS_EVENT_CAP;
        e->ev_len--;
        e->ev_dropped++;
    }
    e->ev_ring[(e->ev_head + e->ev_len) % TS_EVENT_CAP] = ev;
    e->ev_len++;
    SDL_UnlockMutex(e->state_mutex);

    if (e->ev_stage_len < TS_EVENT_CAP)
        e->ev_stage[e->ev_stage_len++] = ev;
}

/* Deliver this tick's staged events to the registered callback, in emission
 * order, outside the state mutex (mirrors the op-completion callback). The
 * callback slot is read AND invoked under cb_mutex so a concurrent
 * tessera_set_event_callback(e, NULL, ...) cannot return while a delivery
 * using the old fn is still in flight (see engine.h). */
static void ts_engine_flush_events(TesseraEngine* e) {
    uint32_t n = e->ev_stage_len;
    e->ev_stage_len = 0;
    if (n == 0) return;
    SDL_LockMutex(e->cb_mutex);
    if (e->ev_cb)
        for (uint32_t i = 0; i < n; ++i) e->ev_cb(&e->ev_stage[i], e->ev_cb_user);
    SDL_UnlockMutex(e->cb_mutex);
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
    if (e->shadow_active && e->gpu.shadow_res > 0) {
        glm_mat4_copy(e->shadow_vp, u->light_vp);
        u->shadow_params[0] = 1.0f;
        u->shadow_params[1] = 1.0f / (float)e->gpu.shadow_res;
        u->shadow_params[2] = e->shadow_bias_const;
        u->shadow_params[3] = e->shadow_bias_slope;
    } else {
        glm_mat4_identity(u->light_vp);
        glm_vec4_zero(u->shadow_params);
    }
}

/* Shadow-map resolution from the quality settings: high presets (msaa 4) get a
 * 4K map, reduced-resolution presets (render_scale <= 0.75, mobile) drop to
 * half. Bounded-board fitting keeps texel density high at every size. */
static uint32_t shadow_map_resolution(const TesseraQuality* q) {
    uint32_t res = q->msaa >= 4 ? 4096 : 2048;
    float rs = q->render_scale > 0.0f ? q->render_scale : 1.0f;
    if (rs <= 0.75f) res /= 2;
    if (res < 512) res = 512;
    return res;
}

/* Conservative world-space bounding sphere of one draw item: translation plus
 * a radius from the model's basis scale (unit meshes span roughly ±0.5..1). */
static void item_bounds(const mat4 model, vec3 out_c, float* out_r) {
    out_c[0] = model[3][0]; out_c[1] = model[3][1]; out_c[2] = model[3][2];
    float sx = glm_vec3_norm((float*)model[0]);
    float sy = glm_vec3_norm((float*)model[1]);
    float sz = glm_vec3_norm((float*)model[2]);
    float s = fmaxf(sx, fmaxf(sy, sz));
    *out_r = 1.5f * (s > 1e-4f ? s : 1.0f);
}

static void aabb_add(vec3 lo, vec3 hi, const vec3 c, float r, bool* have) {
    for (int i = 0; i < 3; ++i) {
        float a = c[i] - r, b = c[i] + r;
        if (!*have) { lo[i] = a; hi[i] = b; }
        else { if (a < lo[i]) lo[i] = a; if (b > hi[i]) hi[i] = b; }
    }
    *have = true;
}

/* Build the light view-projection fitted to the world AABB [lo,hi]: an ortho
 * frustum sized to the box's light-space extents so texel density stays high
 * on bounded boards. Also outputs the light-space depth range (world units)
 * so shader biases can be expressed in world units. */
static void fit_light_matrix(const TesseraLight* light, const vec3 lo, const vec3 hi,
                             mat4 out_vp, float* out_depth_range) {
    vec3 L = { light->dir[0], light->dir[1], light->dir[2] };
    if (glm_vec3_norm(L) < 1e-5f) { L[0] = -0.4f; L[1] = -1.0f; L[2] = -0.3f; }
    glm_vec3_normalize(L);

    vec3 center; glm_vec3_add((float*)lo, (float*)hi, center);
    glm_vec3_scale(center, 0.5f, center);
    vec3 half; glm_vec3_sub((float*)hi, (float*)lo, half);
    glm_vec3_scale(half, 0.5f, half);
    float radius = glm_vec3_norm(half);
    if (radius < 1.0f) radius = 1.0f;

    vec3 eye, back;
    glm_vec3_scale(L, -(radius + 2.0f), back);
    glm_vec3_add(center, back, eye);
    vec3 up = { 0.0f, 1.0f, 0.0f };
    if (fabsf(L[1]) > 0.95f) { up[1] = 0.0f; up[2] = 1.0f; }

    mat4 view; ts_look_at(eye, center, up, view);

    /* Tight light-space bounds of the 8 AABB corners. */
    vec3 vlo = {0}, vhi = {0}; bool have = false;
    for (int i = 0; i < 8; ++i) {
        vec4 c = { (i & 1) ? hi[0] : lo[0],
                   (i & 2) ? hi[1] : lo[1],
                   (i & 4) ? hi[2] : lo[2], 1.0f };
        vec4 v; glm_mat4_mulv(view, c, v);
        aabb_add(vlo, vhi, v, 0.0f, &have);
    }
    const float pad = 0.05f;
    /* RH view looks down -Z: points sit at negative z. Pull the near plane a
     * little toward the light so casters right at the box edge still render. */
    float znear = -vhi[2] - 1.0f;
    float zfar  = -vlo[2] + 1.0f;
    if (znear < 0.01f) znear = 0.01f;
    mat4 proj;
    glm_ortho_rh_zo(vlo[0] - pad, vhi[0] + pad, vlo[1] - pad, vhi[1] + pad,
                    znear, zfar, proj);
    glm_mat4_mul(proj, view, out_vp);
    *out_depth_range = zfar - znear;
}

/* Render the directional shadow map: one depth-only pass from the light over
 * every caster (tiles, entities incl. skinned, dice, cards), fitted each frame
 * to the occupied scene bounds. Runs before the main pass on the same command
 * buffer; the main pass then samples the map with PCF (mesh.fragment). */
void ts_engine_shadow_pass(TesseraEngine* e, SDL_GPUCommandBuffer* cmd) {
    TsGpu* g = &e->gpu;
    /* tessera_set_quality is any-thread (writes under state_mutex); take the
     * frame's one consistent copy here — this pass runs before every other
     * quality consumer of the frame (record_blobs), which read the copy. */
    SDL_LockMutex(e->state_mutex);
    e->quality_frame = e->quality;
    SDL_UnlockMutex(e->state_mutex);
    e->shadow_active = false;
    if (e->quality_frame.shadows != TESSERA_SHADOW_MAP) return;
    if (!g->shadow_pipeline || !g->shadow_card_pipeline) return;

    /* Casters: the opaque draw list (tiles/entities/dice) + card slabs. Built
     * into the frame arena independently of the main pass's own lists. */
    TsDrawItem* items = NULL;
    size_t count = ts_scene_build_drawlist(e, &e->frame_arena, &items);
    TsCardDrawItem* cards = NULL;
    size_t ncards = e->orch ? ts_orch_build_cards(e->orch, e, &e->frame_arena, &cards) : 0;
    if (count == 0 && ncards == 0) return;

    vec3 lo = {0}, hi = {0}; bool have = false;
    for (size_t i = 0; i < count; ++i) {
        if (!items[i].mesh || !items[i].mesh->vbo) continue;
        vec3 c; float r;
        item_bounds(items[i].model, c, &r);
        aabb_add(lo, hi, c, r, &have);
    }
    for (size_t i = 0; i < ncards; ++i) {
        if (!cards[i].mesh || !cards[i].mesh->vbo) continue;
        vec3 c; float r;
        item_bounds(cards[i].model, c, &r);
        aabb_add(lo, hi, c, r, &have);
    }
    if (!have) return;

    float depth_range = 1.0f;
    fit_light_matrix(&e->light, lo, hi, e->shadow_vp, &depth_range);
    /* Sampling biases in normalized depth, from world-unit tuning at board
     * scale (~0.02 units constant, ~0.10 slope-scaled) so they track the
     * per-frame fitted depth range. */
    e->shadow_bias_const = 0.02f / depth_range;
    e->shadow_bias_slope = 0.10f / depth_range;

    if (!ts_gpu_ensure_shadow_map(g, shadow_map_resolution(&e->quality_frame))) return;

    SDL_GPUDepthStencilTargetInfo depth = {
        .texture = g->shadow_map,
        .clear_depth = 1.0f,
        .load_op = SDL_GPU_LOADOP_CLEAR,
        .store_op = SDL_GPU_STOREOP_STORE,   /* sampled by the main pass */
        .stencil_load_op = SDL_GPU_LOADOP_DONT_CARE,
        .stencil_store_op = SDL_GPU_STOREOP_DONT_CARE,
    };
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, NULL, 0, &depth);

    TsShadowFrameUniform sfu;
    glm_mat4_copy(e->shadow_vp, sfu.light_vp);
    SDL_PushGPUVertexUniformData(cmd, 0, &sfu, sizeof sfu);

    /* 0=none, 1=static, 2=skinned, 3=card — lazy pipeline switching. */
    int bound = 0;
    for (size_t i = 0; i < count; ++i) {
        const TsDrawItem* it = &items[i];
        if (!it->mesh || !it->mesh->vbo) continue;
        if (it->skinned && (!it->joints || !g->shadow_skinned_pipeline)) continue;

        int want = it->skinned ? 2 : 1;
        if (want != bound) {
            SDL_BindGPUGraphicsPipeline(pass, want == 2 ? g->shadow_skinned_pipeline
                                                        : g->shadow_pipeline);
            SDL_PushGPUVertexUniformData(cmd, 0, &sfu, sizeof sfu);
            bound = want;
        }

        TsObjectUniform ou;
        glm_mat4_copy((vec4*)it->model, ou.model);
        glm_vec4_copy((float*)it->tint, ou.tint);
        glm_vec4_copy((float*)it->uv_rect, ou.uv_rect);
        SDL_PushGPUVertexUniformData(cmd, 1, &ou, sizeof ou);

        if (it->skinned) {
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
        SDL_DrawGPUIndexedPrimitives(pass, it->mesh->index_count, 1, 0, 0, 0);
    }

    for (size_t i = 0; i < ncards; ++i) {
        const TsCardDrawItem* c = &cards[i];
        if (!c->mesh || !c->mesh->vbo) continue;
        if (bound != 3) {
            SDL_BindGPUGraphicsPipeline(pass, g->shadow_card_pipeline);
            SDL_PushGPUVertexUniformData(cmd, 0, &sfu, sizeof sfu);
            bound = 3;
        }
        TsObjectUniform ou;
        glm_mat4_copy((vec4*)c->model, ou.model);
        glm_vec4_copy((float*)c->tint, ou.tint);
        glm_vec4_copy((vec4){ 0.0f, 0.0f, 1.0f, 1.0f }, ou.uv_rect);
        SDL_PushGPUVertexUniformData(cmd, 1, &ou, sizeof ou);
        SDL_GPUBufferBinding vb = { .buffer = c->mesh->vbo, .offset = 0 };
        SDL_GPUBufferBinding ib = { .buffer = c->mesh->ibo, .offset = 0 };
        SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
        SDL_BindGPUIndexBuffer(pass, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        SDL_DrawGPUIndexedPrimitives(pass, c->mesh->index_count, 1, 0, 0, 0);
    }

    SDL_EndGPURenderPass(pass);
    e->shadow_active = true;
}

/* Cap on distinct highlight composites per frame (each is a mask + fullscreen
 * pass; selections are one or two objects in practice). */
#define TS_MAX_HIGHLIGHT_PASSES 16

/* Selection outline & glow post pass. For each live highlight the flagged
 * object is re-rendered flat into the cached silhouette mask — reusing the
 * frame drawlist (rebuilt into the frame arena like the shadow pass does),
 * filtered by the (kind, id) tags, so highlighted objects are tracked at
 * their live interpolated transforms, skinned pose included — then the mask
 * is dilated (outline) or blurred (glow) over `dst` in a fullscreen pass.
 * Runs after the main pass and after depth-of-field so the selection stays
 * crisp over the blur. */
void ts_engine_highlight_pass(TesseraEngine* e, SDL_GPUCommandBuffer* cmd,
                              SDL_GPUTexture* dst, uint32_t w, uint32_t h) {
    TsGpu* g = &e->gpu;
    if (!e->orch) return;
    if (!g->mask_pipeline || !g->hl_outline_pipeline || !g->hl_glow_pipeline) return;

    TsHighlightItem* hls = NULL;
    size_t nh = ts_orch_build_highlights(e->orch, &e->frame_arena, &hls);
    if (nh == 0) return;
    if (nh > TS_MAX_HIGHLIGHT_PASSES) nh = TS_MAX_HIGHLIGHT_PASSES;
    if (!ts_gpu_ensure_mask_target(g, w, h)) return;

    /* The same live drawlists the main pass consumed, rebuilt into the frame
     * arena; matching draws are re-rendered flat into the silhouette mask. */
    TsDrawItem* items = NULL;
    size_t count = ts_scene_build_drawlist(e, &e->frame_arena, &items);
    TsCardDrawItem* cards = NULL;
    size_t ncards = ts_orch_build_cards(e->orch, e, &e->frame_arena, &cards);

    TsShadowFrameUniform sfu;   /* view_proj in the shared position-only VS */
    glm_mat4_copy(e->camera.view_proj, sfu.light_vp);

    for (size_t hi = 0; hi < nh; ++hi) {
        const TsHighlightItem* hl = &hls[hi];

        /* Skip the whole mask + composite when nothing matches (target not
         * live yet, or already fully faded). */
        bool any = false;
        for (size_t i = 0; i < count && !any; ++i)
            any = items[i].hl_id != 0 && items[i].hl_id == hl->id &&
                  items[i].hl_kind == hl->kind &&
                  items[i].mesh && items[i].mesh->vbo;
        if (!any && hl->kind == TESSERA_HIGHLIGHT_CARD)
            for (size_t i = 0; i < ncards && !any; ++i)
                any = cards[i].hl_id == hl->id && cards[i].mesh && cards[i].mesh->vbo;
        if (!any) continue;

        /* ---- silhouette mask: matching draws, flat, no depth ---- */
        SDL_GPUColorTargetInfo mct = {
            .texture = g->mask_tex,
            .clear_color = (SDL_FColor){0.0f, 0.0f, 0.0f, 0.0f},
            .load_op = SDL_GPU_LOADOP_CLEAR,
            .store_op = SDL_GPU_STOREOP_STORE };
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &mct, 1, NULL);

        /* 0=none, 1=static, 2=skinned, 3=card — lazy pipeline switching. */
        int bound = 0;
        for (size_t i = 0; i < count; ++i) {
            const TsDrawItem* it = &items[i];
            if (it->hl_id == 0 || it->hl_id != hl->id || it->hl_kind != hl->kind) continue;
            if (!it->mesh || !it->mesh->vbo) continue;
            if (it->skinned && (!it->joints || !g->mask_skinned_pipeline)) continue;

            int want = it->skinned ? 2 : 1;
            if (want != bound) {
                SDL_BindGPUGraphicsPipeline(pass, want == 2 ? g->mask_skinned_pipeline
                                                            : g->mask_pipeline);
                SDL_PushGPUVertexUniformData(cmd, 0, &sfu, sizeof sfu);
                bound = want;
            }

            TsObjectUniform ou;
            glm_mat4_copy((vec4*)it->model, ou.model);
            glm_vec4_copy((float*)it->tint, ou.tint);
            glm_vec4_copy((float*)it->uv_rect, ou.uv_rect);
            SDL_PushGPUVertexUniformData(cmd, 1, &ou, sizeof ou);

            if (it->skinned) {
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
            SDL_DrawGPUIndexedPrimitives(pass, it->mesh->index_count, 1, 0, 0, 0);
        }

        if (hl->kind == TESSERA_HIGHLIGHT_CARD && g->mask_card_pipeline) {
            for (size_t i = 0; i < ncards; ++i) {
                const TsCardDrawItem* c = &cards[i];
                if (c->hl_id != hl->id || !c->mesh || !c->mesh->vbo) continue;
                if (bound != 3) {
                    SDL_BindGPUGraphicsPipeline(pass, g->mask_card_pipeline);
                    SDL_PushGPUVertexUniformData(cmd, 0, &sfu, sizeof sfu);
                    bound = 3;
                }
                TsObjectUniform ou;
                glm_mat4_copy((vec4*)c->model, ou.model);
                glm_vec4_copy((float*)c->tint, ou.tint);
                glm_vec4_copy((vec4){ 0.0f, 0.0f, 1.0f, 1.0f }, ou.uv_rect);
                SDL_PushGPUVertexUniformData(cmd, 1, &ou, sizeof ou);
                SDL_GPUBufferBinding vb = { .buffer = c->mesh->vbo, .offset = 0 };
                SDL_GPUBufferBinding ib = { .buffer = c->mesh->ibo, .offset = 0 };
                SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
                SDL_BindGPUIndexBuffer(pass, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
                SDL_DrawGPUIndexedPrimitives(pass, c->mesh->index_count, 1, 0, 0, 0);
            }
        }
        SDL_EndGPURenderPass(pass);

        /* ---- fullscreen composite over the lit (post-DoF) scene ---- */
        TsHighlightPost p = {
            .color = { hl->color[0], hl->color[1], hl->color[2], hl->color[3] },
            .thickness_px = hl->thickness,
            .intensity = hl->intensity,
            .glow = hl->style == TESSERA_HIGHLIGHT_GLOW,
        };
        ts_gpu_highlight_post(g, cmd, g->mask_tex, dst, w, h, &p);
    }
}

/* Draw a blob shadow decal under each live entity (M7). Called after the opaque
 * mesh pass so geometry depth occludes shadows correctly. */
static void record_blobs(TesseraEngine* e, SDL_GPUCommandBuffer* cmd,
                         SDL_GPURenderPass* pass, const TsFrameUniform* fu) {
    TsGpu* g = &e->gpu;
    if (!g->blob_pipeline || !e->orch) return;
    /* Blob decals only in BLOB mode: MAP renders a real depth-map shadow and
     * suppresses the blobs (tile-overlay decals are unaffected). Reads the
     * frame's quality copy taken by ts_engine_shadow_pass (same frame). */
    if (e->quality_frame.shadows != TESSERA_SHADOW_BLOB) return;

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

/* Draw the state-driven tile-overlay decals (move-range fills, threat rings,
 * drop-target highlights) on top of the tiles. Runs after the opaque pass so
 * pieces occlude them; before the blob pass so shadows composite on top. Same
 * no-depth-write discipline as blob shadows, so they never z-fight tile tops. */
static void record_overlays(TesseraEngine* e, SDL_GPUCommandBuffer* cmd,
                            SDL_GPURenderPass* pass, const TsFrameUniform* fu) {
    TsGpu* g = &e->gpu;
    if (!g->overlay_pipeline || !e->orch) return;

    TsOverlayItem* items = NULL;
    size_t n = ts_orch_build_overlays(e->orch, e, &e->frame_arena, &items);
    if (n == 0) return;

    const TsMesh* quad = &e->registry.quad_mesh;
    if (!quad->vbo) return;

    SDL_BindGPUGraphicsPipeline(pass, g->overlay_pipeline);
    SDL_PushGPUVertexUniformData(cmd, 0, fu, sizeof *fu);
    SDL_GPUBufferBinding vb = { .buffer = quad->vbo, .offset = 0 };
    SDL_GPUBufferBinding ib = { .buffer = quad->ibo, .offset = 0 };
    SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
    SDL_BindGPUIndexBuffer(pass, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);

    for (size_t i = 0; i < n; ++i) {
        const TsOverlayItem* it = &items[i];
        TsOverlayUniform ou;
        mat4 m; glm_mat4_identity(m);
        glm_translate(m, (vec3){ it->center[0], it->center[1], it->center[2] });
        float s = TS_TILE_SIZE * it->scale;
        glm_scale(m, (vec3){ s, 1.0f, s });
        glm_mat4_copy(m, ou.model);
        glm_vec4_copy((vec4){ it->color[0], it->color[1], it->color[2], it->color[3] }, ou.tint);
        glm_vec4_copy((vec4){ it->uv.u0, it->uv.v0, it->uv.u1, it->uv.v1 }, ou.uv_rect);
        glm_vec4_copy((vec4){ (float)it->shape, 0.0f, 0.0f, 0.0f }, ou.params);
        SDL_PushGPUVertexUniformData(cmd, 1, &ou, sizeof ou);

        SDL_GPUTexture* tex = ts_registry_atlas_texture(&e->registry, it->atlas);
        SDL_GPUTextureSamplerBinding tsb = {
            .texture = tex ? tex : e->registry.white.texture,
            .sampler = g->linear_sampler };
        SDL_BindGPUFragmentSamplers(pass, 0, &tsb, 1);
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

/* ---- 3D text labels --------------------------------------------------- */
/* How far a flat (ground) label floats above the surface, and how far a
 * billboard label is pulled toward the eye so it wins the depth test against
 * the piece it hovers over (draw-order + camera nudge instead of polygon
 * offset; the pipeline never writes depth). */
#define TS_LABEL_LIFT  0.03f
#define TS_LABEL_NUDGE 0.12f

/* Draw one text run as per-glyph quads along `right`, rows toward `down`,
 * centred on `pos`. The text pipeline + quad buffers are already bound; the
 * font atlas is bound by the caller. */
static void record_text_run(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass,
                            const TsMesh* quad, const TsFontDef* font,
                            const char* text, const vec3 pos, float size,
                            const float color[4], const vec3 right, const vec3 down) {
    float scale = size / font->pixel_height;
    float width = ts_font_text_width(font, text);
    float cursor = -0.5f * width;                       /* centre horizontally */
    float voff = 0.5f * (font->ascent + font->descent); /* centre vertically   */

    const char* p = text;
    for (;;) {
        uint32_t cp = ts_utf8_next(&p);
        if (cp == 0) break;
        const TsGlyph* gl = ts_font_glyph(font, cp);
        if (!gl) continue;
        float gw = (gl->x1 - gl->x0) * scale;
        float gh = (gl->y1 - gl->y0) * scale;
        if (gw > 0.0f && gh > 0.0f) {
            float cx = (cursor + 0.5f * (gl->x0 + gl->x1)) * scale;
            float cy = (voff + 0.5f * (gl->y0 + gl->y1)) * scale;

            TsObjectUniform ou;
            memset(&ou, 0, sizeof ou);
            /* columns: local X -> right*gw, local Z -> down*gh, origin at the
             * glyph centre (the quad mesh spans ±0.5 in X/Z with uv 0..1). */
            for (int k = 0; k < 3; ++k) {
                ou.model[0][k] = right[k] * gw;
                ou.model[2][k] = down[k] * gh;
                ou.model[3][k] = pos[k] + right[k] * cx + down[k] * cy;
            }
            ou.model[1][1] = 1.0f;
            ou.model[3][3] = 1.0f;
            glm_vec4_copy((float*)color, ou.tint);
            glm_vec4_copy((vec4){ gl->u0, gl->v0, gl->u1, gl->v1 }, ou.uv_rect);
            SDL_PushGPUVertexUniformData(cmd, 1, &ou, sizeof ou);
            SDL_DrawGPUIndexedPrimitives(pass, quad->index_count, 1, 0, 0, 0);
        }
        cursor += gl->xadvance;
    }
}

/* Draw the state-driven text labels. Runs last so glyphs composite over every
 * translucent pass; depth-tested (LEQUAL) against geometry with a small nudge
 * toward the camera so a label attached to a piece stays readable. */
static void record_labels(TesseraEngine* e, SDL_GPUCommandBuffer* cmd,
                          SDL_GPURenderPass* pass, const TsFrameUniform* fu) {
    TsGpu* g = &e->gpu;
    if (!g->text_pipeline || !e->orch) return;

    TsLabelItem* items = NULL;
    size_t n = ts_orch_build_labels(e->orch, e, &e->frame_arena, &items);
    if (n == 0) return;

    const TsMesh* quad = &e->registry.quad_mesh;
    if (!quad->vbo) return;

    /* camera basis (rows of the view rotation) for billboards */
    vec3 cam_right = { e->camera.view[0][0], e->camera.view[1][0], e->camera.view[2][0] };
    vec3 cam_up    = { e->camera.view[0][1], e->camera.view[1][1], e->camera.view[2][1] };

    SDL_BindGPUGraphicsPipeline(pass, g->text_pipeline);
    SDL_PushGPUVertexUniformData(cmd, 0, fu, sizeof *fu);
    SDL_GPUBufferBinding vb = { .buffer = quad->vbo, .offset = 0 };
    SDL_GPUBufferBinding ib = { .buffer = quad->ibo, .offset = 0 };
    SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
    SDL_BindGPUIndexBuffer(pass, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);

    for (size_t i = 0; i < n; ++i) {
        const TsLabelItem* it = &items[i];
        TsDef* fd = ts_registry_get(&e->registry, it->font, TS_DEF_FONT);
        if (!fd || !fd->as.font.valid) continue;
        const TsFontDef* font = &fd->as.font;

        vec3 right, down, pos;
        glm_vec3_copy((float*)it->pos, pos);
        if (it->billboard) {
            glm_vec3_copy(cam_right, right);
            glm_vec3_negate_to(cam_up, down);
            vec3 to_eye;
            glm_vec3_sub(e->camera.eye, pos, to_eye);
            float d = glm_vec3_norm(to_eye);
            if (d > 1e-4f) glm_vec3_muladds(to_eye, TS_LABEL_NUDGE / d, pos);
        } else {
            /* flat on the ground: +X right, top toward -Z (map convention) */
            glm_vec3_copy((vec3){ 1.0f, 0.0f, 0.0f }, right);
            glm_vec3_copy((vec3){ 0.0f, 0.0f, 1.0f }, down);
            pos[1] += TS_LABEL_LIFT;
        }

        SDL_GPUTextureSamplerBinding tsb = {
            .texture = font->tex.texture, .sampler = g->linear_sampler };
        SDL_BindGPUFragmentSamplers(pass, 0, &tsb, 1);

        /* crossfade: previous text fades out beneath the new text fading in */
        if (it->prev_text) {
            float c[4] = { it->color[0], it->color[1], it->color[2],
                           it->color[3] * (1.0f - it->text_mix) };
            record_text_run(cmd, pass, quad, font, it->prev_text, pos, it->size,
                            c, right, down);
        }
        float c[4] = { it->color[0], it->color[1], it->color[2],
                       it->color[3] * (it->prev_text ? it->text_mix : 1.0f) };
        record_text_run(cmd, pass, quad, font, it->text, pos, it->size,
                        c, right, down);
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

    /* Directional shadow map: its own depth-only pass before the main pass. */
    ts_engine_shadow_pass(e, cmd);

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

    /* Selection outline/glow: composited after DoF so it stays crisp. */
    ts_engine_highlight_pass(e, cmd, swap, sw, sh);

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
        /* slot 1: the shadow map (or the tiny fallback depth texture — the
         * shader never samples it when shadow_params.x is 0, but Metal still
         * needs a depth-typed texture bound at a depth2d slot). */
        SDL_GPUTexture* smap = (e->shadow_active && g->shadow_map)
                                   ? g->shadow_map : g->shadow_fallback;
        SDL_GPUTextureSamplerBinding tsb[2] = {
            { .texture = it->texture ? it->texture : e->registry.white.texture,
              .sampler = g->linear_sampler },
            { .texture = smap, .sampler = g->point_sampler },
        };
        SDL_BindGPUFragmentSamplers(pass, 0, tsb, 2);
        SDL_DrawGPUIndexedPrimitives(pass, it->mesh->index_count, 1, 0, 0, 0);
    }

    /* Cards use their own pipeline; draw them (depth-writing) before the
     * translucent decal/particle passes. */
    record_cards(e, cmd, pass, &fu);

    /* Tile-overlay decals, then M7 blob shadows, then M6 particles: translucent
     * passes after opaque geometry (shadows composite on top of overlays). */
    record_overlays(e, cmd, pass, &fu);
    record_blobs(e, cmd, pass, &fu);
    ts_fx_record(e, cmd, pass, &fu);

    /* text labels last: they sit on top of every translucent pass */
    record_labels(e, cmd, pass, &fu);
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
        e->cam_follow_y_have = false;   /* re-seat the height filter */
        return;
    }
    if (same) {
        /* Unchanged goal: leave the in-flight tween (or idle-follow) alone so the
         * camera stays locked onto the live target. advance_camera re-resolves G
         * every tick, so tracking continues seamlessly across the scene swap. */
        e->cam_to = G;
        return;
    }
    /* Tween from the current live pose to the new goal over timing.camera_s. A
     * genuinely new goal re-seats the follow-height filter (below). */
    e->cam_from = e->cam_cur;
    e->cam_to = G;
    e->cam_follow_y_have = false;
    float dur = e->timing.camera_s > 0.0f ? e->timing.camera_s : 0.5f;
    ts_tween_start(&e->cam_tween, dur, 0.0f, TS_EASE_IN_OUT_CUBIC);
    e->cam_active = true;
}

/* Modes that frame a live *moving* point via the orbit rig — their goal height
 * bounces as the tracked object hops/tumbles, so its Y is worth low-passing. */
static bool cam_mode_tracks_position(uint32_t mode) {
    return mode == TESSERA_CAMERA_FOCUS_ENTITY ||
           mode == TESSERA_CAMERA_FOCUS_DICE   ||
           mode == TESSERA_CAMERA_FOCUS_DRAW   ||
           mode == TESSERA_CAMERA_FOCUS_TILE;
}

/* Replace the goal's height with a low-passed one so the follow cam glides
 * between the run's start and end heights instead of jumping with each hop.
 * eye and target share the same vertical offset (see ts_orbit_eye), so shifting
 * both by the same delta keeps the framing and only re-seats the rig height. */
static void cam_smooth_follow_y(TesseraEngine* e, TsCamPose* G, float dt) {
    float raw = G->target[1];
    if (!e->cam_follow_y_have) {
        e->cam_follow_y = raw;
        e->cam_follow_y_have = true;
    } else {
        /* Exponential low-pass; tau > a single hop so per-step bounces average
         * out while a real change in floor height is still tracked promptly. */
        const float tau = 0.45f;
        float a = dt > 0.0f ? 1.0f - expf(-dt / tau) : 0.0f;
        e->cam_follow_y += (raw - e->cam_follow_y) * a;
    }
    float dy = e->cam_follow_y - raw;
    G->eye[1]    += dy;
    G->target[1] += dy;
}

static void advance_camera(TesseraEngine* e, float dt) {
    if (!e->cam_spec_have) return;

    float aspect = current_aspect(e);
    TsCamPose G;
    bool resolved = resolve_camera_goal(e, &e->cam_spec, aspect, &G);
    if (resolved && cam_mode_tracks_position(e->cam_spec.mode))
        cam_smooth_follow_y(e, &G, dt);

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
        if (ts_tween_done(&e->cam_tween)) {
            e->cam_active = false;
            ts_engine_emit_event(e, TESSERA_EVENT_CAMERA_ARRIVED,
                                 TESSERA_EVENT_SUBJECT_CAMERA, 0,
                                 (TesseraCoord){0, 0}, 0.0f);
        }
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
    if (e->orch) ts_orch_advance(e->orch, e, (float)dt * mult);
    if (e->fx) ts_fx_advance(e, (float)dt * mult);
    if (e->dice) ts_dice_advance(e->dice, e, (float)dt * mult);
    advance_camera(e, (float)dt * mult);

    ts_engine_settle_operation(e);

    /* Deliver this tick's events to the callback, after everything (incl. the
     * op settle above) has emitted, outside the state mutex. */
    ts_engine_flush_events(e);
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
    /* Bridge the completion into the typed event stream so hosts can consume
     * one uniform ordering (every transition event precedes its op's settle). */
    ts_engine_emit_event(e, TESSERA_EVENT_OP_COMPLETED,
                         TESSERA_EVENT_SUBJECT_OPERATION, done,
                         (TesseraCoord){0, 0}, 0.0f);
    /* Invoked under cb_mutex so a concurrent clear cannot return while this
     * delivery is still using the old fn (see engine.h). */
    SDL_LockMutex(e->cb_mutex);
    if (e->op_cb) e->op_cb(done, e->op_cb_user);
    SDL_UnlockMutex(e->cb_mutex);
}
