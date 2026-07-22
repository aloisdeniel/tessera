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

/* Expanded, camera-facing particle vertex (built per-frame on the CPU). */
typedef struct {
    float pos[3];
    float uv[2];
    float color[4];   /* premultiplied-ish RGBA fade over life */
} TsParticleVertex;

/* Skinned mesh vertex: static attributes plus 4 joint indices + weights. */
typedef struct {
    float   pos[3];
    float   normal[3];
    float   uv[2];
    uint8_t joints[4];
    float   weights[4];
} TsSkinnedVertex;

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
    vec4 camera_pos;  /* xyz eye position, w unused (M7 rim/specular) */
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

    /* Offscreen HDR-ish scene target for post-processing (depth-of-field, M7+). */
    SDL_GPUTexture*      scene_color;
    uint32_t             scene_w, scene_h;

    /* Cached offscreen targets for tessera_render_rgba (per-frame embedding).
     * Reused across frames; recreated only on size change so the render loop
     * does not churn ~30MB of GPU memory every frame. */
    SDL_GPUTexture*        rgba_color;
    SDL_GPUTexture*        rgba_depth;
    SDL_GPUTransferBuffer* rgba_transfer;
    uint32_t               rgba_w, rgba_h;

    /* Pipelines (created in pipeline.c). */
    SDL_GPUGraphicsPipeline* mesh_pipeline;   /* static lit mesh (cel/flat) */
    SDL_GPUGraphicsPipeline* skinned_pipeline;/* GPU-skinned mesh (M5)       */
    SDL_GPUGraphicsPipeline* blob_pipeline;   /* blob-shadow decal (M7)      */
    SDL_GPUGraphicsPipeline* particle_add;    /* additive particles (M6)     */
    SDL_GPUGraphicsPipeline* particle_alpha;  /* alpha particles (M6)        */
    SDL_GPUGraphicsPipeline* dof_pipeline;    /* depth-of-field post pass     */
    SDL_GPUSampler*          linear_sampler;
    SDL_GPUSampler*          point_sampler;   /* nearest, for depth sampling  */

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
/* Upload a mesh with an arbitrary interleaved vertex stride (e.g. skinned). */
bool ts_gpu_upload_mesh_raw(TsGpu* g, const void* verts, uint32_t vcount,
                            uint32_t vstride, const uint32_t* indices,
                            uint32_t icount, TsMesh* out);
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
/* Build a unit quad in the XZ plane (y=0, extent +/-0.5, +Y normal, uv 0..1).
 * Used for blob shadows (M7). Caller owns result. */
void ts_build_quad_xz(TesseraVertex** out_v, uint32_t* out_vc,
                      uint32_t** out_i, uint32_t* out_ic);

/* ---- pipelines (gpu_pipeline.c) ---- */
bool ts_gpu_create_pipelines(TsGpu* g, char* err, size_t err_sz);
bool ts_gpu_create_blob_pipeline(TsGpu* g, char* err, size_t err_sz);       /* M7 */
bool ts_gpu_create_particle_pipelines(TsGpu* g, char* err, size_t err_sz);  /* M6 */
bool ts_gpu_create_skinned_pipeline(TsGpu* g, char* err, size_t err_sz);    /* M5 */
bool ts_gpu_create_dof_pipeline(TsGpu* g, char* err, size_t err_sz);        /* DoF */
void ts_gpu_release_pipelines(TsGpu* g);

/* Ensure the offscreen scene color target matches (w,h). Returns false on fail. */
bool ts_gpu_ensure_scene_target(TsGpu* g, uint32_t w, uint32_t h);

/* Depth-of-field parameters resolved for a frame (world units + clip planes). */
typedef struct {
    float znear, zfar, focus_dist, focus_range, blur_px;
    bool  ortho;
} TsDofParams;

/* Run the DoF post pass: sample `src_color` + `depth` and write the blurred
 * result into `dst_color` (own render pass, no depth target). */
void ts_gpu_dof_post(TsGpu* g, SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* src_color,
                     SDL_GPUTexture* dst_color, SDL_GPUTexture* depth,
                     uint32_t w, uint32_t h, const TsDofParams* p);
/* Load a shader (matches device format to a compiled/MSL blob on disk). */
SDL_GPUShader* ts_gpu_load_shader(TsGpu* g, const char* name,
                                  SDL_GPUShaderStage stage,
                                  uint32_t num_samplers, uint32_t num_uniform_buffers);

/* Override the asset base dir (where shaders/ lives) at runtime. Empty/NULL
 * restores the compile-time TESSERA_ASSET_DIR. Process-global; set before
 * tessera_create so pipeline creation picks it up. Used for bundled assets on
 * iOS/Android where the compiled path does not exist. */
void ts_gpu_set_asset_dir(const char* dir);

/* Ensure the cached tessera_render_rgba targets (color + depth + download
 * transfer buffer) exist at (w,h), recreating on size change. Returns false on
 * failure. Reused across frames so the embedding render loop is allocation-free
 * in steady state. */
bool ts_gpu_ensure_rgba_targets(TsGpu* g, uint32_t w, uint32_t h);

#endif /* TESSERA_GPU_H */
