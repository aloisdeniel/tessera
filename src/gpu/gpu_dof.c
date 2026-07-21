/* gpu_dof.c — depth-of-field post-processing pass.
 *
 * The scene is rendered into an offscreen color target (+ the sampleable depth
 * texture); this pass reconstructs eye-space depth per pixel, computes a circle
 * of confusion around the focal plane, and blurs out-of-focus pixels with a
 * CoC-scaled disc, writing the result to the destination (swapchain or capture
 * texture). Fullscreen triangle, no vertex buffer. */
#include "gpu/gpu.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* Matches DofUniform in assets/shaders/dof.fragment.msl. */
typedef struct {
    float znear, zfar, focus_dist, focus_range;
    float blur_px, texel_x, texel_y, ortho;
} TsDofUniform;

bool ts_gpu_create_dof_pipeline(TsGpu* g, char* err, size_t err_sz) {
    SDL_GPUShader* vs = ts_gpu_load_shader(g, "dof", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
    SDL_GPUShader* fs = ts_gpu_load_shader(g, "dof", SDL_GPU_SHADERSTAGE_FRAGMENT, 2, 1);
    if (!vs || !fs) {
        snprintf(err, err_sz, "failed to load dof shaders");
        if (vs) SDL_ReleaseGPUShader(g->device, vs);
        if (fs) SDL_ReleaseGPUShader(g->device, fs);
        return false;
    }

    SDL_GPUColorTargetDescription color = { .format = g->swapchain_format };
    SDL_GPUGraphicsPipelineCreateInfo pci = {
        .vertex_shader = vs, .fragment_shader = fs,
        .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
        .rasterizer_state = { .fill_mode = SDL_GPU_FILLMODE_FILL,
                              .cull_mode = SDL_GPU_CULLMODE_NONE,
                              .front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE },
        .target_info = { .color_target_descriptions = &color, .num_color_targets = 1 },
    };
    g->dof_pipeline = SDL_CreateGPUGraphicsPipeline(g->device, &pci);
    SDL_ReleaseGPUShader(g->device, vs);
    SDL_ReleaseGPUShader(g->device, fs);
    if (!g->dof_pipeline) {
        snprintf(err, err_sz, "dof pipeline create failed: %s", SDL_GetError());
        return false;
    }

    if (!g->point_sampler) {
        SDL_GPUSamplerCreateInfo sci = {
            .min_filter = SDL_GPU_FILTER_NEAREST, .mag_filter = SDL_GPU_FILTER_NEAREST,
            .mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
            .address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
            .address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
            .address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE };
        g->point_sampler = SDL_CreateGPUSampler(g->device, &sci);
        if (!g->point_sampler) {
            snprintf(err, err_sz, "point sampler create failed: %s", SDL_GetError());
            return false;
        }
    }
    return true;
}

bool ts_gpu_ensure_scene_target(TsGpu* g, uint32_t w, uint32_t h) {
    if (w == 0 || h == 0) return false;
    if (g->scene_color && g->scene_w == w && g->scene_h == h) return true;
    if (g->scene_color) { SDL_ReleaseGPUTexture(g->device, g->scene_color); g->scene_color = NULL; }

    SDL_GPUTextureCreateInfo tci = {
        .type = SDL_GPU_TEXTURETYPE_2D, .format = g->swapchain_format,
        .usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
        .width = w, .height = h, .layer_count_or_depth = 1,
        .num_levels = 1, .sample_count = SDL_GPU_SAMPLECOUNT_1 };
    g->scene_color = SDL_CreateGPUTexture(g->device, &tci);
    if (!g->scene_color) { TS_LOGE(g->log, "scene target create failed: %s", SDL_GetError()); return false; }
    g->scene_w = w; g->scene_h = h;
    return true;
}

void ts_gpu_dof_post(TsGpu* g, SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* src_color,
                     SDL_GPUTexture* dst_color, SDL_GPUTexture* depth,
                     uint32_t w, uint32_t h, const TsDofParams* p) {
    if (!g->dof_pipeline) return;

    SDL_GPUColorTargetInfo ct = {
        .texture = dst_color, .load_op = SDL_GPU_LOADOP_DONT_CARE,
        .store_op = SDL_GPU_STOREOP_STORE };
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &ct, 1, NULL);

    SDL_BindGPUGraphicsPipeline(pass, g->dof_pipeline);
    TsDofUniform u = {
        .znear = p->znear, .zfar = p->zfar,
        .focus_dist = p->focus_dist, .focus_range = p->focus_range,
        .blur_px = p->blur_px,
        .texel_x = w > 0 ? 1.0f / (float)w : 0.0f,
        .texel_y = h > 0 ? 1.0f / (float)h : 0.0f,
        .ortho = p->ortho ? 1.0f : 0.0f };
    SDL_PushGPUFragmentUniformData(cmd, 0, &u, sizeof u);

    SDL_GPUTextureSamplerBinding binds[2] = {
        { .texture = src_color, .sampler = g->linear_sampler },
        { .texture = depth,     .sampler = g->point_sampler },
    };
    SDL_BindGPUFragmentSamplers(pass, 0, binds, 2);
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
}
