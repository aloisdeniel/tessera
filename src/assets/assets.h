/*
 * assets.h — glTF import (M2). Flattens primitives into TesseraVertex arrays,
 * uploads a GPU mesh, and collects animation clip metadata (played in M5).
 *
 * Supported subset (reject the rest with a clear error): triangles, a single
 * base-color texture / factor, one UV set, POSITION+NORMAL+TEXCOORD_0.
 */
#ifndef TESSERA_ASSETS_H
#define TESSERA_ASSETS_H

#include "core/core.h"
#include "gpu/gpu.h"

typedef struct {
    char*    name;       /* arena-owned */
    float    duration;
} TsClipInfo;

typedef struct {
    TsMesh      mesh;
    bool        has_mesh;
    bool        skinned;     /* has a skin (skeletal); animated in M5 */
    uint32_t    joint_count;
    TsClipInfo* clips;       /* arena-owned array */
    uint32_t    clip_count;
    float       base_color[4];
} TsGltfResult;

/* Parse glTF/GLB bytes, upload the mesh, gather clip metadata into `arena`.
 * Returns false and fills err on unsupported/invalid input. */
bool ts_gltf_import(TsGpu* gpu, TsArena* arena, const void* data, size_t size,
                    TsGltfResult* out, char* err, size_t err_sz);

#endif /* TESSERA_ASSETS_H */
