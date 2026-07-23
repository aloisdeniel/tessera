/*
 * dice.h — procedural polyhedral dice with per-face sprites.
 *
 * A dice *definition* is turned into a generated convex model whose face count
 * matches the number of sprites: a cube (6 faces), a two-sided token (2), or an
 * N-gonal barrel otherwise. The sprites are packed into one atlas and the model
 * carries baked per-face UVs (each sprite centred on and filling its face) plus
 * a per-face *rest orientation* (the rotation that turns that face to point +Y),
 * so any face can be made to land face-up.
 *
 * Live dice are thrown imperatively (outside the immutable state snapshot). Each
 * spawns airborne and tumbles along a precomputed trajectory to its rest pose,
 * then can be faded out. The set of live dice is owned by the engine and drawn
 * with the standard static-mesh pipeline (see engine.c / drawlist.c).
 */
#ifndef TESSERA_DICE_H
#define TESSERA_DICE_H

#include "core/core.h"
#include "core/tmath.h"
#include "gpu/gpu.h"
#include "tessera.h"

typedef struct TesseraEngine TesseraEngine;
struct TsDrawItem;   /* engine.h */

#define TS_DICE_MAX_FACES 64u

/* Generated geometry + atlas for one dice definition (owned by the registry).
 * Geometry index/rest data lives in the engine arena; GPU objects (mesh, atlas)
 * are owned here and released via ts_dice_free_model. */
typedef struct {
    TsMesh    mesh;        /* generated POSITION/NORMAL/UV geometry */
    TsTexture atlas;       /* packed per-face sprite atlas          */
    versor*   rest;        /* face_count rest orientations (arena):
                            * rest[f] rotates the model so face f points +Y */
    uint32_t  face_count;
    float     size;        /* approximate diameter (world units)    */
    vec4      tint;        /* sprite multiply colour (1,1,1,1 if unset) */
    bool      valid;
} TsDiceModel;

/* Build the model + packed atlas for `def`. Geometry/rest are allocated from
 * `arena` (engine lifetime); the GPU mesh + atlas are owned by *out. Returns
 * false and fills err on failure. */
bool ts_dice_build_model(TsGpu* gpu, TsArena* arena, const TsLog* log,
                         const TesseraDiceDef* def, TsDiceModel* out,
                         char* err, size_t err_sz);
void ts_dice_free_model(TsGpu* gpu, TsDiceModel* m);

/* ---- live dice set ---------------------------------------------------- */
typedef struct TsDice TsDice;

/* Internal throw spec (built from a TesseraDicePlacement during a state diff). */
typedef struct {
    TesseraDiceId id;
    TesseraDefId  def;
    uint32_t      face;
    float         position[3];
    uint32_t      seed;
    float         throw_s;
} TsDiceThrow;

TsDice* ts_dice_create(void);
void    ts_dice_destroy(TsDice* d);

/* Throw a die (or re-throw an existing one with the same id). Reads the model
 * for `spec->def` from the engine registry. No-op if the def is not a dice. */
void ts_dice_add(TsDice* d, TesseraEngine* e, const TsDiceThrow* spec);
/* Begin removing a die (fade + shrink over fade_s). Unknown id => no-op. */
void ts_dice_remove(TsDice* d, TesseraDiceId id, float fade_s);
/* Begin removing every live die. */
void ts_dice_clear(TsDice* d, float fade_s);
/* Advance all tumbles + fades by dt (already scaled by speed_multiplier). */
void ts_dice_advance(TsDice* d, float dt);

/* Diff the dice placements in prev->next and drive throws/removals. `prev` may
 * be NULL (first state). Creates the live-set lazily on the engine. Called from
 * the engine on each promotion (mirrors ts_fx_on_promote). */
struct TsSnapshot;
void ts_dice_on_promote(TesseraEngine* e, const struct TsSnapshot* prev,
                        const struct TsSnapshot* next, float remove_s);

bool     ts_dice_all_idle(const TsDice* d);
uint32_t ts_dice_count(const TsDice* d);
bool     ts_dice_face(const TsDice* d, TesseraDiceId id, uint32_t* out_face);

/* Ray-test the live dice (one bounding sphere per die, radius from its model
 * size and current scale). On a hit fills *out_id / *out_dist with the nearest
 * die and returns true; returns false on a miss. `o`/`dir` are world-space with
 * `dir` normalized. Reads dice models from the engine registry. */
bool ts_dice_raycast(const TsDice* d, TesseraEngine* e, const float o[3],
                     const float dir[3], TesseraDiceId* out_id, float* out_dist);
/* Current world-space centre of a live die by id (tracks the animating pose).
 * Returns false for an unknown/removing die. Used to project a die to screen. */
bool ts_dice_pos(const TsDice* d, TesseraDiceId id, float out[3]);

/* Draw items the live dice will emit this frame (upper bound). */
size_t ts_dice_drawitem_count(const TsDice* d);
/* Fill `dst` (>= ts_dice_drawitem_count entries) with the dice draw items.
 * Returns the number actually written. */
size_t ts_dice_build_drawlist(TsDice* d, TesseraEngine* e, struct TsDrawItem* dst);

#endif /* TESSERA_DICE_H */
