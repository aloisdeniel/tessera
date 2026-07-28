/* tessera.c — public C ABI. Thin wrappers over the engine internals. */
#include "engine.h"
#include "state.h"
#include "orchestration/orch.h"
#include "fx/fx.h"
#include "dice/dice.h"
#include "audio/audio.h"
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
    e->quality_frame = e->quality;

    e->state_mutex = SDL_CreateMutex();
    e->cb_mutex = SDL_CreateMutex();

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
    if (e->audio) ts_audio_destroy(e->audio);
    if (e->dice) ts_dice_destroy(e->dice);
    if (e->fx) ts_fx_destroy(e->fx, &e->gpu);
    if (e->orch) ts_orch_destroy(e->orch);
    if (e->state) ts_state_destroy(e->state);
    if (e->registry.defs.items) ts_registry_shutdown(&e->registry);
    ts_gpu_shutdown(&e->gpu);
    if (e->state_mutex) SDL_DestroyMutex(e->state_mutex);
    if (e->cb_mutex) SDL_DestroyMutex(e->cb_mutex);
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

/* ---- cards ---- */
TesseraDefId tessera_register_card_def(TesseraEngine* e, const TesseraCardDef* def) {
    if (!e) return 0;
    char err[TS_ERR_CAP]; err[0] = 0;
    TesseraDefId id = ts_registry_add_card(&e->registry, def, err, sizeof err);
    if (!id) ts_engine_set_error(e, "%s", err);
    return id;
}

/* ---- fonts ---- */
TesseraDefId tessera_register_font(TesseraEngine* e, const TesseraBytes* ttf,
                                   float pixel_height) {
    if (!e) return 0;
    char err[TS_ERR_CAP]; err[0] = 0;
    TesseraDefId id = ts_registry_add_font(&e->registry, ttf, pixel_height, err, sizeof err);
    if (!id) ts_engine_set_error(e, "%s", err);
    return id;
}

/* ---- sounds ---- */
TesseraSoundId tessera_register_sound(TesseraEngine* e, const TesseraBytes* wav) {
    if (!e || !wav) return 0;
    if (!e->audio) e->audio = ts_audio_create(&e->log);
    if (!e->audio) { ts_engine_set_error(e, "register_sound: audio init failed"); return 0; }
    char err[TS_ERR_CAP]; err[0] = 0;
    TesseraSoundId id = ts_audio_register(e->audio, wav, err, sizeof err);
    if (!id) ts_engine_set_error(e, "%s", err);
    return id;
}
bool tessera_play_sound(TesseraEngine* e, TesseraSoundId id, float gain) {
    if (!e || !e->audio) return false;
    return ts_audio_play(e->audio, id, gain);
}

/* ---- dice ---- */
TesseraDefId tessera_register_dice_def(TesseraEngine* e, const TesseraDiceDef* def) {
    if (!e) return 0;
    char err[TS_ERR_CAP]; err[0] = 0;
    TesseraDefId id = ts_registry_add_dice(&e->registry, def, err, sizeof err);
    if (!id) ts_engine_set_error(e, "%s", err);
    return id;
}
uint32_t tessera_dice_def_face_count(TesseraEngine* e, TesseraDefId def) {
    if (!e) return 0;
    TsDef* d = ts_registry_get(&e->registry, def, TS_DEF_DICE);
    return d ? d->as.dice.face_count : 0;
}
uint32_t tessera_dice_count(TesseraEngine* e) {
    return (e && e->dice) ? ts_dice_count(e->dice) : 0;
}
bool tessera_dice_face(TesseraEngine* e, TesseraDiceId id, uint32_t* out_face) {
    if (!e || !e->dice) return false;
    return ts_dice_face(e->dice, id, out_face);
}
bool tessera_dice_all_idle(TesseraEngine* e) {
    if (!e || !e->dice) return true;
    return ts_dice_all_idle(e->dice);
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
TesseraOpId tessera_set_state(TesseraEngine* e, const TesseraState* state) {
    if (!e || !state || !e->state) return 0;
    /* Deep-copy off-lock, then publish under the mutex (short critical section). */
    TsSnapshot* snap = ts_snapshot_copy(state);
    if (!snap) { ts_engine_set_error(e, "set_state: snapshot copy failed"); return 0; }
    SDL_LockMutex(e->state_mutex);
    TesseraOpId id = ++e->op_next;   /* monotonic, nonzero; first id is 1 */
    snap->op_id = id;
    ts_state_publish_pending(e->state, snap);
    SDL_UnlockMutex(e->state_mutex);
    return id;
}

bool tessera_operation_completed(TesseraEngine* e, TesseraOpId op) {
    if (!e || op == 0) return true;
    SDL_LockMutex(e->state_mutex);
    bool done = op <= e->op_completed;
    SDL_UnlockMutex(e->state_mutex);
    return done;
}

TesseraOpId tessera_last_completed_operation(TesseraEngine* e) {
    if (!e) return 0;
    SDL_LockMutex(e->state_mutex);
    TesseraOpId c = e->op_completed;
    SDL_UnlockMutex(e->state_mutex);
    return c;
}

void tessera_set_operation_callback(TesseraEngine* e, TesseraOpCompletedFn fn, void* user) {
    if (!e) return;
    /* cb_mutex is held across delivery on the tick thread, so once this
     * returns no in-flight invocation can still be using the old fn/user. */
    SDL_LockMutex(e->cb_mutex);
    e->op_cb = fn;
    e->op_cb_user = user;
    SDL_UnlockMutex(e->cb_mutex);
}

/* ---- typed engine event stream ---- */
uint32_t tessera_poll_events(TesseraEngine* e, TesseraEvent* out, uint32_t cap) {
    if (!e || !out || cap == 0) return 0;
    SDL_LockMutex(e->state_mutex);
    uint32_t n = e->ev_len < cap ? e->ev_len : cap;
    for (uint32_t i = 0; i < n; ++i)
        out[i] = e->ev_ring[(e->ev_head + i) % TS_EVENT_CAP];
    e->ev_head = (e->ev_head + n) % TS_EVENT_CAP;
    e->ev_len -= n;
    SDL_UnlockMutex(e->state_mutex);
    return n;
}

uint32_t tessera_events_dropped(TesseraEngine* e) {
    if (!e) return 0;
    SDL_LockMutex(e->state_mutex);
    uint32_t n = e->ev_dropped;
    SDL_UnlockMutex(e->state_mutex);
    return n;
}

void tessera_set_event_callback(TesseraEngine* e, TesseraEventFn fn, void* user) {
    if (!e) return;
    /* cb_mutex is held across delivery on the tick thread, so once this
     * returns no in-flight invocation can still be using the old fn/user. */
    SDL_LockMutex(e->cb_mutex);
    e->ev_cb = fn;
    e->ev_cb_user = user;
    SDL_UnlockMutex(e->cb_mutex);
}

/* set_timing/set_light write small POD structs the tick reads live without a
 * lock — call them on the tick/render thread (or before starting an
 * engine-driven loop), not concurrently with tick. set_state and set_quality
 * are any-thread. See docs/platforms.md. */
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
    if (e->dice && !ts_dice_all_idle(e->dice)) return false;
    return !e->orch || ts_orch_is_idle(e->orch);
}
void tessera_set_quality(TesseraEngine* e, const TesseraQuality* q) {
    /* Any-thread: written under the state mutex; the render thread takes one
     * frame-consistent copy per frame (quality_frame) under the same mutex. */
    if (!e || !q) return;
    SDL_LockMutex(e->state_mutex);
    e->quality = *q;
    SDL_UnlockMutex(e->state_mutex);
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

bool tessera_dice_screen_position(TesseraEngine* e, TesseraDiceId id, TesseraScreenPos* out) {
    if (!e || !out) return false;
    return ts_engine_dice_screen_position(e, id, out);
}

bool tessera_card_screen_position(TesseraEngine* e, TesseraCardId id, TesseraScreenPos* out) {
    if (!e || !out) return false;
    return ts_engine_card_screen_position(e, id, out);
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
