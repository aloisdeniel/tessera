/*
 * tessera.h — Tessera table-top 3D renderer, public C ABI.
 *
 * A small, embeddable 3D renderer for turn-based, grid-based games. The host
 * registers immutable *definitions* (tile / entity / effect types), then pushes
 * immutable *state* snapshots. The engine deep-copies each snapshot, diffs it
 * against the previous one, and animates the transitions.
 *
 * Design contract (see plan/README.md):
 *   - Pure C, extern "C", only fixed-width types / bool / float / pointers.
 *   - Flat structs; arrays passed as (pointer, count) pairs of POD elements.
 *   - Opaque handles across the boundary; no engine ownership leaks.
 *   - tessera_set_state is thread-safe and deep-copies; caller may free
 *     its input immediately after the call returns.
 *
 * Coordinate system: right-handed, +Y up. Tile top surface at y=0. Grid coord
 * (0,0) centers at the world origin. world = tile_size * (x, 0, y) for square
 * grids in v1.
 */
#ifndef TESSERA_H
#define TESSERA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- symbol visibility ------------------------------------------------ */
#if defined(_WIN32)
#  if defined(TESSERA_BUILD_SHARED)
#    define TESSERA_API __declspec(dllexport)
#  elif defined(TESSERA_USE_SHARED)
#    define TESSERA_API __declspec(dllimport)
#  else
#    define TESSERA_API
#  endif
#else
#  if defined(TESSERA_BUILD_SHARED)
#    define TESSERA_API __attribute__((visibility("default")))
#  else
#    define TESSERA_API
#  endif
#endif

/* ---- version ---------------------------------------------------------- */
#define TESSERA_VERSION_MAJOR 0
#define TESSERA_VERSION_MINOR 1
#define TESSERA_VERSION_PATCH 0
#define TESSERA_VERSION \
    ((TESSERA_VERSION_MAJOR << 16) | (TESSERA_VERSION_MINOR << 8) | TESSERA_VERSION_PATCH)

/* ---- fundamental handle types ----------------------------------------- */
typedef struct TesseraEngine TesseraEngine;
typedef uint32_t TesseraDefId;    /* 0 = invalid / none */
typedef uint64_t TesseraEntityId; /* stable across states; key for diffing */
typedef uint64_t TesseraTileId;   /* optional per-tile instance id (0 = none) */

typedef enum {
    TESSERA_LOG_TRACE = 0,
    TESSERA_LOG_DEBUG = 1,
    TESSERA_LOG_INFO  = 2,
    TESSERA_LOG_WARN  = 3,
    TESSERA_LOG_ERROR = 4
} TesseraLogLevel;

typedef void (*TesseraLogFn)(void* userdata, int level, const char* msg);

/* =======================================================================
 *  Lifecycle
 * ===================================================================== */

typedef struct {
    void*  native_window;       /* NULL => engine creates its own SDL window   */
    int    width, height;       /* drawable size in pixels                     */
    float  pixel_density;       /* 1.0 desktop, 2.0/3.0 retina/mobile          */
    bool   engine_driven_loop;  /* desktop convenience; ignored on mobile      */
    bool   debug;               /* enable GPU debug / validation layers        */
    TesseraLogFn log;           /* optional; NULL => stderr                    */
    void*  log_userdata;
} TesseraConfig;

TESSERA_API TesseraEngine* tessera_create(const TesseraConfig* cfg);
TESSERA_API void           tessera_destroy(TesseraEngine* e);
TESSERA_API void           tessera_resize(TesseraEngine* e, int w, int h, float density);
/* Advance animation + render one frame. Manual (host-driven) mode. */
TESSERA_API void           tessera_tick(TesseraEngine* e, double dt_seconds);

TESSERA_API const char*    tessera_last_error(TesseraEngine* e);
TESSERA_API const char*    tessera_backend_name(TesseraEngine* e);
TESSERA_API uint32_t       tessera_version(void);
TESSERA_API const char*    tessera_version_string(void);

/* =======================================================================
 *  Assets & definitions
 * ===================================================================== */

/* A borrowed byte span (filesystem path OR in-memory buffer). Not retained
 * past the registration call. If data==NULL, path is used to resolve bytes. */
typedef struct {
    const void* data;       /* NULL => load from `path`         */
    size_t      size;
    const char* path;       /* optional filesystem path         */
    const char* debug_name; /* optional, for diagnostics        */
} TesseraBytes;

/* A sprite region into an atlas, in normalized [0,1] UV space. */
typedef struct { float u0, v0, u1, v1; } TesseraRect;

typedef struct {
    TesseraDefId atlas;          /* registered atlas id (0 = untextured/white) */
    TesseraRect  top, side, bottom;
    float        tint[4];        /* multiply color, RGBA                       */
    float        thickness;      /* relative tile height (default 0.25)        */
} TesseraTileDef;

typedef struct {
    TesseraBytes gltf;           /* model bytes or path                        */
    TesseraDefId atlas;          /* optional override texture (0 = from gltf)  */
    float        scale;          /* default 1.0                                */
    float        pivot[3];       /* feet anchor offset (model space)           */
    /* Optional named clip roles, resolved against clips found in the gltf.
     * -1 = unset. Used to auto-select clips during transitions (M5). */
    int32_t      default_anim;
    int32_t      move_anim;
    int32_t      spawn_anim;
    int32_t      despawn_anim;
    /* Optional linked effects played on spawn / despawn (M6). 0 = none. */
    TesseraDefId on_spawn_effect;
    TesseraDefId on_despawn_effect;
} TesseraEntityDef;

/* ---- particle / effect spec (simulated in M6) ------------------------- */
typedef enum { TESSERA_EMIT_BURST = 0, TESSERA_EMIT_CONTINUOUS = 1 } TesseraEmitMode;
typedef enum { TESSERA_BLEND_ALPHA = 0, TESSERA_BLEND_ADD = 1 } TesseraBlendMode;

typedef struct {
    TesseraDefId     atlas;        /* particle sprite sheet (0 = white quad)   */
    TesseraRect      sprite;
    TesseraEmitMode  mode;
    uint32_t         count;        /* burst count, or rate/sec if continuous   */
    float            lifetime_s, lifetime_var;
    float            speed, speed_var;
    float            spread_deg;   /* emission cone half-angle                 */
    float            gravity;      /* world-space +y acceleration              */
    float            size_start, size_end;
    float            color_start[4], color_end[4];
    TesseraBlendMode blend;
    float            duration_s;   /* 0 => one-shot burst; >0 continuous window*/
} TesseraParticleSpec;

typedef struct {
    TesseraParticleSpec on_add;
    TesseraParticleSpec on_remove;
} TesseraEffectDef;

TESSERA_API TesseraDefId tessera_register_atlas(TesseraEngine* e, const TesseraBytes* image);
TESSERA_API TesseraDefId tessera_register_tile_def(TesseraEngine* e, const TesseraTileDef* def);
TESSERA_API TesseraDefId tessera_register_entity_def(TesseraEngine* e, const TesseraEntityDef* def);
TESSERA_API TesseraDefId tessera_register_effect_def(TesseraEngine* e, const TesseraEffectDef* def);

/* Introspection of animation clips discovered in an entity def (M5). */
TESSERA_API uint32_t    tessera_entity_def_anim_count(TesseraEngine* e, TesseraDefId def);
TESSERA_API const char* tessera_entity_def_anim_name(TesseraEngine* e, TesseraDefId def, uint32_t index);

/* =======================================================================
 *  Immutable state
 * ===================================================================== */

typedef struct { int32_t x, y; } TesseraCoord;

/* A continuous board position. Whole numbers land on tile centres, fractions
 * interpolate between them: (0,0) is the centre of tile (0,0), and (0.5,0.5)
 * is the corner shared by tiles (0,0) and (1,1). Used for the camera focus so
 * it can sit between tiles (e.g. centred on an even-sized board). */
typedef struct { float x, y; } TesseraCoordF;

typedef struct {
    TesseraCoord  coord;
    TesseraDefId  tile_def;  /* 0 = no tile (hole)                */
    uint32_t      variant;   /* per-instance variant / seed       */
    TesseraTileId id;        /* optional; identifies this tile instance for
                              * tessera_tile_screen_position (0 = unqueryable) */
} TesseraTilePlacement;

typedef struct {
    TesseraEntityId id;      /* stable across states; diff key    */
    TesseraDefId    def;
    TesseraCoord    coord;
    uint16_t        facing;  /* 0..3 quadrant (square grid)       */
    uint32_t        anim;    /* active animation clip index (M5)  */
} TesseraEntityPlacement;

typedef struct {
    TesseraEntityId id;
    TesseraDefId    def;
    TesseraCoord    coord;
    TesseraEntityId attach_entity_id; /* 0 = anchored to tile coord (M6) */
} TesseraEffectPlacement;

typedef struct {
    TesseraCoordF focus;        /* continuous grid focus; may sit between tiles */
    float distance, yaw, pitch; /* orbit params (radians for yaw/pitch) */
    float fov;                  /* vertical fov in radians              */
} TesseraCamera;

typedef struct {
    const TesseraTilePlacement*   tiles;    size_t tile_count;
    const TesseraEntityPlacement* entities; size_t entity_count;
    const TesseraEffectPlacement* effects;  size_t effect_count;
    TesseraCamera camera;
    uint64_t      epoch;        /* optional caller sequence number */
} TesseraState;

/* Deep-copies the snapshot; diffs against current; animates transitions.
 * Thread-safe. Caller may free its arrays immediately after return. */
TESSERA_API void tessera_set_state(TesseraEngine* e, const TesseraState* state);

/* =======================================================================
 *  Picking / hit-testing (screen ray -> scene)
 * ===================================================================== */

/* Result of a screen-space pick. A ray is cast from the camera through the
 * given pixel and tested against the live (currently animating) scene: each
 * tile against its axis-aligned bounding box (the default tile box — full tile
 * footprint, standard thickness, top at y=0) and each entity against one fixed
 * bounding sphere (identical radius for every entity). The nearest tile and the
 * nearest entity are reported independently, so a caller can prefer whichever is
 * closer, or use the tile for movement and the entity for selection. */
typedef struct {
    bool            hit_tile;
    TesseraCoord    tile;            /* grid coord of the nearest hit tile        */
    float           tile_distance;   /* ray distance to that tile (world units)   */

    bool            hit_entity;
    TesseraEntityId entity;          /* id of the nearest hit entity              */
    float           entity_distance; /* ray distance to that entity (world units) */

    float           ray_origin[3];   /* world-space ray origin (camera)           */
    float           ray_dir[3];      /* normalized world-space ray direction      */
    float           point[3];        /* world-space point of the nearest hit      */
} TesseraPick;

/* Cast a ray from the camera through (screen_x, screen_y) — logical window
 * coordinates with the origin at the top-left, i.e. the same space as SDL mouse
 * / touch events (feed input coordinates straight through, even on hi-DPI) — and
 * fill *out with the nearest tile and entity hit. Returns true if either a tile
 * or an entity was hit. The ray fields in *out are always populated (even on a
 * miss) for custom tests. Uses the current camera pose; call after ticking so
 * the pose matches what is on screen. Not thread-safe with the tick. */
TESSERA_API bool tessera_pick(TesseraEngine* e, float screen_x, float screen_y, TesseraPick* out);

/* =======================================================================
 *  Projection to screen (inverse of picking: scene -> screen)
 * ===================================================================== */

/* Where a world/scene point lands on screen. `x`,`y` are logical window
 * coordinates (top-left origin) — the same space tessera_pick consumes, so the
 * projection round-trips with a pick. `onscreen` is true only when the point is
 * in front of the camera AND inside the viewport; when false (off-screen or
 * behind the camera) `x`,`y` are still filled for edge/off-screen indicators
 * unless the point is behind the near plane, in which case they are 0. `depth`
 * is normalized device depth in 0..1 (0 = near plane), handy for ordering
 * on-screen overlays. `world` echoes the point that was projected. */
typedef struct {
    bool  onscreen;
    float x, y;        /* logical window coords (top-left origin) */
    float depth;       /* NDC depth 0..1 (0 = near plane)         */
    float world[3];    /* world-space point that was projected     */
} TesseraScreenPos;

/* Project an arbitrary world-space point to screen. Returns false only on a
 * null/invalid engine or a zero-sized viewport; otherwise fills *out (check
 * out->onscreen for visibility). Uses the current camera pose — call after
 * ticking so it matches what is on screen. Not thread-safe with the tick. */
TESSERA_API bool tessera_world_to_screen(TesseraEngine* e, const float world[3],
                                         TesseraScreenPos* out);

/* Screen position of a live entity (its ground anchor / placement position),
 * looked up by the stable id it was given in TesseraEntityPlacement. Tracks the
 * current animating position, so it stays glued through moves/hops. Returns
 * false if no live entity has that id (fully despawned, or never placed). */
TESSERA_API bool tessera_entity_screen_position(TesseraEngine* e, TesseraEntityId id,
                                                TesseraScreenPos* out);

/* Screen position of a live tile's top-surface centre, looked up by the id set
 * on its TesseraTilePlacement. Requires the tile to carry a non-zero id.
 * Returns false for id 0 or when no live tile has that id. */
TESSERA_API bool tessera_tile_screen_position(TesseraEngine* e, TesseraTileId id,
                                              TesseraScreenPos* out);

/* =======================================================================
 *  Transitions & timing
 * ===================================================================== */

typedef struct {
    float move_s, add_s, remove_s, tile_s, reflow_s, camera_s;
    float speed_multiplier;   /* global; default 1.0 */
} TesseraTiming;

TESSERA_API void tessera_set_timing(TesseraEngine* e, const TesseraTiming* t);
TESSERA_API bool tessera_is_idle(TesseraEngine* e);  /* true when no transitions active */

/* =======================================================================
 *  Lighting, shadows & quality (M7)
 * ===================================================================== */

typedef enum {
    TESSERA_SHADOW_NONE = 0,
    TESSERA_SHADOW_BLOB = 1,
    TESSERA_SHADOW_MAP  = 2
} TesseraShadowMode;

typedef struct {
    TesseraShadowMode shadows;
    int   msaa;          /* 1, 2, 4 */
    float render_scale;  /* resolution scale for mobile (1.0 = native) */
} TesseraQuality;

typedef struct {
    float dir[3];        /* directional light direction (points from light) */
    float color[3];
    float intensity;
    float ambient[3];
} TesseraLight;

TESSERA_API void tessera_set_quality(TesseraEngine* e, const TesseraQuality* q);
TESSERA_API void tessera_set_light(TesseraEngine* e, const TesseraLight* l);

/* =======================================================================
 *  Camera projection
 * ===================================================================== */

typedef enum {
    TESSERA_PROJECTION_PERSPECTIVE = 0, /* default: perspective foreshortening */
    TESSERA_PROJECTION_ISOMETRIC   = 1  /* orthographic — flat, board-game look */
} TesseraProjection;

/* Choose the camera projection. Isometric uses an orthographic projection sized
 * to match the perspective framing at the focus distance, so the board keeps a
 * similar on-screen size when toggling. The orbit yaw/pitch still apply. */
TESSERA_API void tessera_set_projection(TesseraEngine* e, TesseraProjection mode);

/* =======================================================================
 *  Depth of field (focal blur)
 * ===================================================================== */

/* A focal field: geometry within `focus_range` of the focal plane stays sharp;
 * everything nearer/farther blurs, ramping to full blur one more range beyond.
 * Distances are world units measured from the camera (eye). */
typedef struct {
    bool  enabled;         /* master on/off (default off)                        */
    float focus_distance;  /* distance to the sharp plane; <=0 = auto (orbit focus) */
    float focus_range;     /* half-depth kept fully sharp (world units)          */
    float blur_strength;   /* max blur radius in pixels at full defocus          */
} TesseraFocus;

/* Configure depth-of-field. Passing NULL or {.enabled=false} disables it (the
 * scene renders directly, no post pass). Applied on the next frame. */
TESSERA_API void tessera_set_focus(TesseraEngine* e, const TesseraFocus* focus);

/* =======================================================================
 *  Debug / dev hooks
 * ===================================================================== */

/* Nudge the orbit camera directly (dev only; state.camera overrides). */
TESSERA_API void tessera__debug_orbit(TesseraEngine* e, float dyaw, float dpitch, float dzoom);

/* Render one frame offscreen at (w,h) and write it to a PNG. Headless-friendly;
 * used for golden-image tests and previews. Returns false on failure. */
TESSERA_API bool tessera_capture_png(TesseraEngine* e, int w, int h, const char* png_path);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* TESSERA_H */
