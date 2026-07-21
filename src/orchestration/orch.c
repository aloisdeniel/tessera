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

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TS_HOP_HEIGHT 0.6f
#define TS_TILE_RISE  0.4f   /* tiles rise from below / sink to below by this */

/* ------------------------------------------------------------ lifecycle */
struct TsOrch* ts_orch_create(void) {
    struct TsOrch* o = (struct TsOrch*)calloc(1, sizeof *o);
    return o;
}

void ts_orch_destroy(struct TsOrch* o) {
    if (!o) return;
    free(o->entities);
    free(o->tiles);
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

/* --------------------------------------------------------- on_promote */
void ts_orch_on_promote(struct TsOrch* o, const TsSnapshot* prev,
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
        }
        for (size_t i = 0; i < next->tile_count; ++i) {
            const TesseraTilePlacement* tp = &next->tiles[i];
            if (tp->tile_def == 0) continue;
            TsTileInst* t = orch_add_tile(o);
            t->coord = tp->coord;
            t->def = tp->tile_def;
            t->variant = tp->variant;
            t->from_y = t->to_y = 0.0f;
            t->from_alpha = t->to_alpha = 1.0f;
            t->removing = false;
            t->alive = true;
            ts_tween_start(&t->tween, 0.0f, 0.0f, TS_EASE_LINEAR);
        }
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
        } else {
            TsEntityInst* ni = orch_add_entity(o);
            entity_spawn(ni, ep, &targets[i], timing->add_s);
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

    o->seeded = true;
    free(targets);
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
    return true;
}

bool ts_orch_has_content(const struct TsOrch* o) {
    return o->entity_count > 0 || o->tile_count > 0;
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
        if (d->as.entity.has_mesh && d->as.entity.mesh.vertex_count > 0)
            it->mesh = &d->as.entity.mesh;
        else
            it->mesh = &e->registry.cube_mesh;

        it->texture = ts_registry_atlas_texture(&e->registry, spec->atlas);

        float ds = spec->scale > 0.0f ? spec->scale : 1.0f;
        float s = inst->scale * ds;
        vec3 svec = { s, s, s };
        mat4 m;
        ts_trs(inst->pos, inst->rot, svec, m);
        glm_mat4_copy(m, it->model);

        it->tint[0] = 1.0f; it->tint[1] = 1.0f; it->tint[2] = 1.0f;
        it->tint[3] = inst->alpha;
        it->uv_rect[0] = 0.0f; it->uv_rect[1] = 0.0f;
        it->uv_rect[2] = 1.0f; it->uv_rect[3] = 1.0f;
        ++w;
    }

    return w;
}
