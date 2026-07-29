/* gpu_resources.c — buffer/texture upload + procedural mesh builders. */
#include "gpu/gpu.h"
#include <stdlib.h>
#include <string.h>

/* ---- mesh upload ------------------------------------------------------ */
bool ts_gpu_upload_mesh(TsGpu* g, const TesseraVertex* verts, uint32_t vcount,
                        const uint32_t* indices, uint32_t icount, TsMesh* out) {
    return ts_gpu_upload_mesh_raw(g, verts, vcount, (uint32_t)sizeof(TesseraVertex),
                                  indices, icount, out);
}

bool ts_gpu_upload_mesh_raw(TsGpu* g, const void* verts, uint32_t vcount,
                            uint32_t vstride, const uint32_t* indices,
                            uint32_t icount, TsMesh* out) {
    memset(out, 0, sizeof *out);
    if (!verts || vcount == 0 || !indices || icount == 0 || vstride == 0) return false;

    const uint32_t vbytes = vcount * vstride;
    const uint32_t ibytes = icount * (uint32_t)sizeof(uint32_t);

    SDL_GPUBufferCreateInfo vci = { .usage = SDL_GPU_BUFFERUSAGE_VERTEX, .size = vbytes };
    SDL_GPUBufferCreateInfo ici = { .usage = SDL_GPU_BUFFERUSAGE_INDEX,  .size = ibytes };
    out->vbo = SDL_CreateGPUBuffer(g->device, &vci);
    out->ibo = SDL_CreateGPUBuffer(g->device, &ici);
    if (!out->vbo || !out->ibo) { ts_gpu_free_mesh(g, out); return false; }

    SDL_GPUTransferBufferCreateInfo tci = {
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = vbytes + ibytes };
    SDL_GPUTransferBuffer* tbuf = SDL_CreateGPUTransferBuffer(g->device, &tci);
    if (!tbuf) { ts_gpu_free_mesh(g, out); return false; }

    uint8_t* map = (uint8_t*)SDL_MapGPUTransferBuffer(g->device, tbuf, false);
    memcpy(map, verts, vbytes);
    memcpy(map + vbytes, indices, ibytes);
#ifdef TESSERA_WEB_DEBUG_UPLOAD
    {
        uint32_t sum = 0; const uint8_t* q = map;
        for (uint32_t k = 0; k < vbytes + ibytes; ++k) sum = sum * 33 + q[k];
        const float* fv = (const float*)map;
        SDL_Log("[upload] vb=%u ib=%u sum=%08x v0=(%.3f %.3f %.3f) v1=(%.3f %.3f %.3f)",
                vbytes, ibytes, sum, fv[0], fv[1], fv[2], fv[8], fv[9], fv[10]);
    }
#endif
    SDL_UnmapGPUTransferBuffer(g->device, tbuf);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(g->device);
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTransferBufferLocation src_v = { .transfer_buffer = tbuf, .offset = 0 };
    SDL_GPUBufferRegion dst_v = { .buffer = out->vbo, .offset = 0, .size = vbytes };
    SDL_UploadToGPUBuffer(cp, &src_v, &dst_v, false);
    SDL_GPUTransferBufferLocation src_i = { .transfer_buffer = tbuf, .offset = vbytes };
    SDL_GPUBufferRegion dst_i = { .buffer = out->ibo, .offset = 0, .size = ibytes };
    SDL_UploadToGPUBuffer(cp, &src_i, &dst_i, false);
    SDL_EndGPUCopyPass(cp);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(g->device, tbuf);

    out->vertex_count = vcount;
    out->index_count = icount;
    return true;
}

void ts_gpu_free_mesh(TsGpu* g, TsMesh* m) {
    if (m->vbo) SDL_ReleaseGPUBuffer(g->device, m->vbo);
    if (m->ibo) SDL_ReleaseGPUBuffer(g->device, m->ibo);
    memset(m, 0, sizeof *m);
}

/* ---- texture upload --------------------------------------------------- */
bool ts_gpu_upload_texture(TsGpu* g, const uint8_t* rgba, uint32_t w, uint32_t h,
                           TsTexture* out) {
    memset(out, 0, sizeof *out);
    if (!rgba || w == 0 || h == 0) return false;

    SDL_GPUTextureCreateInfo tci = {
        .type = SDL_GPU_TEXTURETYPE_2D,
        .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER,
        .width = w, .height = h, .layer_count_or_depth = 1,
        .num_levels = 1, .sample_count = SDL_GPU_SAMPLECOUNT_1 };
    out->texture = SDL_CreateGPUTexture(g->device, &tci);
    if (!out->texture) return false;

    const uint32_t bytes = w * h * 4;
    SDL_GPUTransferBufferCreateInfo bci = {
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = bytes };
    SDL_GPUTransferBuffer* tbuf = SDL_CreateGPUTransferBuffer(g->device, &bci);
    if (!tbuf) { ts_gpu_free_texture(g, out); return false; }
    void* map = SDL_MapGPUTransferBuffer(g->device, tbuf, false);
    memcpy(map, rgba, bytes);
    SDL_UnmapGPUTransferBuffer(g->device, tbuf);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(g->device);
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src = {
        .transfer_buffer = tbuf, .offset = 0, .pixels_per_row = w, .rows_per_layer = h };
    SDL_GPUTextureRegion dst = { .texture = out->texture, .w = w, .h = h, .d = 1 };
    SDL_UploadToGPUTexture(cp, &src, &dst, false);
    SDL_EndGPUCopyPass(cp);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(g->device, tbuf);

    out->w = w; out->h = h;
    return true;
}

void ts_gpu_free_texture(TsGpu* g, TsTexture* t) {
    if (t->texture) SDL_ReleaseGPUTexture(g->device, t->texture);
    memset(t, 0, sizeof *t);
}

/* ---- procedural meshes ------------------------------------------------ */
/* A box spanning [-hx,hx] x [ymin,ymax] x [-hz,hz], per-face normals + UVs.
 * 24 verts (4 per face), 36 indices. */
static void build_box(float hx, float ymin, float ymax, float hz,
                      TesseraVertex** ov, uint32_t* ovc,
                      uint32_t** oi, uint32_t* oic) {
    TesseraVertex* v = (TesseraVertex*)malloc(24 * sizeof(TesseraVertex));
    uint32_t* idx = (uint32_t*)malloc(36 * sizeof(uint32_t));
    int vi = 0, ii = 0;

    /* helper to push a quad with CCW winding (front face) given 4 corners */
    struct Corner { float x, y, z, u, w; };
    #define QUAD(nx,ny,nz, ax,ay,az,au,aw, bx,by,bz,bu,bw, cx,cy,cz,cu,cw, dx,dy,dz,du,dw) \
        do { \
            int base = vi; \
            v[vi++] = (TesseraVertex){{ax,ay,az},{nx,ny,nz},{au,aw}}; \
            v[vi++] = (TesseraVertex){{bx,by,bz},{nx,ny,nz},{bu,bw}}; \
            v[vi++] = (TesseraVertex){{cx,cy,cz},{nx,ny,nz},{cu,cw}}; \
            v[vi++] = (TesseraVertex){{dx,dy,dz},{nx,ny,nz},{du,dw}}; \
            idx[ii++]=base+0; idx[ii++]=base+1; idx[ii++]=base+2; \
            idx[ii++]=base+0; idx[ii++]=base+2; idx[ii++]=base+3; \
        } while (0)

    /* +Y top (looking down -Y, CCW from above) */
    QUAD(0,1,0,  -hx,ymax,-hz,0,0,  -hx,ymax,hz,0,1,  hx,ymax,hz,1,1,  hx,ymax,-hz,1,0);
    /* -Y bottom */
    QUAD(0,-1,0, -hx,ymin,hz,0,0,  -hx,ymin,-hz,0,1,  hx,ymin,-hz,1,1,  hx,ymin,hz,1,0);
    /* +Z front */
    QUAD(0,0,1,  -hx,ymin,hz,0,1,  hx,ymin,hz,1,1,  hx,ymax,hz,1,0,  -hx,ymax,hz,0,0);
    /* -Z back */
    QUAD(0,0,-1, hx,ymin,-hz,0,1,  -hx,ymin,-hz,1,1,  -hx,ymax,-hz,1,0,  hx,ymax,-hz,0,0);
    /* +X right */
    QUAD(1,0,0,  hx,ymin,hz,0,1,  hx,ymin,-hz,1,1,  hx,ymax,-hz,1,0,  hx,ymax,hz,0,0);
    /* -X left */
    QUAD(-1,0,0, -hx,ymin,-hz,0,1,  -hx,ymin,hz,1,1,  -hx,ymax,hz,1,0,  -hx,ymax,-hz,0,0);
    #undef QUAD

    *ov = v; *ovc = (uint32_t)vi;
    *oi = idx; *oic = (uint32_t)ii;
}

void ts_build_tile_mesh(float thickness, TesseraVertex** ov, uint32_t* ovc,
                        uint32_t** oi, uint32_t* oic) {
    if (thickness <= 0.0f) thickness = 0.25f;
    const float half = 0.5f * TS_TILE_SIZE;
    /* top surface at y=0, extends down by `thickness` */
    build_box(half, -thickness, 0.0f, half, ov, ovc, oi, oic);
}

void ts_build_unit_cube(TesseraVertex** ov, uint32_t* ovc,
                        uint32_t** oi, uint32_t* oic) {
    const float half = 0.35f * TS_TILE_SIZE;
    /* stands on the ground: y in [0, 2*half] */
    build_box(half, 0.0f, 2.0f * half, half, ov, ovc, oi, oic);
}

void ts_build_quad_xz(TesseraVertex** ov, uint32_t* ovc,
                      uint32_t** oi, uint32_t* oic) {
    TesseraVertex* v = (TesseraVertex*)malloc(4 * sizeof(TesseraVertex));
    uint32_t* idx = (uint32_t*)malloc(6 * sizeof(uint32_t));
    /* +Y-facing quad, CCW seen from above (matches tile top winding) */
    v[0] = (TesseraVertex){{-0.5f, 0.0f, -0.5f}, {0, 1, 0}, {0, 0}};
    v[1] = (TesseraVertex){{-0.5f, 0.0f,  0.5f}, {0, 1, 0}, {0, 1}};
    v[2] = (TesseraVertex){{ 0.5f, 0.0f,  0.5f}, {0, 1, 0}, {1, 1}};
    v[3] = (TesseraVertex){{ 0.5f, 0.0f, -0.5f}, {0, 1, 0}, {1, 0}};
    idx[0] = 0; idx[1] = 1; idx[2] = 2;
    idx[3] = 0; idx[4] = 2; idx[5] = 3;
    *ov = v; *ovc = 4;
    *oi = idx; *oic = 6;
}
