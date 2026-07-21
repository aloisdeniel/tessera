/*
 * registry.h — definition registry: tile / entity / effect / atlas defs.
 * Defs are immutable once registered and referenced by TesseraDefId.
 */
#ifndef TESSERA_REGISTRY_H
#define TESSERA_REGISTRY_H

#include "core/core.h"
#include "gpu/gpu.h"
#include "tessera.h"

typedef enum {
    TS_DEF_NONE = 0,
    TS_DEF_ATLAS,
    TS_DEF_TILE,
    TS_DEF_ENTITY,
    TS_DEF_EFFECT
} TsDefKind;

typedef struct {
    TsTexture tex;
    bool      valid;
} TsAtlasDef;

typedef struct {
    TesseraTileDef spec;   /* value copy */
    bool           valid;
} TsTileDef;

/* Parsed animation clip metadata (data filled in M5; names always available). */
typedef struct {
    char*  name;
    float  duration;
    /* channel data attached in M5 (kept opaque here) */
    void*  clip_data;
} TsAnimClip;

typedef struct {
    TesseraEntityDef spec;      /* value copy (gltf bytes NOT retained) */
    TsMesh           mesh;      /* uploaded geometry */
    bool             has_mesh;
    TsAnimClip*      clips;
    uint32_t         clip_count;
    bool             skinned;
    void*            skin_data; /* runtime skeleton (M5); NULL if static */
    bool             valid;
} TsEntityDef;

typedef struct {
    TesseraEffectDef spec;
    bool             valid;
} TsEffectDef;

/* One registry entry, tagged by kind. Stored in a slot map keyed by DefId. */
typedef struct {
    TsDefKind kind;
    union {
        TsAtlasDef  atlas;
        TsTileDef   tile;
        TsEntityDef entity;
        TsEffectDef effect;
    } as;
} TsDef;

typedef struct {
    TsSlotMap defs;       /* of TsDef; DefId is (TsHandle) */
    TsGpu*    gpu;
    TsArena*  arena;      /* for clip names etc. */
    const TsLog* log;
    TsTexture white;      /* 1x1 white fallback for untextured draws */
    TsMesh    tile_mesh;  /* shared thin-box tile mesh */
    TsMesh    cube_mesh;  /* fallback entity mesh */
} TsRegistry;

/* Resolve the texture to bind for a def's atlas id (falls back to white). */
SDL_GPUTexture* ts_registry_atlas_texture(TsRegistry* r, TesseraDefId atlas);

bool ts_registry_init(TsRegistry* r, TsGpu* gpu, TsArena* arena, const TsLog* log);
void ts_registry_shutdown(TsRegistry* r);

/* Returns def entry or NULL if id is stale/wrong kind. */
TsDef* ts_registry_get(TsRegistry* r, TesseraDefId id, TsDefKind expect);

TesseraDefId ts_registry_add_atlas(TsRegistry* r, const TesseraBytes* image, char* err, size_t err_sz);
TesseraDefId ts_registry_add_tile(TsRegistry* r, const TesseraTileDef* def, char* err, size_t err_sz);
TesseraDefId ts_registry_add_entity(TsRegistry* r, const TesseraEntityDef* def, char* err, size_t err_sz);
TesseraDefId ts_registry_add_effect(TsRegistry* r, const TesseraEffectDef* def, char* err, size_t err_sz);

#endif /* TESSERA_REGISTRY_H */
