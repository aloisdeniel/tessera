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
-- tests/test_ffi_layout.c (`--dump` for the machine-readable table); the ctest
-- gate `ffi_binding_drift` (tools/check_ffi_bindings.py) diffs this file
-- against it and hard-fails on any drift. The ABI in include/tessera.h is
-- FROZEN and evolves append-only.

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
typedef uint64_t TesseraLabelId;
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
    TesseraCardId selected_card;
} TesseraHandPlacement;
typedef enum { TESSERA_OVERLAY_SPRITE = 0, TESSERA_OVERLAY_DISC = 1,
               TESSERA_OVERLAY_RING = 2 } TesseraOverlayShape;
typedef struct {
    TesseraCoord coord; uint32_t shape; TesseraDefId atlas; TesseraRect uv;
    float tint[4];
    float pulse_s, pulse_alpha_min, pulse_alpha_max, pulse_scale_min, pulse_scale_max;
} TesseraOverlayPlacement;
typedef enum { TESSERA_LABEL_ANCHOR_WORLD = 0, TESSERA_LABEL_ANCHOR_ENTITY = 1,
               TESSERA_LABEL_ANCHOR_TILE = 2, TESSERA_LABEL_ANCHOR_DICE = 3,
               TESSERA_LABEL_ANCHOR_CARD = 4, TESSERA_LABEL_ANCHOR_DRAW = 5 } TesseraLabelAnchor;
typedef struct {
    TesseraLabelId id; TesseraDefId font;
    char text[64];
    uint32_t anchor; uint64_t anchor_id;
    float position[3]; float size; float color[4];
    bool billboard;
} TesseraLabelPlacement;
typedef enum { TESSERA_HIGHLIGHT_ENTITY = 0, TESSERA_HIGHLIGHT_TILE = 1,
               TESSERA_HIGHLIGHT_DICE = 2, TESSERA_HIGHLIGHT_CARD = 3 } TesseraHighlightKind;
typedef enum { TESSERA_HIGHLIGHT_OUTLINE = 0,
               TESSERA_HIGHLIGHT_GLOW = 1 } TesseraHighlightStyle;
typedef struct {
    uint64_t target_id; uint32_t kind; uint32_t style;
    float color[4]; float thickness;
    float pulse_s, pulse_min, pulse_max;
} TesseraHighlightPlacement;
typedef struct {
    uint32_t      mode;
    TesseraCoordF focus;
    float distance, yaw, pitch, fov;
    float position[3];
    float orientation[4];
    float target[3];
    uint64_t target_id;
    uint64_t focus_card_id;
    float fit_padding;
} TesseraCamera;

typedef uint64_t TesseraPointLightId;
typedef uint64_t TesseraWorldModelId;
typedef struct {
    TesseraPointLightId id;
    float position[3]; float color[3];
    float intensity; float radius;
} TesseraPointLightPlacement;
typedef struct {
    TesseraWorldModelId id; TesseraDefId def;
    float position[3]; float orientation[4]; float scale;
} TesseraWorldModelPlacement;
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
    const TesseraOverlayPlacement*  overlays;   size_t overlay_count;
    const TesseraLabelPlacement*    labels;     size_t label_count;
    const TesseraHighlightPlacement* highlights; size_t highlight_count;
    const TesseraPointLightPlacement* point_lights; size_t point_light_count;
    const TesseraWorldModelPlacement* world_models; size_t world_model_count;
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

typedef enum {
    TESSERA_EVENT_NONE = 0, TESSERA_EVENT_DICE_CONTACT = 1, TESSERA_EVENT_DICE_SETTLED = 2,
    TESSERA_EVENT_ENTITY_HOP_LANDED = 3, TESSERA_EVENT_ENTITY_WAYPOINT_REACHED = 4,
    TESSERA_EVENT_ENTITY_SPAWNED = 5, TESSERA_EVENT_ENTITY_REMOVED = 6,
    TESSERA_EVENT_CARD_FLIPPED = 7, TESSERA_EVENT_CARD_DEALT = 8,
    TESSERA_EVENT_CAMERA_ARRIVED = 9, TESSERA_EVENT_OP_COMPLETED = 10
} TesseraEventType;
typedef enum {
    TESSERA_EVENT_SUBJECT_NONE = 0, TESSERA_EVENT_SUBJECT_ENTITY = 1,
    TESSERA_EVENT_SUBJECT_DICE = 2, TESSERA_EVENT_SUBJECT_CARD = 3,
    TESSERA_EVENT_SUBJECT_DRAW = 4, TESSERA_EVENT_SUBJECT_CAMERA = 5,
    TESSERA_EVENT_SUBJECT_OPERATION = 6
} TesseraEventSubject;
typedef struct {
    double time;
    uint64_t subject_id;
    uint32_t type; uint32_t subject;
    TesseraCoord coord;
    float value; uint32_t reserved;
} TesseraEvent;

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

TesseraDefId tessera_register_font(TesseraEngine*, const TesseraBytes*, float pixel_height);

typedef uint32_t TesseraSoundId;
TesseraSoundId tessera_register_sound(TesseraEngine*, const TesseraBytes* wav);
bool           tessera_play_sound(TesseraEngine*, TesseraSoundId, float gain);

TesseraDefId tessera_register_dice_def(TesseraEngine*, const TesseraDiceDef*);
uint32_t     tessera_dice_def_face_count(TesseraEngine*, TesseraDefId);
uint32_t     tessera_dice_count(TesseraEngine*);
bool         tessera_dice_face(TesseraEngine*, TesseraDiceId, uint32_t* out_face);
bool         tessera_dice_all_idle(TesseraEngine*);

TesseraOpId tessera_set_state(TesseraEngine*, const TesseraState*);
size_t tessera_state_serialize(const TesseraState* state, void* buf, size_t cap);
TesseraState* tessera_state_deserialize(const void* blob, size_t len);
void tessera_state_free(TesseraState* state);
typedef struct TesseraReplay TesseraReplay;
TesseraReplay* tessera_replay_create(void);
TesseraReplay* tessera_replay_open(const void* data, size_t len);
void tessera_replay_free(TesseraReplay*);
bool tessera_replay_append(TesseraReplay*, uint64_t timestamp_ms, const TesseraState*);
uint32_t tessera_replay_count(const TesseraReplay*);
TesseraState* tessera_replay_get(const TesseraReplay*, uint32_t index, uint64_t* out_timestamp_ms);
size_t tessera_replay_serialize(const TesseraReplay*, void* buf, size_t cap);
bool        tessera_operation_completed(TesseraEngine*, TesseraOpId op);
TesseraOpId tessera_last_completed_operation(TesseraEngine*);
typedef void (*TesseraOpCompletedFn)(TesseraOpId op, void* user);
void        tessera_set_operation_callback(TesseraEngine*, TesseraOpCompletedFn fn, void* user);
uint32_t    tessera_poll_events(TesseraEngine*, TesseraEvent* out, uint32_t cap);
uint32_t    tessera_events_dropped(TesseraEngine*);
typedef void (*TesseraEventFn)(const TesseraEvent* ev, void* user);
void        tessera_set_event_callback(TesseraEngine*, TesseraEventFn fn, void* user);
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

-- Drain pending engine events into a Lua array of TesseraEvent cdata copies
-- (oldest first). `cap` bounds one drain (default 64).
function M.poll_events(e, cap)
    cap = cap or 64
    local buf = ffi.new("TesseraEvent[?]", cap)
    local n = tonumber(lib.tessera_poll_events(e, buf, cap))
    local out = {}
    for i = 0, n - 1 do out[i + 1] = ffi.new("TesseraEvent", buf[i]) end
    return out
end

-- Serialize a TesseraState cdata to a Lua binary string (versioned LE blob,
-- see tessera_state_serialize). Returns nil for a nil state.
function M.serialize_state(state)
    if state == nil then return nil end
    local n = tonumber(lib.tessera_state_serialize(state, nil, 0))
    if n == 0 then return nil end
    local buf = ffi.new("uint8_t[?]", n)
    lib.tessera_state_serialize(state, buf, n)
    return ffi.string(buf, n)
end

-- Reconstruct a TesseraState from a blob string. The result is a single native
-- allocation, garbage-collected via tessera_state_free; push it straight
-- through lib.tessera_set_state. Returns nil on malformed input.
function M.deserialize_state(blob)
    if type(blob) ~= "string" then return nil end
    local st = lib.tessera_state_deserialize(blob, #blob)
    if st == nil then return nil end
    return ffi.gc(st, lib.tessera_state_free)
end

-- Serialize a TesseraReplay cdata (see tessera_replay_*) to a binary string.
function M.serialize_replay(replay)
    if replay == nil then return nil end
    local n = tonumber(lib.tessera_replay_serialize(replay, nil, 0))
    if n == 0 then return nil end
    local buf = ffi.new("uint8_t[?]", n)
    lib.tessera_replay_serialize(replay, buf, n)
    return ffi.string(buf, n)
end

-- Parse a replay container string; the handle is garbage-collected. Use
-- lib.tessera_replay_count / M.replay_get to walk the records.
function M.open_replay(blob)
    if type(blob) ~= "string" then return nil end
    local r = lib.tessera_replay_open(blob, #blob)
    if r == nil then return nil end
    return ffi.gc(r, lib.tessera_replay_free)
end

-- Record `index` (0-based) of a replay: returns state (GC-managed), timestamp_ms.
function M.replay_get(replay, index)
    local ts = ffi.new("uint64_t[1]")
    local st = lib.tessera_replay_get(replay, index, ts)
    if st == nil then return nil end
    return ffi.gc(st, lib.tessera_state_free), tonumber(ts[0])
end

return M
