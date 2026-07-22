/* gpu_pipeline_card.c — the flat-card graphics pipeline.
 *
 * Cards use a bespoke vertex (pos/normal/uv + a face selector) and three
 * fragment samplers (front-visible, front-hidden, back). The fragment shader
 * crossfades the front between visible and hidden by a per-instance mix factor
 * and picks the right texture per face, so one draw call textures the whole
 * slab. Alpha-blended (fade in/out), depth-tested and depth-writing. */
#include "gpu/gpu.h"
#include "card/card.h"
#include <stdio.h>

bool ts_gpu_create_card_pipeline(TsGpu* g, char* err, size_t err_sz) {
    /* vertex: frame(0) + object(1) uniforms; fragment: frame(0) + object(1) + 3 samplers. */
    SDL_GPUShader* vs = ts_gpu_load_shader(g, "card", SDL_GPU_SHADERSTAGE_VERTEX, 0, 2);
    SDL_GPUShader* fs = ts_gpu_load_shader(g, "card", SDL_GPU_SHADERSTAGE_FRAGMENT, 3, 2);
    if (!vs || !fs) {
        snprintf(err, err_sz, "failed to load card shaders");
        if (vs) SDL_ReleaseGPUShader(g->device, vs);
        if (fs) SDL_ReleaseGPUShader(g->device, fs);
        return false;
    }

    SDL_GPUVertexBufferDescription vbdesc = {
        .slot = 0,
        .pitch = sizeof(TsCardVertex),
        .input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX,
        .instance_step_rate = 0,
    };
    SDL_GPUVertexAttribute attrs[4] = {
        { .location = 0, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,
          .offset = offsetof(TsCardVertex, pos) },
        { .location = 1, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,
          .offset = offsetof(TsCardVertex, normal) },
        { .location = 2, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,
          .offset = offsetof(TsCardVertex, uv) },
        { .location = 3, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT,
          .offset = offsetof(TsCardVertex, face) },
    };

    SDL_GPUColorTargetDescription color = {
        .format = g->swapchain_format,
        .blend_state = {
            .enable_blend          = true,
            .src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA,
            .dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
            .color_blend_op        = SDL_GPU_BLENDOP_ADD,
            .src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE,
            .dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
            .alpha_blend_op        = SDL_GPU_BLENDOP_ADD,
        },
    };
    SDL_GPUGraphicsPipelineCreateInfo pci = {
        .vertex_shader = vs,
        .fragment_shader = fs,
        .vertex_input_state = {
            .vertex_buffer_descriptions = &vbdesc,
            .num_vertex_buffers = 1,
            .vertex_attributes = attrs,
            .num_vertex_attributes = 4,
        },
        .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
        .rasterizer_state = {
            .fill_mode = SDL_GPU_FILLMODE_FILL,
            .cull_mode = SDL_GPU_CULLMODE_BACK,
            .front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE,
        },
        .depth_stencil_state = {
            .compare_op = SDL_GPU_COMPAREOP_LESS,
            .enable_depth_test = true,
            .enable_depth_write = true,
        },
        .target_info = {
            .color_target_descriptions = &color,
            .num_color_targets = 1,
            .depth_stencil_format = g->depth_format,
            .has_depth_stencil_target = true,
        },
    };
    g->card_pipeline = SDL_CreateGPUGraphicsPipeline(g->device, &pci);
    SDL_ReleaseGPUShader(g->device, vs);
    SDL_ReleaseGPUShader(g->device, fs);
    if (!g->card_pipeline) {
        snprintf(err, err_sz, "card pipeline create failed: %s", SDL_GetError());
        return false;
    }
    return true;
}
