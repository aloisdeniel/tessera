/* gpu_shadow.c — directional shadow-map pipelines + depth target.
 *
 * TESSERA_SHADOW_MAP renders a depth-only pass from the directional light into
 * a sampleable depth texture (same DEPTH_STENCIL_TARGET|SAMPLER pattern the
 * depth-of-field post pass relies on), which the mesh fragment shader then
 * samples with 3x3 PCF. Three pipeline variants share the trivial depth-only
 * shaders and differ only in vertex pitch: static meshes/tiles/dice
 * (TesseraVertex), skinned entities (TsSkinnedVertex + joint palette) and
 * cards (TsCardVertex — the shadow vertex shader reads location 0 only, so a
 * pitch change is all the card layout needs). */
#include "gpu/gpu.h"
#include "card/card.h"
#include <stddef.h>
#include <stdio.h>

/* Hardware slope-scaled depth bias applied while rendering the map. Tuned at
 * board scale (tiles = 1 world unit, depth range a few tens of units) against
 * acne on tile tops vs peter-panning under piece bases. */
#define TS_SHADOW_BIAS_CONSTANT 4.0f
#define TS_SHADOW_BIAS_SLOPE    2.5f

static SDL_GPUGraphicsPipeline* make_depth_pipeline(
        TsGpu* g, SDL_GPUShader* vs, SDL_GPUShader* fs,
        const SDL_GPUVertexBufferDescription* vbdesc,
        const SDL_GPUVertexAttribute* attrs, uint32_t attr_count) {
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
            .cull_mode = SDL_GPU_CULLMODE_BACK,
            .front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE,
            .depth_bias_constant_factor = TS_SHADOW_BIAS_CONSTANT,
            .depth_bias_slope_factor    = TS_SHADOW_BIAS_SLOPE,
            .enable_depth_bias = true,
        },
        .depth_stencil_state = {
            .compare_op = SDL_GPU_COMPAREOP_LESS,
            .enable_depth_test = true,
            .enable_depth_write = true,
        },
        .target_info = {
            .num_color_targets = 0,
            .depth_stencil_format = g->depth_format,
            .has_depth_stencil_target = true,
        },
    };
    return SDL_CreateGPUGraphicsPipeline(g->device, &pci);
}

bool ts_gpu_create_shadow_pipelines(TsGpu* g, char* err, size_t err_sz) {
    SDL_GPUShader* vs      = ts_gpu_load_shader(g, "shadow",     SDL_GPU_SHADERSTAGE_VERTEX, 0, 2);
    SDL_GPUShader* vs_skin = ts_gpu_load_shader(g, "shadowskin", SDL_GPU_SHADERSTAGE_VERTEX, 0, 3);
    SDL_GPUShader* fs      = ts_gpu_load_shader(g, "shadow",     SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0);
    if (!vs || !vs_skin || !fs) {
        snprintf(err, err_sz, "failed to load shadow shaders");
        if (vs)      SDL_ReleaseGPUShader(g->device, vs);
        if (vs_skin) SDL_ReleaseGPUShader(g->device, vs_skin);
        if (fs)      SDL_ReleaseGPUShader(g->device, fs);
        return false;
    }

    /* Static meshes, tiles, dice: only position feeds the depth pass. */
    SDL_GPUVertexBufferDescription vb_static = {
        .slot = 0, .pitch = sizeof(TesseraVertex),
        .input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX, .instance_step_rate = 0 };
    SDL_GPUVertexAttribute attr_pos = {
        .location = 0, .buffer_slot = 0,
        .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,
        .offset = offsetof(TesseraVertex, pos) };
    g->shadow_pipeline = make_depth_pipeline(g, vs, fs, &vb_static, &attr_pos, 1);

    /* Cards: same shader, TsCardVertex pitch. */
    SDL_GPUVertexBufferDescription vb_card = {
        .slot = 0, .pitch = sizeof(TsCardVertex),
        .input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX, .instance_step_rate = 0 };
    SDL_GPUVertexAttribute attr_card_pos = {
        .location = 0, .buffer_slot = 0,
        .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,
        .offset = offsetof(TsCardVertex, pos) };
    g->shadow_card_pipeline = make_depth_pipeline(g, vs, fs, &vb_card, &attr_card_pos, 1);

    /* Skinned entities: position + joints + weights (palette in slot 2), so
     * animated casters shadow in their posed shape. */
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
    g->shadow_skinned_pipeline = make_depth_pipeline(g, vs_skin, fs, &vb_skin, attrs_skin, 3);

    SDL_ReleaseGPUShader(g->device, vs);
    SDL_ReleaseGPUShader(g->device, vs_skin);
    SDL_ReleaseGPUShader(g->device, fs);
    if (!g->shadow_pipeline || !g->shadow_card_pipeline || !g->shadow_skinned_pipeline) {
        snprintf(err, err_sz, "shadow pipeline create failed: %s", SDL_GetError());
        return false;
    }

    /* Tiny always-valid depth texture for the mesh shader's shadow slot while
     * mapping is off (the shader branches on shadow_params.x and never samples
     * it, but Metal still requires a depth-typed texture bound there). */
    if (!g->shadow_fallback) {
        SDL_GPUTextureCreateInfo tci = {
            .type = SDL_GPU_TEXTURETYPE_2D, .format = g->depth_format,
            .usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
            .width = 4, .height = 4, .layer_count_or_depth = 1,
            .num_levels = 1, .sample_count = SDL_GPU_SAMPLECOUNT_1 };
        g->shadow_fallback = SDL_CreateGPUTexture(g->device, &tci);
        if (!g->shadow_fallback) {
            snprintf(err, err_sz, "shadow fallback texture create failed: %s", SDL_GetError());
            return false;
        }
    }
    return true;
}

bool ts_gpu_ensure_shadow_map(TsGpu* g, uint32_t res) {
    if (res == 0) return false;
    if (g->shadow_map && g->shadow_res == res) return true;
    if (g->shadow_map) { SDL_ReleaseGPUTexture(g->device, g->shadow_map); g->shadow_map = NULL; }
    g->shadow_res = 0;

    SDL_GPUTextureCreateInfo tci = {
        .type = SDL_GPU_TEXTURETYPE_2D, .format = g->depth_format,
        .usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
        .width = res, .height = res, .layer_count_or_depth = 1,
        .num_levels = 1, .sample_count = SDL_GPU_SAMPLECOUNT_1 };
    g->shadow_map = SDL_CreateGPUTexture(g->device, &tci);
    if (!g->shadow_map) {
        TS_LOGE(g->log, "shadow map create failed: %s", SDL_GetError());
        return false;
    }
    g->shadow_res = res;
    return true;
}
