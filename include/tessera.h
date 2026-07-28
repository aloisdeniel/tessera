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
typedef uint64_t TesseraLabelId;  /* live text-label instance id (0 = invalid) */
typedef uint64_t TesseraPointLightId; /* live point-light instance id (0 = invalid) */
typedef uint64_t TesseraWorldModelId; /* live world-model instance id (0 = invalid) */
typedef uint64_t TesseraOpId;     /* set_state operation id (0 = none/complete) */

/* Point lights shaded per frame; extra live lights are ignored. */
#define TESSERA_MAX_POINT_LIGHTS 8

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
    TesseraCardId   attach_card_id;   /* 0 = none; anchors to a live single
                                       * card's displayed face and follows it
                                       * (takes precedence over the entity) */
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
 * `spread_deg` / `radius` / `card_spacing` default when <= 0.
 *
 * `selected_card` (0 = none) singles one of the hand's cards out: the fan
 * parts around it — its neighbours slide aside — while the chosen card lifts
 * clear of the arc, un-rolled and in front, so it is fully visible. Everything
 * tweens on change like any other placement edit. Pairs naturally with the
 * FOCUS_HAND camera's `focus_card_id`. Ignored when no card in the hand
 * matches. */
typedef struct {
    TesseraHandId id;
    float         position[3];
    float         orientation[4]; /* quaternion xyzw (all-zero => identity) */
    float         spread_deg;
    float         radius;
    float         card_spacing;
    TesseraCardId selected_card;  /* 0 = no selection */
} TesseraHandPlacement;

/* ---- tile overlays (ground decals) ------------------------------------ */
/* Built-in overlay shapes. SPRITE samples `atlas`/`uv` across the tile (atlas
 * 0 => a solid tinted quad); DISC and RING are procedural fills that ignore
 * the atlas. */
typedef enum {
    TESSERA_OVERLAY_SPRITE = 0,
    TESSERA_OVERLAY_DISC   = 1,
    TESSERA_OVERLAY_RING   = 2
} TesseraOverlayShape;

/* A flat decal rendered on top of the tile at `coord` (move-range fills,
 * threat rings, drop-target highlights, ...). Keyed by `coord` when diffing:
 * an overlay whose coord newly appears fades in, one that vanishes fades out,
 * and a tint change crossfades from the currently displayed tint. At most one
 * overlay per coord. `tint` multiplies the sprite / fills the shape (all-zero
 * => white).
 *
 * Optional pulse: when `pulse_s > 0` the overlay breathes with that period —
 * its alpha oscillates between `pulse_alpha_min..pulse_alpha_max` (used when
 * `pulse_alpha_max > 0`) and/or its footprint scales between
 * `pulse_scale_min..pulse_scale_max` (used when `pulse_scale_max > 0`). */
typedef struct {
    TesseraCoord coord;      /* board tile the decal sits on (diff key)     */
    uint32_t     shape;      /* TesseraOverlayShape                         */
    TesseraDefId atlas;      /* SPRITE source atlas (0 = untextured/white)  */
    TesseraRect  uv;         /* SPRITE sub-rect of `atlas`                  */
    float        tint[4];    /* RGBA multiply (all-zero => white)           */
    float        pulse_s;    /* breathe period in seconds (<= 0 => steady)  */
    float        pulse_alpha_min, pulse_alpha_max;
    float        pulse_scale_min, pulse_scale_max;
} TesseraOverlayPlacement;

/* ---- world-anchored text labels --------------------------------------- */
/* What a label is glued to. WORLD anchors at `position` directly; the other
 * modes anchor to a live object by id (mirroring the camera FOCUS_* modes) and
 * treat `position` as an offset from that object's LIVE animating transform, so
 * the label stays glued through moves/hops/throws. */
typedef enum {
    TESSERA_LABEL_ANCHOR_WORLD  = 0, /* position is a world point               */
    TESSERA_LABEL_ANCHOR_ENTITY = 1, /* anchor_id = TesseraEntityId             */
    TESSERA_LABEL_ANCHOR_TILE   = 2, /* anchor_id = TesseraTileId (non-zero id) */
    TESSERA_LABEL_ANCHOR_DICE   = 3, /* anchor_id = TesseraDiceId               */
    TESSERA_LABEL_ANCHOR_CARD   = 4, /* anchor_id = TesseraCardId (single card) */
    TESSERA_LABEL_ANCHOR_DRAW   = 5  /* anchor_id = TesseraCardDrawId (pile)    */
} TesseraLabelAnchor;

/* Max label text bytes, including the NUL terminator. */
#define TESSERA_LABEL_TEXT_CAP 64

/* A 3D text label (scores, HP, dice totals, board coordinates), rendered from
 * a font registered with tessera_register_font. Keyed by `id` when diffing: a
 * label that newly appears fades in, one that vanishes fades out, and a text
 * or color change crossfades from what is currently displayed. `text` is
 * UTF-8 (ASCII + Latin-1 glyphs are baked; others are skipped), bounded by
 * TESSERA_LABEL_TEXT_CAP and always NUL-terminated on copy. `size` is the
 * line height in world units (<= 0 => 0.5). `color` multiplies the glyphs
 * (all-zero => white). With `billboard` the label always faces the camera;
 * otherwise it lies flat on the ground plane (+X right, top toward -Z),
 * lifted a hair like a decal. A label anchored to an object that is not live
 * is hidden until the object appears. */
typedef struct {
    TesseraLabelId id;         /* stable across states; diff key            */
    TesseraDefId   font;       /* registered font (0 => label is skipped)   */
    char           text[TESSERA_LABEL_TEXT_CAP]; /* UTF-8, NUL-terminated   */
    uint32_t       anchor;     /* TesseraLabelAnchor                        */
    uint64_t       anchor_id;  /* live object id (anchor != WORLD)          */
    float          position[3];/* world point (WORLD) or offset from anchor */
    float          size;       /* line height in world units (<=0 => 0.5)   */
    float          color[4];   /* RGBA multiply (all-zero => white)         */
    bool           billboard;  /* face the camera each frame                */
} TesseraLabelPlacement;

/* ---- selection highlights (outline / glow post pass) ------------------ */
/* What a highlight is attached to, mirroring the camera FOCUS_* / label
 * anchor object-reference conventions: a live object kind + its instance id. */
typedef enum {
    TESSERA_HIGHLIGHT_ENTITY = 0, /* target_id = TesseraEntityId               */
    TESSERA_HIGHLIGHT_TILE   = 1, /* target_id = TesseraTileId (non-zero id)   */
    TESSERA_HIGHLIGHT_DICE   = 2, /* target_id = TesseraDiceId                 */
    TESSERA_HIGHLIGHT_CARD   = 3  /* target_id = TesseraCardId (single card)   */
} TesseraHighlightKind;

typedef enum {
    TESSERA_HIGHLIGHT_OUTLINE = 0, /* crisp colored rim hugging the silhouette */
    TESSERA_HIGHLIGHT_GLOW    = 1  /* soft additive halo over + around it      */
} TesseraHighlightStyle;

/* A screen-space selection highlight on one live object. The flagged object is
 * re-rendered into a silhouette mask at its LIVE animating transform each
 * frame, then composited over the lit scene (after depth-of-field, so the
 * selection stays crisp) as a dilated outline or a blurred additive glow.
 * Keyed by (`kind`, `target_id`) when diffing: a highlight that newly appears
 * fades in, one that vanishes fades out, and a color change crossfades from
 * the currently displayed color. `color` is RGBA (all-zero => white);
 * `thickness` is the outline width / glow radius in pixels (<= 0 => default).
 *
 * Optional pulse: when `pulse_s > 0` the highlight breathes with that period,
 * its intensity oscillating between `pulse_min..pulse_max` (used when
 * `pulse_max > 0`). The pulse animates in the engine and never keeps a
 * transition from reporting idle. A highlight whose target is not live is
 * simply not drawn until the object appears. */
typedef struct {
    uint64_t target_id;  /* live object id, interpreted per `kind` (diff key) */
    uint32_t kind;       /* TesseraHighlightKind                              */
    uint32_t style;      /* TesseraHighlightStyle                             */
    float    color[4];   /* RGBA (all-zero => white)                          */
    float    thickness;  /* outline width / glow radius, px (<= 0 => default) */
    float    pulse_s;    /* breathe period in seconds (<= 0 => steady)        */
    float    pulse_min, pulse_max; /* intensity range (used when max > 0)     */
} TesseraHighlightPlacement;

/* ---- point lights (positional sphere lights) --------------------------- */
/* A positional light with spherical falloff, lighting the scene IN ADDITION
 * to the global directional + ambient light (tessera_set_light). Keyed by
 * `id` when diffing: a light that newly appears fades its intensity in, one
 * that vanishes fades out, and position/color/intensity/radius changes tween
 * from the currently shown values. At most TESSERA_MAX_POINT_LIGHTS lights
 * are shaded per frame (extras are ignored, nearest-in-array first). */
typedef struct {
    TesseraPointLightId id;   /* stable instance id (diff key; 0 = skipped) */
    float position[3];        /* world units                                */
    float color[3];           /* RGB, 0..1                                  */
    float intensity;          /* scales color (0 = off)                     */
    float radius;             /* falloff range, world units (<=0 => 6)      */
} TesseraPointLightPlacement;

/* ---- world models (decoration around / beneath the board) -------------- */
/* A static model placed in continuous WORLD coordinates (not the tile grid):
 * scenery dressing the space around and under the board — cliffs the board
 * sits on, rocks, trees, ruins. The placement's origin plane (`position[1]`
 * == 0) is JUST BELOW THE TILES: tile tops are y=0 and tiles extend down
 * 0.25 world units, so world models attach to the board's underside and
 * decoration rises up around it. Uses a registered *entity* def for its
 * geometry (skinned models render in their rest pose). Keyed by `id`: new
 * models grow in, vanished ones shrink out, and transform changes tween.
 * World models cast/receive light and shadows but are not pickable and do
 * not affect camera fitting. */
typedef struct {
    TesseraWorldModelId id;   /* stable instance id (diff key; 0 = skipped) */
    TesseraDefId def;         /* registered entity def (its glTF model)     */
    float position[3];        /* world units; y 0 = the tiles' underside    */
    float orientation[4];     /* quaternion xyzw (all-zero => identity)     */
    float scale;              /* extra scale multiplier (<= 0 => 1)         */
} TesseraWorldModelPlacement;

typedef enum {
    TESSERA_CAMERA_ORBIT        = 0, /* grid focus + distance/yaw/pitch (default) */
    TESSERA_CAMERA_MANUAL       = 1, /* eye position + orientation quaternion      */
    TESSERA_CAMERA_TARGET       = 2, /* eye position + look-at target              */
    TESSERA_CAMERA_FOCUS_TILE   = 3, /* frame a live tile   (target_id)            */
    TESSERA_CAMERA_FOCUS_ENTITY = 4, /* follow a live entity                       */
    TESSERA_CAMERA_FOCUS_DICE   = 5, /* follow a live die                          */
    TESSERA_CAMERA_FOCUS_DRAW   = 6, /* follow a live card pile                    */
    TESSERA_CAMERA_FOCUS_CARD   = 7, /* frame a live card fullscreen (target_id)   */
    TESSERA_CAMERA_FOCUS_HAND   = 8  /* frame a live hand (+ optional focus_card_id)*/
} TesseraCameraMode;

typedef struct {
    uint32_t      mode;          /* TesseraCameraMode; 0 = ORBIT (default)         */

    /* ORBIT + FOCUS_TILE/ENTITY/DICE/DRAW framing. ORBIT frames `focus` (a grid
     * coord); the FOCUS_* modes frame the live object `target_id` from `distance`
     * world units at orbit angles `yaw`/`pitch`. `fov` (vertical radians; <=0 =>
     * keep current) applies to every mode. */
    TesseraCoordF focus;         /* ORBIT grid focus                               */
    float distance;              /* ORBIT + FOCUS_* : dst from the framed point    */
    float yaw, pitch;            /* ORBIT + FOCUS_* : orbit angles (radians)       */
    float fov;                   /* all modes; vertical fov radians (<=0 => keep)  */

    /* MANUAL: eye at `position`, oriented by `orientation` (quaternion xyzw;
     * fwd = q·-Z, up = q·+Y). TARGET: eye at `position`, looks at `target`. */
    float position[3];
    float orientation[4];        /* quaternion xyzw (all-zero => identity)         */
    float target[3];

    /* FOCUS_* object id, interpreted per mode: tile / entity / dice / draw /
     * card / hand instance id. */
    uint64_t target_id;

    /* FOCUS_HAND: optional card in the hand to bring fullscreen-centre (0=none). */
    uint64_t focus_card_id;

    /* FOCUS_CARD / FOCUS_HAND: fraction of the frame kept clear around the fitted
     * card(s) (<=0 => default 0.08). Clamp to [0, 0.9]. */
    float fit_padding;
} TesseraCamera;

typedef struct {
    const TesseraTilePlacement*   tiles;    size_t tile_count;
    const TesseraEntityPlacement* entities; size_t entity_count;
    const TesseraEffectPlacement* effects;  size_t effect_count;
    TesseraCamera camera;
    uint64_t      epoch;        /* optional caller sequence number; carried in
                                 * the tessera_state_serialize blob header    */
    /* Appended after epoch so the offsets above stay stable. */
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

/* Deep-copies the snapshot; diffs against current; animates transitions.
 * Thread-safe. Caller may free its arrays immediately after return.
 *
 * Returns a monotonically increasing, nonzero *operation id*. The transition
 * this state triggers is "complete" once the engine has promoted it and every
 * resulting animation (entities, cards, dice, effects, camera) has settled.
 * Track completion three ways (all any-thread):
 *   - tessera_operation_completed(e, id) — poll a specific id;
 *   - tessera_last_completed_operation(e) — the highest id done so far;
 *   - tessera_set_operation_callback(...)  — an event fired per completion.
 * Because ids are monotonic, an operation is complete iff id <= the last
 * completed id; a superseded operation (a newer set_state replaced one that had
 * not promoted yet) completes no later than the operation that superseded it. */
TESSERA_API TesseraOpId tessera_set_state(TesseraEngine* e, const TesseraState* state);

/* True once operation `op` has completed (its transition fully animated). Ids
 * are monotonic, so this is `op <= tessera_last_completed_operation(e)`. op == 0
 * always returns true (nothing to wait for). Any-thread. */
TESSERA_API bool tessera_operation_completed(TesseraEngine* e, TesseraOpId op);

/* The highest operation id whose transition has fully settled (0 if none yet).
 * Any-thread. */
TESSERA_API TesseraOpId tessera_last_completed_operation(TesseraEngine* e);

/* Callback invoked once per operation as its transition completes, with the
 * completed id. Fired on the tick thread (the thread that drives tessera_tick /
 * the engine-driven render loop), from inside the tick after the transition has
 * gone idle. Do NOT call back into tessera_set_state or other mutating tessera_*
 * from it. Registering/clearing is any-thread: the slot is mutex-guarded and
 * held across delivery, so once a call passing fn = NULL returns no in-flight
 * invocation still uses the old fn (the host may then release it). `user` is
 * handed back verbatim. */
typedef void (*TesseraOpCompletedFn)(TesseraOpId op, void* user);
TESSERA_API void tessera_set_operation_callback(TesseraEngine* e,
                                                TesseraOpCompletedFn fn, void* user);

/* =======================================================================
 *  Engine event stream (sound / haptics / FX sync)
 *
 *  As transitions animate, the engine emits typed events at the moments a
 *  host wants to react to — a die striking the felt, a piece landing from a
 *  hop, a card flipping over. Events accumulate in a fixed-capacity ring the
 *  host drains with tessera_poll_events (any-thread); an optional callback
 *  (tessera_set_event_callback) additionally delivers each event on the tick
 *  thread, mirroring the operation callback. Nothing allocates per event.
 * ===================================================================== */

typedef enum {
    TESSERA_EVENT_NONE                    = 0,
    TESSERA_EVENT_DICE_CONTACT            = 1,  /* die touched the ground; value = impact speed  */
    TESSERA_EVENT_DICE_SETTLED            = 2,  /* tumble/slide finished; value = face index     */
    TESSERA_EVENT_ENTITY_HOP_LANDED       = 3,  /* hop-arc touchdown (each hop of a move)        */
    TESSERA_EVENT_ENTITY_WAYPOINT_REACHED = 4,  /* multi-step segment handoff; value = step no.  */
    TESSERA_EVENT_ENTITY_SPAWNED          = 5,  /* spawn transition completed                    */
    TESSERA_EVENT_ENTITY_REMOVED          = 6,  /* removal transition completed (culled)         */
    TESSERA_EVENT_CARD_FLIPPED            = 7,  /* hidden<->visible flip began; value = hidden   */
    TESSERA_EVENT_CARD_DEALT              = 8,  /* card spawned off a source pile (deal began)   */
    TESSERA_EVENT_CAMERA_ARRIVED          = 9,  /* camera tween settled at its goal pose         */
    TESSERA_EVENT_OP_COMPLETED            = 10  /* set_state operation settled; subject_id = op  */
} TesseraEventType;

/* What subject_id refers to (the object the event happened to). */
typedef enum {
    TESSERA_EVENT_SUBJECT_NONE      = 0,
    TESSERA_EVENT_SUBJECT_ENTITY    = 1,  /* subject_id = TesseraEntityId   */
    TESSERA_EVENT_SUBJECT_DICE      = 2,  /* subject_id = TesseraDiceId     */
    TESSERA_EVENT_SUBJECT_CARD      = 3,  /* subject_id = TesseraCardId     */
    TESSERA_EVENT_SUBJECT_DRAW      = 4,  /* subject_id = TesseraCardDrawId */
    TESSERA_EVENT_SUBJECT_CAMERA    = 5,  /* subject_id = 0                 */
    TESSERA_EVENT_SUBJECT_OPERATION = 6   /* subject_id = TesseraOpId       */
} TesseraEventSubject;

/* One engine event. `time` is the engine clock (accumulated tick seconds) at
 * emission. `coord` is the board tile nearest the subject where that is
 * meaningful (entity hops/waypoints/spawns, dice contacts/settles, cards),
 * else (0,0). `value` is a small per-type payload (see TesseraEventType). */
typedef struct {
    double       time;        /* engine tick time (s)                        */
    uint64_t     subject_id;  /* object id, interpreted per `subject`        */
    uint32_t     type;        /* TesseraEventType                            */
    uint32_t     subject;     /* TesseraEventSubject                         */
    TesseraCoord coord;       /* board coord where meaningful, else (0,0)    */
    float        value;       /* small payload (impact speed, face, step...) */
    uint32_t     reserved;    /* always 0                                    */
} TesseraEvent;

/* Drain up to `cap` pending events into `out`, oldest first, and return how
 * many were written (0 = none pending). Consumes what it returns. Any-thread,
 * lock-protected, no allocation. The engine buffers a bounded number of
 * events (currently 256); when the ring overflows the OLDEST events are
 * dropped and the dropped total (tessera_events_dropped) grows — poll at
 * least once per frame-ish to keep everything. */
TESSERA_API uint32_t tessera_poll_events(TesseraEngine* e, TesseraEvent* out, uint32_t cap);

/* Total number of events dropped to ring overflow since engine creation
 * (cumulative; 0 when the host keeps up). Any-thread. */
TESSERA_API uint32_t tessera_events_dropped(TesseraEngine* e);

/* Callback invoked once per event, in emission order, on the tick thread at
 * the end of the tick that produced it (outside the engine's state mutex) —
 * modeled on tessera_set_operation_callback. The TesseraEvent pointer is only
 * valid for the duration of the call; copy it out if you keep it (async
 * marshalling layers should poll instead of dereferencing later). Do NOT call
 * back into tessera_set_state or other mutating tessera_* from it.
 * Registering/clearing is any-thread: the slot is mutex-guarded and held
 * across delivery, so once a call passing fn = NULL returns no in-flight
 * invocation still uses the old fn (the host may then release it). Events are
 * delivered to the callback in addition to (not instead of) the poll ring. */
typedef void (*TesseraEventFn)(const TesseraEvent* ev, void* user);
TESSERA_API void tessera_set_event_callback(TesseraEngine* e,
                                            TesseraEventFn fn, void* user);

/* =======================================================================
 *  State serialization, save / undo & replay
 *
 *  A TesseraState can be flattened to a self-contained, versioned binary
 *  blob (little-endian on every platform) and later reconstructed — the
 *  building block for save games, undo stacks and replays. Serialization is
 *  pure data: no engine is involved, and the blob carries EVERY state field
 *  (tiles, entities incl. multi-step move paths, effects, cards incl. paths,
 *  piles, hands, dice, overlays, labels, highlights, camera, epoch).
 * ===================================================================== */

/* State blob header constants: magic ("TSST" as stored little-endian) +
 * format version. tessera_state_deserialize rejects unknown values cleanly. */
#define TESSERA_STATE_BLOB_MAGIC   0x54535354u  /* bytes "TSST" on disk */
#define TESSERA_STATE_BLOB_VERSION 3u  /* 3: effects carry attach_card_id */

/* Serialize `state` into `buf` and return the REQUIRED byte size. Two-call
 * sizing: call with buf=NULL (or cap=0) to measure, allocate, then call again
 * with the buffer. When `cap` is smaller than the required size nothing
 * useful is written (the return value is still the required size). Returns 0
 * only for a NULL state. The blob starts with a versioned header (magic,
 * version, total size, then TesseraState.epoch as the caller's sequence
 * number) so future ABI growth can migrate old blobs. */
TESSERA_API size_t tessera_state_serialize(const TesseraState* state,
                                           void* buf, size_t cap);

/* Reconstruct a state from a blob produced by tessera_state_serialize. The
 * returned TesseraState and every array it points at live in ONE allocation;
 * free it with tessera_state_free (and nothing else). It is a normal state —
 * push it straight through tessera_set_state. Returns NULL on any malformed
 * input (bad magic/version/size, truncated arrays, garbage) without crashing. */
TESSERA_API TesseraState* tessera_state_deserialize(const void* blob, size_t len);

/* Free a state returned by tessera_state_deserialize / tessera_replay_get. */
TESSERA_API void tessera_state_free(TesseraState* state);

/* ---- replay: a timestamped sequence of state blobs -------------------- */
/* Container header constants ("TSRP" as stored little-endian). */
#define TESSERA_REPLAY_MAGIC   0x50525354u  /* bytes "TSRP" on disk */
#define TESSERA_REPLAY_VERSION 1u

/* A replay is an in-memory sequence of (timestamp_ms, state blob) records
 * behind an opaque handle. Record states as a game plays with
 * tessera_replay_append, flatten the whole container with
 * tessera_replay_serialize (two-call sizing, same as state blobs) and write
 * it wherever you like; later tessera_replay_open parses those bytes back and
 * tessera_replay_get hands each recorded state to tessera_set_state. */
typedef struct TesseraReplay TesseraReplay;

/* New empty replay (for recording). NULL on allocation failure. */
TESSERA_API TesseraReplay* tessera_replay_create(void);

/* Parse a serialized replay container. Deep-copies `data`; the caller may
 * free it immediately. Returns NULL on malformed input without crashing. */
TESSERA_API TesseraReplay* tessera_replay_open(const void* data, size_t len);

TESSERA_API void tessera_replay_free(TesseraReplay* r);

/* Append one record: `state` is serialized immediately (the caller keeps
 * ownership of its arrays). `timestamp_ms` is host-defined (e.g. ms since
 * recording started); playback order is append order. */
TESSERA_API bool tessera_replay_append(TesseraReplay* r, uint64_t timestamp_ms,
                                       const TesseraState* state);

/* Number of records. 0 for NULL. */
TESSERA_API uint32_t tessera_replay_count(const TesseraReplay* r);

/* Reconstruct record `index` (0-based, append order). Optionally writes the
 * record's timestamp to *out_timestamp_ms. Free the returned state with
 * tessera_state_free. NULL on a bad index / corrupt record. */
TESSERA_API TesseraState* tessera_replay_get(const TesseraReplay* r, uint32_t index,
                                             uint64_t* out_timestamp_ms);

/* Flatten the container to bytes; two-call sizing like
 * tessera_state_serialize. Returns 0 only for a NULL replay. */
TESSERA_API size_t tessera_replay_serialize(const TesseraReplay* r,
                                            void* buf, size_t cap);

/* =======================================================================
 *  Picking / hit-testing (screen ray -> scene)
 * ===================================================================== */

/* Result of a screen-space pick. A ray is cast from the camera through the
 * given pixel and tested against the live (currently animating) scene: each
 * tile against its axis-aligned bounding box (the default tile box — full tile
 * footprint, standard thickness, top at y=0), each entity against one fixed
 * bounding sphere (identical radius for every entity), each live die against a
 * bounding sphere sized to its model, and each single card against its oriented
 * bounding box (the flat slab). Piles/draws are not picked. The nearest hit of
 * each kind is reported independently, so a caller can prefer whichever is
 * closer, or use the tile for movement and the entity/die/card for selection. */
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

    bool            hit_dice;
    TesseraDiceId   dice;            /* id of the nearest hit die                 */
    float           dice_distance;   /* ray distance to that die (world units)    */

    bool            hit_card;
    TesseraCardId   card;            /* id of the nearest hit (single) card       */
    float           card_distance;   /* ray distance to that card (world units)   */
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

/* Screen position of a live die's centre, looked up by its TesseraDicePlacement
 * id. Tracks the die's current animating position (throw, slide, or at rest), so
 * it stays glued as the die moves. Returns false if no live die has that id. */
TESSERA_API bool tessera_dice_screen_position(TesseraEngine* e, TesseraDiceId id,
                                              TesseraScreenPos* out);

/* Screen position of a live (single) card's centre, looked up by its
 * TesseraCardPlacement id. Tracks the card's current animating transform.
 * Returns false for id 0, a pile/draw id, or when no live card has that id. */
TESSERA_API bool tessera_card_screen_position(TesseraEngine* e, TesseraCardId id,
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
    TESSERA_SHADOW_NONE = 0,   /* no contact shadows                           */
    TESSERA_SHADOW_BLOB = 1,   /* soft dark decal under each entity (cheapest) */
    TESSERA_SHADOW_MAP  = 2    /* directional depth map + PCF; suppresses blobs */
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

/* Set render quality. Any-thread (mutex-guarded; the renderer takes one
 * consistent copy per frame), so hosts may toggle e.g. the shadow mode live. */
TESSERA_API void tessera_set_quality(TesseraEngine* e, const TesseraQuality* q);
/* Set the directional light + ambient. Call on the tick/render thread (or
 * before starting an engine-driven loop), not concurrently with tick. */
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
 *  Fonts (world-anchored 3D text labels)
 *
 *  Register a TrueType/OpenType font (`ttf` is file bytes or a path, like an
 *  atlas image); the engine bakes ASCII + Latin-1 glyphs at `pixel_height`
 *  into a GPU atlas at registration. Labels then reference the returned def
 *  id from TesseraState.labels (see TesseraLabelPlacement) — everything is
 *  state-driven. `pixel_height` is the rasterized glyph height in texels
 *  (<= 0 => 48); pick roughly the label's tallest on-screen pixel size.
 * ===================================================================== */

TESSERA_API TesseraDefId tessera_register_font(TesseraEngine* e, const TesseraBytes* ttf,
                                               float pixel_height);

/* =======================================================================
 *  Sound effects (SDL audio playback)
 *
 *  Short fire-and-forget clips played through the system's default output —
 *  card flips, dice landings, fanfares — typically triggered by the host off
 *  the engine event stream. Each clip gets its own device stream, so
 *  different clips mix freely and re-triggering a clip restarts it. When no
 *  playback device exists (headless CI) registration still validates and
 *  returns ids, and playback is a silent no-op returning false.
 * ===================================================================== */

typedef uint32_t TesseraSoundId;  /* 0 = invalid / none */

/* Register a sound from WAV file bytes or a path (`wav`, like an atlas
 * image; any PCM/float WAV SDL can parse). Call during setup, like the other
 * register calls. Returns 0 on failure (see tessera_last_error). */
TESSERA_API TesseraSoundId tessera_register_sound(TesseraEngine* e, const TesseraBytes* wav);

/* (Re)start clip `id` at `gain` (1 = as authored, clamped at 0). Any-thread.
 * Returns false when the id is unknown or playback is unavailable. */
TESSERA_API bool tessera_play_sound(TesseraEngine* e, TesseraSoundId id, float gain);

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
