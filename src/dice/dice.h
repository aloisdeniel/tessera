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

TsDice* ts_dice_create(void);
void    ts_dice_destroy(TsDice* d);

/* Throw a die (or re-throw an existing one with the same id). Reads the model
 * for `spec->def` from the engine registry. No-op if the def is not a dice. */
void ts_dice_add(TsDice* d, TesseraEngine* e, const TesseraDiceThrow* spec);
/* Begin removing a die (fade + shrink over fade_s). Unknown id => no-op. */
void ts_dice_remove(TsDice* d, TesseraDiceId id, float fade_s);
/* Begin removing every live die. */
void ts_dice_clear(TsDice* d, float fade_s);
/* Advance all tumbles + fades by dt (already scaled by speed_multiplier). */
void ts_dice_advance(TsDice* d, float dt);

bool     ts_dice_all_idle(const TsDice* d);
uint32_t ts_dice_count(const TsDice* d);
bool     ts_dice_face(const TsDice* d, TesseraDiceId id, uint32_t* out_face);

/* Draw items the live dice will emit this frame (upper bound). */
size_t ts_dice_drawitem_count(const TsDice* d);
/* Fill `dst` (>= ts_dice_drawitem_count entries) with the dice draw items.
 * Returns the number actually written. */
size_t ts_dice_build_drawlist(TsDice* d, TesseraEngine* e, struct TsDrawItem* dst);

#endif /* TESSERA_DICE_H */
