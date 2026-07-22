/* tessera.c — public C ABI. Thin wrappers over the engine internals. */
#include "engine.h"
#include "state.h"
#include "orchestration/orch.h"
#include "fx/fx.h"
#include <stdlib.h>
#include <string.h>

static void default_light(TesseraLight* l) {
    l->dir[0] = -0.4f; l->dir[1] = -1.0f; l->dir[2] = -0.3f;
    l->color[0] = 1.0f; l->color[1] = 0.98f; l->color[2] = 0.92f;
    l->intensity = 1.0f;
    l->ambient[0] = 0.28f; l->ambient[1] = 0.30f; l->ambient[2] = 0.36f;
}
static void default_timing(TesseraTiming* t) {
    t->move_s = 0.35f; t->add_s = 0.3f; t->remove_s = 0.25f;
    t->tile_s = 0.3f; t->reflow_s = 0.3f; t->camera_s = 0.5f;
    t->speed_multiplier = 1.0f;
}

TesseraEngine* tessera_create(const TesseraConfig* cfg) {
    if (!cfg) return NULL;
    TesseraEngine* e = (TesseraEngine*)calloc(1, sizeof(TesseraEngine));
    if (!e) return NULL;
    e->config = *cfg;
    ts_log_init(&e->log, cfg->log, cfg->log_userdata);

    ts_arena_init(&e->frame_arena, 256 * 1024);
    ts_arena_init(&e->perm_arena, 256 * 1024);
    default_light(&e->light);
    default_timing(&e->timing);
    e->quality.shadows = TESSERA_SHADOW_BLOB;
    e->quality.msaa = 1;
    e->quality.render_scale = 1.0f;

    e->state_mutex = SDL_CreateMutex();

    char err[TS_ERR_CAP];
    if (!ts_gpu_init(&e->gpu, cfg, &e->log, err, sizeof err)) {
        ts_engine_set_error(e, "%s", err);
        /* keep the engine so the caller can read last_error, then destroy */
        return e;
    }
    if (!ts_gpu_create_pipelines(&e->gpu, err, sizeof err)) {
        ts_engine_set_error(e, "%s", err);
        return e;
    }
    if (!ts_registry_init(&e->registry, &e->gpu, &e->perm_arena, &e->log)) {
        ts_engine_set_error(e, "registry init failed");
        return e;
    }
    ts_camera_init(&e->camera);
    e->state = ts_state_create();
    TS_LOGI(&e->log, "tessera %s ready", tessera_version_string());
    return e;
}

void tessera_destroy(TesseraEngine* e) {
    if (!e) return;
    if (e->gpu.device) SDL_WaitForGPUIdle(e->gpu.device);
    if (e->fx) ts_fx_destroy(e->fx, &e->gpu);
    if (e->orch) ts_orch_destroy(e->orch);
    if (e->state) ts_state_destroy(e->state);
    if (e->registry.defs.items) ts_registry_shutdown(&e->registry);
    ts_gpu_shutdown(&e->gpu);
    if (e->state_mutex) SDL_DestroyMutex(e->state_mutex);
    ts_arena_destroy(&e->frame_arena);
    ts_arena_destroy(&e->perm_arena);
    free(e);
}

void tessera_resize(TesseraEngine* e, int w, int h, float density) {
    if (!e) return;
    ts_gpu_resize(&e->gpu, w, h, density);
    e->camera.dirty = true;
}

void tessera_tick(TesseraEngine* e, double dt_seconds) {
    if (!e || !e->gpu.device) return;
    ts_engine_tick(e, dt_seconds);
}

const char* tessera_last_error(TesseraEngine* e) {
    return e ? e->error : "null engine";
}

const char* tessera_backend_name(TesseraEngine* e) {
    if (!e || !e->gpu.device) return "none";
    return SDL_GetGPUDeviceDriver(e->gpu.device);
}

uint32_t tessera_version(void) { return TESSERA_VERSION; }
const char* tessera_version_string(void) {
    return "0.1.0";
}

/* ---- definitions ---- */
TesseraDefId tessera_register_atlas(TesseraEngine* e, const TesseraBytes* image) {
    if (!e) return 0;
    char err[TS_ERR_CAP]; err[0] = 0;
    TesseraDefId id = ts_registry_add_atlas(&e->registry, image, err, sizeof err);
    if (!id) ts_engine_set_error(e, "%s", err);
    return id;
}
TesseraDefId tessera_register_tile_def(TesseraEngine* e, const TesseraTileDef* def) {
    if (!e) return 0;
    char err[TS_ERR_CAP]; err[0] = 0;
    TesseraDefId id = ts_registry_add_tile(&e->registry, def, err, sizeof err);
    if (!id) ts_engine_set_error(e, "%s", err);
    return id;
}
TesseraDefId tessera_register_entity_def(TesseraEngine* e, const TesseraEntityDef* def) {
    if (!e) return 0;
    char err[TS_ERR_CAP]; err[0] = 0;
    TesseraDefId id = ts_registry_add_entity(&e->registry, def, err, sizeof err);
    if (!id) ts_engine_set_error(e, "%s", err);
    return id;
}
TesseraDefId tessera_register_effect_def(TesseraEngine* e, const TesseraEffectDef* def) {
    if (!e) return 0;
    char err[TS_ERR_CAP]; err[0] = 0;
    TesseraDefId id = ts_registry_add_effect(&e->registry, def, err, sizeof err);
    if (!id) ts_engine_set_error(e, "%s", err);
    return id;
}

uint32_t tessera_entity_def_anim_count(TesseraEngine* e, TesseraDefId def) {
    if (!e) return 0;
    TsDef* d = ts_registry_get(&e->registry, def, TS_DEF_ENTITY);
    return d ? d->as.entity.clip_count : 0;
}
const char* tessera_entity_def_anim_name(TesseraEngine* e, TesseraDefId def, uint32_t index) {
    if (!e) return NULL;
    TsDef* d = ts_registry_get(&e->registry, def, TS_DEF_ENTITY);
    if (!d || index >= d->as.entity.clip_count) return NULL;
    return d->as.entity.clips[index].name;
}

/* ---- state / timing / quality / light ---- */
void tessera_set_state(TesseraEngine* e, const TesseraState* state) {
    if (!e || !state || !e->state) return;
    /* Deep-copy off-lock, then publish under the mutex (short critical section). */
    TsSnapshot* snap = ts_snapshot_copy(state);
    if (!snap) { ts_engine_set_error(e, "set_state: snapshot copy failed"); return; }
    SDL_LockMutex(e->state_mutex);
    ts_state_publish_pending(e->state, snap);
    SDL_UnlockMutex(e->state_mutex);
}

/* set_timing/set_quality/set_light write small POD structs the tick reads live
 * without a lock — call them on the tick/render thread (or before starting an
 * engine-driven loop), not concurrently with tick. Only set_state is any-thread.
 * See docs/platforms.md. */
void tessera_set_timing(TesseraEngine* e, const TesseraTiming* t) {
    if (e && t) e->timing = *t;
}
bool tessera_is_idle(TesseraEngine* e) {
    if (!e) return true;
    if (e->cam_active) return false;
    /* not idle while a pending snapshot is waiting to promote */
    bool pending;
    SDL_LockMutex(e->state_mutex);
    pending = e->state && e->state->has_pending;
    SDL_UnlockMutex(e->state_mutex);
    if (pending) return false;
    if (e->fx && !ts_fx_is_idle(e->fx)) return false;
    return !e->orch || ts_orch_is_idle(e->orch);
}
void tessera_set_quality(TesseraEngine* e, const TesseraQuality* q) {
    if (e && q) e->quality = *q;
}
void tessera_set_light(TesseraEngine* e, const TesseraLight* l) {
    if (e && l) e->light = *l;
}
void tessera_set_projection(TesseraEngine* e, TesseraProjection mode) {
    if (!e) return;
    e->camera.ortho = (mode == TESSERA_PROJECTION_ISOMETRIC);
    e->camera.dirty = true;
}
void tessera_set_focus(TesseraEngine* e, const TesseraFocus* focus) {
    if (!e) return;
    if (focus) e->focus = *focus;
    else       e->focus = (TesseraFocus){0};
}
void tessera__debug_orbit(TesseraEngine* e, float dyaw, float dpitch, float dzoom) {
    if (e) ts_camera_orbit(&e->camera, dyaw, dpitch, dzoom);
}

bool tessera_pick(TesseraEngine* e, float screen_x, float screen_y, TesseraPick* out) {
    if (!e || !out) return false;
    return ts_engine_pick(e, screen_x, screen_y, out);
}

bool tessera_world_to_screen(TesseraEngine* e, const float world[3], TesseraScreenPos* out) {
    if (!e || !world || !out) return false;
    return ts_engine_world_to_screen(e, (const float*)world, out);
}

bool tessera_entity_screen_position(TesseraEngine* e, TesseraEntityId id, TesseraScreenPos* out) {
    if (!e || !out) return false;
    return ts_engine_entity_screen_position(e, id, out);
}

bool tessera_tile_screen_position(TesseraEngine* e, TesseraTileId id, TesseraScreenPos* out) {
    if (!e || !out) return false;
    return ts_engine_tile_screen_position(e, id, out);
}

bool tessera_camera_fit_distance(TesseraEngine* e,
                                 const TesseraTileId* tiles, size_t tile_count,
                                 const TesseraEntityId* entities, size_t entity_count,
                                 float padding, float* out_distance) {
    if (!e || !out_distance) return false;
    return ts_engine_fit_distance(e, tiles, tile_count, entities, entity_count,
                                  padding, out_distance);
}

bool tessera_capture_png(TesseraEngine* e, int w, int h, const char* png_path) {
    if (!e || !e->gpu.device || w <= 0 || h <= 0 || !png_path) return false;
    return ts_engine_capture_png(e, (uint32_t)w, (uint32_t)h, png_path);
}

bool tessera_render_rgba(TesseraEngine* e, double dt_seconds, int w, int h,
                         void* out_rgba, size_t out_size) {
    if (!e || !e->gpu.device || w <= 0 || h <= 0 || !out_rgba) return false;
    ts_engine_advance(e, dt_seconds);
    return ts_engine_render_rgba(e, (uint32_t)w, (uint32_t)h,
                                 (uint8_t*)out_rgba, out_size);
}

void tessera_set_asset_dir(const char* dir) {
    ts_gpu_set_asset_dir(dir);
}
