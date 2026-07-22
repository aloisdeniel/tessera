/* registry.c — definition registry (atlas/tile/entity/effect).
 *
 * M2 note: entity glTF import is stubbed to a fallback cube mesh here; the
 * asset pipeline (assets/gltf.c) replaces the mesh build once available. */
#include "registry.h"
#include "assets/assets.h"
#include "stb_image.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Resolve TesseraBytes (path or in-memory) to a malloc'd buffer. */
static void* resolve_bytes(const TesseraBytes* b, size_t* out_size) {
    if (b->data && b->size) {
        void* copy = malloc(b->size);
        if (!copy) return NULL;
        memcpy(copy, b->data, b->size);
        *out_size = b->size;
        return copy;
    }
    if (b->path) {
        FILE* f = fopen(b->path, "rb");
        if (!f) return NULL;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n <= 0) { fclose(f); return NULL; }
        void* buf = malloc((size_t)n);
        size_t rd = buf ? fread(buf, 1, (size_t)n, f) : 0;
        fclose(f);
        if (!buf || rd != (size_t)n) { free(buf); return NULL; }
        *out_size = (size_t)n;
        return buf;
    }
    return NULL;
}

/* Copy a NUL-terminated string into the arena. */
static char* arena_strdup(TsArena* a, const char* s) {
    size_t len = strlen(s);
    char* out = (char*)ts_arena_alloc(a, len + 1, 1);
    if (out) memcpy(out, s, len + 1);
    return out;
}

bool ts_registry_init(TsRegistry* r, TsGpu* gpu, TsArena* arena, const TsLog* log) {
    memset(r, 0, sizeof *r);
    r->gpu = gpu;
    r->arena = arena;
    r->log = log;
    if (!ts_slotmap_init(&r->defs, sizeof(TsDef), 64)) return false;

    /* 1x1 white fallback texture */
    uint8_t white_px[4] = {255, 255, 255, 255};
    ts_gpu_upload_texture(gpu, white_px, 1, 1, &r->white);

    /* soft radial particle sprite (white RGB, gaussian-ish alpha) */
    enum { PD = 32 };
    uint8_t dot[PD * PD * 4];
    for (int y = 0; y < PD; ++y) for (int x = 0; x < PD; ++x) {
        float dx = (x + 0.5f) / PD - 0.5f, dy = (y + 0.5f) / PD - 0.5f;
        float d = sqrtf(dx * dx + dy * dy) * 2.0f;   /* 0 centre .. 1 edge */
        float a = 1.0f - d; if (a < 0) a = 0; a = a * a;
        uint8_t* p = &dot[(y * PD + x) * 4];
        p[0] = p[1] = p[2] = 255;
        p[3] = (uint8_t)(a * 255.0f + 0.5f);
    }
    ts_gpu_upload_texture(gpu, dot, PD, PD, &r->particle_dot);

    /* shared meshes */
    TesseraVertex* v; uint32_t vc; uint32_t* i; uint32_t ic;
    ts_build_tile_mesh(0.25f, &v, &vc, &i, &ic);
    ts_gpu_upload_mesh(gpu, v, vc, i, ic, &r->tile_mesh);
    free(v); free(i);
    ts_build_unit_cube(&v, &vc, &i, &ic);
    ts_gpu_upload_mesh(gpu, v, vc, i, ic, &r->cube_mesh);
    free(v); free(i);
    ts_build_quad_xz(&v, &vc, &i, &ic);
    ts_gpu_upload_mesh(gpu, v, vc, i, ic, &r->quad_mesh);
    free(v); free(i);
    return true;
}

void ts_registry_shutdown(TsRegistry* r) {
    /* free per-def GPU resources */
    for (size_t idx = 0; idx < r->defs.capacity; ++idx) {
        if (!r->defs.occupied[idx]) continue;
        TsDef* d = (TsDef*)(r->defs.items + idx * r->defs.item_size);
        if (d->kind == TS_DEF_ATLAS && d->as.atlas.valid)
            ts_gpu_free_texture(r->gpu, &d->as.atlas.tex);
        if (d->kind == TS_DEF_ENTITY && d->as.entity.has_mesh)
            ts_gpu_free_mesh(r->gpu, &d->as.entity.mesh);
        if (d->kind == TS_DEF_DICE)
            ts_dice_free_model(r->gpu, &d->as.dice);
    }
    ts_gpu_free_texture(r->gpu, &r->white);
    ts_gpu_free_texture(r->gpu, &r->particle_dot);
    ts_gpu_free_mesh(r->gpu, &r->tile_mesh);
    ts_gpu_free_mesh(r->gpu, &r->cube_mesh);
    ts_gpu_free_mesh(r->gpu, &r->quad_mesh);
    ts_slotmap_destroy(&r->defs);
}

TsDef* ts_registry_get(TsRegistry* r, TesseraDefId id, TsDefKind expect) {
    TsDef* d = (TsDef*)ts_slotmap_get(&r->defs, (TsHandle)id);
    if (!d) return NULL;
    if (expect != TS_DEF_NONE && d->kind != expect) return NULL;
    return d;
}

SDL_GPUTexture* ts_registry_atlas_texture(TsRegistry* r, TesseraDefId atlas) {
    if (atlas) {
        TsDef* d = ts_registry_get(r, atlas, TS_DEF_ATLAS);
        if (d && d->as.atlas.valid) return d->as.atlas.tex.texture;
    }
    return r->white.texture;
}

SDL_GPUTexture* ts_registry_particle_texture(TsRegistry* r, TesseraDefId atlas) {
    if (atlas) {
        TsDef* d = ts_registry_get(r, atlas, TS_DEF_ATLAS);
        if (d && d->as.atlas.valid) return d->as.atlas.tex.texture;
    }
    return r->particle_dot.texture;   /* soft dot instead of hard white quad */
}

TesseraDefId ts_registry_add_atlas(TsRegistry* r, const TesseraBytes* image,
                                   char* err, size_t err_sz) {
    if (!image) { snprintf(err, err_sz, "register_atlas: null image"); return 0; }
    size_t size = 0;
    void* bytes = resolve_bytes(image, &size);
    if (!bytes) { snprintf(err, err_sz, "register_atlas: cannot read image bytes"); return 0; }

    int w = 0, h = 0, comp = 0;
    stbi_uc* px = stbi_load_from_memory((const stbi_uc*)bytes, (int)size, &w, &h, &comp, 4);
    free(bytes);
    if (!px) { snprintf(err, err_sz, "register_atlas: decode failed: %s", stbi_failure_reason()); return 0; }

    TsTexture tex;
    bool ok = ts_gpu_upload_texture(r->gpu, px, (uint32_t)w, (uint32_t)h, &tex);
    stbi_image_free(px);
    if (!ok) { snprintf(err, err_sz, "register_atlas: GPU upload failed"); return 0; }

    void* slot;
    TsHandle h2 = ts_slotmap_alloc(&r->defs, &slot);
    if (!h2) { ts_gpu_free_texture(r->gpu, &tex); snprintf(err, err_sz, "register_atlas: registry full"); return 0; }
    TsDef* d = (TsDef*)slot;
    d->kind = TS_DEF_ATLAS;
    d->as.atlas.tex = tex;
    d->as.atlas.valid = true;
    return (TesseraDefId)h2;
}

TesseraDefId ts_registry_add_tile(TsRegistry* r, const TesseraTileDef* def,
                                  char* err, size_t err_sz) {
    if (!def) { snprintf(err, err_sz, "register_tile: null def"); return 0; }
    if (def->atlas != 0 && !ts_registry_get(r, def->atlas, TS_DEF_ATLAS)) {
        snprintf(err, err_sz, "register_tile: invalid atlas id %u", def->atlas);
        return 0;
    }
    void* slot;
    TsHandle h = ts_slotmap_alloc(&r->defs, &slot);
    if (!h) { snprintf(err, err_sz, "register_tile: registry full"); return 0; }
    TsDef* d = (TsDef*)slot;
    d->kind = TS_DEF_TILE;
    d->as.tile.spec = *def;
    if (d->as.tile.spec.thickness <= 0.0f) d->as.tile.spec.thickness = 0.25f;
    d->as.tile.valid = true;
    return (TesseraDefId)h;
}

TesseraDefId ts_registry_add_entity(TsRegistry* r, const TesseraEntityDef* def,
                                    char* err, size_t err_sz) {
    if (!def) { snprintf(err, err_sz, "register_entity: null def"); return 0; }
    void* slot;
    TsHandle h = ts_slotmap_alloc(&r->defs, &slot);
    if (!h) { snprintf(err, err_sz, "register_entity: registry full"); return 0; }
    TsDef* d = (TsDef*)slot;
    d->kind = TS_DEF_ENTITY;
    d->as.entity.spec = *def;
    d->as.entity.mesh = r->cube_mesh;
    d->as.entity.base_color[0] = d->as.entity.base_color[1] =
        d->as.entity.base_color[2] = d->as.entity.base_color[3] = 1.0f;
    d->as.entity.has_mesh = false;   /* shared mesh, not owned by this def */
    d->as.entity.skinned = false;
    d->as.entity.clips = NULL;
    d->as.entity.clip_count = 0;

    /* Import glTF geometry/clips when bytes or a path are supplied; otherwise
     * keep the shared fallback cube (no error). */
    if (def->gltf.data || def->gltf.path) {
        size_t size = 0;
        void* bytes = resolve_bytes(&def->gltf, &size);
        if (!bytes) {
            TS_LOGW(r->log, "register_entity: cannot read gltf bytes; using cube fallback");
        } else {
            TsGltfResult res;
            char gerr[256] = {0};
            bool ok = ts_gltf_import(r->gpu, r->arena, bytes, size, &res, gerr, sizeof gerr);
            free(bytes);
            if (!ok) {
                TS_LOGW(r->log, "register_entity: gltf import failed (%s); using cube fallback", gerr);
            } else {
                d->as.entity.mesh = res.mesh;
                memcpy(d->as.entity.base_color, res.base_color, sizeof res.base_color);
                d->as.entity.has_mesh = res.has_mesh;
                d->as.entity.skinned = res.skinned;
                d->as.entity.skin_data = res.skin;   /* arena-owned TsSkinData */
                d->as.entity.clip_count = res.clip_count;
                if (res.clip_count > 0) {
                    d->as.entity.clips = TS_ARENA_ARR(r->arena, TsAnimClip, res.clip_count);
                    for (uint32_t c = 0; c < res.clip_count; ++c) {
                        d->as.entity.clips[c].name =
                            res.clips[c].name ? arena_strdup(r->arena, res.clips[c].name) : NULL;
                        d->as.entity.clips[c].duration = res.clips[c].duration;
                        d->as.entity.clips[c].clip_data = NULL;
                    }
                }
            }
        }
    }

    d->as.entity.valid = true;
    return (TesseraDefId)h;
}

TesseraDefId ts_registry_add_dice(TsRegistry* r, const TesseraDiceDef* def,
                                  char* err, size_t err_sz) {
    if (!def) { snprintf(err, err_sz, "register_dice: null def"); return 0; }
    TsDiceModel model;
    if (!ts_dice_build_model(r->gpu, r->arena, r->log, def, &model, err, err_sz))
        return 0;
    void* slot;
    TsHandle h = ts_slotmap_alloc(&r->defs, &slot);
    if (!h) {
        ts_dice_free_model(r->gpu, &model);
        snprintf(err, err_sz, "register_dice: registry full");
        return 0;
    }
    TsDef* d = (TsDef*)slot;
    d->kind = TS_DEF_DICE;
    d->as.dice = model;
    return (TesseraDefId)h;
}

TesseraDefId ts_registry_add_effect(TsRegistry* r, const TesseraEffectDef* def,
                                    char* err, size_t err_sz) {
    if (!def) { snprintf(err, err_sz, "register_effect: null def"); return 0; }
    void* slot;
    TsHandle h = ts_slotmap_alloc(&r->defs, &slot);
    if (!h) { snprintf(err, err_sz, "register_effect: registry full"); return 0; }
    TsDef* d = (TsDef*)slot;
    d->kind = TS_DEF_EFFECT;
    d->as.effect.spec = *def;
    d->as.effect.valid = true;
    return (TesseraDefId)h;
}
