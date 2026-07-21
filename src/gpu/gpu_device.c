/* gpu_device.c — SDL_GPU device, window, swapchain, depth buffer. */
#include "gpu/gpu.h"
#include <string.h>

static SDL_GPUTextureFormat pick_depth_format(SDL_GPUDevice* dev) {
    if (SDL_GPUTextureSupportsFormat(dev, SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
            SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET))
        return SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    if (SDL_GPUTextureSupportsFormat(dev, SDL_GPU_TEXTUREFORMAT_D24_UNORM,
            SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET))
        return SDL_GPU_TEXTUREFORMAT_D24_UNORM;
    return SDL_GPU_TEXTUREFORMAT_D16_UNORM;
}

bool ts_gpu_init(TsGpu* g, const TesseraConfig* cfg, const TsLog* log,
                 char* err, size_t err_sz) {
    memset(g, 0, sizeof *g);
    g->log = log;
    g->width = cfg->width > 0 ? cfg->width : 1280;
    g->height = cfg->height > 0 ? cfg->height : 720;
    g->pixel_density = cfg->pixel_density > 0 ? cfg->pixel_density : 1.0f;

    if (!SDL_WasInit(SDL_INIT_VIDEO) && !SDL_Init(SDL_INIT_VIDEO)) {
        snprintf(err, err_sz, "SDL_Init(VIDEO) failed: %s", SDL_GetError());
        return false;
    }

    /* We accept MSL (Metal) and SPIR-V (Vulkan) shader formats. */
    SDL_GPUShaderFormat formats = SDL_GPU_SHADERFORMAT_MSL | SDL_GPU_SHADERFORMAT_SPIRV;
    g->device = SDL_CreateGPUDevice(formats, cfg->debug, NULL);
    if (!g->device) {
        snprintf(err, err_sz, "SDL_CreateGPUDevice failed: %s", SDL_GetError());
        return false;
    }

    if (cfg->native_window) {
        /* Attach to a host-provided native view (mobile/embedding). The host
         * passes an SDL_Window* it created; deeper native handles are wired in
         * milestone 8. */
        g->window = (SDL_Window*)cfg->native_window;
        g->owns_window = false;
    } else {
        g->window = SDL_CreateWindow("Tessera", g->width, g->height,
                                     SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (!g->window) {
            snprintf(err, err_sz, "SDL_CreateWindow failed: %s", SDL_GetError());
            SDL_DestroyGPUDevice(g->device);
            g->device = NULL;
            return false;
        }
        g->owns_window = true;
    }

    if (!SDL_ClaimWindowForGPUDevice(g->device, g->window)) {
        snprintf(err, err_sz, "ClaimWindowForGPUDevice failed: %s", SDL_GetError());
        return false;
    }
    SDL_SetGPUSwapchainParameters(g->device, g->window,
        SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_VSYNC);

    g->swapchain_format = SDL_GetGPUSwapchainTextureFormat(g->device, g->window);
    g->depth_format = pick_depth_format(g->device);

    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(g->window, &pw, &ph);
    if (pw > 0 && ph > 0) { g->width = pw; g->height = ph; }
    ts_gpu_ensure_depth(g, (uint32_t)g->width, (uint32_t)g->height);

    TS_LOGI(log, "GPU device: driver=%s swapchain_fmt=%d depth_fmt=%d %dx%d",
            SDL_GetGPUDeviceDriver(g->device), (int)g->swapchain_format,
            (int)g->depth_format, g->width, g->height);
    return true;
}

bool ts_gpu_ensure_depth(TsGpu* g, uint32_t w, uint32_t h) {
    if (w == 0 || h == 0) return false;
    if (g->depth_texture && g->depth_w == w && g->depth_h == h) return true;
    if (g->depth_texture) {
        SDL_ReleaseGPUTexture(g->device, g->depth_texture);
        g->depth_texture = NULL;
    }
    SDL_GPUTextureCreateInfo info = {0};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = g->depth_format;
    info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    info.width = w;
    info.height = h;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    g->depth_texture = SDL_CreateGPUTexture(g->device, &info);
    if (!g->depth_texture) {
        TS_LOGE(g->log, "depth texture create failed: %s", SDL_GetError());
        return false;
    }
    g->depth_w = w;
    g->depth_h = h;
    return true;
}

void ts_gpu_resize(TsGpu* g, int w, int h, float density) {
    if (w > 0) g->width = w;
    if (h > 0) g->height = h;
    if (density > 0) g->pixel_density = density;
    ts_gpu_ensure_depth(g, (uint32_t)g->width, (uint32_t)g->height);
}

void ts_gpu_shutdown(TsGpu* g) {
    if (!g->device) return;
    if (g->mesh_pipeline) SDL_ReleaseGPUGraphicsPipeline(g->device, g->mesh_pipeline);
    if (g->linear_sampler) SDL_ReleaseGPUSampler(g->device, g->linear_sampler);
    if (g->depth_texture) SDL_ReleaseGPUTexture(g->device, g->depth_texture);
    if (g->window) {
        SDL_ReleaseWindowFromGPUDevice(g->device, g->window);
        if (g->owns_window) SDL_DestroyWindow(g->window);
    }
    SDL_DestroyGPUDevice(g->device);
    memset(g, 0, sizeof *g);
}
