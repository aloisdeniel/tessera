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
typedef uint64_t TesseraCardId;   /* live card instance id (0 = invalid)      */
typedef uint64_t TesseraCardDrawId;/* live card-pile instance id (0 = invalid) */
typedef uint64_t TesseraHandId;   /* live hand instance id (0 = none/invalid)  */
typedef uint64_t TesseraDiceId;   /* live die instance id (0 = invalid)        */

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
    /* Optional multi-step move. When `path` is non-NULL and `path_count > 1`,
     * an entity whose coord changed walks *through* the listed coords in order
     * (`path[0]` first), hopping from tile to tile, instead of gliding straight
     * to `coord`. The last entry must equal `coord` (the resting tile, still
     * used for layout/picking). The whole walk takes `2 * move_s` (twice a
     * single-tile move, so the hops stay legible), split evenly across the
     * `path_count` steps. NULL or a count of 0/1 behaves exactly like a plain
     * single move to `coord` at the normal `move_s`. The array is copied by
     * tessera_set_state; the caller may free it immediately after. */
    const TesseraCoord* path;
    uint32_t            path_count;
} TesseraEntityPlacement;

typedef struct {
    TesseraEntityId id;
    TesseraDefId    def;
    TesseraCoord    coord;
    TesseraEntityId attach_entity_id; /* 0 = anchored to tile coord (M6) */
} TesseraEffectPlacement;

/* A die in the scene. A placement that newly appears (by id) is thrown: it
 * spawns airborne and tumbles to rest with `face` up, centred at `position`
 * (world space; nothing constrains it to a tile). `seed` varies the tumble;
 * `throw_s` is the tumble duration (<= 0 => default). A die absent from the next
 * state fades out. Changing def/face/seed/position re-throws it. */
typedef struct {
    TesseraDiceId id;
    TesseraDefId  def;
    uint32_t      face;
    float         position[3];
    uint32_t      seed;
    float         throw_s;
} TesseraDicePlacement;

/* A card in the scene. `orientation` is a quaternion (xyzw); identity lays the
 * card flat with its front (+Y) facing up. `hidden` shows the concealing front
 * texture (crossfades when toggled). If `hand` is non-zero the card is arranged
 * by that hand's fan and `position`/`orientation` are ignored; `hand_slot`
 * orders it in the fan (lower = one end). Position/orientation changes tween.
 *
 * `source_draw` names a card-pile (`TesseraCardDrawPlacement.id`) this card is
 * dealt from: when the card first appears, if that pile is present it spawns
 * resting on top of the pile and slides/flips to its target instead of fading
 * in from nowhere. Ignored after the first frame and when the pile is absent. */
typedef struct {
    TesseraCardId id;
    TesseraDefId  def;
    float         position[3];
    float         orientation[4]; /* quaternion xyzw (all-zero => identity) */
    bool          hidden;
    TesseraHandId hand;           /* 0 => free placement                    */
    uint32_t      hand_slot;
    TesseraCardDrawId source_draw;/* 0 => none; deal-from-pile spawn source */
    /* Optional multi-step move for a *free* card (ignored while `hand != 0`).
     * When `path` is non-NULL and `path_count > 1`, the card tweens through the
     * listed positions in order (each 3 floats: x,y,z; `path[0]` first) rather
     * than sliding straight to `position`. The last position must equal
     * `position`. The whole move takes `2 * move_s` (twice a single move), split
     * evenly across the `path_count` steps. NULL or a count of 0/1 is a plain
     * single move at the normal `move_s`. `path` points at `path_count * 3`
     * floats; copied by tessera_set_state. */
    const float*  path;
    uint32_t      path_count;
} TesseraCardPlacement;

/* A pile of cards drawn as one slab, always resting face-up on the ground.
 * `count` sets the pile thickness (tweens when it changes). The top face shows
 * the def's `visible` (or `hidden` when `top_hidden`) texture; the bottom face
 * always shows the def's `hidden` texture. `orientation` is a quaternion;
 * identity lies flat, top face up. */
typedef struct {
    TesseraCardDrawId id;
    TesseraDefId      def;
    float             position[3];
    float             orientation[4]; /* quaternion xyzw (all-zero => identity) */
    uint32_t          count;
    bool              top_hidden;
} TesseraCardDrawPlacement;

/* A hand: a world-space anchor that fans out the cards assigned to it (the cards
 * whose `hand` field equals this id). `orientation` is a quaternion; identity
 * faces the card fronts toward +Z and spreads the fan along +X. All of
 * `spread_deg` / `radius` / `card_spacing` default when <= 0. */
typedef struct {
    TesseraHandId id;
    float         position[3];
    float         orientation[4]; /* quaternion xyzw (all-zero => identity) */
    float         spread_deg;
    float         radius;
    float         card_spacing;
} TesseraHandPlacement;

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
    /* Appended after epoch so the offsets above stay stable. */
    const TesseraCardPlacement*     cards;      size_t card_count;
    const TesseraCardDrawPlacement* card_draws; size_t card_draw_count;
    const TesseraHandPlacement*     hands;      size_t hand_count;
    const TesseraDicePlacement*     dice;       size_t dice_count;
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

/* Compute the orbit-camera `distance` (zoom) at which every listed tile and
 * entity is on screen — the smallest distance that still keeps them all inside
 * the viewport, so the framing is as tight as possible. Keeps the current
 * camera focus / yaw / pitch / fov / projection and uses the live drawable
 * aspect, so it re-frames correctly after a window resize. `padding` is a
 * fractional screen margin kept clear around the targets (0 = flush to the
 * edges, 0.1 ≈ a 10%% border); it is clamped to [0, 0.9]. Tiles are bounded by
 * their footprint, entities by an approximate standing box. Pass tiles/entities
 * as (pointer, count) pairs; either may be NULL/0. Writes the distance to
 * *out_distance and returns true when at least one target resolves to a live
 * instance; returns false (and leaves the camera to you) otherwise.
 *
 * This is a pure query — it does NOT move the camera. Feed the result into the
 * `distance` of the TesseraCamera you push via tessera_set_state (or reuse your
 * existing pose with the new distance). Call it after pushing the state whose
 * tiles/entities you are fitting, and after tessera_resize, so the live scene
 * and aspect are current. Not thread-safe with the tick. */
TESSERA_API bool tessera_camera_fit_distance(TesseraEngine* e,
                                             const TesseraTileId* tiles, size_t tile_count,
                                             const TesseraEntityId* entities, size_t entity_count,
                                             float padding, float* out_distance);

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
 *  Dice (procedural polyhedral dice with per-face sprites)
 *
 *  Register a dice *definition* from a set of face sprites; the engine builds a
 *  matching convex model (a cube for 6 faces, a two-sided token for 2, an
 *  N-gonal barrel otherwise) and packs the sprites into one atlas, UV-mapped so
 *  each sprite is centred on and fills its face. Dice are then placed into the
 *  scene through tessera_set_state (see TesseraDicePlacement below): a die that
 *  newly appears in a state spawns airborne and tumbles along a precomputed
 *  trajectory, settling with the requested face pointing up; a die that vanishes
 *  from the state fades out. Everything is state-driven — nothing imperative.
 * ===================================================================== */

/* One face of a die: an encoded sprite image (PNG/JPG bytes or a filesystem
 * path), decoded like an atlas image. Shown centred on and filling the face. */
typedef struct { TesseraBytes sprite; } TesseraDiceFace;

/* A die type. `faces` lists `face_count` sprites (face index 0..count-1);
 * `face_count` must be >= 2. `size` is the model's approximate diameter in world
 * units (<= 0 => 1.0). `tint` multiplies the sprites (all-zero => white). */
typedef struct {
    const TesseraDiceFace* faces;
    size_t                 face_count;
    float                  size;
    float                  tint[4];
} TesseraDiceDef;

TESSERA_API TesseraDefId tessera_register_dice_def(TesseraEngine* e, const TesseraDiceDef* def);

/* Number of faces of a registered dice def (0 if `def` is not a dice def). */
TESSERA_API uint32_t tessera_dice_def_face_count(TesseraEngine* e, TesseraDefId def);

/* Number of live dice, including those still fading in or out. */
TESSERA_API uint32_t tessera_dice_count(TesseraEngine* e);

/* The face targeted (settling or settled) by a live die. Returns false for an
 * unknown id; on success writes the face index to *out_face. */
TESSERA_API bool tessera_dice_face(TesseraEngine* e, TesseraDiceId id, uint32_t* out_face);

/* True when every live die has settled (no tumble or fade in progress). */
TESSERA_API bool tessera_dice_all_idle(TesseraEngine* e);

/* =======================================================================
 *  Cards (flat textured cards, piles, and hands)
 *
 *  A card *definition* names three textures (as registered atlas ids, so the
 *  shared ones cost nothing to reuse): the real front (`visible`), a concealing
 *  front (`hidden`, shown so onlookers cannot deduce the card even while it is
 *  in view), and the `back`. The engine builds a thin rounded slab a little
 *  smaller than 2x3 tiles. Cards, card piles (draws) and hands are all placed
 *  through tessera_set_state and animate on diff:
 *    - a card toggling hidden<->visible crossfades its front texture;
 *    - a card's position/orientation change tweens like an entity;
 *    - a pile's card count change tweens its thickness;
 *    - a hand overrides the positions of the cards assigned to it, arranging
 *      them in a world-space fan that follows the hand's transform.
 * ===================================================================== */

/* A card type. Each face references a registered atlas id (0 => white) plus the
 * sub-rect of that atlas to use. `hidden`/`back` are typically shared across
 * every card def. Dimensions are world units; <= 0 selects a sensible default
 * (a slab a little smaller than 2x3 tiles). `tint` multiplies all faces. */
typedef struct {
    TesseraDefId visible_atlas;  TesseraRect visible_uv;  /* the real front face   */
    TesseraDefId hidden_atlas;   TesseraRect hidden_uv;   /* concealing front face */
    TesseraDefId back_atlas;     TesseraRect back_uv;     /* the reverse face      */
    float width;         /* across the short edge  (<= 0 => default ~1.84) */
    float height;        /* along the long edge    (<= 0 => default ~2.76) */
    float thickness;     /* single-card thickness  (<= 0 => default 0.03)  */
    float corner_radius; /* rounded-corner radius  (<= 0 => default 0.12)  */
    float tint[4];
} TesseraCardDef;

TESSERA_API TesseraDefId tessera_register_card_def(TesseraEngine* e, const TesseraCardDef* def);

/* =======================================================================
 *  Debug / dev hooks
 * ===================================================================== */

/* =======================================================================
 *  Debug / dev hooks
 * ===================================================================== */

/* Nudge the orbit camera directly (dev only; state.camera overrides). */
TESSERA_API void tessera__debug_orbit(TesseraEngine* e, float dyaw, float dpitch, float dzoom);

/* Render one frame offscreen at (w,h) and write it to a PNG. Headless-friendly;
 * used for golden-image tests and previews. Returns false on failure. */
TESSERA_API bool tessera_capture_png(TesseraEngine* e, int w, int h, const char* png_path);

/* =======================================================================
 *  Embedding: render into a host-owned surface
 * ===================================================================== */

/* Advance animation by `dt_seconds` and render one frame offscreen into
 * `out_rgba` (RGBA8, top-left origin, w*h*4 bytes; `out_size` must be >= that).
 * This is `tessera_tick` for embedders that present the frame themselves — a
 * platform view / texture compositor (e.g. Flutter) that uploads the pixels to
 * its own GPU surface — instead of presenting to the engine's own window. When
 * embedding, create the engine with a hidden host window as `native_window` so
 * it never shows its own window; drive this once per display refresh in place of
 * `tessera_tick`. Render/main thread. Returns false on failure (see
 * tessera_last_error). */
TESSERA_API bool tessera_render_rgba(TesseraEngine* e, double dt_seconds,
                                     int w, int h, void* out_rgba, size_t out_size);

/* Override the directory that holds the `shaders/` folder (and other assets),
 * used when the engine loads its pipeline shaders. Pass NULL/"" to restore the
 * compile-time default. Process-global — call BEFORE tessera_create so pipeline
 * creation reads from the right place. Needed on iOS/Android, where assets are
 * bundled (app bundle / extracted from the APK) rather than at the build-time
 * path. Harmless on desktop (the default already points at the build tree). */
TESSERA_API void tessera_set_asset_dir(const char* dir);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* TESSERA_H */
