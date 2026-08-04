/*
 * engine.h — the internal TesseraEngine. Ties every subsystem together.
 *
 * Frozen contract: subsystems that arrive in later milestones are held as
 * forward-declared pointers so this header stays stable. Core subsystems that
 * exist from M0/M1 (gpu, registry, camera) are embedded by value.
 */
#ifndef TESSERA_ENGINE_H
#define TESSERA_ENGINE_H

#include <SDL3/SDL.h>
#include "core/core.h"
#include "gpu/gpu.h"
#include "registry.h"
#include "scene/scene.h"
#include "anim/anim.h"
#include "tessera.h"

/* Forward-declared subsystems (defined in their own headers, heap-owned). */
typedef struct TsStateStore TsStateStore;   /* state.h  — M3 */
typedef struct TsOrch       TsOrch;         /* orchestration/orch.h — M4 */
typedef struct TsFx         TsFx;           /* fx/fx.h  — M6 */
typedef struct TsDice       TsDice;         /* dice/dice.h — thrown dice */
typedef struct TsAudio      TsAudio;        /* audio/audio.h — sound effects */
typedef struct TsLua        TsLua;          /* lua/lua_vm.h — embedded game scripting */

#define TS_ERR_CAP 512

/* Fixed capacity of the engine event ring (and the per-tick callback stage).
 * Overflow drops the OLDEST events and bumps ev_dropped. */
#define TS_EVENT_CAP 256

struct TesseraEngine {
    TesseraConfig config;
    TsLog         log;
    char          error[TS_ERR_CAP];

    TsArena       frame_arena;   /* reset each frame */
    TsArena       perm_arena;    /* engine lifetime  */

    TsGpu         gpu;
    TsRegistry    registry;
    TsCamera      camera;

    /* camera transition (M7): tween the camera pose from its current pose to the
     * goal pose resolved from state.camera against the live scene each tick. */
    TsCamPose     cam_from, cam_to;  /* tween endpoints (world poses)          */
    TsCamPose     cam_cur;           /* last pose written to the live camera   */
    TsTween       cam_tween;
    bool          cam_active;   /* a glide is in progress                      */
    bool          cam_have;     /* a pose has been set once                    */
    TesseraCamera cam_spec;     /* last promoted camera spec (resolved/tick)   */
    bool          cam_spec_have;/* cam_spec holds a promoted spec              */
    TesseraCamera cam_pending;  /* imperative goal (tessera_set_camera), any-
                                 * thread under state_mutex; tick applies it   */
    bool          cam_pending_have;
    float         cam_follow_y; /* low-passed framed-point height (follow modes)*/
    bool          cam_follow_y_have;/* cam_follow_y seeded for the current follow*/

    TesseraTiming timing;
    TesseraLight  light;
    TesseraQuality quality;       /* guarded by state_mutex (set any-thread)   */
    TesseraQuality quality_frame; /* frame-consistent copy, render thread only */
    TesseraFocus  focus;        /* depth-of-field (off by default) */

    /* directional shadow map (TESSERA_SHADOW_MAP): light matrix fitted to the
     * scene bounds by the depth pre-pass each frame, consumed by the main pass. */
    mat4          shadow_vp;
    bool          shadow_active;      /* a map was rendered this frame */
    float         shadow_bias_const;  /* sampling biases in normalized depth,  */
    float         shadow_bias_slope;  /* rescaled from the fitted depth range  */

    /* thread-safety for set_state handoff */
    SDL_Mutex*    state_mutex;
    /* guards the op/event callback slots below AND is held across their
     * invocation, so tessera_set_*_callback(e, NULL, ...) returning guarantees
     * no in-flight delivery still uses the old fn (hosts may then release it).
     * Lock order: cb_mutex may be taken before state_mutex, never after. */
    SDL_Mutex*    cb_mutex;
    TsStateStore* state;    /* current / target / pending snapshots (M3) */

    /* operation tracking: each set_state gets a monotonic id; an operation
     * completes when its promoted transition next goes idle. `op_next`/
     * `op_completed` are guarded by state_mutex (read any-thread); the rest are
     * touched only on the tick thread. See tessera_set_state / _operation_*. */
    TesseraOpId          op_next;        /* last id handed out (0 = none)        */
    TesseraOpId          op_completed;   /* highest id whose transition settled  */
    TesseraOpId          op_inflight;    /* id of the promoted, not-yet-idle op  */
    bool                 op_has_inflight;
    TesseraOpCompletedFn op_cb;
    void*                op_cb_user;

    /* typed engine event stream: a fixed ring drained by tessera_poll_events
     * (any-thread; ev_head/ev_len/ev_dropped guarded by state_mutex) plus a
     * tick-local stage so the optional callback fires on the tick thread
     * OUTSIDE the mutex at the end of the tick that emitted the events. */
    TesseraEvent   ev_ring[TS_EVENT_CAP];
    uint32_t       ev_head, ev_len;
    uint32_t       ev_dropped;        /* cumulative overflow drops */
    TesseraEventFn ev_cb;
    void*          ev_cb_user;
    TesseraEvent   ev_stage[TS_EVENT_CAP];
    uint32_t       ev_stage_len;      /* tick thread only */

    TsOrch*       orch;     /* diff + tween instances (M4)               */
    TsFx*         fx;       /* particle systems (M6)                     */
    TsDice*       dice;     /* imperatively-thrown dice (outside state)  */
    TsAudio*      audio;    /* sound effects (lazy, first register)      */
    TsLua*        lua;      /* embedded Lua game (lazy, first load)      */

    double        clock;          /* accumulated engine time (s) */
    bool          have_rendered;
    SDL_Thread*   render_thread;  /* engine-driven loop (desktop)   */
    SDL_AtomicInt running;
};

/* One opaque draw request produced by the scene, consumed by the renderer.
 * Decouples the GPU pass from state internals (which arrive in M3). */
typedef struct TsDrawItem {
    const TsMesh*   mesh;
    SDL_GPUTexture* texture;
    mat4            model;
    vec4            tint;
    vec4            uv_rect;   /* atlas remap: u0,v0,u1,v1 */
    /* skinning (M5): when skinned, `joints` points at a joint_count-long
     * palette of skinning matrices in the frame arena. */
    bool            skinned;
    const mat4*     joints;
    uint32_t        joint_count;
    /* selection-highlight source tag: which live object produced this item
     * (hl_kind = TesseraHighlightKind; hl_id 0 = not highlightable). Lets the
     * highlight post pass re-render matching draws into the silhouette mask. */
    uint32_t        hl_kind;
    uint64_t        hl_id;
} TsDrawItem;

/* Build the frame's draw list into `arena` and return count; *out points at
 * the arena-allocated array. Implemented in scene/drawlist.c (demo board until
 * M3 wires it to the live state). */
size_t ts_scene_build_drawlist(TesseraEngine* e, TsArena* arena, TsDrawItem** out);

/* Set the per-engine last-error string (printf-style). */
void ts_engine_set_error(TesseraEngine* e, const char* fmt, ...);

/* Emit one typed engine event (tick thread only; called from the advance
 * paths — orchestrator, dice, camera, op settle). Stamps the engine clock,
 * pushes into the poll ring (dropping the oldest on overflow) and stages the
 * event for the end-of-tick callback flush. Cheap: no allocation. */
void ts_engine_emit_event(TesseraEngine* e, uint32_t type, uint32_t subject,
                          uint64_t subject_id, TesseraCoord coord, float value);

/* Drive the embedded Lua game's playback queue (lua/lua_vm.c). Called on the
 * tick thread from ts_engine_advance, right after the op settle, so a queued
 * state is pushed the same tick the previous operation completes. No-op while
 * e->lua is NULL. */
void ts_lua_advance(TesseraEngine* e);

/* Advance animation clocks + render exactly one frame. */
void ts_engine_tick(TesseraEngine* e, double dt);

/* Advance animation clocks by dt WITHOUT rendering (promote pending state, diff,
 * advance orchestrator/fx/camera). ts_engine_tick == advance + render. Split out
 * so embedders can render offscreen instead of to the window swapchain. */
void ts_engine_advance(TesseraEngine* e, double dt);

/* Render the current scene (called inside tick). Split out for testability. */
void ts_engine_render(TesseraEngine* e);

/* Render one frame offscreen at (w,h) into `out_rgba` (RGBA8, top-left origin,
 * w*h*4 bytes; out_size must be >= that). Same offscreen path as capture_png but
 * no file I/O — for host compositors that display the frame themselves (e.g. a
 * Flutter platform view uploading to a Metal texture). Returns false on failure. */
bool ts_engine_render_rgba(TesseraEngine* e, uint32_t w, uint32_t h,
                           uint8_t* out_rgba, size_t out_size);

/* Record the frame's draw calls into an already-begun render pass. Shared by
 * the on-screen renderer and the offscreen capture path. */
void ts_engine_record_draws(TesseraEngine* e, SDL_GPUCommandBuffer* cmd,
                            SDL_GPURenderPass* pass, uint32_t vp_w, uint32_t vp_h);

/* Render the directional shadow map (its own depth-only render pass) when
 * quality.shadows == TESSERA_SHADOW_MAP. Must run on `cmd` BEFORE the main
 * render pass begins; sets e->shadow_active/shadow_vp for record_draws. */
void ts_engine_shadow_pass(TesseraEngine* e, SDL_GPUCommandBuffer* cmd);

/* Selection outline & glow post pass: for each live highlight, re-render the
 * flagged object(s) flat into the silhouette mask (at their live interpolated
 * transforms), then composite a dilated outline / blurred additive glow over
 * `dst`. Runs AFTER the main pass (and after depth-of-field, so the selection
 * stays crisp) on the same command buffer. */
void ts_engine_highlight_pass(TesseraEngine* e, SDL_GPUCommandBuffer* cmd,
                              SDL_GPUTexture* dst, uint32_t w, uint32_t h);

/* Render one frame offscreen at (w,h) and write it to `png_path`. Returns
 * false and sets last-error on failure. Useful for headless golden tests. */
bool ts_engine_capture_png(TesseraEngine* e, uint32_t w, uint32_t h, const char* png_path);

/* Screen-ray pick against the live scene (scene/pick.c). Fills *out; returns
 * true if a tile or entity was hit. */
bool ts_engine_pick(TesseraEngine* e, float screen_x, float screen_y, TesseraPick* out);

/* Inverse of picking (scene/pick.c): project scene points back to the screen
 * using the current camera pose. */
bool ts_engine_world_to_screen(TesseraEngine* e, const vec3 world, TesseraScreenPos* out);
bool ts_engine_entity_screen_position(TesseraEngine* e, TesseraEntityId id, TesseraScreenPos* out);
bool ts_engine_tile_screen_position(TesseraEngine* e, TesseraTileId id, TesseraScreenPos* out);
bool ts_engine_dice_screen_position(TesseraEngine* e, TesseraDiceId id, TesseraScreenPos* out);
bool ts_engine_card_screen_position(TesseraEngine* e, TesseraCardId id, TesseraScreenPos* out);

/* Smallest orbit distance at which every listed tile + entity fits the viewport
 * (scene/pick.c). Writes *out_distance; returns false if nothing resolves. */
bool ts_engine_fit_distance(TesseraEngine* e,
                            const TesseraTileId* tiles, size_t tile_count,
                            const TesseraEntityId* entities, size_t entity_count,
                            float padding, float* out_distance);

/* Depth-of-field helpers shared by the render + capture paths. */
bool ts_engine_dof_active(const TesseraEngine* e);
void ts_engine_resolve_dof(const TesseraEngine* e, TsDofParams* p);

#endif /* TESSERA_ENGINE_H */
