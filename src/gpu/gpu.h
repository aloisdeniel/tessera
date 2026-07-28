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
    /* Directional shadow map (TESSERA_SHADOW_MAP). Appended so shaders that
     * declare only the prefix above keep working unchanged. */
    mat4 light_vp;      /* world -> light clip (ortho, fitted per frame) */
    vec4 shadow_params; /* x enable, y 1/resolution, z const bias, w slope bias */
    /* Positional point lights (state-driven; see TesseraPointLightPlacement).
     * Appended, same prefix rule as above. */
    vec4 point_count;   /* x = live light count (as float), yzw unused    */
    vec4 point_pos[TESSERA_MAX_POINT_LIGHTS];   /* xyz world pos, w radius */
    vec4 point_color[TESSERA_MAX_POINT_LIGHTS]; /* rgb color, a intensity  */
} TsFrameUniform;

/* Depth-only pass per-frame uniform (world -> light clip). */
typedef struct {
    mat4 light_vp;
} TsShadowFrameUniform;

/* Per-object uniform. */
typedef struct {
    mat4 model;
    vec4 tint;
    vec4 uv_rect;   /* remap base UVs into an atlas region (u0,v0,u1,v1) */
} TsObjectUniform;

/* Per-overlay uniform: a tile decal quad. params.x carries the shape
 * (TesseraOverlayShape); tint already includes fade + pulse alpha. */
typedef struct {
    mat4 model;
    vec4 tint;
    vec4 uv_rect;   /* SPRITE atlas remap (u0,v0,u1,v1) */
    vec4 params;    /* x = shape; yzw unused */
} TsOverlayUniform;

/* Per-card uniform: three atlas rects (front visible / hidden / back) plus a
 * crossfade factor. Front fragments lerp visible<->hidden by params.x. */
typedef struct {
    mat4 model;
    vec4 tint;
    vec4 uv_visible;
    vec4 uv_hidden;
    vec4 uv_back;
    vec4 params;    /* x = mix (0 visible .. 1 hidden); yzw unused */
} TsCardObjectUniform;

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
    SDL_GPUGraphicsPipeline* overlay_pipeline;/* tile-overlay decal          */
    SDL_GPUGraphicsPipeline* text_pipeline;   /* glyph quads (3D labels)     */
    SDL_GPUGraphicsPipeline* particle_add;    /* additive particles (M6)     */
    SDL_GPUGraphicsPipeline* particle_alpha;  /* alpha particles (M6)        */
    SDL_GPUGraphicsPipeline* dof_pipeline;    /* depth-of-field post pass     */
    SDL_GPUGraphicsPipeline* card_pipeline;   /* flat card slab (3-sampler)   */
    SDL_GPUGraphicsPipeline* shadow_pipeline;        /* depth-only, TesseraVertex   */
    SDL_GPUGraphicsPipeline* shadow_skinned_pipeline;/* depth-only, TsSkinnedVertex */
    SDL_GPUGraphicsPipeline* shadow_card_pipeline;   /* depth-only, TsCardVertex    */
    SDL_GPUGraphicsPipeline* mask_pipeline;         /* flat silhouette, TesseraVertex   */
    SDL_GPUGraphicsPipeline* mask_skinned_pipeline; /* flat silhouette, TsSkinnedVertex */
    SDL_GPUGraphicsPipeline* mask_card_pipeline;    /* flat silhouette, TsCardVertex    */
    SDL_GPUGraphicsPipeline* hl_outline_pipeline;   /* dilate mask -> crisp outline     */
    SDL_GPUGraphicsPipeline* hl_glow_pipeline;      /* blur mask -> additive glow       */
    SDL_GPUSampler*          linear_sampler;
    SDL_GPUSampler*          point_sampler;   /* nearest, for depth sampling  */

    /* Directional shadow map (TESSERA_SHADOW_MAP): a sampleable depth target
     * rendered from the light each frame — same DEPTH_STENCIL|SAMPLER pattern
     * as the DoF depth. `shadow_fallback` is a tiny always-valid depth texture
     * bound at the shadow slot when mapping is off (never actually sampled). */
    SDL_GPUTexture* shadow_map;
    uint32_t        shadow_res;
    SDL_GPUTexture* shadow_fallback;

    /* Selection-highlight silhouette mask: a single-channel (R8 when the
     * device supports it) offscreen color target, cached and resized with the
     * frame like the DoF scene target. */
    SDL_GPUTexture*      mask_tex;
    uint32_t             mask_w, mask_h;
    SDL_GPUTextureFormat mask_format;

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
bool ts_gpu_create_overlay_pipeline(TsGpu* g, char* err, size_t err_sz);    /* tile decals */
bool ts_gpu_create_text_pipeline(TsGpu* g, char* err, size_t err_sz);       /* 3D labels   */
bool ts_gpu_create_particle_pipelines(TsGpu* g, char* err, size_t err_sz);  /* M6 */
bool ts_gpu_create_skinned_pipeline(TsGpu* g, char* err, size_t err_sz);    /* M5 */
bool ts_gpu_create_dof_pipeline(TsGpu* g, char* err, size_t err_sz);        /* DoF */
bool ts_gpu_create_card_pipeline(TsGpu* g, char* err, size_t err_sz);       /* cards */
bool ts_gpu_create_shadow_pipelines(TsGpu* g, char* err, size_t err_sz);    /* shadow map */
bool ts_gpu_create_highlight_pipelines(TsGpu* g, char* err, size_t err_sz); /* outline/glow */
void ts_gpu_release_pipelines(TsGpu* g);

/* Ensure the offscreen scene color target matches (w,h). Returns false on fail. */
bool ts_gpu_ensure_scene_target(TsGpu* g, uint32_t w, uint32_t h);

/* Ensure the shadow map depth target exists at `res`×`res` (gpu_shadow.c). */
bool ts_gpu_ensure_shadow_map(TsGpu* g, uint32_t res);

/* Ensure the highlight silhouette mask target matches (w,h) (gpu_highlight.c). */
bool ts_gpu_ensure_mask_target(TsGpu* g, uint32_t w, uint32_t h);

/* One highlight composite resolved for the post pass (see gpu_highlight.c). */
typedef struct {
    float color[4];     /* rgb outline/glow color; a = base opacity (fade) */
    float thickness_px; /* outline width / glow radius in pixels           */
    float intensity;    /* pulse modulation (multiplies the alpha)         */
    bool  glow;         /* true = additive glow, false = crisp outline     */
} TsHighlightPost;

/* Composite the silhouette `mask` over `dst` as an outline or glow: a
 * fullscreen pass (dst is loaded, not cleared) that dilates/blurs the mask. */
void ts_gpu_highlight_post(TsGpu* g, SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* mask,
                           SDL_GPUTexture* dst, uint32_t w, uint32_t h,
                           const TsHighlightPost* p);

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
