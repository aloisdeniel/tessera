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

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TS_HOP_HEIGHT 0.6f
#define TS_TILE_RISE  0.4f   /* tiles rise from below / sink to below by this */
/* Tile tops sit at y=0 and entities stand on y=0, so a mesh's bottom face is
 * coplanar with the tile face and z-fights. Lift the *rendered* model a hair
 * (the logical pos stays put, so shadows/picking are unaffected). Kept clear of
 * the blob-shadow plane (y=0.02) so the base doesn't z-fight the shadow either. */
#define TS_ENTITY_LIFT 0.035f

/* ---- card constants ---- */
#define TS_CARD_FLIP_S   0.32f    /* visible<->hidden crossfade duration       */
#define TS_CARD_THICK_S  0.30f    /* pile thickness (count) tween duration     */
#define TS_CARD_PER_CARD 0.012f   /* pile thickness added per extra card       */
#define TS_CARD_MAX_THICK 0.9f    /* clamp pile thickness (world units)        */
/* hand-fan defaults (used when the placement leaves a field <= 0) */
#define TS_HAND_SPREAD   0.5f     /* total fan angle (radians)                 */
#define TS_HAND_SPACING  0.62f    /* lateral spacing between cards (world)     */
#define TS_HAND_RADIUS   3.0f     /* arc dip radius (world)                    */

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
    inst->alive = true;
    glm_vec3_copy((float*)t->pos, inst->pos);
    glm_quat_copy((float*)t->rot, inst->rot);
    inst->scale = 0.0f;
    inst->alpha = 0.0f;
    ts_tween_start(&inst->tween, add_s, 0.0f, TS_EASE_OUT_BACK);
}

static void entity_retarget(TsEntityInst* inst, const TesseraEntityPlacement* ep,
                            const TsTarget* t, bool changed,
                            const TesseraTiming* timing) {
    inst->def = ep->def;
    inst->facing = ep->facing;
    inst->anim = ep->anim;
    /* from = current interpolated transform */
    glm_vec3_copy(inst->pos, inst->from_pos);
    glm_quat_copy(inst->rot, inst->from_rot);
    inst->from_scale = inst->scale;
    inst->from_alpha = inst->alpha;
    /* to = target */
    glm_vec3_copy((float*)t->pos, inst->to_pos);
    glm_quat_copy((float*)t->rot, inst->to_rot);
    inst->to_scale = t->scale;
    inst->to_alpha = 1.0f;
    inst->arc = changed;
    inst->removing = false;
    inst->alive = true;
    ts_tween_start(&inst->tween, changed ? timing->move_s : timing->reflow_s,
                   0.0f, TS_EASE_OUT_CUBIC);
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
 * rotation stands it up to face +Z. */
static void hand_fan_target(const TesseraHandPlacement* h, uint32_t r, uint32_t n,
                            vec3 out_pos, versor out_rot) {
    float spread  = h->spread_deg > 0.0f ? glm_rad(h->spread_deg) : TS_HAND_SPREAD;
    float spacing = h->card_spacing > 0.0f ? h->card_spacing : TS_HAND_SPACING;
    float radius  = h->radius > 0.0f ? h->radius : TS_HAND_RADIUS;

    float t = (n > 1) ? ((float)r / (float)(n - 1) - 0.5f) : 0.0f;  /* -0.5..0.5 */
    float theta = t * spread;
    float xoff = ((float)r - (float)(n - 1) * 0.5f) * spacing;
    float yoff = -(1.0f - cosf(theta)) * radius;
    float zoff = (float)r * 0.01f;                 /* stagger toward the viewer */
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

/* Target transform for card index `i` in `next` (free placement or hand fan). */
static void card_target(const TsSnapshot* next, size_t i, vec3 out_pos, versor out_rot) {
    const TesseraCardPlacement* cp = &next->cards[i];
    const TesseraHandPlacement* h = find_hand(next, cp->hand);
    if (cp->hand != 0 && h) {
        /* rank within the hand + total count */
        uint32_t n = 0, r = 0;
        for (size_t j = 0; j < next->card_count; ++j) {
            const TesseraCardPlacement* o = &next->cards[j];
            if (o->id == 0 || o->hand != cp->hand) continue;  /* id==0 is skipped by card_diff */
            n++;
            if (j != i && card_before(o, cp)) r++;
        }
        if (n == 0) n = 1;
        hand_fan_target(h, r, n, out_pos, out_rot);
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

/* Retarget a live card toward a new target, tweening from the current pose. */
static void card_retarget(TsCardInst* c, TesseraDefId def, const vec3 pos,
                          const versor rot, bool hidden, float thick, uint32_t count,
                          const TesseraTiming* timing) {
    c->def = def;
    glm_vec3_copy(c->pos, c->from_pos); glm_vec3_copy((float*)pos, c->to_pos);
    glm_quat_copy(c->rot, c->from_rot); glm_quat_copy((float*)rot, c->to_rot);
    c->from_scale = c->scale; c->to_scale = 1.0f;
    c->from_alpha = c->alpha; c->to_alpha = 1.0f;
    c->removing = false; c->alive = true;
    ts_tween_start(&c->tween, timing->move_s, 0.0f, TS_EASE_OUT_CUBIC);

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
        if (seed) {
            card_snap(orch_add_card(o), cp->id, cp->def, false, pos, rot,
                      cp->hidden, thick, 1);
            continue;
        }
        TsCardInst* c = orch_find_card(o, cp->id, false);
        if (c) card_retarget(c, cp->def, pos, rot, cp->hidden, thick, 1, timing);
        else   card_spawn(orch_add_card(o), cp->id, cp->def, false, pos, rot,
                          cp->hidden, thick, 1, timing->add_s);
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
        if (c) card_retarget(c, dp->def, pos, rot, dp->top_hidden, thick, dp->count, timing);
        else   card_spawn(orch_add_card(o), dp->id, dp->def, true, pos, rot,
                          dp->top_hidden, thick, dp->count, timing->add_s);
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
void ts_orch_advance(struct TsOrch* o, float dt) {
    /* entities */
    for (size_t i = 0; i < o->entity_count;) {
        TsEntityInst* inst = &o->entities[i];
        ts_tween_advance(&inst->tween, dt);
        float p = ts_tween_value01(&inst->tween);

        glm_vec3_lerp(inst->from_pos, inst->to_pos, p, inst->pos);
        if (inst->arc) inst->pos[1] += TS_HOP_HEIGHT * ts_arc(p);
        glm_quat_slerp(inst->from_rot, inst->to_rot, p, inst->rot);
        inst->scale = ts_lerpf(inst->from_scale, inst->to_scale, p);
        inst->alpha = ts_lerpf(inst->from_alpha, inst->to_alpha, p);

        /* clear the "moving" flag when the positional tween completes so the
         * clip crossfades back from walk to idle. */
        if (inst->anim_moving && ts_tween_done(&inst->tween)) inst->anim_moving = false;
        advance_anim(inst, dt);

        if (inst->removing && ts_tween_done(&inst->tween)) {
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
}

/* ----------------------------------------------------------- queries */
bool ts_orch_is_idle(const struct TsOrch* o) {
    for (size_t i = 0; i < o->entity_count; ++i) {
        const TsEntityInst* inst = &o->entities[i];
        if (inst->removing || !ts_tween_done(&inst->tween)) return false;
    }
    for (size_t i = 0; i < o->tile_count; ++i) {
        const TsTileInst* t = &o->tiles[i];
        if (t->removing || !ts_tween_done(&t->tween)) return false;
    }
    for (size_t i = 0; i < o->card_count; ++i) {
        const TsCardInst* c = &o->cards[i];
        if (c->removing || !ts_tween_done(&c->tween) ||
            !ts_tween_done(&c->mix_tween) || !ts_tween_done(&c->thick_tween))
            return false;
    }
    return true;
}

bool ts_orch_has_content(const struct TsOrch* o) {
    return o->entity_count > 0 || o->tile_count > 0 || o->card_count > 0;
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

/* ----------------------------------------------------------- drawlist */
static bool tint_is_zero(const float t[4]) {
    return t[0] == 0.0f && t[1] == 0.0f && t[2] == 0.0f && t[3] == 0.0f;
}

size_t ts_orch_build_drawlist(struct TsOrch* o, TesseraEngine* e,
                              TsArena* arena, struct TsDrawItem** out) {
    size_t cap = o->tile_count + o->entity_count;
    TsDrawItem* items = cap ? TS_ARENA_ARR(arena, TsDrawItem, cap) : NULL;
    *out = items;
    if (!items) return 0;

    size_t w = 0;

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
