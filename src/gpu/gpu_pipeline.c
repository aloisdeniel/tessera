/* gpu_pipeline.c — shader loading + the static mesh graphics pipeline. */
#include "gpu/gpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef TESSERA_ASSET_DIR
#define TESSERA_ASSET_DIR "assets"
#endif

static void* read_file(const char* path, size_t* out_size) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    void* buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)n, f);
    fclose(f);
    ((char*)buf)[rd] = 0;
    *out_size = rd;
    return buf;
}

SDL_GPUShader* ts_gpu_load_shader(TsGpu* g, const char* name,
                                  SDL_GPUShaderStage stage,
                                  uint32_t num_samplers, uint32_t num_uniform_buffers) {
    SDL_GPUShaderFormat fmts = SDL_GetGPUShaderFormats(g->device);
    const char* stage_str = (stage == SDL_GPU_SHADERSTAGE_VERTEX) ? "vertex" : "fragment";
    const char* ext;
    SDL_GPUShaderFormat use_fmt;
    const char* entry;

    if (fmts & SDL_GPU_SHADERFORMAT_MSL) {
        ext = "msl"; use_fmt = SDL_GPU_SHADERFORMAT_MSL;
        entry = (stage == SDL_GPU_SHADERSTAGE_VERTEX) ? "vs_main" : "fs_main";
    } else if (fmts & SDL_GPU_SHADERFORMAT_SPIRV) {
        ext = "spv"; use_fmt = SDL_GPU_SHADERFORMAT_SPIRV;
        entry = "main";
    } else {
        TS_LOGE(g->log, "no supported shader format for '%s'", name);
        return NULL;
    }

    char path[1024];
    snprintf(path, sizeof path, "%s/shaders/%s.%s.%s",
             TESSERA_ASSET_DIR, name, stage_str, ext);
    size_t size = 0;
    void* code = read_file(path, &size);
    if (!code) {
        TS_LOGE(g->log, "cannot open shader '%s'", path);
        return NULL;
    }

    SDL_GPUShaderCreateInfo info = {
        .code = (const Uint8*)code,
        .code_size = size,
        .entrypoint = entry,
        .format = use_fmt,
        .stage = stage,
        .num_samplers = num_samplers,
        .num_storage_textures = 0,
        .num_storage_buffers = 0,
        .num_uniform_buffers = num_uniform_buffers,
    };
    SDL_GPUShader* sh = SDL_CreateGPUShader(g->device, &info);
    free(code);
    if (!sh) TS_LOGE(g->log, "SDL_CreateGPUShader('%s') failed: %s", path, SDL_GetError());
    return sh;
}

bool ts_gpu_create_pipelines(TsGpu* g, char* err, size_t err_sz) {
    SDL_GPUShader* vs = ts_gpu_load_shader(g, "mesh", SDL_GPU_SHADERSTAGE_VERTEX, 0, 2);
    SDL_GPUShader* fs = ts_gpu_load_shader(g, "mesh", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
    if (!vs || !fs) {
        snprintf(err, err_sz, "failed to load mesh shaders");
        if (vs) SDL_ReleaseGPUShader(g->device, vs);
        if (fs) SDL_ReleaseGPUShader(g->device, fs);
        return false;
    }

    SDL_GPUVertexBufferDescription vbdesc = {
        .slot = 0,
        .pitch = sizeof(TesseraVertex),
        .input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX,
        .instance_step_rate = 0,
    };
    SDL_GPUVertexAttribute attrs[3] = {
        { .location = 0, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,
          .offset = offsetof(TesseraVertex, pos) },
        { .location = 1, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,
          .offset = offsetof(TesseraVertex, normal) },
        { .location = 2, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,
          .offset = offsetof(TesseraVertex, uv) },
    };

    /* Alpha blending so per-instance tint.a (add/remove fade in/out) renders.
     * Standard non-premultiplied over: opaque items (a=1) are unaffected. */
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
            .num_vertex_attributes = 3,
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
    g->mesh_pipeline = SDL_CreateGPUGraphicsPipeline(g->device, &pci);
    SDL_ReleaseGPUShader(g->device, vs);
    SDL_ReleaseGPUShader(g->device, fs);
    if (!g->mesh_pipeline) {
        snprintf(err, err_sz, "mesh pipeline create failed: %s", SDL_GetError());
        return false;
    }

    SDL_GPUSamplerCreateInfo sci = {
        .min_filter = SDL_GPU_FILTER_LINEAR,
        .mag_filter = SDL_GPU_FILTER_LINEAR,
        .mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR,
        .address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
        .address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
        .address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
    };
    g->linear_sampler = SDL_CreateGPUSampler(g->device, &sci);
    if (!g->linear_sampler) {
        snprintf(err, err_sz, "sampler create failed: %s", SDL_GetError());
        return false;
    }

    if (!ts_gpu_create_blob_pipeline(g, err, err_sz)) return false;
    if (!ts_gpu_create_particle_pipelines(g, err, err_sz)) return false;
    if (!ts_gpu_create_skinned_pipeline(g, err, err_sz)) return false;
    return true;
}

/* Release every pipeline/sampler owned by the GPU wrapper (called at shutdown
 * and after a device-lost recreate). Safe on NULLs. */
void ts_gpu_release_pipelines(TsGpu* g) {
    if (g->mesh_pipeline)     SDL_ReleaseGPUGraphicsPipeline(g->device, g->mesh_pipeline);
    if (g->skinned_pipeline)  SDL_ReleaseGPUGraphicsPipeline(g->device, g->skinned_pipeline);
    if (g->blob_pipeline)     SDL_ReleaseGPUGraphicsPipeline(g->device, g->blob_pipeline);
    if (g->particle_add)      SDL_ReleaseGPUGraphicsPipeline(g->device, g->particle_add);
    if (g->particle_alpha)    SDL_ReleaseGPUGraphicsPipeline(g->device, g->particle_alpha);
    if (g->linear_sampler)    SDL_ReleaseGPUSampler(g->device, g->linear_sampler);
    g->mesh_pipeline = g->skinned_pipeline = g->blob_pipeline = NULL;
    g->particle_add = g->particle_alpha = NULL;
    g->linear_sampler = NULL;
}

/* Shared standard interleaved-vertex input description (pos/normal/uv). */
static void fill_vertex_input(SDL_GPUVertexBufferDescription* vb,
                              SDL_GPUVertexAttribute* attrs) {
    vb->slot = 0;
    vb->pitch = sizeof(TesseraVertex);
    vb->input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    vb->instance_step_rate = 0;
    attrs[0] = (SDL_GPUVertexAttribute){ .location = 0, .buffer_slot = 0,
        .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, .offset = offsetof(TesseraVertex, pos) };
    attrs[1] = (SDL_GPUVertexAttribute){ .location = 1, .buffer_slot = 0,
        .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, .offset = offsetof(TesseraVertex, normal) };
    attrs[2] = (SDL_GPUVertexAttribute){ .location = 2, .buffer_slot = 0,
        .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, .offset = offsetof(TesseraVertex, uv) };
}

/* Blob-shadow pipeline: a dark radial decal, alpha-blended onto the ground.
 * Depth-tested (so geometry occludes it) but no depth write. */
bool ts_gpu_create_blob_pipeline(TsGpu* g, char* err, size_t err_sz) {
    SDL_GPUShader* vs = ts_gpu_load_shader(g, "blob", SDL_GPU_SHADERSTAGE_VERTEX, 0, 2);
    SDL_GPUShader* fs = ts_gpu_load_shader(g, "blob", SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0);
    if (!vs || !fs) {
        snprintf(err, err_sz, "failed to load blob shaders");
        if (vs) SDL_ReleaseGPUShader(g->device, vs);
        if (fs) SDL_ReleaseGPUShader(g->device, fs);
        return false;
    }
    SDL_GPUVertexBufferDescription vbdesc;
    SDL_GPUVertexAttribute attrs[3];
    fill_vertex_input(&vbdesc, attrs);

    SDL_GPUColorTargetDescription color = {
        .format = g->swapchain_format,
        .blend_state = {
            .enable_blend          = true,
            .src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA,
            .dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
            .color_blend_op        = SDL_GPU_BLENDOP_ADD,
            .src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO,
            .dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE,
            .alpha_blend_op        = SDL_GPU_BLENDOP_ADD,
        },
    };
    SDL_GPUGraphicsPipelineCreateInfo pci = {
        .vertex_shader = vs,
        .fragment_shader = fs,
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
            .enable_depth_write = false,
        },
        .target_info = {
            .color_target_descriptions = &color, .num_color_targets = 1,
            .depth_stencil_format = g->depth_format,
            .has_depth_stencil_target = true,
        },
    };
    g->blob_pipeline = SDL_CreateGPUGraphicsPipeline(g->device, &pci);
    SDL_ReleaseGPUShader(g->device, vs);
    SDL_ReleaseGPUShader(g->device, fs);
    if (!g->blob_pipeline) {
        snprintf(err, err_sz, "blob pipeline create failed: %s", SDL_GetError());
        return false;
    }
    return true;
}
