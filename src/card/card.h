/*
 * card.h — flat textured cards, piles (draws), and hands.
 *
 * A card *definition* is turned into a thin rounded slab (a little smaller than
 * 2x3 tiles). Three textures are referenced by registered atlas id: the real
 * front (`visible`), a concealing front (`hidden`), and the `back`. The slab is
 * drawn with a dedicated pipeline (see gpu_pipeline_card.c) that samples all
 * three and crossfades the front between visible and hidden by a per-instance
 * `mix` factor, so flipping a card fades one face into the other.
 *
 * The mesh carries a per-vertex face code (0 = front, 1 = back, 2 = edge rim)
 * so a single draw call textures each face correctly. A card pile reuses the
 * same mesh scaled in thickness by its card count, and binds the def's hidden
 * texture as its bottom face.
 *
 * Live cards / piles / hands live in the immutable state snapshot; the
 * orchestrator diffs and tweens them (orch.c), then emits TsCardDrawItems that
 * the engine records with the card pipeline (engine.c).
 */
#ifndef TESSERA_CARD_H
#define TESSERA_CARD_H

#include "core/core.h"
#include "core/tmath.h"
#include "gpu/gpu.h"
#include "tessera.h"

typedef struct TesseraEngine TesseraEngine;

/* Per-vertex face code. */
#define TS_CARD_FACE_FRONT 0.0f
#define TS_CARD_FACE_BACK  1.0f
#define TS_CARD_FACE_EDGE  2.0f

/* Card mesh vertex: standard attributes plus a face selector. */
typedef struct {
    float pos[3];
    float normal[3];
    float uv[2];
    float face;   /* 0 = front, 1 = back, 2 = edge rim */
} TsCardVertex;

/* Generated geometry + resolved texture references for one card definition. */
typedef struct {
    TsMesh mesh;               /* rounded thin slab (unit thickness = `thickness`) */
    float  width, height, thickness, corner_radius;
    vec4   tint;
    TesseraDefId visible_atlas, hidden_atlas, back_atlas;
    TesseraRect  visible_uv, hidden_uv, back_uv;
    bool   valid;
} TsCardModel;

/* Build the slab mesh for `def`; resolve/validate the three atlas ids. */
bool ts_card_build_model(TsGpu* gpu, const TsLog* log, const TesseraCardDef* def,
                         TsCardModel* out, char* err, size_t err_sz);
void ts_card_free_model(TsGpu* gpu, TsCardModel* m);

/* One card or pile to draw with the card pipeline. Textures are already
 * resolved (a pile binds the def's hidden texture as its back/bottom face). */
typedef struct {
    const TsMesh*   mesh;
    mat4            model;
    vec4            tint;
    vec4            uv_visible, uv_hidden, uv_back;  /* u0,v0,u1,v1 */
    SDL_GPUTexture *tex_visible, *tex_hidden, *tex_back;
    float           mix;   /* 0 = front shows visible, 1 = front shows hidden */
    /* selection-highlight source tag: the single card's id (0 for piles /
     * anything not highlightable). Consumed by the highlight post pass. */
    uint64_t        hl_id;
} TsCardDrawItem;

#endif /* TESSERA_CARD_H */
