/*
 * orch.c — state diff, per-instance animation, transitions, drawlist (M4).
 *
 * Owns the live animating representation of the scene. On each promotion it
 * diffs prev->next, (re)targeting tweens from the *current* interpolated
 * transform so mid-flight state pushes blend smoothly. Each frame it advances
 * the tweens, culls finished removals, and emits the frame draw list.
 */
#include "orchestration/orch.h"

#include "engine.h"
#include "scene/scene.h"
#include "anim/skeleton.h"
#include "dice/dice.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Board tile nearest a world point (inverse of ts_grid_to_world), for the
 * coord field of emitted engine events. */
static TesseraCoord orch_event_coord(const vec3 p) {
    TesseraCoord c = { (int32_t)lroundf(p[0] / TS_TILE_SIZE),
                       (int32_t)lroundf(p[2] / TS_TILE_SIZE) };
    return c;
}

#define TS_HOP_HEIGHT 0.6f
#define TS_TILE_RISE  0.4f   /* tiles rise from below / sink to below by this */
/* Tile tops sit at y=0 and entities stand on y=0, so a mesh's bottom face is
 * coplanar with the tile face and z-fights. Lift the *rendered* model a hair
 * (the logical pos stays put, so shadows/picking are unaffected). Kept clear of
 * the blob-shadow plane (y=0.02) so the base doesn't z-fight the shadow either. */
#define TS_ENTITY_LIFT 0.035f
/* Overlay decals float between the blob-shadow plane (y=0.02) and the entity
 * lift, so they never z-fight the tile top; shadows composite on top of them
 * (blobs draw later; neither pass writes depth). */
#define TS_OVERLAY_LIFT 0.028f

/* ---- card constants ---- */
#define TS_CARD_FLIP_S   0.32f    /* visible<->hidden crossfade duration       */
#define TS_CARD_THICK_S  0.30f    /* pile thickness (count) tween duration     */
#define TS_CARD_PER_CARD 0.012f   /* pile thickness added per extra card       */
#define TS_CARD_MAX_THICK 0.9f    /* clamp pile thickness (world units)        */
/* hand-fan defaults (used when the placement leaves a field <= 0) */
#define TS_HAND_SPREAD   0.5f     /* total fan angle (radians)                 */
#define TS_HAND_SPACING  0.62f    /* lateral spacing between cards (world)     */
#define TS_HAND_RADIUS   3.0f     /* arc dip radius (world)                    */
/* When a card enters a hand it first flies to a staging point above its slot
 * and a bit toward the viewer, then drops in — so it clears the cards already
 * fanned out instead of slicing through them. */
#define TS_HAND_APPROACH_RISE  1.2f  /* world-up lift of the staging point      */
#define TS_HAND_APPROACH_FRONT 0.7f  /* forward offset (hand-local +Z) of same  */
/* Selected-card presentation (TesseraHandPlacement.selected_card): the fan
 * parts around the chosen card, which lifts clear of the arc, un-rolled and in
 * front of its neighbours so it is fully visible. */
#define TS_HAND_SEL_GAP   0.6f    /* extra lateral shift, fraction of spacing  */
#define TS_HAND_SEL_RAISE 0.55f   /* hand-local +Y lift of the selected card   */
#define TS_HAND_SEL_FRONT 0.25f   /* hand-local +Z pull-out of same            */
/* Point-light falloff range when the placement leaves radius <= 0. */
#define TS_PLIGHT_DEFAULT_RADIUS 6.0f
/* World-model origin plane: the tiles' underside (tile tops are y=0 and the
 * shared tile prism extends down 0.25 * TS_TILE_SIZE — see ts_build_tile_mesh),
 * so decoration attaches beneath the board and rises around it. */
#define TS_WORLD_MODEL_BASE_Y (-0.25f * TS_TILE_SIZE)

/* ------------------------------------------------------------ lifecycle */
struct TsOrch* ts_orch_create(void) {
    struct TsOrch* o = (struct TsOrch*)calloc(1, sizeof *o);
    return o;
}

void ts_orch_destroy(struct TsOrch* o) {
    if (!o) return;
    free(o->entities);
    free(o->tiles);
    free(o->cards);
    free(o->overlays);
    free(o->labels);
    free(o->highlights);
    free(o->point_lights);
    free(o->world_models);
    free(o);
}

/* ------------------------------------------------------------- growable */
static TsEntityInst* orch_add_entity(struct TsOrch* o) {
    if (o->entity_count == o->entity_cap) {
        size_t nc = o->entity_cap ? o->entity_cap * 2 : 8;
        o->entities = (TsEntityInst*)realloc(o->entities, nc * sizeof(TsEntityInst));
        o->entity_cap = nc;
    }
    TsEntityInst* inst = &o->entities[o->entity_count++];
    memset(inst, 0, sizeof *inst);
    return inst;
}

static TsTileInst* orch_add_tile(struct TsOrch* o) {
    if (o->tile_count == o->tile_cap) {
        size_t nc = o->tile_cap ? o->tile_cap * 2 : 8;
        o->tiles = (TsTileInst*)realloc(o->tiles, nc * sizeof(TsTileInst));
        o->tile_cap = nc;
    }
    TsTileInst* inst = &o->tiles[o->tile_count++];
    memset(inst, 0, sizeof *inst);
    return inst;
}

static TsCardInst* orch_add_card(struct TsOrch* o) {
    if (o->card_count == o->card_cap) {
        size_t nc = o->card_cap ? o->card_cap * 2 : 8;
        o->cards = (TsCardInst*)realloc(o->cards, nc * sizeof(TsCardInst));
        o->card_cap = nc;
    }
    TsCardInst* inst = &o->cards[o->card_count++];
    memset(inst, 0, sizeof *inst);
    return inst;
}

static TsOverlayInst* orch_add_overlay(struct TsOrch* o) {
    if (o->overlay_count == o->overlay_cap) {
        size_t nc = o->overlay_cap ? o->overlay_cap * 2 : 8;
        o->overlays = (TsOverlayInst*)realloc(o->overlays, nc * sizeof(TsOverlayInst));
        o->overlay_cap = nc;
    }
    TsOverlayInst* inst = &o->overlays[o->overlay_count++];
    memset(inst, 0, sizeof *inst);
    return inst;
}

static TsLabelInst* orch_add_label(struct TsOrch* o) {
    if (o->label_count == o->label_cap) {
        size_t nc = o->label_cap ? o->label_cap * 2 : 8;
        o->labels = (TsLabelInst*)realloc(o->labels, nc * sizeof(TsLabelInst));
        o->label_cap = nc;
    }
    TsLabelInst* inst = &o->labels[o->label_count++];
    memset(inst, 0, sizeof *inst);
    return inst;
}

static TsHighlightInst* orch_add_highlight(struct TsOrch* o) {
    if (o->highlight_count == o->highlight_cap) {
        size_t nc = o->highlight_cap ? o->highlight_cap * 2 : 8;
        o->highlights = (TsHighlightInst*)realloc(o->highlights, nc * sizeof(TsHighlightInst));
        o->highlight_cap = nc;
    }
    TsHighlightInst* inst = &o->highlights[o->highlight_count++];
    memset(inst, 0, sizeof *inst);
    return inst;
}

static TsHighlightInst* orch_find_highlight(struct TsOrch* o, uint32_t kind, uint64_t id) {
    for (size_t i = 0; i < o->highlight_count; ++i)
        if (o->highlights[i].kind == kind && o->highlights[i].target_id == id)
            return &o->highlights[i];
    return NULL;
}

static TsLabelInst* orch_find_label(struct TsOrch* o, TesseraLabelId id) {
    for (size_t i = 0; i < o->label_count; ++i)
        if (o->labels[i].id == id) return &o->labels[i];
    return NULL;
}

static TsCardInst* orch_find_card(struct TsOrch* o, uint64_t id, bool is_draw) {
    for (size_t i = 0; i < o->card_count; ++i)
        if (o->cards[i].id == id && o->cards[i].is_draw == is_draw)
            return &o->cards[i];
    return NULL;
}

/* ------------------------------------------------------------- lookups */
static TsEntityInst* orch_find_entity(struct TsOrch* o, TesseraEntityId id) {
    for (size_t i = 0; i < o->entity_count; ++i)
        if (o->entities[i].id == id) return &o->entities[i];
    return NULL;
}

static TsTileInst* orch_find_tile(struct TsOrch* o, TesseraCoord c) {
    for (size_t i = 0; i < o->tile_count; ++i)
        if (o->tiles[i].coord.x == c.x && o->tiles[i].coord.y == c.y)
            return &o->tiles[i];
    return NULL;
}

static TsOverlayInst* orch_find_overlay(struct TsOrch* o, TesseraCoord c) {
    for (size_t i = 0; i < o->overlay_count; ++i)
        if (o->overlays[i].coord.x == c.x && o->overlays[i].coord.y == c.y)
            return &o->overlays[i];
    return NULL;
}

static bool snapshot_has_entity(const TsSnapshot* s, TesseraEntityId id) {
    if (!s) return false;
    for (size_t i = 0; i < s->entity_count; ++i)
        if (s->entities[i].id == id) return true;
    return false;
}

static bool snapshot_has_tile(const TsSnapshot* s, TesseraCoord c) {
    if (!s) return false;
    for (size_t i = 0; i < s->tile_count; ++i)
        if (s->tiles[i].tile_def != 0 &&
            s->tiles[i].coord.x == c.x && s->tiles[i].coord.y == c.y)
            return true;
    return false;
}

/* Old coord for an entity id in a snapshot; returns false if not present. */
static bool snapshot_entity_coord(const TsSnapshot* s, TesseraEntityId id,
                                  TesseraCoord* out) {
    if (!s) return false;
    for (size_t i = 0; i < s->entity_count; ++i)
        if (s->entities[i].id == id) { *out = s->entities[i].coord; return true; }
    return false;
}

/* ---------------------------------------------------------- target calc */
typedef struct {
    vec3   pos;
    versor rot;
    float  scale;
    float  alpha;
} TsTarget;

/* Compute world targets for every entity in `next` via the layout solver,
 * grouping by coord and slotting id-sorted within each group. */
static void compute_targets(const TsSnapshot* next, TsTarget* targets) {
    size_t n = next->entity_count;
    if (n == 0) return;

    bool* done = (bool*)calloc(n, sizeof(bool));
    size_t* grp = (size_t*)malloc(n * sizeof(size_t));
    if (!done || !grp) { free(done); free(grp); return; }

    for (size_t i = 0; i < n; ++i) {
        if (done[i]) continue;
        TesseraCoord coord = next->entities[i].coord;

        /* gather the whole group sharing this coord */
        size_t gc = 0;
        for (size_t j = i; j < n; ++j) {
            if (done[j]) continue;
            if (next->entities[j].coord.x == coord.x &&
                next->entities[j].coord.y == coord.y) {
                grp[gc++] = j;
                done[j] = true;
            }
        }

        /* insertion-sort the group by entity id ascending (stable slots) */
        for (size_t a = 1; a < gc; ++a) {
            size_t key = grp[a];
            TesseraEntityId kid = next->entities[key].id;
            size_t b = a;
            while (b > 0 && next->entities[grp[b - 1]].id > kid) {
                grp[b] = grp[b - 1];
                --b;
            }
            grp[b] = key;
        }

        TsLayout layout;
        ts_layout_solve((uint32_t)gc, &layout);

        for (size_t s = 0; s < gc; ++s) {
            size_t idx = grp[s];
            const TesseraEntityPlacement* ep = &next->entities[idx];
            vec3 world;
            ts_grid_to_world(coord.x, coord.y, world);
            uint32_t slot = (s < TS_MAX_SLOTS) ? (uint32_t)s : (TS_MAX_SLOTS - 1);
            world[0] += layout.offset[slot][0] * TS_TILE_SIZE;
            world[2] += layout.offset[slot][1] * TS_TILE_SIZE;
            glm_vec3_copy(world, targets[idx].pos);
            targets[idx].scale = layout.scale;
            targets[idx].alpha = 1.0f;
            ts_facing_quat(ep->facing, targets[idx].rot);
        }
    }

    free(done);
    free(grp);
}

/* --------------------------------------------------------- entity apply */
static void entity_snap(TsEntityInst* inst, const TesseraEntityPlacement* ep,
                        const TsTarget* t) {
    inst->id = ep->id;
    inst->def = ep->def;
    inst->facing = ep->facing;
    inst->anim = ep->anim;
    glm_vec3_copy((float*)t->pos, inst->from_pos);
    glm_vec3_copy((float*)t->pos, inst->to_pos);
    glm_quat_copy((float*)t->rot, inst->from_rot);
    glm_quat_copy((float*)t->rot, inst->to_rot);
    inst->from_scale = inst->to_scale = t->scale;
    inst->from_alpha = inst->to_alpha = 1.0f;
    inst->arc = false;
    inst->removing = false;
    inst->alive = true;
    inst->seg_count = 1;
    inst->seg_index = 0;
    glm_vec3_copy((float*)t->pos, inst->pos);
    glm_quat_copy((float*)t->rot, inst->rot);
    inst->scale = t->scale;
    inst->alpha = 1.0f;
    ts_tween_start(&inst->tween, 0.0f, 0.0f, TS_EASE_LINEAR);
}

static void entity_spawn(TsEntityInst* inst, const TesseraEntityPlacement* ep,
                         const TsTarget* t, float add_s) {
    inst->id = ep->id;
    inst->def = ep->def;
    inst->facing = ep->facing;
    inst->anim = ep->anim;
    glm_vec3_copy((float*)t->pos, inst->from_pos);
    glm_vec3_copy((float*)t->pos, inst->to_pos);
    glm_quat_copy((float*)t->rot, inst->from_rot);
    glm_quat_copy((float*)t->rot, inst->to_rot);
    inst->from_scale = 0.0f;
    inst->to_scale = t->scale;
    inst->from_alpha = 0.0f;
    inst->to_alpha = 1.0f;
    inst->arc = false;
    inst->removing = false;
    inst->spawning = true;
    inst->alive = true;
    inst->seg_count = 1;
    inst->seg_index = 0;
    glm_vec3_copy((float*)t->pos, inst->pos);
    glm_quat_copy((float*)t->rot, inst->rot);
    inst->scale = 0.0f;
    inst->alpha = 0.0f;
    ts_tween_start(&inst->tween, add_s, 0.0f, TS_EASE_OUT_BACK);
}

/* True when the current interpolated transform already sits at the target
 * (within a tight epsilon). A static re-emit of an unchanged instance hits this,
 * letting the caller skip starting a real tween — which would otherwise report
 * *not idle* for a full move_s/reflow_s while nothing visibly moves, padding
 * every multi-beat sequence and needlessly holding the UI disabled. A mid-flight
 * retarget (transform not yet settled) fails the test and still animates. */
static bool pose_settled(const vec3 pos, const versor rot, float scale, float alpha,
                         const vec3 tpos, const versor trot, float tscale) {
    vec3 d;
    glm_vec3_sub((float*)tpos, (float*)pos, d);
    if (glm_vec3_norm2(d) > 1e-6f) return false;
    if (fabsf(scale - tscale) > 1e-3f) return false;
    if (fabsf(alpha - 1.0f) > 1e-3f) return false;
    /* |dot| ~ 1 means the quaternions represent the same orientation. */
    if (fabsf(glm_quat_dot((float*)rot, (float*)trot)) < 0.99999f) return false;
    return true;
}

/* Set up an entity's positional journey toward its target. When `use_path` and
 * the placement supplies >1 waypoints, the entity walks *through* them: the
 * intermediate waypoints are tile centres and the final endpoint is the layout
 * target `t`. The whole move takes `total_s`, split evenly across the segments
 * (so each hop runs faster than a single-tile move). Starts the first segment's
 * tween from the current interpolated position. */
static void entity_set_journey(TsEntityInst* inst, const TesseraEntityPlacement* ep,
                               const TsTarget* t, float total_s, bool arc,
                               bool use_path) {
    uint32_t pc = (use_path && ep->path) ? ep->path_count : 0;
    uint32_t segs = pc > 1 ? pc : 1;
    if (segs > TS_MAX_STEPS) segs = TS_MAX_STEPS;

    for (uint32_t k = 0; k < segs; ++k) {
        if (k == segs - 1) {
            glm_vec3_copy((float*)t->pos, inst->seg_pts[k]);  /* final = layout target */
        } else {
            ts_grid_to_world(ep->path[k].x, ep->path[k].y, inst->seg_pts[k]);
        }
    }
    inst->seg_count = segs;
    inst->seg_index = 0;
    inst->seg_dur = total_s / (float)segs;
    inst->arc = arc;
    glm_vec3_copy(inst->pos, inst->from_pos);
    glm_vec3_copy(inst->seg_pts[0], inst->to_pos);
    ts_tween_start(&inst->tween, inst->seg_dur, 0.0f, TS_EASE_OUT_CUBIC);
}

static void entity_retarget(TsEntityInst* inst, const TesseraEntityPlacement* ep,
                            const TsTarget* t, bool changed,
                            const TesseraTiming* timing) {
    inst->def = ep->def;
    inst->facing = ep->facing;
    inst->anim = ep->anim;
    /* rotation/scale/alpha retarget from the current transform; they converge
     * over the first segment (from == to on every later segment). */
    glm_quat_copy(inst->rot, inst->from_rot);
    inst->from_scale = inst->scale;
    inst->from_alpha = inst->alpha;
    glm_quat_copy((float*)t->rot, inst->to_rot);
    inst->to_scale = t->scale;
    inst->to_alpha = 1.0f;
    inst->removing = false;
    inst->alive = true;
    /* Position: a changed coord animates (arced, walking any supplied path); an
     * unchanged coord just reflows straight to the (possibly re-slotted) target.
     * A real multi-step walk takes twice a single move so each hop stays legible
     * (a single-tile move keeps its normal move_s). */
    bool multi = changed && ep->path && ep->path_count > 1;
    float total = changed ? (multi ? 2.0f * timing->move_s : timing->move_s)
                          : timing->reflow_s;
    /* Unchanged and already at rest at the target → snap (zero duration) rather
     * than run a do-nothing reflow tween that keeps the engine "busy". */
    if (!changed && pose_settled(inst->pos, inst->rot, inst->scale, inst->alpha,
                                 t->pos, t->rot, t->scale)) {
        total = 0.0f;
    }
    entity_set_journey(inst, ep, t, total, changed, changed);
}

static void entity_remove(TsEntityInst* inst, float remove_s) {
    glm_vec3_copy(inst->pos, inst->from_pos);
    glm_quat_copy(inst->rot, inst->from_rot);
    glm_vec3_copy(inst->pos, inst->to_pos);
    glm_quat_copy(inst->rot, inst->to_rot);
    inst->from_scale = inst->scale;
    inst->to_scale = 0.0f;
    inst->from_alpha = inst->alpha;
    inst->to_alpha = 0.0f;
    inst->arc = false;
    inst->removing = true;
    inst->spawning = false;   /* an aborted spawn never reports SPAWNED */
    inst->seg_count = 1;   /* drop any in-flight multi-step path */
    inst->seg_index = 0;
    ts_tween_start(&inst->tween, remove_s, 0.0f, TS_EASE_IN_CUBIC);
}

/* ----------------------------------------------------------- tile apply */
static float tile_cur_y(const TsTileInst* t) {
    return ts_lerpf(t->from_y, t->to_y, ts_tween_value01(&t->tween));
}
static float tile_cur_alpha(const TsTileInst* t) {
    return ts_lerpf(t->from_alpha, t->to_alpha, ts_tween_value01(&t->tween));
}

/* -------------------------------------------------------- anim state */
/* Resolve the move clip role for an entity def (-1 if none/def missing). */
static int32_t resolve_move_anim(TesseraEngine* e, TesseraDefId def) {
    TsDef* d = ts_registry_get(&e->registry, def, TS_DEF_ENTITY);
    return d ? d->as.entity.spec.move_anim : -1;
}

/* Configure the animation state machine for an instance after a diff step. */
static void setup_anim(TsEntityInst* inst, TesseraEngine* e,
                       const TesseraEntityPlacement* ep, bool moving, bool fresh) {
    inst->base_anim = (int32_t)ep->anim;
    inst->move_anim = resolve_move_anim(e, ep->def);
    inst->anim_moving = moving;
    if (fresh) {
        inst->cur_clip = -1;        /* forces initial (blend-free) select */
        inst->clip_time = 0.0f;
        inst->blend_clip = -1;
        inst->blend_from_time = 0.0f;
        inst->blend_t = 1.0f;
        inst->blend_dur = 0.0f;
    }
}

/* ============================================================ cards */
/* Quaternion from a placement's xyzw (all-zero => identity). */
static void placement_quat(const float q[4], versor out) {
    if (q[0] == 0.0f && q[1] == 0.0f && q[2] == 0.0f && q[3] == 0.0f) {
        glm_quat_identity(out);
        return;
    }
    versor v = { q[0], q[1], q[2], q[3] };
    glm_quat_normalize_to(v, out);
}

/* Find a hand placement by id in a snapshot. */
static const TesseraHandPlacement* find_hand(const TsSnapshot* s, TesseraHandId id) {
    if (!s || id == 0) return NULL;
    for (size_t i = 0; i < s->hand_count; ++i)
        if (s->hands[i].id == id) return &s->hands[i];
    return NULL;
}

/* Order two cards within a hand: by hand_slot, then id (stable fan slots). */
static bool card_before(const TesseraCardPlacement* a, const TesseraCardPlacement* b) {
    if (a->hand_slot != b->hand_slot) return a->hand_slot < b->hand_slot;
    return a->id < b->id;
}

/* World transform of a card fanned in a hand: rank r of count N. Cards face the
 * hand's local +Z, spread along local +X and dip at the ends; each is rolled
 * about the front axis so the fan splays. The model front is +Y, so a base
 * rotation stands it up to face +Z.
 *
 * `sel_r` is the rank of the hand's selected card (-1 = none): the other cards
 * shift a gap away from it on their side, and the selected card itself loses
 * its roll/dip, lifts and comes to the very front — fully visible. */
static void hand_fan_target(const TesseraHandPlacement* h, uint32_t r, uint32_t n,
                            int sel_r, vec3 out_pos, versor out_rot) {
    float spread  = h->spread_deg > 0.0f ? glm_rad(h->spread_deg) : TS_HAND_SPREAD;
    float spacing = h->card_spacing > 0.0f ? h->card_spacing : TS_HAND_SPACING;
    float radius  = h->radius > 0.0f ? h->radius : TS_HAND_RADIUS;

    float t = (n > 1) ? ((float)r / (float)(n - 1) - 0.5f) : 0.0f;  /* -0.5..0.5 */
    float theta = t * spread;
    float xoff = ((float)r - (float)(n - 1) * 0.5f) * spacing;
    float yoff = -(1.0f - cosf(theta)) * radius;
    float zoff = (float)r * 0.01f;                 /* stagger toward the viewer */
    if (sel_r >= 0) {
        if ((int)r == sel_r) {
            theta = 0.0f;                          /* straight and readable    */
            yoff  = TS_HAND_SEL_RAISE;             /* lifted clear of the arc  */
            zoff  = (float)n * 0.01f + TS_HAND_SEL_FRONT;  /* in front of all  */
        } else {
            /* part the fan: slide away from the selected card's side */
            xoff += ((int)r < sel_r ? -1.0f : 1.0f) * spacing * TS_HAND_SEL_GAP;
        }
    }
    vec3 local = { xoff, yoff, zoff };

    versor hrot; placement_quat(h->orientation, hrot);
    vec3 rotated; glm_quat_rotatev(hrot, local, rotated);
    out_pos[0] = h->position[0] + rotated[0];
    out_pos[1] = h->position[1] + rotated[1];
    out_pos[2] = h->position[2] + rotated[2];

    /* stand the card up (front +Y -> +Z), then roll it in-plane by -theta */
    versor base; glm_quatv(base, GLM_PI_2f, (vec3){1.0f, 0.0f, 0.0f});
    versor roll; glm_quatv(roll, -theta, (vec3){0.0f, 0.0f, 1.0f});
    versor local_rot; glm_quat_mul(roll, base, local_rot);
    glm_quat_mul(hrot, local_rot, out_rot);
    glm_quat_normalize(out_rot);
}

/* Staging waypoint for a card entering a hand: above its final fan slot
 * (`final_pos`) and a bit toward the viewer (the hand's local +Z front). Flying
 * through it lets the card arrive over the fan and drop into place rather than
 * clipping through the cards already there. */
static void hand_approach_target(const TesseraHandPlacement* h, const vec3 final_pos,
                                 vec3 out) {
    versor hrot; placement_quat(h->orientation, hrot);
    vec3 front = { 0.0f, 0.0f, 1.0f };
    glm_quat_rotatev(hrot, front, front);
    out[0] = final_pos[0] + front[0] * TS_HAND_APPROACH_FRONT;
    out[1] = final_pos[1] + front[1] * TS_HAND_APPROACH_FRONT + TS_HAND_APPROACH_RISE;
    out[2] = final_pos[2] + front[2] * TS_HAND_APPROACH_FRONT;
}

/* Target transform for card index `i` in `next` (free placement or hand fan). */
static void card_target(const TsSnapshot* next, size_t i, vec3 out_pos, versor out_rot) {
    const TesseraCardPlacement* cp = &next->cards[i];
    const TesseraHandPlacement* h = find_hand(next, cp->hand);
    if (cp->hand != 0 && h) {
        /* rank within the hand + total count (+ the selected card's rank) */
        uint32_t n = 0, r = 0;
        int sel_r = -1;
        const TesseraCardPlacement* sel = NULL;
        if (h->selected_card != 0) {
            for (size_t j = 0; j < next->card_count; ++j) {
                const TesseraCardPlacement* o = &next->cards[j];
                if (o->id == h->selected_card && o->hand == cp->hand) { sel = o; break; }
            }
        }
        for (size_t j = 0; j < next->card_count; ++j) {
            const TesseraCardPlacement* o = &next->cards[j];
            if (o->id == 0 || o->hand != cp->hand) continue;  /* id==0 is skipped by card_diff */
            n++;
            if (j != i && card_before(o, cp)) r++;
        }
        if (sel) {
            sel_r = 0;
            for (size_t j = 0; j < next->card_count; ++j) {
                const TesseraCardPlacement* o = &next->cards[j];
                if (o->id == 0 || o->hand != cp->hand || o == sel) continue;
                if (card_before(o, sel)) sel_r++;
            }
        }
        if (n == 0) n = 1;
        hand_fan_target(h, r, n, sel_r, out_pos, out_rot);
        return;
    }
    out_pos[0] = cp->position[0];
    out_pos[1] = cp->position[1];
    out_pos[2] = cp->position[2];
    placement_quat(cp->orientation, out_rot);
}

/* Pile thickness (world units) for a card count. */
static float draw_thickness(TesseraEngine* e, TesseraDefId def, uint32_t count) {
    float base = 0.03f;
    TsDef* d = ts_registry_get(&e->registry, def, TS_DEF_CARD);
    if (d && d->as.card.valid) base = d->as.card.thickness;
    float extra = count > 1 ? (float)(count - 1) * TS_CARD_PER_CARD : 0.0f;
    float t = base + extra;
    return t > TS_CARD_MAX_THICK ? TS_CARD_MAX_THICK : t;
}

static void card_set_journey(TsCardInst* c, const float* path, uint32_t path_count,
                             const vec3 final_pos, float total_s);

/* Snap a card instance to its target (tweens complete). */
static void card_snap(TsCardInst* c, uint64_t id, TesseraDefId def, bool is_draw,
                      const vec3 pos, const versor rot, bool hidden,
                      float thick, uint32_t count) {
    c->id = id; c->def = def; c->is_draw = is_draw;
    glm_vec3_copy((float*)pos, c->from_pos); glm_vec3_copy((float*)pos, c->to_pos);
    glm_quat_copy((float*)rot, c->from_rot); glm_quat_copy((float*)rot, c->to_rot);
    c->from_scale = c->to_scale = 1.0f;
    c->from_alpha = c->to_alpha = 1.0f;
    c->hidden = hidden;
    c->from_mix = c->to_mix = hidden ? 1.0f : 0.0f;
    c->from_thick = c->to_thick = thick;
    c->count = count;
    c->removing = false; c->alive = true;
    c->seg_count = 1; c->seg_index = 0;
    glm_vec3_copy((float*)pos, c->pos); glm_quat_copy((float*)rot, c->rot);
    c->scale = 1.0f; c->alpha = 1.0f; c->mix = c->to_mix; c->thick = thick;
    ts_tween_start(&c->tween, 0.0f, 0.0f, TS_EASE_LINEAR);
    ts_tween_start(&c->mix_tween, 0.0f, 0.0f, TS_EASE_LINEAR);
    ts_tween_start(&c->thick_tween, 0.0f, 0.0f, TS_EASE_LINEAR);
}

/* Spawn a card (fade + pop in) at its target. */
static void card_spawn(TsCardInst* c, uint64_t id, TesseraDefId def, bool is_draw,
                       const vec3 pos, const versor rot, bool hidden,
                       float thick, uint32_t count, float add_s) {
    card_snap(c, id, def, is_draw, pos, rot, hidden, thick, count);
    c->from_scale = 0.0f; c->to_scale = 1.0f;
    c->from_alpha = 0.0f; c->to_alpha = 1.0f;
    c->scale = 0.0f; c->alpha = 0.0f;
    ts_tween_start(&c->tween, add_s, 0.0f, TS_EASE_OUT_BACK);
}

/* Resolve the current top-surface pose of a live draw pile (for deal-from-pile
 * spawns). Prefers the live instance (may be mid-animation); falls back to the
 * new snapshot's placement. Returns false when the pile isn't present. */
static bool draw_top_pose(struct TsOrch* o, TesseraEngine* e, const TsSnapshot* next,
                          TesseraCardDrawId draw_id, vec3 out_pos, versor out_rot,
                          bool* out_hidden) {
    vec3   pos; versor rot; float thick; bool hidden;
    TsCardInst* d = orch_find_card(o, draw_id, true);
    if (d && d->alive && !d->removing) {
        glm_vec3_copy(d->pos, pos); glm_quat_copy(d->rot, rot);
        thick = d->thick; hidden = d->hidden;
    } else {
        const TesseraCardDrawPlacement* dp = NULL;
        for (size_t i = 0; next && i < next->card_draw_count; ++i)
            if (next->card_draws[i].id == draw_id) { dp = &next->card_draws[i]; break; }
        if (!dp) return false;
        pos[0] = dp->position[0]; pos[1] = dp->position[1]; pos[2] = dp->position[2];
        placement_quat(dp->orientation, rot);
        thick = draw_thickness(e, dp->def, dp->count);
        hidden = dp->top_hidden;
    }
    /* lift to the top face along the pile's local up axis */
    vec3 up = { 0.0f, thick, 0.0f };
    glm_quat_rotatev(rot, up, up);
    glm_vec3_add(pos, up, out_pos);
    glm_quat_copy(rot, out_rot);
    if (out_hidden) *out_hidden = hidden;
    return true;
}

/* Spawn a card resting on a source pile and slide/flip it to its target. Full
 * size + opacity throughout (it's lifted off the deck, not popped from nowhere).
 * Crossfades the front if the pile top and the target differ (a draw reveal).
 * When `approach` is non-NULL (dealing into a hand) the card flies through that
 * staging point first, taking twice as long, so it drops into the fan cleanly. */
static void card_spawn_from(TsCardInst* c, uint64_t id, TesseraDefId def,
                            const vec3 pos, const versor rot, bool hidden,
                            float thick, const vec3 src_pos, const versor src_rot,
                            bool src_hidden, const float* approach, float move_s) {
    card_snap(c, id, def, false, pos, rot, hidden, thick, 1);
    /* current pose = the pile top; rotation converges toward `rot` over seg 0 */
    glm_vec3_copy((float*)src_pos, c->pos); glm_quat_copy((float*)src_rot, c->rot);
    glm_quat_copy((float*)src_rot, c->from_rot); glm_quat_copy((float*)rot, c->to_rot);
    float flight = move_s;
    if (approach) {                             /* fly up over the hand, then drop */
        float path2[6] = { approach[0], approach[1], approach[2],
                           pos[0], pos[1], pos[2] };
        card_set_journey(c, path2, 2, pos, 2.0f * move_s);
        flight = 2.0f * move_s;
    } else {
        glm_vec3_copy((float*)src_pos, c->from_pos); glm_vec3_copy((float*)pos, c->to_pos);
        ts_tween_start(&c->tween, move_s, 0.0f, TS_EASE_OUT_CUBIC);
    }
    if (src_hidden != hidden) {                 /* reveal: crossfade during flight */
        c->from_mix = src_hidden ? 1.0f : 0.0f;
        c->to_mix   = hidden ? 1.0f : 0.0f;
        c->mix      = c->from_mix;
        ts_tween_start(&c->mix_tween, flight, 0.0f, TS_EASE_IN_OUT_CUBIC);
    }
}

/* Set up a card's positional journey toward `final_pos`. With >1 waypoints the
 * card tweens *through* them (each 3 floats: x,y,z), the last coinciding with
 * `final_pos`. The whole move takes `total_s`, split evenly across the steps.
 * Starts the first segment from the current pose. */
static void card_set_journey(TsCardInst* c, const float* path, uint32_t path_count,
                             const vec3 final_pos, float total_s) {
    uint32_t segs = (path && path_count > 1) ? path_count : 1;
    if (segs > TS_MAX_STEPS) segs = TS_MAX_STEPS;
    for (uint32_t k = 0; k < segs; ++k) {
        if (k == segs - 1) {
            glm_vec3_copy((float*)final_pos, c->seg_pts[k]);
        } else {
            c->seg_pts[k][0] = path[k * 3 + 0];
            c->seg_pts[k][1] = path[k * 3 + 1];
            c->seg_pts[k][2] = path[k * 3 + 2];
        }
    }
    c->seg_count = segs;
    c->seg_index = 0;
    c->seg_dur = total_s / (float)segs;
    glm_vec3_copy(c->pos, c->from_pos);
    glm_vec3_copy(c->seg_pts[0], c->to_pos);
    ts_tween_start(&c->tween, c->seg_dur, 0.0f, TS_EASE_OUT_CUBIC);
}

/* Retarget a live card toward a new target, tweening from the current pose.
 * `path`/`path_count` (free cards only) walk it through intermediate waypoints. */
static void card_retarget(TsCardInst* c, TesseraDefId def, const vec3 pos,
                          const versor rot, bool hidden, float thick, uint32_t count,
                          const float* path, uint32_t path_count,
                          const TesseraTiming* timing) {
    c->def = def;
    glm_quat_copy(c->rot, c->from_rot); glm_quat_copy((float*)rot, c->to_rot);
    c->from_scale = c->scale; c->to_scale = 1.0f;
    c->from_alpha = c->alpha; c->to_alpha = 1.0f;
    c->removing = false; c->alive = true;
    /* A real multi-step move takes twice a single move (matches entities). */
    bool multi = path && path_count > 1;
    float total = multi ? 2.0f * timing->move_s : timing->move_s;
    /* Static re-emit (already at rest at the target pose) → snap instead of
     * starting a full move_s tween that reports not-idle while nothing moves.
     * The flip/thickness crossfades below are guarded separately, so an
     * in-flight reveal still completes. */
    if (!multi && pose_settled(c->pos, c->rot, c->scale, c->alpha,
                               pos, rot, 1.0f)) {
        total = 0.0f;
    }
    card_set_journey(c, path, path_count, pos, total);

    /* Only (re)start the flip crossfade when the target state actually changed;
     * otherwise leave any in-flight flip running so it completes (restarting a
     * zero-duration tween here would freeze it at its current blended value). */
    if (hidden != c->hidden) {                     /* flip: crossfade the front */
        c->from_mix = c->mix; c->to_mix = hidden ? 1.0f : 0.0f;
        ts_tween_start(&c->mix_tween, TS_CARD_FLIP_S, 0.0f, TS_EASE_IN_OUT_CUBIC);
        c->hidden = hidden;
    }
    /* Likewise for pile thickness: only restart when the target count changed,
     * so an in-progress grow/shrink is not cut short. */
    if (thick != c->to_thick) {                    /* count changed: grow/shrink */
        c->from_thick = c->thick; c->to_thick = thick;
        ts_tween_start(&c->thick_tween, TS_CARD_THICK_S, 0.0f, TS_EASE_OUT_CUBIC);
    }
    c->count = count;
}

static void card_remove(TsCardInst* c, float remove_s) {
    glm_vec3_copy(c->pos, c->from_pos); glm_vec3_copy(c->pos, c->to_pos);
    glm_quat_copy(c->rot, c->from_rot); glm_quat_copy(c->rot, c->to_rot);
    c->from_scale = c->scale; c->to_scale = 0.0f;
    c->from_alpha = c->alpha; c->to_alpha = 0.0f;
    c->from_mix = c->to_mix = c->mix;
    c->from_thick = c->to_thick = c->thick;
    c->removing = true;
    c->seg_count = 1; c->seg_index = 0;   /* drop any in-flight multi-step path */
    ts_tween_start(&c->tween, remove_s, 0.0f, TS_EASE_IN_CUBIC);
    ts_tween_start(&c->mix_tween, 0.0f, 0.0f, TS_EASE_LINEAR);
    ts_tween_start(&c->thick_tween, 0.0f, 0.0f, TS_EASE_LINEAR);
}

/* Diff cards + card-draws (present in both single card and pile forms). */
static void card_diff(struct TsOrch* o, TesseraEngine* e, const TsSnapshot* next,
                      const TesseraTiming* timing, bool seed) {
    size_t nc  = next ? next->card_count : 0;
    size_t ncd = next ? next->card_draw_count : 0;

    if (seed) o->card_count = 0;

    /* single cards */
    for (size_t i = 0; i < nc; ++i) {
        const TesseraCardPlacement* cp = &next->cards[i];
        if (cp->id == 0) continue;
        vec3 pos; versor rot; card_target(next, i, pos, rot);
        float thick = draw_thickness(e, cp->def, 1);
        const TesseraHandPlacement* nh = find_hand(next, cp->hand);
        if (seed) {
            TsCardInst* ni = orch_add_card(o);
            card_snap(ni, cp->id, cp->def, false, pos, rot, cp->hidden, thick, 1);
            ni->hand = cp->hand;
            continue;
        }
        TsCardInst* c = orch_find_card(o, cp->id, false);
        if (c) {
            /* a hidden<->visible change on a live card starts the flip
             * crossfade below — surface it as a CARD_FLIPPED event */
            if (!c->removing && c->hidden != cp->hidden)
                ts_engine_emit_event(e, TESSERA_EVENT_CARD_FLIPPED,
                                     TESSERA_EVENT_SUBJECT_CARD, cp->id,
                                     orch_event_coord(c->pos),
                                     cp->hidden ? 1.0f : 0.0f);
            if (nh && c->hand != cp->hand) {
                /* Entering a hand: fly up in front of it, then settle into the
                 * slot, so the card doesn't slice through the ones already
                 * fanned out. Two waypoints: staging point, then the fan slot. */
                vec3 wp; hand_approach_target(nh, pos, wp);
                float path2[6] = { wp[0], wp[1], wp[2], pos[0], pos[1], pos[2] };
                card_retarget(c, cp->def, pos, rot, cp->hidden, thick, 1, path2, 2, timing);
            } else {
                /* A hand card is placed by the fan, so its path (if any) is ignored. */
                const float* cpath = (cp->hand == 0) ? cp->path : NULL;
                uint32_t cpc = (cp->hand == 0) ? cp->path_count : 0;
                card_retarget(c, cp->def, pos, rot, cp->hidden, thick, 1, cpath, cpc, timing);
            }
            c->hand = cp->hand;
            continue;
        }
        /* New card: deal it from its source pile if that pile is present. */
        TsCardInst* ni = orch_add_card(o);
        vec3 src_pos; versor src_rot; bool src_hidden;
        if (cp->source_draw != 0 &&
            draw_top_pose(o, e, next, cp->source_draw, src_pos, src_rot, &src_hidden)) {
            /* Dealing straight into a hand: stage above the fan before dropping. */
            vec3 wp; const float* wpp = NULL;
            if (nh) { hand_approach_target(nh, pos, wp); wpp = wp; }
            card_spawn_from(ni, cp->id, cp->def, pos, rot, cp->hidden,
                            thick, src_pos, src_rot, src_hidden, wpp, timing->move_s);
            ts_engine_emit_event(e, TESSERA_EVENT_CARD_DEALT,
                                 TESSERA_EVENT_SUBJECT_CARD, cp->id,
                                 orch_event_coord(src_pos), 0.0f);
        } else {
            card_spawn(ni, cp->id, cp->def, false, pos, rot,
                       cp->hidden, thick, 1, timing->add_s);
        }
        ni->hand = cp->hand;
    }

    /* card piles (draws) */
    for (size_t i = 0; i < ncd; ++i) {
        const TesseraCardDrawPlacement* dp = &next->card_draws[i];
        if (dp->id == 0) continue;
        vec3 pos = { dp->position[0], dp->position[1], dp->position[2] };
        versor rot; placement_quat(dp->orientation, rot);
        float thick = draw_thickness(e, dp->def, dp->count);
        if (seed) {
            card_snap(orch_add_card(o), dp->id, dp->def, true, pos, rot,
                      dp->top_hidden, thick, dp->count);
            continue;
        }
        TsCardInst* c = orch_find_card(o, dp->id, true);
        if (c) {
            if (!c->removing && c->hidden != dp->top_hidden)
                ts_engine_emit_event(e, TESSERA_EVENT_CARD_FLIPPED,
                                     TESSERA_EVENT_SUBJECT_DRAW, dp->id,
                                     orch_event_coord(c->pos),
                                     dp->top_hidden ? 1.0f : 0.0f);
            card_retarget(c, dp->def, pos, rot, dp->top_hidden, thick, dp->count, NULL, 0, timing);
        } else {
            card_spawn(orch_add_card(o), dp->id, dp->def, true, pos, rot,
                       dp->top_hidden, thick, dp->count, timing->add_s);
        }
    }

    if (seed) return;

    /* live cards/piles absent from next: begin removal */
    for (size_t i = 0; i < o->card_count; ++i) {
        TsCardInst* c = &o->cards[i];
        if (c->removing) continue;
        bool present = false;
        if (c->is_draw) {
            for (size_t j = 0; j < ncd; ++j)
                if (next->card_draws[j].id == c->id) { present = true; break; }
        } else {
            for (size_t j = 0; j < nc; ++j)
                if (next->cards[j].id == c->id) { present = true; break; }
        }
        if (!present) card_remove(c, timing->remove_s);
    }
}

/* ============================================================ overlays */
/* Effective overlay tint (all-zero => white, matching tiles). */
static void overlay_tint(const TesseraOverlayPlacement* op, float out[4]) {
    if (op->tint[0] == 0.0f && op->tint[1] == 0.0f &&
        op->tint[2] == 0.0f && op->tint[3] == 0.0f) {
        out[0] = out[1] = out[2] = out[3] = 1.0f;
        return;
    }
    memcpy(out, op->tint, 4 * sizeof(float));
}

/* Copy the placement's visual + pulse spec onto the instance (the pulse clock
 * keeps running so a re-emit doesn't restart the breathe mid-cycle). */
static void overlay_set_spec(TsOverlayInst* v, const TesseraOverlayPlacement* op) {
    v->coord = op->coord;
    v->shape = op->shape;
    v->atlas = op->atlas;
    v->uv    = op->uv;
    v->pulse_s         = op->pulse_s;
    v->pulse_alpha_min = op->pulse_alpha_min;
    v->pulse_alpha_max = op->pulse_alpha_max;
    v->pulse_scale_min = op->pulse_scale_min;
    v->pulse_scale_max = op->pulse_scale_max;
}

static void overlay_snap(TsOverlayInst* v, const TesseraOverlayPlacement* op) {
    overlay_set_spec(v, op);
    float t[4]; overlay_tint(op, t);
    for (int k = 0; k < 4; ++k) v->from_tint[k] = v->to_tint[k] = v->tint[k] = t[k];
    v->from_alpha = v->to_alpha = v->alpha = 1.0f;
    v->pulse_t = 0.0f;
    v->removing = false;
    v->alive = true;
    ts_tween_start(&v->tween, 0.0f, 0.0f, TS_EASE_LINEAR);
}

static void overlay_spawn(TsOverlayInst* v, const TesseraOverlayPlacement* op,
                          float fade_s) {
    overlay_snap(v, op);
    v->from_alpha = 0.0f;
    v->alpha = 0.0f;
    ts_tween_start(&v->tween, fade_s, 0.0f, TS_EASE_OUT_CUBIC);
}

/* Retarget from the current interpolated tint/alpha (crossfade). An unchanged,
 * settled re-emit snaps (zero duration) so it doesn't hold the engine busy;
 * shape/atlas/uv swap immediately (only tint + fade animate). */
static void overlay_retarget(TsOverlayInst* v, const TesseraOverlayPlacement* op,
                             float fade_s) {
    float t[4]; overlay_tint(op, t);
    bool settled = !v->removing && fabsf(v->alpha - 1.0f) < 1e-3f;
    for (int k = 0; k < 4 && settled; ++k)
        if (fabsf(v->tint[k] - t[k]) > 1e-3f) settled = false;
    overlay_set_spec(v, op);
    for (int k = 0; k < 4; ++k) { v->from_tint[k] = v->tint[k]; v->to_tint[k] = t[k]; }
    v->from_alpha = v->alpha;
    v->to_alpha = 1.0f;
    v->removing = false;
    v->alive = true;
    ts_tween_start(&v->tween, settled ? 0.0f : fade_s, 0.0f, TS_EASE_OUT_CUBIC);
}

static void overlay_remove(TsOverlayInst* v, float fade_s) {
    for (int k = 0; k < 4; ++k) { v->from_tint[k] = v->tint[k]; v->to_tint[k] = v->tint[k]; }
    v->from_alpha = v->alpha;
    v->to_alpha = 0.0f;
    v->removing = true;
    ts_tween_start(&v->tween, fade_s, 0.0f, TS_EASE_IN_CUBIC);
}

/* Diff overlays by coord: added fade in, removed fade out, changed tint
 * crossfades from the current interpolated value. */
static void overlay_diff(struct TsOrch* o, const TsSnapshot* next,
                         const TesseraTiming* timing, bool seed) {
    size_t n = next ? next->overlay_count : 0;

    if (seed) o->overlay_count = 0;

    for (size_t i = 0; i < n; ++i) {
        const TesseraOverlayPlacement* op = &next->overlays[i];
        if (seed) { overlay_snap(orch_add_overlay(o), op); continue; }
        TsOverlayInst* v = orch_find_overlay(o, op->coord);
        if (v) overlay_retarget(v, op, timing->tile_s);
        else   overlay_spawn(orch_add_overlay(o), op, timing->tile_s);
    }

    if (seed) return;

    /* live overlays absent from next: begin fade-out */
    for (size_t i = 0; i < o->overlay_count; ++i) {
        TsOverlayInst* v = &o->overlays[i];
        if (v->removing) continue;
        bool present = false;
        for (size_t j = 0; j < n; ++j)
            if (next->overlays[j].coord.x == v->coord.x &&
                next->overlays[j].coord.y == v->coord.y) { present = true; break; }
        if (!present) overlay_remove(v, timing->tile_s);
    }
}

/* ============================================================ labels */
/* Effective label color (all-zero => white, matching tiles/overlays). */
static void label_color(const TesseraLabelPlacement* lp, float out[4]) {
    if (lp->color[0] == 0.0f && lp->color[1] == 0.0f &&
        lp->color[2] == 0.0f && lp->color[3] == 0.0f) {
        out[0] = out[1] = out[2] = out[3] = 1.0f;
        return;
    }
    memcpy(out, lp->color, 4 * sizeof(float));
}

/* Copy the placement's non-animated spec onto the instance. */
static void label_set_spec(TsLabelInst* v, const TesseraLabelPlacement* lp) {
    v->id        = lp->id;
    v->font      = lp->font;
    v->anchor    = lp->anchor;
    v->anchor_id = lp->anchor_id;
    v->billboard = lp->billboard;
    v->size      = lp->size > 0.0f ? lp->size : 0.5f;
}

static void label_snap(TsLabelInst* v, const TesseraLabelPlacement* lp) {
    label_set_spec(v, lp);
    memcpy(v->text, lp->text, TESSERA_LABEL_TEXT_CAP);
    v->text[TESSERA_LABEL_TEXT_CAP - 1] = 0;
    v->prev_text[0] = 0;
    v->text_mix = 1.0f;
    float c[4]; label_color(lp, c);
    for (int k = 0; k < 4; ++k) v->from_color[k] = v->to_color[k] = v->color[k] = c[k];
    vec3 off = { lp->position[0], lp->position[1], lp->position[2] };
    glm_vec3_copy(off, v->from_off);
    glm_vec3_copy(off, v->to_off);
    glm_vec3_copy(off, v->off);
    v->from_alpha = v->to_alpha = v->alpha = 1.0f;
    v->removing = false;
    v->alive = true;
    ts_tween_start(&v->tween, 0.0f, 0.0f, TS_EASE_LINEAR);
    ts_tween_start(&v->text_tween, 0.0f, 0.0f, TS_EASE_LINEAR);
}

static void label_spawn(TsLabelInst* v, const TesseraLabelPlacement* lp, float fade_s) {
    label_snap(v, lp);
    v->from_alpha = 0.0f;
    v->alpha = 0.0f;
    ts_tween_start(&v->tween, fade_s, 0.0f, TS_EASE_OUT_CUBIC);
}

/* Retarget from the current interpolated offset/color/alpha; a text change
 * crossfades from the currently shown string. An unchanged, settled re-emit
 * snaps (zero duration) so it never holds the engine busy. */
static void label_retarget(TsLabelInst* v, const TesseraLabelPlacement* lp, float fade_s) {
    float c[4]; label_color(lp, c);
    vec3 off = { lp->position[0], lp->position[1], lp->position[2] };
    bool text_changed = strncmp(v->text, lp->text, TESSERA_LABEL_TEXT_CAP - 1) != 0;

    bool settled = !v->removing && fabsf(v->alpha - 1.0f) < 1e-3f &&
                   ts_tween_done(&v->text_tween) && !text_changed;
    for (int k = 0; k < 4 && settled; ++k)
        if (fabsf(v->color[k] - c[k]) > 1e-3f) settled = false;
    if (settled) {
        vec3 d;
        glm_vec3_sub(off, v->off, d);
        if (glm_vec3_norm2(d) > 1e-6f) settled = false;
    }

    label_set_spec(v, lp);
    if (text_changed) {
        /* crossfade from what is currently displayed */
        memcpy(v->prev_text, v->text, TESSERA_LABEL_TEXT_CAP);
        memcpy(v->text, lp->text, TESSERA_LABEL_TEXT_CAP);
        v->text[TESSERA_LABEL_TEXT_CAP - 1] = 0;
        v->text_mix = 0.0f;
        ts_tween_start(&v->text_tween, fade_s, 0.0f, TS_EASE_IN_OUT_CUBIC);
    }
    for (int k = 0; k < 4; ++k) { v->from_color[k] = v->color[k]; v->to_color[k] = c[k]; }
    glm_vec3_copy(v->off, v->from_off);
    glm_vec3_copy(off, v->to_off);
    v->from_alpha = v->alpha;
    v->to_alpha = 1.0f;
    v->removing = false;
    v->alive = true;
    ts_tween_start(&v->tween, settled ? 0.0f : fade_s, 0.0f, TS_EASE_OUT_CUBIC);
}

static void label_remove(TsLabelInst* v, float fade_s) {
    for (int k = 0; k < 4; ++k) { v->from_color[k] = v->color[k]; v->to_color[k] = v->color[k]; }
    glm_vec3_copy(v->off, v->from_off);
    glm_vec3_copy(v->off, v->to_off);
    v->from_alpha = v->alpha;
    v->to_alpha = 0.0f;
    v->removing = true;
    ts_tween_start(&v->tween, fade_s, 0.0f, TS_EASE_IN_CUBIC);
}

/* Diff labels by id: added fade in, removed fade out, text/color/offset
 * changes crossfade/retween from the current interpolated values. */
static void label_diff(struct TsOrch* o, const TsSnapshot* next,
                       const TesseraTiming* timing, bool seed) {
    size_t n = next ? next->label_count : 0;

    if (seed) o->label_count = 0;

    for (size_t i = 0; i < n; ++i) {
        const TesseraLabelPlacement* lp = &next->labels[i];
        if (lp->id == 0 || lp->font == 0) continue;
        if (seed) { label_snap(orch_add_label(o), lp); continue; }
        TsLabelInst* v = orch_find_label(o, lp->id);
        if (v) label_retarget(v, lp, timing->tile_s);
        else   label_spawn(orch_add_label(o), lp, timing->add_s);
    }

    if (seed) return;

    /* live labels absent from next: begin fade-out */
    for (size_t i = 0; i < o->label_count; ++i) {
        TsLabelInst* v = &o->labels[i];
        if (v->removing) continue;
        bool present = false;
        for (size_t j = 0; j < n; ++j)
            if (next->labels[j].id == v->id &&
                next->labels[j].font != 0) { present = true; break; }
        if (!present) label_remove(v, timing->remove_s);
    }
}

/* ============================================================ point lights */
static TsPointLightInst* orch_add_plight(struct TsOrch* o) {
    if (o->point_light_count == o->point_light_cap) {
        size_t nc = o->point_light_cap ? o->point_light_cap * 2 : 8;
        o->point_lights =
            (TsPointLightInst*)realloc(o->point_lights, nc * sizeof(TsPointLightInst));
        o->point_light_cap = nc;
    }
    TsPointLightInst* v = &o->point_lights[o->point_light_count++];
    memset(v, 0, sizeof *v);
    return v;
}

static TsPointLightInst* orch_find_plight(struct TsOrch* o, TesseraPointLightId id) {
    for (size_t i = 0; i < o->point_light_count; ++i)
        if (o->point_lights[i].id == id) return &o->point_lights[i];
    return NULL;
}

static void plight_targets(TsPointLightInst* v, const TesseraPointLightPlacement* p) {
    glm_vec3_copy((float*)p->position, v->to_pos);
    memcpy(v->to_color, p->color, 3 * sizeof(float));
    v->to_intensity = p->intensity;
    v->to_radius = p->radius > 0.0f ? p->radius : TS_PLIGHT_DEFAULT_RADIUS;
}

static void plight_snap(TsPointLightInst* v, const TesseraPointLightPlacement* p) {
    v->id = p->id;
    plight_targets(v, p);
    glm_vec3_copy(v->to_pos, v->from_pos);
    glm_vec3_copy(v->to_pos, v->pos);
    memcpy(v->from_color, v->to_color, 3 * sizeof(float));
    memcpy(v->color, v->to_color, 3 * sizeof(float));
    v->from_intensity = v->intensity = v->to_intensity;
    v->from_radius = v->radius = v->to_radius;
    v->removing = false;
    v->alive = true;
    ts_tween_start(&v->tween, 0.0f, 0.0f, TS_EASE_LINEAR);
}

static void plight_spawn(TsPointLightInst* v, const TesseraPointLightPlacement* p,
                         float fade_s) {
    plight_snap(v, p);
    v->from_intensity = 0.0f;   /* fade the light up from dark */
    v->intensity = 0.0f;
    ts_tween_start(&v->tween, fade_s, 0.0f, TS_EASE_OUT_CUBIC);
}

/* Retarget from the current interpolated values; an unchanged, settled
 * re-emit snaps (zero duration) so it never holds the engine busy. */
static void plight_retarget(TsPointLightInst* v, const TesseraPointLightPlacement* p,
                            float move_s) {
    float to_r = p->radius > 0.0f ? p->radius : TS_PLIGHT_DEFAULT_RADIUS;
    bool settled = !v->removing && ts_tween_done(&v->tween) &&
        glm_vec3_distance((float*)p->position, v->pos) < 1e-4f &&
        fabsf(v->intensity - p->intensity) < 1e-4f &&
        fabsf(v->radius - to_r) < 1e-4f &&
        fabsf(v->color[0] - p->color[0]) < 1e-4f &&
        fabsf(v->color[1] - p->color[1]) < 1e-4f &&
        fabsf(v->color[2] - p->color[2]) < 1e-4f;
    glm_vec3_copy(v->pos, v->from_pos);
    memcpy(v->from_color, v->color, 3 * sizeof(float));
    v->from_intensity = v->intensity;
    v->from_radius = v->radius;
    plight_targets(v, p);
    v->removing = false;
    ts_tween_start(&v->tween, settled ? 0.0f : move_s, 0.0f, TS_EASE_IN_OUT_CUBIC);
}

static void plight_remove(TsPointLightInst* v, float fade_s) {
    glm_vec3_copy(v->pos, v->from_pos);
    glm_vec3_copy(v->pos, v->to_pos);
    memcpy(v->from_color, v->color, 3 * sizeof(float));
    memcpy(v->to_color, v->color, 3 * sizeof(float));
    v->from_intensity = v->intensity;
    v->to_intensity = 0.0f;      /* dim to dark, then cull */
    v->from_radius = v->to_radius = v->radius;
    v->removing = true;
    ts_tween_start(&v->tween, fade_s, 0.0f, TS_EASE_IN_CUBIC);
}

static void plight_diff(struct TsOrch* o, const TsSnapshot* next,
                        const TesseraTiming* timing, bool seed) {
    size_t n = next ? next->point_light_count : 0;

    if (seed) o->point_light_count = 0;

    for (size_t i = 0; i < n; ++i) {
        const TesseraPointLightPlacement* p = &next->point_lights[i];
        if (p->id == 0) continue;
        if (seed) { plight_snap(orch_add_plight(o), p); continue; }
        TsPointLightInst* v = orch_find_plight(o, p->id);
        if (v) plight_retarget(v, p, timing->move_s);
        else   plight_spawn(orch_add_plight(o), p, timing->add_s);
    }

    if (seed) return;

    /* live lights absent from next: dim out */
    for (size_t i = 0; i < o->point_light_count; ++i) {
        TsPointLightInst* v = &o->point_lights[i];
        if (v->removing) continue;
        bool present = false;
        for (size_t j = 0; j < n; ++j)
            if (next->point_lights[j].id == v->id) { present = true; break; }
        if (!present) plight_remove(v, timing->remove_s);
    }
}

/* ============================================================ world models */
static TsWorldModelInst* orch_add_wmodel(struct TsOrch* o) {
    if (o->world_model_count == o->world_model_cap) {
        size_t nc = o->world_model_cap ? o->world_model_cap * 2 : 8;
        o->world_models =
            (TsWorldModelInst*)realloc(o->world_models, nc * sizeof(TsWorldModelInst));
        o->world_model_cap = nc;
    }
    TsWorldModelInst* v = &o->world_models[o->world_model_count++];
    memset(v, 0, sizeof *v);
    return v;
}

static TsWorldModelInst* orch_find_wmodel(struct TsOrch* o, TesseraWorldModelId id) {
    for (size_t i = 0; i < o->world_model_count; ++i)
        if (o->world_models[i].id == id) return &o->world_models[i];
    return NULL;
}

/* Placement -> target transform: position is world units with y biased so the
 * origin plane sits just below the tiles (see TS_WORLD_MODEL_BASE_Y). */
static void wmodel_targets(TsWorldModelInst* v, const TesseraWorldModelPlacement* p) {
    v->to_pos[0] = p->position[0];
    v->to_pos[1] = p->position[1] + TS_WORLD_MODEL_BASE_Y;
    v->to_pos[2] = p->position[2];
    placement_quat(p->orientation, v->to_rot);
    v->to_scale = p->scale > 0.0f ? p->scale : 1.0f;
    v->to_alpha = 1.0f;
}

static void wmodel_snap(TsWorldModelInst* v, const TesseraWorldModelPlacement* p) {
    v->id = p->id;
    v->def = p->def;
    wmodel_targets(v, p);
    glm_vec3_copy(v->to_pos, v->from_pos);
    glm_vec3_copy(v->to_pos, v->pos);
    glm_quat_copy(v->to_rot, v->from_rot);
    glm_quat_copy(v->to_rot, v->rot);
    v->from_scale = v->scale = v->to_scale;
    v->from_alpha = v->alpha = v->to_alpha;
    v->removing = false;
    v->alive = true;
    ts_tween_start(&v->tween, 0.0f, 0.0f, TS_EASE_LINEAR);
}

static void wmodel_spawn(TsWorldModelInst* v, const TesseraWorldModelPlacement* p,
                         float add_s) {
    wmodel_snap(v, p);
    v->from_scale = 0.0f;   /* grow in, like an entity spawn */
    v->from_alpha = 0.0f;
    v->scale = 0.0f;
    v->alpha = 0.0f;
    ts_tween_start(&v->tween, add_s, 0.0f, TS_EASE_OUT_CUBIC);
}

static void wmodel_retarget(TsWorldModelInst* v, const TesseraWorldModelPlacement* p,
                            float move_s) {
    vec3 tp = { p->position[0], p->position[1] + TS_WORLD_MODEL_BASE_Y, p->position[2] };
    versor tq; placement_quat(p->orientation, tq);
    float ts = p->scale > 0.0f ? p->scale : 1.0f;
    bool settled = !v->removing && ts_tween_done(&v->tween) &&
        glm_vec3_distance(tp, v->pos) < 1e-4f &&
        fabsf(glm_quat_dot(tq, v->rot)) > 1.0f - 1e-5f &&
        fabsf(v->scale - ts) < 1e-4f && fabsf(v->alpha - 1.0f) < 1e-3f;
    glm_vec3_copy(v->pos, v->from_pos);
    glm_quat_copy(v->rot, v->from_rot);
    v->from_scale = v->scale;
    v->from_alpha = v->alpha;
    v->def = p->def;
    wmodel_targets(v, p);
    v->removing = false;
    ts_tween_start(&v->tween, settled ? 0.0f : move_s, 0.0f, TS_EASE_IN_OUT_CUBIC);
}

static void wmodel_remove(TsWorldModelInst* v, float remove_s) {
    glm_vec3_copy(v->pos, v->from_pos);
    glm_vec3_copy(v->pos, v->to_pos);
    glm_quat_copy(v->rot, v->from_rot);
    glm_quat_copy(v->rot, v->to_rot);
    v->from_scale = v->scale; v->to_scale = 0.0f;
    v->from_alpha = v->alpha; v->to_alpha = 0.0f;
    v->removing = true;
    ts_tween_start(&v->tween, remove_s, 0.0f, TS_EASE_IN_CUBIC);
}

static void wmodel_diff(struct TsOrch* o, const TsSnapshot* next,
                        const TesseraTiming* timing, bool seed) {
    size_t n = next ? next->world_model_count : 0;

    if (seed) o->world_model_count = 0;

    for (size_t i = 0; i < n; ++i) {
        const TesseraWorldModelPlacement* p = &next->world_models[i];
        if (p->id == 0 || p->def == 0) continue;
        if (seed) { wmodel_snap(orch_add_wmodel(o), p); continue; }
        TsWorldModelInst* v = orch_find_wmodel(o, p->id);
        if (v) wmodel_retarget(v, p, timing->move_s);
        else   wmodel_spawn(orch_add_wmodel(o), p, timing->add_s);
    }

    if (seed) return;

    /* live models absent from next: shrink out */
    for (size_t i = 0; i < o->world_model_count; ++i) {
        TsWorldModelInst* v = &o->world_models[i];
        if (v->removing) continue;
        bool present = false;
        for (size_t j = 0; j < n; ++j)
            if (next->world_models[j].id == v->id &&
                next->world_models[j].def != 0) { present = true; break; }
        if (!present) wmodel_remove(v, timing->remove_s);
    }
}

/* ============================================================ highlights */
/* Effective highlight color (all-zero => white, matching overlays/labels). */
static void highlight_color(const TesseraHighlightPlacement* hp, float out[4]) {
    if (hp->color[0] == 0.0f && hp->color[1] == 0.0f &&
        hp->color[2] == 0.0f && hp->color[3] == 0.0f) {
        out[0] = out[1] = out[2] = out[3] = 1.0f;
        return;
    }
    memcpy(out, hp->color, 4 * sizeof(float));
}

/* Copy the placement's non-animated spec onto the instance (the pulse clock
 * keeps running so a re-emit doesn't restart the breathe mid-cycle). */
static void highlight_set_spec(TsHighlightInst* v, const TesseraHighlightPlacement* hp) {
    v->kind      = hp->kind;
    v->target_id = hp->target_id;
    v->style     = hp->style;
    v->thickness = hp->thickness;
    v->pulse_s   = hp->pulse_s;
    v->pulse_min = hp->pulse_min;
    v->pulse_max = hp->pulse_max;
}

static void highlight_snap(TsHighlightInst* v, const TesseraHighlightPlacement* hp) {
    highlight_set_spec(v, hp);
    float c[4]; highlight_color(hp, c);
    for (int k = 0; k < 4; ++k) v->from_color[k] = v->to_color[k] = v->color[k] = c[k];
    v->from_alpha = v->to_alpha = v->alpha = 1.0f;
    v->pulse_t = 0.0f;
    v->removing = false;
    v->alive = true;
    ts_tween_start(&v->tween, 0.0f, 0.0f, TS_EASE_LINEAR);
}

static void highlight_spawn(TsHighlightInst* v, const TesseraHighlightPlacement* hp,
                            float fade_s) {
    highlight_snap(v, hp);
    v->from_alpha = 0.0f;
    v->alpha = 0.0f;
    ts_tween_start(&v->tween, fade_s, 0.0f, TS_EASE_OUT_CUBIC);
}

/* Retarget from the current interpolated color/alpha (crossfade). An unchanged,
 * settled re-emit snaps (zero duration) so it doesn't hold the engine busy;
 * style/thickness swap immediately (only color + fade animate). */
static void highlight_retarget(TsHighlightInst* v, const TesseraHighlightPlacement* hp,
                               float fade_s) {
    float c[4]; highlight_color(hp, c);
    bool settled = !v->removing && fabsf(v->alpha - 1.0f) < 1e-3f;
    for (int k = 0; k < 4 && settled; ++k)
        if (fabsf(v->color[k] - c[k]) > 1e-3f) settled = false;
    highlight_set_spec(v, hp);
    for (int k = 0; k < 4; ++k) { v->from_color[k] = v->color[k]; v->to_color[k] = c[k]; }
    v->from_alpha = v->alpha;
    v->to_alpha = 1.0f;
    v->removing = false;
    v->alive = true;
    ts_tween_start(&v->tween, settled ? 0.0f : fade_s, 0.0f, TS_EASE_OUT_CUBIC);
}

static void highlight_remove(TsHighlightInst* v, float fade_s) {
    for (int k = 0; k < 4; ++k) { v->from_color[k] = v->color[k]; v->to_color[k] = v->color[k]; }
    v->from_alpha = v->alpha;
    v->to_alpha = 0.0f;
    v->removing = true;
    ts_tween_start(&v->tween, fade_s, 0.0f, TS_EASE_IN_CUBIC);
}

/* Diff highlights by (kind, target id): added fade in, removed fade out,
 * changed color crossfades from the current interpolated value. */
static void highlight_diff(struct TsOrch* o, const TsSnapshot* next,
                           const TesseraTiming* timing, bool seed) {
    size_t n = next ? next->highlight_count : 0;

    if (seed) o->highlight_count = 0;

    for (size_t i = 0; i < n; ++i) {
        const TesseraHighlightPlacement* hp = &next->highlights[i];
        if (hp->target_id == 0) continue;
        if (seed) { highlight_snap(orch_add_highlight(o), hp); continue; }
        TsHighlightInst* v = orch_find_highlight(o, hp->kind, hp->target_id);
        if (v) highlight_retarget(v, hp, timing->tile_s);
        else   highlight_spawn(orch_add_highlight(o), hp, timing->tile_s);
    }

    if (seed) return;

    /* live highlights absent from next: begin fade-out */
    for (size_t i = 0; i < o->highlight_count; ++i) {
        TsHighlightInst* v = &o->highlights[i];
        if (v->removing) continue;
        bool present = false;
        for (size_t j = 0; j < n; ++j)
            if (next->highlights[j].kind == v->kind &&
                next->highlights[j].target_id == v->target_id) { present = true; break; }
        if (!present) highlight_remove(v, timing->tile_s);
    }
}

/* --------------------------------------------------------- on_promote */
void ts_orch_on_promote(struct TsOrch* o, TesseraEngine* e, const TsSnapshot* prev,
                        const TsSnapshot* next, const TesseraTiming* timing) {
    if (!next) return;

    bool seed = (prev == NULL) || !o->seeded;

    size_t n = next->entity_count;
    TsTarget* targets = NULL;
    if (n > 0) {
        targets = (TsTarget*)malloc(n * sizeof(TsTarget));
        if (!targets) return;
        for (size_t i = 0; i < n; ++i) {
            glm_vec3_zero(targets[i].pos);
            glm_quat_identity(targets[i].rot);
            targets[i].scale = 1.0f;
            targets[i].alpha = 1.0f;
        }
        compute_targets(next, targets);
    }

    if (seed) {
        /* SNAP: rebuild all live instances at their targets, tweens complete. */
        o->entity_count = 0;
        o->tile_count = 0;
        for (size_t i = 0; i < n; ++i) {
            TsEntityInst* inst = orch_add_entity(o);
            entity_snap(inst, &next->entities[i], &targets[i]);
            setup_anim(inst, e, &next->entities[i], false, true);
        }
        for (size_t i = 0; i < next->tile_count; ++i) {
            const TesseraTilePlacement* tp = &next->tiles[i];
            if (tp->tile_def == 0) continue;
            TsTileInst* t = orch_add_tile(o);
            t->coord = tp->coord;
            t->id = tp->id;
            t->def = tp->tile_def;
            t->variant = tp->variant;
            t->from_y = t->to_y = 0.0f;
            t->from_alpha = t->to_alpha = 1.0f;
            t->removing = false;
            t->alive = true;
            ts_tween_start(&t->tween, 0.0f, 0.0f, TS_EASE_LINEAR);
        }
        card_diff(o, e, next, timing, true);
        overlay_diff(o, next, timing, true);
        label_diff(o, next, timing, true);
        highlight_diff(o, next, timing, true);
        plight_diff(o, next, timing, true);
        wmodel_diff(o, next, timing, true);
        o->seeded = true;
        free(targets);
        return;
    }

    /* -------- animated diff -------- */
    /* Entities present in next: retarget existing or spawn new. */
    for (size_t i = 0; i < n; ++i) {
        const TesseraEntityPlacement* ep = &next->entities[i];
        TsEntityInst* inst = orch_find_entity(o, ep->id);
        if (inst) {
            TesseraCoord oldc;
            bool changed;
            if (snapshot_entity_coord(prev, ep->id, &oldc)) {
                changed = (oldc.x != ep->coord.x) || (oldc.y != ep->coord.y);
            } else {
                /* not in prev: fall back to a positional heuristic */
                float dx = targets[i].pos[0] - inst->pos[0];
                float dz = targets[i].pos[2] - inst->pos[2];
                changed = (dx * dx + dz * dz) > (0.5f * TS_TILE_SIZE) * (0.5f * TS_TILE_SIZE);
            }
            entity_retarget(inst, ep, &targets[i], changed, timing);
            setup_anim(inst, e, ep, changed, false);
        } else {
            TsEntityInst* ni = orch_add_entity(o);
            entity_spawn(ni, ep, &targets[i], timing->add_s);
            setup_anim(ni, e, ep, false, true);
        }
    }

    /* Entities live but absent from next: begin removal (unless already so). */
    for (size_t i = 0; i < o->entity_count; ++i) {
        TsEntityInst* inst = &o->entities[i];
        if (inst->removing) continue;
        if (!snapshot_has_entity(next, inst->id))
            entity_remove(inst, timing->remove_s);
    }

    /* Tiles present in next: ensure + retarget. */
    for (size_t i = 0; i < next->tile_count; ++i) {
        const TesseraTilePlacement* tp = &next->tiles[i];
        if (tp->tile_def == 0) continue;
        TsTileInst* t = orch_find_tile(o, tp->coord);
        if (!t) {
            t = orch_add_tile(o);
            t->coord = tp->coord;
            t->id = tp->id;
            t->def = tp->tile_def;
            t->variant = tp->variant;
            t->from_y = -TS_TILE_RISE;
            t->to_y = 0.0f;
            t->from_alpha = 0.0f;
            t->to_alpha = 1.0f;
            t->removing = false;
            t->alive = true;
            ts_tween_start(&t->tween, timing->tile_s, 0.0f, TS_EASE_OUT_CUBIC);
        } else if (t->removing || t->def != tp->tile_def || t->variant != tp->variant) {
            /* changed def/variant, or resurrecting a sinking tile: small pop */
            t->from_y = tile_cur_y(t);
            t->from_alpha = tile_cur_alpha(t);
            t->id = tp->id;
            t->def = tp->tile_def;
            t->variant = tp->variant;
            t->to_y = 0.0f;
            t->to_alpha = 1.0f;
            t->removing = false;
            ts_tween_start(&t->tween, timing->tile_s, 0.0f, TS_EASE_OUT_CUBIC);
        }
    }

    /* Tiles live but absent from next: begin sink-out removal. */
    for (size_t i = 0; i < o->tile_count; ++i) {
        TsTileInst* t = &o->tiles[i];
        if (t->removing) continue;
        if (!snapshot_has_tile(next, t->coord)) {
            t->from_y = tile_cur_y(t);
            t->from_alpha = tile_cur_alpha(t);
            t->to_y = -TS_TILE_RISE;
            t->to_alpha = 0.0f;
            t->removing = true;
            ts_tween_start(&t->tween, timing->tile_s, 0.0f, TS_EASE_IN_CUBIC);
        }
    }

    card_diff(o, e, next, timing, false);
    overlay_diff(o, next, timing, false);
    label_diff(o, next, timing, false);
    highlight_diff(o, next, timing, false);
    plight_diff(o, next, timing, false);
    wmodel_diff(o, next, timing, false);

    o->seeded = true;
    free(targets);
}

/* Advance the per-entity animation clock + crossfade (M5). dt already scaled. */
static void advance_anim(TsEntityInst* inst, float dt) {
    int32_t desired = (inst->anim_moving && inst->move_anim >= 0)
                      ? inst->move_anim : inst->base_anim;
    if (desired != inst->cur_clip) {
        if (inst->cur_clip >= 0) {
            inst->blend_clip = inst->cur_clip;
            inst->blend_from_time = inst->clip_time;
            inst->blend_t = 0.0f;
            inst->blend_dur = 0.18f;
        } else {
            inst->blend_clip = -1;
            inst->blend_t = 1.0f;
        }
        inst->cur_clip = desired;
        inst->clip_time = 0.0f;
    }
    inst->clip_time += dt;
    if (inst->blend_clip >= 0) {
        inst->blend_from_time += dt;
        if (inst->blend_dur > 0.0f) inst->blend_t += dt / inst->blend_dur;
        if (inst->blend_t >= 1.0f) { inst->blend_t = 1.0f; inst->blend_clip = -1; }
    }
}

/* ----------------------------------------------------------- advance */
void ts_orch_advance(struct TsOrch* o, TesseraEngine* e, float dt) {
    /* entities */
    for (size_t i = 0; i < o->entity_count;) {
        TsEntityInst* inst = &o->entities[i];
        ts_tween_advance(&inst->tween, dt);
        /* Multi-step: when a segment finishes and more remain, hand the leftover
         * time to the next segment so the walk stays smooth and exactly on time. */
        while (ts_tween_done(&inst->tween) && inst->seg_index + 1 < inst->seg_count) {
            float over = inst->tween.elapsed - (inst->tween.delay + inst->tween.duration);
            inst->seg_index++;
            glm_vec3_copy(inst->to_pos, inst->from_pos);
            glm_vec3_copy(inst->seg_pts[inst->seg_index], inst->to_pos);
            glm_quat_copy(inst->to_rot, inst->from_rot);   /* rot already converged */
            inst->from_scale = inst->to_scale;
            inst->from_alpha = inst->to_alpha;
            ts_tween_start(&inst->tween, inst->seg_dur, 0.0f, TS_EASE_OUT_CUBIC);
            if (over > 0.0f) ts_tween_advance(&inst->tween, over);
            /* segment handoff: the walk just touched down on a waypoint
             * (from_pos now holds the reached point) */
            if (e) {
                TesseraCoord wc = orch_event_coord(inst->from_pos);
                ts_engine_emit_event(e, TESSERA_EVENT_ENTITY_WAYPOINT_REACHED,
                                     TESSERA_EVENT_SUBJECT_ENTITY, inst->id,
                                     wc, (float)inst->seg_index);
                if (inst->arc)
                    ts_engine_emit_event(e, TESSERA_EVENT_ENTITY_HOP_LANDED,
                                         TESSERA_EVENT_SUBJECT_ENTITY, inst->id,
                                         wc, 0.0f);
            }
        }
        float p = ts_tween_value01(&inst->tween);

        glm_vec3_lerp(inst->from_pos, inst->to_pos, p, inst->pos);
        if (inst->arc) inst->pos[1] += TS_HOP_HEIGHT * ts_arc(p);
        glm_quat_slerp(inst->from_rot, inst->to_rot, p, inst->rot);
        inst->scale = ts_lerpf(inst->from_scale, inst->to_scale, p);
        inst->alpha = ts_lerpf(inst->from_alpha, inst->to_alpha, p);

        /* clear the "moving" flag when the whole (possibly multi-step) move
         * completes so the clip crossfades back from walk to idle. */
        if (inst->anim_moving && ts_tween_done(&inst->tween) &&
            inst->seg_index + 1 >= inst->seg_count) {
            inst->anim_moving = false;
            /* final touchdown of an arced move (single hop or last step) */
            if (e && inst->arc)
                ts_engine_emit_event(e, TESSERA_EVENT_ENTITY_HOP_LANDED,
                                     TESSERA_EVENT_SUBJECT_ENTITY, inst->id,
                                     orch_event_coord(inst->to_pos), 0.0f);
        }
        /* spawn transition settled */
        if (inst->spawning && ts_tween_done(&inst->tween)) {
            inst->spawning = false;
            if (e)
                ts_engine_emit_event(e, TESSERA_EVENT_ENTITY_SPAWNED,
                                     TESSERA_EVENT_SUBJECT_ENTITY, inst->id,
                                     orch_event_coord(inst->pos), 0.0f);
        }
        advance_anim(inst, dt);

        if (inst->removing && ts_tween_done(&inst->tween)) {
            if (e)
                ts_engine_emit_event(e, TESSERA_EVENT_ENTITY_REMOVED,
                                     TESSERA_EVENT_SUBJECT_ENTITY, inst->id,
                                     orch_event_coord(inst->pos), 0.0f);
            o->entities[i] = o->entities[o->entity_count - 1];
            o->entity_count--;
            continue;
        }
        ++i;
    }

    /* tiles */
    for (size_t i = 0; i < o->tile_count;) {
        TsTileInst* t = &o->tiles[i];
        ts_tween_advance(&t->tween, dt);
        if (t->removing && ts_tween_done(&t->tween)) {
            o->tiles[i] = o->tiles[o->tile_count - 1];
            o->tile_count--;
            continue;
        }
        ++i;
    }

    /* cards + piles */
    for (size_t i = 0; i < o->card_count;) {
        TsCardInst* c = &o->cards[i];
        ts_tween_advance(&c->tween, dt);
        ts_tween_advance(&c->mix_tween, dt);
        ts_tween_advance(&c->thick_tween, dt);
        /* Multi-step: roll leftover time into the next segment (see entities). */
        while (ts_tween_done(&c->tween) && c->seg_index + 1 < c->seg_count) {
            float over = c->tween.elapsed - (c->tween.delay + c->tween.duration);
            c->seg_index++;
            glm_vec3_copy(c->to_pos, c->from_pos);
            glm_vec3_copy(c->seg_pts[c->seg_index], c->to_pos);
            glm_quat_copy(c->to_rot, c->from_rot);
            c->from_scale = c->to_scale;
            c->from_alpha = c->to_alpha;
            ts_tween_start(&c->tween, c->seg_dur, 0.0f, TS_EASE_OUT_CUBIC);
            if (over > 0.0f) ts_tween_advance(&c->tween, over);
            /* card segment handoff (multi-step path / hand-approach staging) */
            if (e)
                ts_engine_emit_event(e, TESSERA_EVENT_ENTITY_WAYPOINT_REACHED,
                                     c->is_draw ? TESSERA_EVENT_SUBJECT_DRAW
                                                : TESSERA_EVENT_SUBJECT_CARD,
                                     c->id, orch_event_coord(c->from_pos),
                                     (float)c->seg_index);
        }
        float p = ts_tween_value01(&c->tween);
        glm_vec3_lerp(c->from_pos, c->to_pos, p, c->pos);
        glm_quat_slerp(c->from_rot, c->to_rot, p, c->rot);
        c->scale = ts_lerpf(c->from_scale, c->to_scale, p);
        c->alpha = ts_lerpf(c->from_alpha, c->to_alpha, p);
        c->mix   = ts_lerpf(c->from_mix, c->to_mix, ts_tween_value01(&c->mix_tween));
        c->thick = ts_lerpf(c->from_thick, c->to_thick, ts_tween_value01(&c->thick_tween));

        if (c->removing && ts_tween_done(&c->tween)) {
            o->cards[i] = o->cards[o->card_count - 1];
            o->card_count--;
            continue;
        }
        ++i;
    }

    /* overlays */
    for (size_t i = 0; i < o->overlay_count;) {
        TsOverlayInst* v = &o->overlays[i];
        ts_tween_advance(&v->tween, dt);
        float p = ts_tween_value01(&v->tween);
        for (int k = 0; k < 4; ++k)
            v->tint[k] = ts_lerpf(v->from_tint[k], v->to_tint[k], p);
        v->alpha = ts_lerpf(v->from_alpha, v->to_alpha, p);
        v->pulse_t += dt;   /* free-running; never blocks idle */

        if (v->removing && ts_tween_done(&v->tween)) {
            o->overlays[i] = o->overlays[o->overlay_count - 1];
            o->overlay_count--;
            continue;
        }
        ++i;
    }

    /* labels */
    for (size_t i = 0; i < o->label_count;) {
        TsLabelInst* v = &o->labels[i];
        ts_tween_advance(&v->tween, dt);
        ts_tween_advance(&v->text_tween, dt);
        float p = ts_tween_value01(&v->tween);
        for (int k = 0; k < 4; ++k)
            v->color[k] = ts_lerpf(v->from_color[k], v->to_color[k], p);
        glm_vec3_lerp(v->from_off, v->to_off, p, v->off);
        v->alpha = ts_lerpf(v->from_alpha, v->to_alpha, p);
        v->text_mix = ts_tween_value01(&v->text_tween);

        if (v->removing && ts_tween_done(&v->tween)) {
            o->labels[i] = o->labels[o->label_count - 1];
            o->label_count--;
            continue;
        }
        ++i;
    }

    /* highlights */
    for (size_t i = 0; i < o->highlight_count;) {
        TsHighlightInst* v = &o->highlights[i];
        ts_tween_advance(&v->tween, dt);
        float p = ts_tween_value01(&v->tween);
        for (int k = 0; k < 4; ++k)
            v->color[k] = ts_lerpf(v->from_color[k], v->to_color[k], p);
        v->alpha = ts_lerpf(v->from_alpha, v->to_alpha, p);
        v->pulse_t += dt;   /* free-running; never blocks idle */

        if (v->removing && ts_tween_done(&v->tween)) {
            o->highlights[i] = o->highlights[o->highlight_count - 1];
            o->highlight_count--;
            continue;
        }
        ++i;
    }

    /* point lights */
    for (size_t i = 0; i < o->point_light_count;) {
        TsPointLightInst* v = &o->point_lights[i];
        ts_tween_advance(&v->tween, dt);
        float p = ts_tween_value01(&v->tween);
        glm_vec3_lerp(v->from_pos, v->to_pos, p, v->pos);
        for (int k = 0; k < 3; ++k)
            v->color[k] = ts_lerpf(v->from_color[k], v->to_color[k], p);
        v->intensity = ts_lerpf(v->from_intensity, v->to_intensity, p);
        v->radius = ts_lerpf(v->from_radius, v->to_radius, p);

        if (v->removing && ts_tween_done(&v->tween)) {
            o->point_lights[i] = o->point_lights[o->point_light_count - 1];
            o->point_light_count--;
            continue;
        }
        ++i;
    }

    /* world models */
    for (size_t i = 0; i < o->world_model_count;) {
        TsWorldModelInst* v = &o->world_models[i];
        ts_tween_advance(&v->tween, dt);
        float p = ts_tween_value01(&v->tween);
        glm_vec3_lerp(v->from_pos, v->to_pos, p, v->pos);
        glm_quat_slerp(v->from_rot, v->to_rot, p, v->rot);
        v->scale = ts_lerpf(v->from_scale, v->to_scale, p);
        v->alpha = ts_lerpf(v->from_alpha, v->to_alpha, p);

        if (v->removing && ts_tween_done(&v->tween)) {
            o->world_models[i] = o->world_models[o->world_model_count - 1];
            o->world_model_count--;
            continue;
        }
        ++i;
    }
}

/* ----------------------------------------------------------- queries */
bool ts_orch_is_idle(const struct TsOrch* o) {
    for (size_t i = 0; i < o->entity_count; ++i) {
        const TsEntityInst* inst = &o->entities[i];
        if (inst->removing || !ts_tween_done(&inst->tween) ||
            inst->seg_index + 1 < inst->seg_count) return false;
    }
    for (size_t i = 0; i < o->tile_count; ++i) {
        const TsTileInst* t = &o->tiles[i];
        if (t->removing || !ts_tween_done(&t->tween)) return false;
    }
    for (size_t i = 0; i < o->card_count; ++i) {
        const TsCardInst* c = &o->cards[i];
        if (c->removing || !ts_tween_done(&c->tween) ||
            !ts_tween_done(&c->mix_tween) || !ts_tween_done(&c->thick_tween) ||
            c->seg_index + 1 < c->seg_count)
            return false;
    }
    for (size_t i = 0; i < o->overlay_count; ++i) {
        const TsOverlayInst* v = &o->overlays[i];
        if (v->removing || !ts_tween_done(&v->tween)) return false;
    }
    for (size_t i = 0; i < o->label_count; ++i) {
        const TsLabelInst* v = &o->labels[i];
        if (v->removing || !ts_tween_done(&v->tween) ||
            !ts_tween_done(&v->text_tween)) return false;
    }
    for (size_t i = 0; i < o->highlight_count; ++i) {
        const TsHighlightInst* v = &o->highlights[i];
        if (v->removing || !ts_tween_done(&v->tween)) return false;
    }
    for (size_t i = 0; i < o->point_light_count; ++i) {
        const TsPointLightInst* v = &o->point_lights[i];
        if (v->removing || !ts_tween_done(&v->tween)) return false;
    }
    for (size_t i = 0; i < o->world_model_count; ++i) {
        const TsWorldModelInst* v = &o->world_models[i];
        if (v->removing || !ts_tween_done(&v->tween)) return false;
    }
    return true;
}

bool ts_orch_has_content(const struct TsOrch* o) {
    return o->entity_count > 0 || o->tile_count > 0 || o->card_count > 0 ||
           o->overlay_count > 0 || o->label_count > 0 || o->highlight_count > 0 ||
           o->point_light_count > 0 || o->world_model_count > 0;
}

size_t ts_orch_get_point_lights(const struct TsOrch* o,
                                TsPointLightItem* out, size_t cap) {
    size_t w = 0;
    for (size_t i = 0; i < o->point_light_count && w < cap; ++i) {
        const TsPointLightInst* v = &o->point_lights[i];
        TsPointLightItem* it = &out[w++];
        glm_vec3_copy((float*)v->pos, it->pos);
        it->radius = v->radius;
        memcpy(it->color, v->color, 3 * sizeof(float));
        it->intensity = v->intensity;
    }
    return w;
}

bool ts_orch_entity_pos(const struct TsOrch* o, TesseraEntityId id, vec3 out) {
    for (size_t i = 0; i < o->entity_count; ++i) {
        if (o->entities[i].id == id) {
            glm_vec3_copy((float*)o->entities[i].pos, out);
            return true;
        }
    }
    return false;
}

bool ts_orch_tile_pos(const struct TsOrch* o, TesseraTileId id, vec3 out) {
    if (id == 0) return false;
    for (size_t i = 0; i < o->tile_count; ++i) {
        const TsTileInst* t = &o->tiles[i];
        if (t->id != id) continue;
        vec3 w;
        ts_grid_to_world(t->coord.x, t->coord.y, w);
        w[1] += tile_cur_y(t);   /* follow the current rise/sink */
        glm_vec3_copy(w, out);
        return true;
    }
    return false;
}

bool ts_orch_card_pos(const struct TsOrch* o, TesseraCardId id, vec3 out) {
    if (!o || id == 0) return false;
    for (size_t i = 0; i < o->card_count; ++i) {
        const TsCardInst* c = &o->cards[i];
        if (c->alive && !c->is_draw && c->id == id) {
            glm_vec3_copy((float*)c->pos, out);
            return true;
        }
    }
    return false;
}

bool ts_orch_draw_pos(const struct TsOrch* o, TesseraCardDrawId id, vec3 out) {
    if (!o || id == 0) return false;
    for (size_t i = 0; i < o->card_count; ++i) {
        const TsCardInst* c = &o->cards[i];
        if (c->alive && c->is_draw && c->id == id) {
            glm_vec3_copy((float*)c->pos, out);
            return true;
        }
    }
    return false;
}

bool ts_orch_card_transform(struct TsOrch* o, TesseraEngine* e, TesseraCardId id,
                            vec3 out_pos, versor out_rot, float* out_w, float* out_h) {
    if (!o || id == 0) return false;
    for (size_t i = 0; i < o->card_count; ++i) {
        TsCardInst* c = &o->cards[i];
        if (!c->alive || c->is_draw || c->id != id) continue;
        TsDef* d = ts_registry_get(&e->registry, c->def, TS_DEF_CARD);
        if (!d || !d->as.card.valid) return false;
        const TsCardModel* m = &d->as.card;
        glm_vec3_copy((float*)c->pos, out_pos);
        glm_quat_copy((float*)c->rot, out_rot);
        if (out_w) *out_w = m->width  * c->scale;
        if (out_h) *out_h = m->height * c->scale;
        return true;
    }
    return false;
}

TesseraHandId ts_orch_card_hand(const struct TsOrch* o, TesseraCardId id) {
    if (!o || id == 0) return 0;
    for (size_t i = 0; i < o->card_count; ++i) {
        const TsCardInst* c = &o->cards[i];
        if (c->alive && !c->is_draw && c->id == id) return c->hand;
    }
    return 0;
}

bool ts_orch_hand_extent(struct TsOrch* o, TesseraEngine* e, TesseraHandId hand,
                         const vec3 right, const vec3 up, const vec3 H,
                         float* out_half_w, float* out_half_h) {
    if (!o || hand == 0) return false;
    float hw = 0.0f, hh = 0.0f;
    size_t found = 0;
    for (size_t i = 0; i < o->card_count; ++i) {
        const TsCardInst* c = &o->cards[i];
        if (!c->alive || c->is_draw || c->hand != hand) continue;
        TsDef* d = ts_registry_get(&e->registry, c->def, TS_DEF_CARD);
        if (!d || !d->as.card.valid) continue;
        const TsCardModel* m = &d->as.card;
        vec3 rel; glm_vec3_sub((float*)c->pos, (float*)H, rel);
        float pr = glm_vec3_dot(rel, (float*)right);
        float pu = glm_vec3_dot(rel, (float*)up);
        float ew = fabsf(pr) + 0.5f * m->width  * c->scale;
        float eh = fabsf(pu) + 0.5f * m->height * c->scale;
        if (ew > hw) hw = ew;
        if (eh > hh) hh = eh;
        ++found;
    }
    if (found == 0) return false;
    if (out_half_w) *out_half_w = hw;
    if (out_half_h) *out_half_h = hh;
    return true;
}

/* ----------------------------------------------------------- drawlist */
static bool tint_is_zero(const float t[4]) {
    return t[0] == 0.0f && t[1] == 0.0f && t[2] == 0.0f && t[3] == 0.0f;
}

size_t ts_orch_build_drawlist(struct TsOrch* o, TesseraEngine* e,
                              TsArena* arena, struct TsDrawItem** out) {
    size_t cap = o->tile_count + o->entity_count + o->world_model_count;
    TsDrawItem* items = cap ? TS_ARENA_ARR(arena, TsDrawItem, cap) : NULL;
    *out = items;
    if (!items) return 0;

    size_t w = 0;

    /* world models (decoration): drawn first so they sit visually beneath the
     * board wherever depth ties. Static scenery — skinned models render in
     * their rest pose. Not pickable, not highlightable. */
    for (size_t i = 0; i < o->world_model_count; ++i) {
        TsWorldModelInst* v = &o->world_models[i];
        TsDef* d = ts_registry_get(&e->registry, v->def, TS_DEF_ENTITY);
        if (!d) continue;
        const TesseraEntityDef* spec = &d->as.entity.spec;

        TsDrawItem* it = &items[w];
        memset(it, 0, sizeof *it);
        if (d->as.entity.has_mesh && d->as.entity.mesh.vertex_count > 0)
            it->mesh = &d->as.entity.mesh;
        else
            it->mesh = &e->registry.cube_mesh;
        it->texture = ts_registry_atlas_texture(&e->registry, spec->atlas);

        if (d->as.entity.skinned) {
            it->skinned = true;
            TsSkinData* sd = (TsSkinData*)d->as.entity.skin_data;
            uint32_t jc = sd ? sd->skeleton.joint_count : 0;
            mat4* palette = jc > 0 ? TS_ARENA_ARR(arena, mat4, jc) : NULL;
            if (palette) {
                TsJointPose pose[TS_MAX_JOINTS];
                ts_clip_sample(&sd->skeleton, NULL, 0.0f, true, pose); /* rest */
                ts_skeleton_skinning(&sd->skeleton, pose, palette);
                it->joints = palette;
                it->joint_count = jc;
            }
        }

        float ds = spec->scale > 0.0f ? spec->scale : 1.0f;
        float s = v->scale * ds;
        vec3 svec = { s, s, s };
        mat4 m;
        ts_trs(v->pos, v->rot, svec, m);
        glm_mat4_copy(m, it->model);

        const float* bc = d->as.entity.base_color;
        it->tint[0] = bc[0]; it->tint[1] = bc[1]; it->tint[2] = bc[2];
        it->tint[3] = v->alpha * bc[3];
        it->uv_rect[0] = 0.0f; it->uv_rect[1] = 0.0f;
        it->uv_rect[2] = 1.0f; it->uv_rect[3] = 1.0f;
        ++w;
    }

    /* tiles */
    for (size_t i = 0; i < o->tile_count; ++i) {
        TsTileInst* t = &o->tiles[i];
        TsDef* d = ts_registry_get(&e->registry, t->def, TS_DEF_TILE);
        if (!d) continue;
        const TesseraTileDef* spec = &d->as.tile.spec;

        TsDrawItem* it = &items[w];
        memset(it, 0, sizeof *it);
        it->mesh = &e->registry.tile_mesh;
        it->texture = ts_registry_atlas_texture(&e->registry, spec->atlas);
        it->hl_kind = TESSERA_HIGHLIGHT_TILE;
        it->hl_id = t->id;   /* 0 = unqueryable, never matches a highlight */

        float y = tile_cur_y(t);
        float a = tile_cur_alpha(t);

        mat4 m;
        glm_mat4_identity(m);
        vec3 world;
        ts_grid_to_world(t->coord.x, t->coord.y, world);
        world[1] += y;
        glm_translate(m, world);
        glm_mat4_copy(m, it->model);

        vec4 tint;
        if (tint_is_zero(spec->tint)) {
            glm_vec4_one(tint);
        } else {
            glm_vec4_copy((float*)spec->tint, tint);
        }
        tint[3] *= a;
        glm_vec4_copy(tint, it->tint);

        if (spec->atlas != 0) {
            it->uv_rect[0] = spec->top.u0;
            it->uv_rect[1] = spec->top.v0;
            it->uv_rect[2] = spec->top.u1;
            it->uv_rect[3] = spec->top.v1;
        } else {
            it->uv_rect[0] = 0.0f; it->uv_rect[1] = 0.0f;
            it->uv_rect[2] = 1.0f; it->uv_rect[3] = 1.0f;
        }
        ++w;
    }

    /* entities */
    for (size_t i = 0; i < o->entity_count; ++i) {
        TsEntityInst* inst = &o->entities[i];
        TsDef* d = ts_registry_get(&e->registry, inst->def, TS_DEF_ENTITY);
        if (!d) continue;
        const TesseraEntityDef* spec = &d->as.entity.spec;

        TsDrawItem* it = &items[w];
        memset(it, 0, sizeof *it);
        if (d->as.entity.has_mesh && d->as.entity.mesh.vertex_count > 0)
            it->mesh = &d->as.entity.mesh;
        else
            it->mesh = &e->registry.cube_mesh;

        it->texture = ts_registry_atlas_texture(&e->registry, spec->atlas);
        it->hl_kind = TESSERA_HIGHLIGHT_ENTITY;
        it->hl_id = inst->id;

        /* Skinned meshes carry the skinned vertex format, so they MUST be drawn
         * with the skinned pipeline. Flag the format up front; the renderer skips
         * a skinned item that lacks a palette rather than binding it to the
         * static pipeline (vertex-layout mismatch). */
        if (d->as.entity.skinned) {
            it->skinned = true;
            TsSkinData* sd = (TsSkinData*)d->as.entity.skin_data;
            uint32_t jc = sd ? sd->skeleton.joint_count : 0;
            mat4* palette = jc > 0 ? TS_ARENA_ARR(arena, mat4, jc) : NULL;
            if (palette) {
                TsJointPose poseA[TS_MAX_JOINTS], poseB[TS_MAX_JOINTS], poseF[TS_MAX_JOINTS];
                const TsClip* cur = (inst->cur_clip >= 0 && (uint32_t)inst->cur_clip < sd->clip_count)
                                    ? &sd->clips[inst->cur_clip] : NULL;
                ts_clip_sample(&sd->skeleton, cur, inst->clip_time, true, poseA);
                if (inst->blend_clip >= 0 && (uint32_t)inst->blend_clip < sd->clip_count) {
                    ts_clip_sample(&sd->skeleton, &sd->clips[inst->blend_clip],
                                   inst->blend_from_time, true, poseB);
                    ts_pose_blend(poseB, poseA, inst->blend_t, jc, poseF);
                    ts_skeleton_skinning(&sd->skeleton, poseF, palette);
                } else {
                    ts_skeleton_skinning(&sd->skeleton, poseA, palette);
                }
                it->joints = palette;
                it->joint_count = jc;
            }
        }

        float ds = spec->scale > 0.0f ? spec->scale : 1.0f;
        float s = inst->scale * ds;
        vec3 svec = { s, s, s };
        vec3 draw_pos;
        glm_vec3_copy(inst->pos, draw_pos);
        draw_pos[1] += TS_ENTITY_LIFT;   /* clear the tile top so the base doesn't z-fight */
        mat4 m;
        ts_trs(draw_pos, inst->rot, svec, m);
        glm_mat4_copy(m, it->model);

        const float* bc = d->as.entity.base_color;
        it->tint[0] = bc[0]; it->tint[1] = bc[1]; it->tint[2] = bc[2];
        it->tint[3] = inst->alpha * bc[3];
        it->uv_rect[0] = 0.0f; it->uv_rect[1] = 0.0f;
        it->uv_rect[2] = 1.0f; it->uv_rect[3] = 1.0f;
        ++w;
    }

    return w;
}

/* ----------------------------------------------------------- cards */
size_t ts_orch_build_cards(struct TsOrch* o, TesseraEngine* e,
                           TsArena* arena, TsCardDrawItem** out) {
    *out = NULL;
    if (o->card_count == 0) return 0;
    TsCardDrawItem* items = TS_ARENA_ARR(arena, TsCardDrawItem, o->card_count);
    if (!items) return 0;
    *out = items;

    size_t w = 0;
    for (size_t i = 0; i < o->card_count; ++i) {
        TsCardInst* c = &o->cards[i];
        if (c->scale <= 0.001f || c->alpha <= 0.003f) continue;
        TsDef* d = ts_registry_get(&e->registry, c->def, TS_DEF_CARD);
        if (!d || !d->as.card.valid) continue;
        const TsCardModel* cm = &d->as.card;

        TsCardDrawItem* it = &items[w];
        memset(it, 0, sizeof *it);
        it->mesh = &cm->mesh;
        it->hl_id = c->is_draw ? 0 : c->id;   /* piles are not highlightable */

        float base = cm->thickness > 0.0f ? cm->thickness : 0.03f;
        float sy = c->scale * (c->is_draw ? (c->thick / base) : 1.0f);
        vec3 sc = { c->scale, sy, c->scale };
        vec3 dpos; glm_vec3_copy(c->pos, dpos);
        if (c->is_draw) {   /* rest the pile base on its placement point */
            vec3 up = { 0.0f, 0.5f * c->thick * c->scale, 0.0f }, wl;
            glm_quat_rotatev(c->rot, up, wl);
            glm_vec3_add(dpos, wl, dpos);
        }
        ts_trs(dpos, c->rot, sc, it->model);

        it->tint[0] = cm->tint[0]; it->tint[1] = cm->tint[1];
        it->tint[2] = cm->tint[2]; it->tint[3] = cm->tint[3] * c->alpha;
        it->mix = c->mix;

        glm_vec4_copy((float*)&cm->visible_uv, it->uv_visible);
        glm_vec4_copy((float*)&cm->hidden_uv,  it->uv_hidden);
        it->tex_visible = ts_registry_atlas_texture(&e->registry, cm->visible_atlas);
        it->tex_hidden  = ts_registry_atlas_texture(&e->registry, cm->hidden_atlas);
        /* a pile's bottom face always shows the hidden texture */
        if (c->is_draw) {
            glm_vec4_copy((float*)&cm->hidden_uv, it->uv_back);
            it->tex_back = ts_registry_atlas_texture(&e->registry, cm->hidden_atlas);
        } else {
            glm_vec4_copy((float*)&cm->back_uv, it->uv_back);
            it->tex_back = ts_registry_atlas_texture(&e->registry, cm->back_atlas);
        }
        ++w;
    }
    return w;
}

/* ----------------------------------------------------------- blob shadows */
size_t ts_orch_build_blobs(struct TsOrch* o, TesseraEngine* e,
                           TsArena* arena, TsBlob** out) {
    *out = NULL;
    if (o->entity_count == 0) return 0;
    TsBlob* blobs = TS_ARENA_ARR(arena, TsBlob, o->entity_count);
    if (!blobs) return 0;
    *out = blobs;

    size_t w = 0;
    for (size_t i = 0; i < o->entity_count; ++i) {
        TsEntityInst* inst = &o->entities[i];
        TsDef* d = ts_registry_get(&e->registry, inst->def, TS_DEF_ENTITY);
        if (!d) continue;
        float ds = d->as.entity.spec.scale > 0.0f ? d->as.entity.spec.scale : 1.0f;
        float s = inst->scale * ds;
        if (s <= 0.001f || inst->alpha <= 0.02f) continue;

        TsBlob* b = &blobs[w++];
        b->center[0] = inst->pos[0];
        b->center[1] = 0.02f;            /* just above the tile surface */
        b->center[2] = inst->pos[2];
        b->radius = 0.55f * s * TS_TILE_SIZE;
        /* fade the shadow as the entity hops (higher = fainter, larger) */
        float lift = inst->pos[1] > 0.0f ? inst->pos[1] : 0.0f;
        float lift_fade = 1.0f / (1.0f + lift * 1.5f);
        b->radius *= (1.0f + lift * 0.4f);
        b->alpha = 0.38f * inst->alpha * lift_fade;
    }
    return w;
}

/* ----------------------------------------------------------- overlays */
size_t ts_orch_build_overlays(struct TsOrch* o, TesseraEngine* e,
                              TsArena* arena, TsOverlayItem** out) {
    (void)e;
    *out = NULL;
    if (o->overlay_count == 0) return 0;
    TsOverlayItem* items = TS_ARENA_ARR(arena, TsOverlayItem, o->overlay_count);
    if (!items) return 0;
    *out = items;

    size_t w = 0;
    for (size_t i = 0; i < o->overlay_count; ++i) {
        const TsOverlayInst* v = &o->overlays[i];

        float amul = 1.0f, smul = 1.0f;
        if (v->pulse_s > 0.0f) {
            /* raised-cosine breathe, starting at the min */
            float ph = 0.5f - 0.5f * cosf(v->pulse_t * (2.0f * GLM_PIf) / v->pulse_s);
            if (v->pulse_alpha_max > 0.0f)
                amul = ts_lerpf(v->pulse_alpha_min, v->pulse_alpha_max, ph);
            if (v->pulse_scale_max > 0.0f)
                smul = ts_lerpf(v->pulse_scale_min, v->pulse_scale_max, ph);
        }
        float a = v->alpha * v->tint[3] * amul;
        if (a <= 0.003f || smul <= 0.001f) continue;

        TsOverlayItem* it = &items[w++];
        vec3 wpos;
        ts_grid_to_world(v->coord.x, v->coord.y, wpos);
        it->center[0] = wpos[0];
        it->center[1] = TS_OVERLAY_LIFT;   /* just above the tile top, below pieces */
        it->center[2] = wpos[2];
        it->scale = smul;
        it->shape = v->shape;
        it->atlas = v->atlas;
        /* an all-zero uv means "the whole atlas" (and untextured always is) */
        if (v->atlas == 0 || (v->uv.u0 == 0.0f && v->uv.v0 == 0.0f &&
                              v->uv.u1 == 0.0f && v->uv.v1 == 0.0f)) {
            it->uv = (TesseraRect){ 0.0f, 0.0f, 1.0f, 1.0f };
        } else {
            it->uv = v->uv;
        }
        it->color[0] = v->tint[0];
        it->color[1] = v->tint[1];
        it->color[2] = v->tint[2];
        it->color[3] = a;
    }
    return w;
}

/* ----------------------------------------------------------- labels */
/* Resolve a label's anchor point against the LIVE interpolated scene (the
 * same sources the camera FOCUS_* modes track). Returns false when the
 * anchored object is not live — the label is hidden that frame. */
static bool label_anchor_pos(struct TsOrch* o, TesseraEngine* e,
                             const TsLabelInst* v, vec3 out) {
    switch (v->anchor) {
    case TESSERA_LABEL_ANCHOR_ENTITY:
        return ts_orch_entity_pos(o, (TesseraEntityId)v->anchor_id, out);
    case TESSERA_LABEL_ANCHOR_TILE:
        return ts_orch_tile_pos(o, (TesseraTileId)v->anchor_id, out);
    case TESSERA_LABEL_ANCHOR_DICE:
        return e->dice && ts_dice_pos(e->dice, (TesseraDiceId)v->anchor_id, out);
    case TESSERA_LABEL_ANCHOR_CARD:
        return ts_orch_card_pos(o, (TesseraCardId)v->anchor_id, out);
    case TESSERA_LABEL_ANCHOR_DRAW:
        return ts_orch_draw_pos(o, (TesseraCardDrawId)v->anchor_id, out);
    default:
        glm_vec3_zero(out);
        return true;                     /* WORLD: offset alone is the point */
    }
}

/* ----------------------------------------------------------- highlights */
size_t ts_orch_build_highlights(struct TsOrch* o, TsArena* arena,
                                TsHighlightItem** out) {
    *out = NULL;
    if (o->highlight_count == 0) return 0;
    TsHighlightItem* items = TS_ARENA_ARR(arena, TsHighlightItem, o->highlight_count);
    if (!items) return 0;
    *out = items;

    size_t w = 0;
    for (size_t i = 0; i < o->highlight_count; ++i) {
        const TsHighlightInst* v = &o->highlights[i];

        float imul = 1.0f;
        if (v->pulse_s > 0.0f && v->pulse_max > 0.0f) {
            /* raised-cosine breathe, starting at the min */
            float ph = 0.5f - 0.5f * cosf(v->pulse_t * (2.0f * GLM_PIf) / v->pulse_s);
            imul = ts_lerpf(v->pulse_min, v->pulse_max, ph);
        }
        float a = v->alpha * v->color[3];
        if (a * imul <= 0.003f) continue;

        TsHighlightItem* it = &items[w++];
        it->kind = v->kind;
        it->id = v->target_id;
        it->style = v->style;
        it->color[0] = v->color[0];
        it->color[1] = v->color[1];
        it->color[2] = v->color[2];
        it->color[3] = a;
        it->thickness = v->thickness > 0.0f ? v->thickness : 3.0f;
        it->intensity = imul;
    }
    return w;
}

size_t ts_orch_build_labels(struct TsOrch* o, TesseraEngine* e,
                            TsArena* arena, TsLabelItem** out) {
    *out = NULL;
    if (o->label_count == 0) return 0;
    TsLabelItem* items = TS_ARENA_ARR(arena, TsLabelItem, o->label_count);
    if (!items) return 0;
    *out = items;

    size_t w = 0;
    for (size_t i = 0; i < o->label_count; ++i) {
        TsLabelInst* v = &o->labels[i];
        float a = v->alpha * v->color[3];
        if (a <= 0.003f) continue;
        vec3 base;
        if (!label_anchor_pos(o, e, v, base)) continue;

        TsLabelItem* it = &items[w++];
        glm_vec3_add(base, v->off, it->pos);
        it->size = v->size;
        it->color[0] = v->color[0];
        it->color[1] = v->color[1];
        it->color[2] = v->color[2];
        it->color[3] = a;
        it->text_mix = v->text_mix;
        it->text = v->text;
        it->prev_text = (v->text_mix < 0.999f && v->prev_text[0]) ? v->prev_text : NULL;
        it->font = v->font;
        it->billboard = v->billboard;
    }
    return w;
}
