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
} TsEntityInst;

/* Live per-tile instance (keyed by coord). */
typedef struct {
    TesseraCoord coord;
    TesseraDefId def;
    uint32_t     variant;
    float   from_y, to_y;       /* rise/sink offset */
    float   from_alpha, to_alpha;
    TsTween tween;
    bool    removing;
    bool    alive;
} TsTileInst;

struct TsOrch {
    TsEntityInst* entities; size_t entity_count, entity_cap;
    TsTileInst*   tiles;    size_t tile_count,   tile_cap;
    bool          seeded;   /* first promotion snaps instead of animating */
};

struct TsOrch* ts_orch_create(void);
void ts_orch_destroy(struct TsOrch* o);

/* Diff prev->next and (re)start transitions. `prev` may be NULL (first state:
 * snap in). timing drives durations; camera handled by the engine. */
void ts_orch_on_promote(struct TsOrch* o, const TsSnapshot* prev,
                        const TsSnapshot* next, const TesseraTiming* timing);

/* Advance all tweens by dt (already scaled by speed_multiplier by the caller)
 * and recompute interpolated transforms; cull completed removals. */
void ts_orch_advance(struct TsOrch* o, float dt);

/* True when no transitions are active. */
bool ts_orch_is_idle(const struct TsOrch* o);

/* True once any state has been promoted (there are live instances to draw). */
bool ts_orch_has_content(const struct TsOrch* o);

/* Build the frame draw list from live instances into `arena`. Returns count. */
size_t ts_orch_build_drawlist(struct TsOrch* o, TesseraEngine* e,
                              TsArena* arena, struct TsDrawItem** out);

#endif /* TESSERA_ORCH_H */
