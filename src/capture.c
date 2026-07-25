/* capture.c — offscreen render to an RGBA texture, download to a caller buffer,
 * optionally write PNG. Headless-friendly; the basis for golden-image tests (M9)
 * and for host compositors that display frames themselves (render_rgba). */
#include "engine.h"
#include "fx/fx.h"
#include "stb_image_write.h"
#include <stdlib.h>
#include <string.h>

/* Render the current scene offscreen at (w,h) and download RGBA8 into out_rgba
 * (top-left origin). Shared by ts_engine_render_rgba (public) and capture_png. */
bool ts_engine_render_rgba(TesseraEngine* e, uint32_t w, uint32_t h,
                           uint8_t* out_rgba, size_t out_size) {
    if (!e) return false;
    TsGpu* g = &e->gpu;
    if (!g->device) { ts_engine_set_error(e, "render_rgba: no GPU device"); return false; }
    if (w == 0 || h == 0) { ts_engine_set_error(e, "render_rgba: bad size"); return false; }
    if (!out_rgba || out_size < (size_t)w * h * 4) {
        ts_engine_set_error(e, "render_rgba: buffer too small");
        return false;
    }

    bool ok = false;
    /* Reuse cached per-frame targets (color + depth + download buffer) so the
     * embedding render loop does not allocate/free ~30MB of GPU memory every
     * frame. Recreated only on size change. */
    if (!ts_gpu_ensure_rgba_targets(g, w, h)) {
        ts_engine_set_error(e, "render_rgba: target create failed");
        return false;
    }
    SDL_GPUTexture* color = g->rgba_color;
    SDL_GPUTexture* depth = g->rgba_depth;
    SDL_GPUTransferBuffer* dl = g->rgba_transfer;
    bool bgra = (g->swapchain_format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM ||
                 g->swapchain_format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB);

    ts_arena_reset(&e->frame_arena);
    /* Refresh the camera (and its cached eye) so particle billboards face it. */
    e->camera.dirty = true;
    ts_camera_update(&e->camera, (h > 0) ? (float)w / (float)h : 1.7778f);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(g->device);

    ts_fx_prepare(e, cmd);

    /* Directional shadow map: its own depth-only pass before the main pass. */
    ts_engine_shadow_pass(e, cmd);

    /* DoF renders the scene offscreen, then blurs into the capture color. */
    bool dof = ts_engine_dof_active(e) && ts_gpu_ensure_scene_target(g, w, h);

    SDL_GPUColorTargetInfo ct = {
        .texture = dof ? g->scene_color : color,
        .clear_color = (SDL_FColor){0.08f, 0.10f, 0.14f, 1.0f},
        .load_op = SDL_GPU_LOADOP_CLEAR, .store_op = SDL_GPU_STOREOP_STORE };
    SDL_GPUDepthStencilTargetInfo dt = {
        .texture = depth, .clear_depth = 1.0f,
        .load_op = SDL_GPU_LOADOP_CLEAR,
        .store_op = dof ? SDL_GPU_STOREOP_STORE : SDL_GPU_STOREOP_DONT_CARE,
        .stencil_load_op = SDL_GPU_LOADOP_DONT_CARE,
        .stencil_store_op = SDL_GPU_STOREOP_DONT_CARE };

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &ct, 1, &dt);
    ts_engine_record_draws(e, cmd, pass, w, h);
    SDL_EndGPURenderPass(pass);

    if (dof) {
        TsDofParams dp; ts_engine_resolve_dof(e, &dp);
        ts_gpu_dof_post(g, cmd, g->scene_color, color, depth, w, h, &dp);
    }

    /* Selection outline/glow: composited after DoF so it stays crisp. */
    ts_engine_highlight_pass(e, cmd, color, w, h);

    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion region = { .texture = color, .w = w, .h = h, .d = 1 };
    SDL_GPUTextureTransferInfo tinfo = {
        .transfer_buffer = dl, .offset = 0, .pixels_per_row = w, .rows_per_layer = h };
    SDL_DownloadFromGPUTexture(cp, &region, &tinfo);
    SDL_EndGPUCopyPass(cp);

    /* Submit with an auto-released fence and block on GPU idle. Do NOT use
     * SubmitAndAcquireFence + ReleaseGPUFence here: releasing the fence returns
     * it to SDL's pool, the next frame re-acquires and resets it, and the just-
     * submitted command buffer (which aliases that fence pointer) then never
     * passes the completion check — so it is never cleaned and leaks its
     * command buffer + used-resource arrays every frame. WaitForGPUIdle cleans
     * command buffers with their fences still auto-owned, so nothing leaks. */
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_WaitForGPUIdle(g->device);

    void* mapped = SDL_MapGPUTransferBuffer(g->device, dl, false);
    if (!mapped) { ts_engine_set_error(e, "render_rgba: map failed"); return false; }
    memcpy(out_rgba, mapped, (size_t)w * h * 4);
    SDL_UnmapGPUTransferBuffer(g->device, dl);
    if (bgra) {
        for (uint32_t i = 0; i < w * h * 4; i += 4) {
            uint8_t t = out_rgba[i]; out_rgba[i] = out_rgba[i + 2]; out_rgba[i + 2] = t;
        }
    }
    ok = true;
    /* Cached targets (color/depth/dl) are engine-owned; freed in ts_gpu_shutdown. */
    return ok;
}

bool ts_engine_capture_png(TesseraEngine* e, uint32_t w, uint32_t h, const char* png_path) {
    if (!e || w == 0 || h == 0) return false;
    uint8_t* pixels = (uint8_t*)malloc((size_t)w * h * 4);
    if (!pixels) { ts_engine_set_error(e, "capture: out of memory"); return false; }

    bool ok = ts_engine_render_rgba(e, w, h, pixels, (size_t)w * h * 4);
    if (ok) {
        ok = stbi_write_png(png_path, (int)w, (int)h, 4, pixels, (int)(w * 4)) != 0;
        if (!ok) ts_engine_set_error(e, "capture: png write failed for %s", png_path);
    }
    free(pixels);
    return ok;
}
