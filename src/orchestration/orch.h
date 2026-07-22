/*
 * orch.h — state diff, per-instance animation, transitions, drawlist (M4).
 *
 * The orchestrator owns the live, animating representation of the scene. On
 * each promotion it diffs the previous snapshot against the new one and starts
 * tweens; each frame it advances them and produces the draw list. Retargeting:
 * new tweens always start from the *current interpolated* transform, so a state
 * pushed mid-animation blends smoothly instead of snapping.
 */
#ifndef TESSERA_ORCH_H
#define TESSERA_ORCH_H

#include "core/core.h"
#include "core/tmath.h"
#include "anim/anim.h"
#include "card/card.h"
#include "state.h"
#include "tessera.h"

/* forward */
typedef struct TesseraEngine TesseraEngine;
struct TsDrawItem;   /* defined in engine.h */

/* Live per-entity instance. */
typedef struct {
    TesseraEntityId id;
    TesseraDefId    def;
    uint16_t        facing;
    uint32_t        anim;
    /* endpoints */
    vec3    from_pos, to_pos;
    versor  from_rot, to_rot;
    float   from_scale, to_scale;
    float   from_alpha, to_alpha;
    TsTween tween;
    bool    arc;        /* hop during a move */
    bool    removing;   /* fading out; cull when tween completes */
    bool    alive;
    /* current interpolated (recomputed each advance) */
    vec3    pos; versor rot; float scale; float alpha;

    /* skeletal animation state machine (M5) */
    int32_t base_anim;        /* state-selected clip (idle/…) */
    int32_t move_anim;        /* resolved move clip, or -1     */
    bool    anim_moving;      /* a positional move is underway  */
    int32_t cur_clip;         /* currently playing clip, or -1  */
    float   clip_time;        /* playback time of cur_clip      */
    int32_t blend_clip;       /* crossfade source clip, or -1   */
    float   blend_from_time;  /* frozen time of the source clip */
    float   blend_t;          /* crossfade progress 0..1        */
    float   blend_dur;        /* crossfade duration (s)         */
} TsEntityInst;

/* Live per-tile instance (keyed by coord). */
typedef struct {
    TesseraCoord  coord;
    TesseraTileId id;           /* host-supplied instance id (0 = none) */
    TesseraDefId  def;
    uint32_t      variant;
    float   from_y, to_y;       /* rise/sink offset */
    float   from_alpha, to_alpha;
    TsTween tween;
    bool    removing;
    bool    alive;
} TsTileInst;

/* Live per-card instance. Covers both single cards and card piles (draws); a
 * pile sets `is_draw` and animates its `thick` (thickness from card count). The
 * front (or pile top) crossfades visible<->hidden via `mix`. Cards attached to a
 * hand have their transform overridden by the hand fan (computed on diff). */
typedef struct {
    uint64_t     id;         /* card id or draw id (namespaced by is_draw) */
    TesseraDefId def;
    bool         is_draw;
    /* transform endpoints */
    vec3    from_pos, to_pos;
    versor  from_rot, to_rot;
    float   from_scale, to_scale;
    float   from_alpha, to_alpha;
    TsTween tween;
    /* front crossfade (0 = visible, 1 = hidden) */
    float   from_mix, to_mix;
    TsTween mix_tween;
    /* pile thickness (world units) */
    float   from_thick, to_thick;
    TsTween thick_tween;
    uint32_t count;
    bool    hidden;          /* last target hidden / top_hidden */
    bool    removing, alive;
    /* current interpolated */
    vec3    pos; versor rot; float scale, alpha, mix, thick;
} TsCardInst;

struct TsOrch {
    TsEntityInst* entities; size_t entity_count, entity_cap;
    TsTileInst*   tiles;    size_t tile_count,   tile_cap;
    TsCardInst*   cards;    size_t card_count,   card_cap;
    bool          seeded;   /* first promotion snaps instead of animating */
};

struct TsOrch* ts_orch_create(void);
void ts_orch_destroy(struct TsOrch* o);

/* Diff prev->next and (re)start transitions. `prev` may be NULL (first state:
 * snap in). timing drives durations; camera handled by the engine. `e` gives
 * access to entity defs (for resolving move/idle clip roles). */
void ts_orch_on_promote(struct TsOrch* o, TesseraEngine* e, const TsSnapshot* prev,
                        const TsSnapshot* next, const TesseraTiming* timing);

/* Advance all tweens by dt (already scaled by speed_multiplier by the caller)
 * and recompute interpolated transforms + animation clocks; cull removals. */
void ts_orch_advance(struct TsOrch* o, float dt);

/* True when no transitions are active. */
bool ts_orch_is_idle(const struct TsOrch* o);

/* True once any state has been promoted (there are live instances to draw). */
bool ts_orch_has_content(const struct TsOrch* o);

/* Current interpolated world position of a live entity by id. Returns false if
 * the id is not present. Used by the fx system to follow attached emitters. */
bool ts_orch_entity_pos(const struct TsOrch* o, TesseraEntityId id, vec3 out);

/* World position of a live tile's top-surface centre (includes the current
 * rise/sink offset), looked up by its instance id. Returns false for id 0 or an
 * unknown id. Used to project a tile back to screen (inverse of picking). */
bool ts_orch_tile_pos(const struct TsOrch* o, TesseraTileId id, vec3 out);

/* Build the frame draw list from live instances into `arena`. Returns count. */
size_t ts_orch_build_drawlist(struct TsOrch* o, TesseraEngine* e,
                              TsArena* arena, struct TsDrawItem** out);

/* Build the card/pile draw items into `arena` (drawn with the card pipeline).
 * Returns count with *out pointing at the arena array. */
size_t ts_orch_build_cards(struct TsOrch* o, TesseraEngine* e,
                           TsArena* arena, TsCardDrawItem** out);

/* A blob-shadow decal cast by an entity onto the ground plane (M7). */
typedef struct {
    vec3  center;   /* world position; y is forced to the ground offset */
    float radius;   /* disc half-extent in world units */
    float alpha;    /* opacity (fades with the entity) */
} TsBlob;

/* Emit one blob per live entity into `arena`. Returns count. */
size_t ts_orch_build_blobs(struct TsOrch* o, TesseraEngine* e,
                           TsArena* arena, TsBlob** out);

#endif /* TESSERA_ORCH_H */
