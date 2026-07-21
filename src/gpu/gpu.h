/*
 * gpu.h — SDL_GPU device, swapchain, meshes, textures, pipelines.
 * Wraps the SDL_GPU objects the renderer needs; all internal.
 */
#ifndef TESSERA_GPU_H
#define TESSERA_GPU_H

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include "core/core.h"
#include "core/tmath.h"

/* Canonical interleaved vertex used by all static meshes. */
typedef struct {
    float pos[3];
    float normal[3];
    float uv[2];
} TesseraVertex;

/* A GPU mesh: vertex + index buffers already uploaded. */
typedef struct {
    SDL_GPUBuffer* vbo;
    SDL_GPUBuffer* ibo;
    uint32_t       vertex_count;
    uint32_t       index_count;
} TsMesh;

/* A GPU texture with its sampler-ready view. */
typedef struct {
    SDL_GPUTexture* texture;
    uint32_t        w, h;
    uint64_t        content_hash;  /* for dedup */
} TsTexture;

/* Per-frame uniform (std140-ish, 16-byte aligned). */
typedef struct {
    mat4 view_proj;
    vec4 light_dir;   /* xyz dir, w unused */
    vec4 ambient;     /* rgb ambient, a intensity */
    vec4 light_color; /* rgb, a intensity */
} TsFrameUniform;

/* Per-object uniform. */
typedef struct {
    mat4 model;
    vec4 tint;
    vec4 uv_rect;   /* remap base UVs into an atlas region (u0,v0,u1,v1) */
} TsObjectUniform;

typedef struct {
    SDL_GPUDevice* device;
    SDL_Window*    window;
    bool           owns_window;
    const TsLog*   log;

    SDL_GPUTextureFormat swapchain_format;
    SDL_GPUTextureFormat depth_format;
    SDL_GPUTexture*      depth_texture;
    uint32_t             depth_w, depth_h;

    /* Pipelines (created in pipeline.c). */
    SDL_GPUGraphicsPipeline* mesh_pipeline;   /* static lit mesh */
    SDL_GPUSampler*          linear_sampler;

    int   width, height;
    float pixel_density;
} TsGpu;

/* ---- device / swapchain (gpu_device.c) ---- */
bool ts_gpu_init(TsGpu* g, const TesseraConfig* cfg, const TsLog* log, char* err, size_t err_sz);
void ts_gpu_shutdown(TsGpu* g);
void ts_gpu_resize(TsGpu* g, int w, int h, float density);
/* Ensure a depth texture matching (w,h) exists. */
bool ts_gpu_ensure_depth(TsGpu* g, uint32_t w, uint32_t h);

/* ---- buffers / meshes (gpu_resources.c) ---- */
bool ts_gpu_upload_mesh(TsGpu* g, const TesseraVertex* verts, uint32_t vcount,
                        const uint32_t* indices, uint32_t icount, TsMesh* out);
void ts_gpu_free_mesh(TsGpu* g, TsMesh* m);
/* Upload RGBA8 pixels into a new GPU texture (mipmapped). */
bool ts_gpu_upload_texture(TsGpu* g, const uint8_t* rgba, uint32_t w, uint32_t h,
                           TsTexture* out);
void ts_gpu_free_texture(TsGpu* g, TsTexture* t);

/* Build the unit tile mesh (thin box, top at y=0). Caller owns result. */
void ts_build_tile_mesh(float thickness, TesseraVertex** out_v, uint32_t* out_vc,
                        uint32_t** out_i, uint32_t* out_ic);
/* Build a unit cube centered on the ground (fallback entity mesh). */
void ts_build_unit_cube(TesseraVertex** out_v, uint32_t* out_vc,
                        uint32_t** out_i, uint32_t* out_ic);

/* ---- pipelines (gpu_pipeline.c) ---- */
bool ts_gpu_create_pipelines(TsGpu* g, char* err, size_t err_sz);
/* Load a shader (matches device format to a compiled/MSL blob on disk). */
SDL_GPUShader* ts_gpu_load_shader(TsGpu* g, const char* name,
                                  SDL_GPUShaderStage stage,
                                  uint32_t num_samplers, uint32_t num_uniform_buffers);

#endif /* TESSERA_GPU_H */
