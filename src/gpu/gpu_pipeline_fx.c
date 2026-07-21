/* gpu_pipeline_fx.c — particle pipelines (M6): additive + alpha billboards. */
#include "gpu/gpu.h"
#include <stddef.h>
#include <stdio.h>

static SDL_GPUGraphicsPipeline* make_particle_pipeline(TsGpu* g, bool additive) {
    SDL_GPUShader* vs = ts_gpu_load_shader(g, "particle", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
    SDL_GPUShader* fs = ts_gpu_load_shader(g, "particle", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0);
    if (!vs || !fs) {
        if (vs) SDL_ReleaseGPUShader(g->device, vs);
        if (fs) SDL_ReleaseGPUShader(g->device, fs);
        return NULL;
    }

    SDL_GPUVertexBufferDescription vbdesc = {
        .slot = 0, .pitch = sizeof(TsParticleVertex),
        .input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX, .instance_step_rate = 0,
    };
    SDL_GPUVertexAttribute attrs[3] = {
        { .location = 0, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,
          .offset = offsetof(TsParticleVertex, pos) },
        { .location = 1, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,
          .offset = offsetof(TsParticleVertex, uv) },
        { .location = 2, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4,
          .offset = offsetof(TsParticleVertex, color) },
    };

    /* Additive: src*a + dst. Alpha: standard over. Both premultiplied by the
     * vertex color's alpha in-shader (color already carries the fade). */
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
        .vertex_input_state = {
            .vertex_buffer_descriptions = &vbdesc, .num_vertex_buffers = 1,
            .vertex_attributes = attrs, .num_vertex_attributes = 3,
        },
        .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
        .rasterizer_state = {
            .fill_mode = SDL_GPU_FILLMODE_FILL,
            .cull_mode = SDL_GPU_CULLMODE_NONE,
            .front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE,
        },
        .depth_stencil_state = {
            .compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL,
            .enable_depth_test = true,
            .enable_depth_write = false,   /* translucent: test, don't write */
        },
        .target_info = {
            .color_target_descriptions = &color, .num_color_targets = 1,
            .depth_stencil_format = g->depth_format,
            .has_depth_stencil_target = true,
        },
    };
    SDL_GPUGraphicsPipeline* p = SDL_CreateGPUGraphicsPipeline(g->device, &pci);
    SDL_ReleaseGPUShader(g->device, vs);
    SDL_ReleaseGPUShader(g->device, fs);
    return p;
}

bool ts_gpu_create_particle_pipelines(TsGpu* g, char* err, size_t err_sz) {
    g->particle_add   = make_particle_pipeline(g, true);
    g->particle_alpha = make_particle_pipeline(g, false);
    if (!g->particle_add || !g->particle_alpha) {
        snprintf(err, err_sz, "particle pipeline create failed: %s", SDL_GetError());
        return false;
    }
    return true;
}
