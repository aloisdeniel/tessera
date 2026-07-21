/* capture.c — offscreen render to an RGBA texture, download, write PNG.
 * Headless-friendly; the basis for golden-image tests (M9). */
#include "engine.h"
#include "stb_image_write.h"
#include <stdlib.h>
#include <string.h>

bool ts_engine_capture_png(TesseraEngine* e, uint32_t w, uint32_t h, const char* png_path) {
    TsGpu* g = &e->gpu;
    if (!g->device) { ts_engine_set_error(e, "capture: no GPU device"); return false; }
    if (w == 0 || h == 0) { ts_engine_set_error(e, "capture: bad size"); return false; }

    bool ok = false;
    SDL_GPUTexture* color = NULL;
    SDL_GPUTexture* depth = NULL;
    SDL_GPUTransferBuffer* dl = NULL;
    uint8_t* pixels = NULL;

    /* Match the mesh pipeline's color target format (the swapchain format). */
    SDL_GPUTextureCreateInfo cci = {
        .type = SDL_GPU_TEXTURETYPE_2D,
        .format = g->swapchain_format,
        .usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
        .width = w, .height = h, .layer_count_or_depth = 1,
        .num_levels = 1, .sample_count = SDL_GPU_SAMPLECOUNT_1 };
    color = SDL_CreateGPUTexture(g->device, &cci);
    bool bgra = (g->swapchain_format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM ||
                 g->swapchain_format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB);

    SDL_GPUTextureCreateInfo dci = {
        .type = SDL_GPU_TEXTURETYPE_2D, .format = g->depth_format,
        .usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET,
        .width = w, .height = h, .layer_count_or_depth = 1,
        .num_levels = 1, .sample_count = SDL_GPU_SAMPLECOUNT_1 };
    depth = SDL_CreateGPUTexture(g->device, &dci);
    if (!color || !depth) { ts_engine_set_error(e, "capture: texture create failed"); goto done; }

    SDL_GPUTransferBufferCreateInfo tci = {
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, .size = w * h * 4 };
    dl = SDL_CreateGPUTransferBuffer(g->device, &tci);
    if (!dl) { ts_engine_set_error(e, "capture: transfer buffer failed"); goto done; }

    ts_arena_reset(&e->frame_arena);
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(g->device);

    SDL_GPUColorTargetInfo ct = {
        .texture = color,
        .clear_color = (SDL_FColor){0.08f, 0.10f, 0.14f, 1.0f},
        .load_op = SDL_GPU_LOADOP_CLEAR, .store_op = SDL_GPU_STOREOP_STORE };
    SDL_GPUDepthStencilTargetInfo dt = {
        .texture = depth, .clear_depth = 1.0f,
        .load_op = SDL_GPU_LOADOP_CLEAR, .store_op = SDL_GPU_STOREOP_DONT_CARE,
        .stencil_load_op = SDL_GPU_LOADOP_DONT_CARE,
        .stencil_store_op = SDL_GPU_STOREOP_DONT_CARE };

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &ct, 1, &dt);
    ts_engine_record_draws(e, cmd, pass, w, h);
    SDL_EndGPURenderPass(pass);

    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion region = { .texture = color, .w = w, .h = h, .d = 1 };
    SDL_GPUTextureTransferInfo tinfo = {
        .transfer_buffer = dl, .offset = 0, .pixels_per_row = w, .rows_per_layer = h };
    SDL_DownloadFromGPUTexture(cp, &region, &tinfo);
    SDL_EndGPUCopyPass(cp);

    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (fence) {
        SDL_WaitForGPUFences(g->device, true, &fence, 1);
        SDL_ReleaseGPUFence(g->device, fence);
    }

    void* mapped = SDL_MapGPUTransferBuffer(g->device, dl, false);
    if (!mapped) { ts_engine_set_error(e, "capture: map failed"); goto done; }
    pixels = (uint8_t*)malloc(w * h * 4);
    memcpy(pixels, mapped, w * h * 4);
    SDL_UnmapGPUTransferBuffer(g->device, dl);
    if (bgra) {
        for (uint32_t i = 0; i < w * h * 4; i += 4) {
            uint8_t t = pixels[i]; pixels[i] = pixels[i + 2]; pixels[i + 2] = t;
        }
    }

    ok = stbi_write_png(png_path, (int)w, (int)h, 4, pixels, (int)(w * 4)) != 0;
    if (!ok) ts_engine_set_error(e, "capture: png write failed for %s", png_path);

done:
    free(pixels);
    if (dl) SDL_ReleaseGPUTransferBuffer(g->device, dl);
    if (color) SDL_ReleaseGPUTexture(g->device, color);
    if (depth) SDL_ReleaseGPUTexture(g->device, depth);
    return ok;
}
