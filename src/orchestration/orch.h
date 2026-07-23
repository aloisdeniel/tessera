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

/* Max waypoints in a single multi-step move (excess steps are clamped). */
#define TS_MAX_STEPS 24

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
    /* multi-step move: `tween`/`from_pos`/`to_pos` drive one segment at a time;
     * seg_pts holds every segment endpoint (last = the layout target). */
    vec3     seg_pts[TS_MAX_STEPS];
    uint32_t seg_count;   /* number of segments (>=1; 1 = plain single move) */
    uint32_t seg_index;   /* segment currently animating toward seg_pts[idx]  */
    float    seg_dur;     /* per-segment duration                             */
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
    uint64_t hand;           /* hand this card belongs to (0 = free); tracks the
                              * last-applied hand so a fresh entry can be detected */
    bool    hidden;          /* last target hidden / top_hidden */
    bool    removing, alive;
    /* multi-step move (free cards only): segment endpoints, last = target */
    vec3     seg_pts[TS_MAX_STEPS];
    uint32_t seg_count;      /* number of segments (>=1) */
    uint32_t seg_index;      /* segment currently animating */
    float    seg_dur;        /* per-segment duration */
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

/* Current interpolated world centre of a live *single* card (not a pile/draw)
 * by id. Returns false for id 0, a pile id, or an unknown id. Used to project a
 * card back to screen (inverse of picking). */
bool ts_orch_card_pos(const struct TsOrch* o, TesseraCardId id, vec3 out);

/* Live world position of a card *pile/draw* (is_draw==true) by id. Returns
 * false for id 0 or an unknown id. */
bool ts_orch_draw_pos(const struct TsOrch* o, TesseraCardDrawId id, vec3 out);

/* Live transform + slab dims of a single card by id. Returns false for id 0,
 * a pile id, or unknown. out_w/out_h are the card model's world width/height
 * (TsCardModel.width/height × the inst's current scale). Reads the card model
 * dims from `e->registry`. */
bool ts_orch_card_transform(struct TsOrch* o, TesseraEngine* e, TesseraCardId id,
                            vec3 out_pos, versor out_rot, float* out_w, float* out_h);

/* Hand id a live single card is assigned to (0 if none/unknown). */
TesseraHandId ts_orch_card_hand(const struct TsOrch* o, TesseraCardId id);

/* Extent of a hand's live cards in the (right,up) plane about centre H:
 * half_w/half_h are max |proj onto r/u| over card centres, plus half a card
 * (w/2,h/2) from each card's def. Returns false if 0 live cards in the hand. */
bool ts_orch_hand_extent(struct TsOrch* o, TesseraEngine* e, TesseraHandId hand,
                         const vec3 right, const vec3 up, const vec3 H,
                         float* out_half_w, float* out_half_h);

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
