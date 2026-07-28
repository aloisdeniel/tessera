/*
 * fx.h — CPU-simulated, GPU-rendered particle system (M6).
 *
 * Effects have no persistent geometry: a state's EFFECT_ADD spawns an `on_add`
 * emitter, EFFECT_REMOVE spawns `on_remove`, and each emitter lives only until
 * its particles expire. Emitters may be anchored to a tile coord or attached to
 * a live entity (aura that follows a moving unit) or a live single card (a
 * burst riding the card's displayed face through deals, lunges and flips).
 */
#ifndef TESSERA_FX_H
#define TESSERA_FX_H

#include <SDL3/SDL.h>
#include "core/core.h"
#include "core/tmath.h"
#include "gpu/gpu.h"
#include "tessera.h"

typedef struct TesseraEngine TesseraEngine;
struct TsSnapshot;

/* One live particle. */
typedef struct {
    vec3     pos;
    vec3     vel;
    float    age, life;
    uint32_t seed;
} TsParticle;

/* A live emitter instantiated from a TesseraParticleSpec. */
typedef struct {
    TesseraParticleSpec spec;
    SDL_GPUTexture*     texture;      /* resolved atlas or white */
    vec3                anchor;       /* world emission point */
    TesseraEntityId     attach;       /* 0 = static anchor, else follow entity */
    TesseraCardId       attach_card;  /* 0 = none, else follow card (wins) */
    float               age;          /* emitter age (s) */
    float               emit_accum;   /* fractional continuous emission */
    bool                additive;
    bool                alive;
    TsParticle*         parts;
    size_t              part_count, part_cap;
} TsEmitter;

struct TsFx {
    TsEmitter*        emitters;
    size_t            count, cap;

    /* per-frame dynamic geometry */
    TsParticleVertex* scratch;        /* CPU build buffer */
    size_t            scratch_cap;    /* in vertices */
    SDL_GPUBuffer*    vbo;            /* dynamic vertex buffer */
    SDL_GPUBuffer*    ibo;            /* static quad index buffer */
    size_t            vbo_cap;        /* in vertices */
    size_t            ibo_cap;        /* in indices */

    uint32_t          add_vcount;     /* additive verts this frame */
    uint32_t          alpha_vcount;   /* alpha verts this frame */
    uint32_t          alpha_voffset;  /* where alpha verts start */
    size_t            global_cap;     /* max live particles across all emitters */
};

struct TsFx* ts_fx_create(void);
void ts_fx_destroy(struct TsFx* fx, TsGpu* gpu);

/* Spawn an emitter from a spec at a world anchor (attach=attach_card=0) or
 * following an entity / a single card (the card wins when both are set).
 * No-op if the spec would emit nothing. */
void ts_fx_spawn(struct TsFx* fx, TesseraEngine* e, const TesseraParticleSpec* spec,
                 const vec3 anchor, TesseraEntityId attach, TesseraCardId attach_card);

/* Diff effects/entities between snapshots and spawn the matching emitters. */
void ts_fx_on_promote(TesseraEngine* e, const struct TsSnapshot* prev,
                      const struct TsSnapshot* next);

/* Integrate every emitter/particle by dt; refresh attached anchors; cull. */
void ts_fx_advance(TesseraEngine* e, float dt);

/* True when no emitters/particles remain. */
bool ts_fx_is_idle(const struct TsFx* fx);

/* Build + upload this frame's billboard geometry (copy pass on `cmd`). Call
 * before beginning the render pass. Safe with a NULL fx (no-op). */
void ts_fx_prepare(TesseraEngine* e, SDL_GPUCommandBuffer* cmd);

/* Draw the prepared particles inside an active render pass. */
void ts_fx_record(TesseraEngine* e, SDL_GPUCommandBuffer* cmd,
                  SDL_GPURenderPass* pass, const TsFrameUniform* fu);

#endif /* TESSERA_FX_H */
