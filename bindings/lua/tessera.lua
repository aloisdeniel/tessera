-- tessera.lua — LuaJIT FFI binding for the Tessera renderer.
--
-- Usage:
--   local tessera = require("tessera")           -- loads libtessera
--   local eng = tessera.create{ width=1280, height=720 }
--   ... register defs, push states ...
--
-- Requires LuaJIT (uses the FFI library). The struct layout below MUST match
-- include/tessera.h exactly. The canonical layout reference (sizeof of every
-- struct + offsetof of every field, for the target ABI) is the C self-test
-- tests/test_ffi_layout.c; run it and cross-check if anything drifts. The ABI
-- in include/tessera.h is FROZEN and evolves append-only.

local ffi = require("ffi")

ffi.cdef[[
typedef struct TesseraEngine TesseraEngine;
typedef uint32_t TesseraDefId;
typedef uint64_t TesseraEntityId;
typedef uint64_t TesseraTileId;
typedef enum { TESSERA_LOG_TRACE=0, TESSERA_LOG_DEBUG=1, TESSERA_LOG_INFO=2,
               TESSERA_LOG_WARN=3, TESSERA_LOG_ERROR=4 } TesseraLogLevel;
typedef void (*TesseraLogFn)(void* userdata, int level, const char* msg);

typedef struct {
    void*  native_window;
    int    width, height;
    float  pixel_density;
    bool   engine_driven_loop;
    bool   debug;
    TesseraLogFn log; void* log_userdata;
} TesseraConfig;

typedef struct { const void* data; size_t size; const char* path; const char* debug_name; } TesseraBytes;
typedef struct { float u0, v0, u1, v1; } TesseraRect;

typedef struct {
    TesseraDefId atlas;
    TesseraRect  top, side, bottom;
    float        tint[4];
    float        thickness;
} TesseraTileDef;

typedef struct {
    TesseraBytes gltf;
    TesseraDefId atlas;
    float        scale;
    float        pivot[3];
    int32_t      default_anim, move_anim, spawn_anim, despawn_anim;
    TesseraDefId on_spawn_effect, on_despawn_effect;
} TesseraEntityDef;

typedef enum { TESSERA_EMIT_BURST = 0, TESSERA_EMIT_CONTINUOUS = 1 } TesseraEmitMode;
typedef enum { TESSERA_BLEND_ALPHA = 0, TESSERA_BLEND_ADD = 1 } TesseraBlendMode;

typedef struct {
    TesseraDefId     atlas;
    TesseraRect      sprite;
    TesseraEmitMode  mode;
    uint32_t         count;
    float            lifetime_s, lifetime_var, speed, speed_var, spread_deg, gravity;
    float            size_start, size_end, color_start[4], color_end[4];
    TesseraBlendMode blend;
    float            duration_s;
} TesseraParticleSpec;

typedef struct { TesseraParticleSpec on_add, on_remove; } TesseraEffectDef;

typedef uint64_t TesseraDiceId;
typedef uint64_t TesseraCardId;
typedef uint64_t TesseraCardDrawId;
typedef uint64_t TesseraHandId;
typedef uint64_t TesseraOpId;
typedef struct { TesseraBytes sprite; } TesseraDiceFace;
typedef struct {
    const TesseraDiceFace* faces; size_t face_count;
    float size; float tint[4];
} TesseraDiceDef;

typedef struct {
    TesseraDefId visible_atlas; TesseraRect visible_uv;
    TesseraDefId hidden_atlas;  TesseraRect hidden_uv;
    TesseraDefId back_atlas;    TesseraRect back_uv;
    float width, height, thickness, corner_radius;
    float tint[4];
} TesseraCardDef;

typedef struct { int32_t x, y; } TesseraCoord;
typedef struct { float x, y; } TesseraCoordF;
typedef struct { TesseraCoord coord; TesseraDefId tile_def; uint32_t variant; TesseraTileId id; } TesseraTilePlacement;
typedef struct { TesseraEntityId id; TesseraDefId def; TesseraCoord coord; uint16_t facing; uint32_t anim; const TesseraCoord* path; uint32_t path_count; } TesseraEntityPlacement;
typedef struct { TesseraEntityId id; TesseraDefId def; TesseraCoord coord; TesseraEntityId attach_entity_id; } TesseraEffectPlacement;
typedef struct {
    TesseraDiceId id; TesseraDefId def; uint32_t face;
    float position[3]; uint32_t seed; float throw_s;
} TesseraDicePlacement;
typedef struct {
    TesseraCardId id; TesseraDefId def;
    float position[3]; float orientation[4];
    bool hidden; TesseraHandId hand; uint32_t hand_slot;
    TesseraCardDrawId source_draw;
    const float* path; uint32_t path_count;
} TesseraCardPlacement;
typedef struct {
    TesseraCardDrawId id; TesseraDefId def;
    float position[3]; float orientation[4];
    uint32_t count; bool top_hidden;
} TesseraCardDrawPlacement;
typedef struct {
    TesseraHandId id; float position[3]; float orientation[4];
    float spread_deg, radius, card_spacing;
} TesseraHandPlacement;
typedef struct { TesseraCoordF focus; float distance, yaw, pitch, fov; } TesseraCamera;

typedef struct {
    const TesseraTilePlacement*   tiles;    size_t tile_count;
    const TesseraEntityPlacement* entities; size_t entity_count;
    const TesseraEffectPlacement* effects;  size_t effect_count;
    TesseraCamera camera;
    uint64_t      epoch;
    const TesseraCardPlacement*     cards;      size_t card_count;
    const TesseraCardDrawPlacement* card_draws; size_t card_draw_count;
    const TesseraHandPlacement*     hands;      size_t hand_count;
    const TesseraDicePlacement*     dice;       size_t dice_count;
} TesseraState;

typedef struct {
    bool            hit_tile;    TesseraCoord tile;   float tile_distance;
    bool            hit_entity;  TesseraEntityId entity; float entity_distance;
    float           ray_origin[3]; float ray_dir[3]; float point[3];
    bool            hit_dice;    TesseraDiceId dice;  float dice_distance;
    bool            hit_card;    TesseraCardId card;  float card_distance;
} TesseraPick;

typedef struct {
    bool  onscreen; float x, y; float depth; float world[3];
} TesseraScreenPos;

typedef struct { float move_s, add_s, remove_s, tile_s, reflow_s, camera_s, speed_multiplier; } TesseraTiming;
typedef enum { TESSERA_SHADOW_NONE=0, TESSERA_SHADOW_BLOB=1, TESSERA_SHADOW_MAP=2 } TesseraShadowMode;
typedef struct { TesseraShadowMode shadows; int msaa; float render_scale; } TesseraQuality;
typedef struct { float dir[3]; float color[3]; float intensity; float ambient[3]; } TesseraLight;
typedef enum { TESSERA_PROJECTION_PERSPECTIVE=0, TESSERA_PROJECTION_ISOMETRIC=1 } TesseraProjection;
typedef struct { bool enabled; float focus_distance, focus_range, blur_strength; } TesseraFocus;

TesseraEngine* tessera_create(const TesseraConfig*);
void           tessera_destroy(TesseraEngine*);
void           tessera_resize(TesseraEngine*, int, int, float);
void           tessera_tick(TesseraEngine*, double);
const char*    tessera_last_error(TesseraEngine*);
const char*    tessera_backend_name(TesseraEngine*);
uint32_t       tessera_version(void);
const char*    tessera_version_string(void);

TesseraDefId tessera_register_atlas(TesseraEngine*, const TesseraBytes*);
TesseraDefId tessera_register_tile_def(TesseraEngine*, const TesseraTileDef*);
TesseraDefId tessera_register_entity_def(TesseraEngine*, const TesseraEntityDef*);
TesseraDefId tessera_register_effect_def(TesseraEngine*, const TesseraEffectDef*);
uint32_t     tessera_entity_def_anim_count(TesseraEngine*, TesseraDefId);
const char*  tessera_entity_def_anim_name(TesseraEngine*, TesseraDefId, uint32_t);

TesseraDefId tessera_register_card_def(TesseraEngine*, const TesseraCardDef*);

TesseraDefId tessera_register_dice_def(TesseraEngine*, const TesseraDiceDef*);
uint32_t     tessera_dice_def_face_count(TesseraEngine*, TesseraDefId);
uint32_t     tessera_dice_count(TesseraEngine*);
bool         tessera_dice_face(TesseraEngine*, TesseraDiceId, uint32_t* out_face);
bool         tessera_dice_all_idle(TesseraEngine*);

TesseraOpId tessera_set_state(TesseraEngine*, const TesseraState*);
bool        tessera_operation_completed(TesseraEngine*, TesseraOpId op);
TesseraOpId tessera_last_completed_operation(TesseraEngine*);
typedef void (*TesseraOpCompletedFn)(TesseraOpId op, void* user);
void        tessera_set_operation_callback(TesseraEngine*, TesseraOpCompletedFn fn, void* user);
bool tessera_pick(TesseraEngine*, float screen_x, float screen_y, TesseraPick* out);
bool tessera_world_to_screen(TesseraEngine*, const float world[3], TesseraScreenPos* out);
bool tessera_entity_screen_position(TesseraEngine*, TesseraEntityId id, TesseraScreenPos* out);
bool tessera_tile_screen_position(TesseraEngine*, TesseraTileId id, TesseraScreenPos* out);
bool tessera_dice_screen_position(TesseraEngine*, TesseraDiceId id, TesseraScreenPos* out);
bool tessera_card_screen_position(TesseraEngine*, TesseraCardId id, TesseraScreenPos* out);
bool tessera_camera_fit_distance(TesseraEngine*, const TesseraTileId* tiles, size_t tile_count, const TesseraEntityId* entities, size_t entity_count, float padding, float* out_distance);
void tessera_set_timing(TesseraEngine*, const TesseraTiming*);
bool tessera_is_idle(TesseraEngine*);
void tessera_set_quality(TesseraEngine*, const TesseraQuality*);
void tessera_set_light(TesseraEngine*, const TesseraLight*);
void tessera_set_projection(TesseraEngine*, TesseraProjection mode);
void tessera_set_focus(TesseraEngine*, const TesseraFocus*);

void tessera__debug_orbit(TesseraEngine*, float dyaw, float dpitch, float dzoom);
bool tessera_capture_png(TesseraEngine*, int w, int h, const char* png_path);
]]

local lib = ffi.load("tessera")

local M = { C = lib, ffi = ffi }

-- Convenience wrapper: pass a Lua table of config fields.
function M.create(cfg)
    cfg = cfg or {}
    local c = ffi.new("TesseraConfig")
    c.width = cfg.width or 1280
    c.height = cfg.height or 720
    c.pixel_density = cfg.pixel_density or 1.0
    c.debug = cfg.debug and true or false
    c.engine_driven_loop = cfg.engine_driven_loop and true or false
    local e = lib.tessera_create(c)
    if e == nil then error("tessera_create failed") end
    return e
end

return M
