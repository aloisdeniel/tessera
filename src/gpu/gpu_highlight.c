/* gpu_highlight.c — selection outline & glow post pass.
 *
 * Flagged objects are re-rendered flat into an offscreen single-channel
 * silhouette mask (reusing the shadow pass's position-only vertex shaders with
 * a color target instead of a depth one), then a fullscreen pass dilates the
 * mask into a crisp colored outline or blurs it into an additive glow and
 * composites the result over the lit scene. Runs after depth-of-field so the
 * selection stays sharp. Same cached-target + fullscreen-triangle machinery as
 * the DoF pass (the composite reuses the dof fullscreen vertex shader). */
#include "gpu/gpu.h"
#include "card/card.h"
#include <stddef.h>
#include <stdio.h>

/* Matches HighlightUniform in assets/shaders/highlight.fragment.msl. */
typedef struct {
    float color[4];    /* rgb outline/glow color, a = base opacity (fade)   */
    float params[4];   /* x thickness px, y texel_x, z texel_y, w intensity */
    float params2[4];  /* x style: 0 = outline, 1 = glow                    */
} TsHighlightUniform;

/* Flat silhouette pipeline: position-only vertex (shared shadow shaders),
 * every covered pixel written as 1 into the mask. No depth target, so the
 * silhouette is the full outline even where the object is occluded. */
static SDL_GPUGraphicsPipeline* make_mask_pipeline(
        TsGpu* g, SDL_GPUShader* vs, SDL_GPUShader* fs,
        const SDL_GPUVertexBufferDescription* vbdesc,
        const SDL_GPUVertexAttribute* attrs, uint32_t attr_count) {
    SDL_GPUColorTargetDescription color = { .format = g->mask_format };
    SDL_GPUGraphicsPipelineCreateInfo pci = {
        .vertex_shader = vs,
        .fragment_shader = fs,
        .vertex_input_state = {
            .vertex_buffer_descriptions = vbdesc, .num_vertex_buffers = 1,
            .vertex_attributes = attrs, .num_vertex_attributes = attr_count,
        },
        .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
        .rasterizer_state = {
            .fill_mode = SDL_GPU_FILLMODE_FILL,
            .cull_mode = SDL_GPU_CULLMODE_NONE,
            .front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE,
        },
        .target_info = { .color_target_descriptions = &color, .num_color_targets = 1 },
    };
    return SDL_CreateGPUGraphicsPipeline(g->device, &pci);
}

/* Fullscreen composite pipeline over the lit scene: standard alpha blend for
 * the crisp outline, additive for the glow halo. */
static SDL_GPUGraphicsPipeline* make_composite_pipeline(
        TsGpu* g, SDL_GPUShader* vs, SDL_GPUShader* fs, bool additive) {
    SDL_GPUColorTargetDescription color = {
        .format = g->swapchain_format,
        .blend_state = {
            .enable_blend          = true,
            .src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA,
            .dst_color_blendfactor = additive ? SDL_GPU_BLENDFACTOR_ONE
                                              : SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
            .color_blend_op        = SDL_GPU_BLENDOP_ADD,
            .src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO,
            .dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE,
            .alpha_blend_op        = SDL_GPU_BLENDOP_ADD,
        },
    };
    SDL_GPUGraphicsPipelineCreateInfo pci = {
        .vertex_shader = vs, .fragment_shader = fs,
        .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
        .rasterizer_state = { .fill_mode = SDL_GPU_FILLMODE_FILL,
                              .cull_mode = SDL_GPU_CULLMODE_NONE,
                              .front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE },
        .target_info = { .color_target_descriptions = &color, .num_color_targets = 1 },
    };
    return SDL_CreateGPUGraphicsPipeline(g->device, &pci);
}

bool ts_gpu_create_highlight_pipelines(TsGpu* g, char* err, size_t err_sz) {
    /* Single-channel mask when the device can render + sample it. */
    g->mask_format = SDL_GPUTextureSupportsFormat(
            g->device, SDL_GPU_TEXTUREFORMAT_R8_UNORM, SDL_GPU_TEXTURETYPE_2D,
            SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER)
        ? SDL_GPU_TEXTUREFORMAT_R8_UNORM
        : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;

    /* Mask pass: the shadow pass's position-only vertex shaders + a trivial
     * "write 1" fragment. Same three vertex layouts as the shadow pipelines. */
    SDL_GPUShader* vs      = ts_gpu_load_shader(g, "shadow",     SDL_GPU_SHADERSTAGE_VERTEX, 0, 2);
    SDL_GPUShader* vs_skin = ts_gpu_load_shader(g, "shadowskin", SDL_GPU_SHADERSTAGE_VERTEX, 0, 3);
    SDL_GPUShader* fs_mask = ts_gpu_load_shader(g, "mask",       SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0);
    if (!vs || !vs_skin || !fs_mask) {
        snprintf(err, err_sz, "failed to load highlight mask shaders");
        if (vs)      SDL_ReleaseGPUShader(g->device, vs);
        if (vs_skin) SDL_ReleaseGPUShader(g->device, vs_skin);
        if (fs_mask) SDL_ReleaseGPUShader(g->device, fs_mask);
        return false;
    }

    SDL_GPUVertexBufferDescription vb_static = {
        .slot = 0, .pitch = sizeof(TesseraVertex),
        .input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX, .instance_step_rate = 0 };
    SDL_GPUVertexAttribute attr_pos = {
        .location = 0, .buffer_slot = 0,
        .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,
        .offset = offsetof(TesseraVertex, pos) };
    g->mask_pipeline = make_mask_pipeline(g, vs, fs_mask, &vb_static, &attr_pos, 1);

    SDL_GPUVertexBufferDescription vb_card = {
        .slot = 0, .pitch = sizeof(TsCardVertex),
        .input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX, .instance_step_rate = 0 };
    SDL_GPUVertexAttribute attr_card_pos = {
        .location = 0, .buffer_slot = 0,
        .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,
        .offset = offsetof(TsCardVertex, pos) };
    g->mask_card_pipeline = make_mask_pipeline(g, vs, fs_mask, &vb_card, &attr_card_pos, 1);

    SDL_GPUVertexBufferDescription vb_skin = {
        .slot = 0, .pitch = sizeof(TsSkinnedVertex),
        .input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX, .instance_step_rate = 0 };
    SDL_GPUVertexAttribute attrs_skin[3] = {
        { .location = 0, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,
          .offset = offsetof(TsSkinnedVertex, pos) },
        { .location = 3, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4,
          .offset = offsetof(TsSkinnedVertex, joints) },
        { .location = 4, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4,
          .offset = offsetof(TsSkinnedVertex, weights) },
    };
    g->mask_skinned_pipeline = make_mask_pipeline(g, vs_skin, fs_mask, &vb_skin, attrs_skin, 3);

    SDL_ReleaseGPUShader(g->device, vs);
    SDL_ReleaseGPUShader(g->device, vs_skin);
    SDL_ReleaseGPUShader(g->device, fs_mask);
    if (!g->mask_pipeline || !g->mask_card_pipeline || !g->mask_skinned_pipeline) {
        snprintf(err, err_sz, "highlight mask pipeline create failed: %s", SDL_GetError());
        return false;
    }

    /* Composite pass: the dof fullscreen-triangle vertex + the outline/glow
     * fragment; two pipelines differing only in blend state. */
    SDL_GPUShader* vs_fs = ts_gpu_load_shader(g, "dof", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
    SDL_GPUShader* fs_hl = ts_gpu_load_shader(g, "highlight", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
    if (!vs_fs || !fs_hl) {
        snprintf(err, err_sz, "failed to load highlight composite shaders");
        if (vs_fs) SDL_ReleaseGPUShader(g->device, vs_fs);
        if (fs_hl) SDL_ReleaseGPUShader(g->device, fs_hl);
        return false;
    }
    g->hl_outline_pipeline = make_composite_pipeline(g, vs_fs, fs_hl, false);
    g->hl_glow_pipeline    = make_composite_pipeline(g, vs_fs, fs_hl, true);
    SDL_ReleaseGPUShader(g->device, vs_fs);
    SDL_ReleaseGPUShader(g->device, fs_hl);
    if (!g->hl_outline_pipeline || !g->hl_glow_pipeline) {
        snprintf(err, err_sz, "highlight composite pipeline create failed: %s", SDL_GetError());
        return false;
    }
    return true;
}

bool ts_gpu_ensure_mask_target(TsGpu* g, uint32_t w, uint32_t h) {
    if (w == 0 || h == 0) return false;
    if (g->mask_tex && g->mask_w == w && g->mask_h == h) return true;
    if (g->mask_tex) { SDL_ReleaseGPUTexture(g->device, g->mask_tex); g->mask_tex = NULL; }
    g->mask_w = g->mask_h = 0;

    SDL_GPUTextureCreateInfo tci = {
        .type = SDL_GPU_TEXTURETYPE_2D, .format = g->mask_format,
        .usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
        .width = w, .height = h, .layer_count_or_depth = 1,
        .num_levels = 1, .sample_count = SDL_GPU_SAMPLECOUNT_1 };
    g->mask_tex = SDL_CreateGPUTexture(g->device, &tci);
    if (!g->mask_tex) { TS_LOGE(g->log, "highlight mask create failed: %s", SDL_GetError()); return false; }
    g->mask_w = w; g->mask_h = h;
    return true;
}

void ts_gpu_highlight_post(TsGpu* g, SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* mask,
                           SDL_GPUTexture* dst, uint32_t w, uint32_t h,
                           const TsHighlightPost* p) {
    SDL_GPUGraphicsPipeline* pipe = p->glow ? g->hl_glow_pipeline : g->hl_outline_pipeline;
    if (!pipe) return;

    SDL_GPUColorTargetInfo ct = {
        .texture = dst,
        .load_op = SDL_GPU_LOADOP_LOAD,   /* composite over the lit scene */
        .store_op = SDL_GPU_STOREOP_STORE };
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &ct, 1, NULL);

    SDL_BindGPUGraphicsPipeline(pass, pipe);
    TsHighlightUniform u = {
        .color = { p->color[0], p->color[1], p->color[2], p->color[3] },
        .params = { p->thickness_px,
                    w > 0 ? 1.0f / (float)w : 0.0f,
                    h > 0 ? 1.0f / (float)h : 0.0f,
                    p->intensity },
        .params2 = { p->glow ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f },
    };
    SDL_PushGPUFragmentUniformData(cmd, 0, &u, sizeof u);

    SDL_GPUTextureSamplerBinding bind = { .texture = mask, .sampler = g->linear_sampler };
    SDL_BindGPUFragmentSamplers(pass, 0, &bind, 1);
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
}
