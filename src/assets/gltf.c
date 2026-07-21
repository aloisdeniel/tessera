/* gltf.c — glTF/GLB import (M2).
 *
 * Parses model bytes with cgltf, flattens the first mesh's triangle primitives
 * into interleaved TesseraVertex data, uploads a GPU mesh, and gathers clip
 * metadata (names + durations) into the caller's arena. Only the supported
 * subset is honored: triangles, POSITION/NORMAL/TEXCOORD_0, one base color. */
#include "assets/assets.h"
#include "cgltf.h"
#include <stdlib.h>
#include <string.h>

/* Copy a NUL-terminated string into the arena. */
static char* arena_strdup(TsArena* arena, const char* s) {
    size_t len = strlen(s);
    char* out = (char*)ts_arena_alloc(arena, len + 1, 1);
    if (out) memcpy(out, s, len + 1);
    return out;
}

bool ts_gltf_import(TsGpu* gpu, TsArena* arena, const void* data, size_t size,
                    TsGltfResult* out, char* err, size_t err_sz) {
    memset(out, 0, sizeof *out);
    out->base_color[0] = out->base_color[1] = out->base_color[2] = out->base_color[3] = 1.0f;

    cgltf_options opts;
    memset(&opts, 0, sizeof opts);

    cgltf_data* cg = NULL;
    if (cgltf_parse(&opts, data, size, &cg) != cgltf_result_success || !cg) {
        snprintf(err, err_sz, "gltf: parse failed");
        return false;
    }

    /* Loads embedded/GLB buffers from memory; external .bin (path NULL) fails. */
    if (cgltf_load_buffers(&opts, cg, NULL) != cgltf_result_success) {
        snprintf(err, err_sz, "gltf: cannot load buffers (external buffers unsupported)");
        cgltf_free(cg);
        return false;
    }

    if (cg->meshes_count == 0) {
        snprintf(err, err_sz, "gltf: no meshes");
        cgltf_free(cg);
        return false;
    }

    const cgltf_mesh* mesh = &cg->meshes[0];

    /* Growing local vertex/index arrays. */
    TesseraVertex* verts = NULL;
    uint32_t* indices = NULL;
    uint32_t vcount = 0, icount = 0;
    uint32_t vcap = 0, icap = 0;
    bool oom = false;

    for (size_t p = 0; p < mesh->primitives_count && !oom; ++p) {
        const cgltf_primitive* prim = &mesh->primitives[p];
        if (prim->type != cgltf_primitive_type_triangles) continue;

        const cgltf_accessor* pos = NULL;
        const cgltf_accessor* nrm = NULL;
        const cgltf_accessor* uv0 = NULL;
        for (size_t a = 0; a < prim->attributes_count; ++a) {
            const cgltf_attribute* attr = &prim->attributes[a];
            if (attr->type == cgltf_attribute_type_position) pos = attr->data;
            else if (attr->type == cgltf_attribute_type_normal) nrm = attr->data;
            else if (attr->type == cgltf_attribute_type_texcoord && attr->index == 0) uv0 = attr->data;
        }
        if (!pos || pos->count == 0) continue;  /* nothing to draw */

        uint32_t base = vcount;
        uint32_t prim_vcount = (uint32_t)pos->count;

        /* Grow vertex array. */
        if (vcount + prim_vcount > vcap) {
            uint32_t ncap = vcap ? vcap * 2 : 256;
            while (ncap < vcount + prim_vcount) ncap *= 2;
            TesseraVertex* nv = (TesseraVertex*)realloc(verts, ncap * sizeof *nv);
            if (!nv) { oom = true; break; }
            verts = nv; vcap = ncap;
        }

        for (uint32_t i = 0; i < prim_vcount; ++i) {
            TesseraVertex* v = &verts[vcount + i];
            float tmp[3] = {0, 0, 0};
            cgltf_accessor_read_float(pos, i, tmp, 3);
            v->pos[0] = tmp[0]; v->pos[1] = tmp[1]; v->pos[2] = tmp[2];

            tmp[0] = 0; tmp[1] = 1; tmp[2] = 0;
            if (nrm) cgltf_accessor_read_float(nrm, i, tmp, 3);
            v->normal[0] = tmp[0]; v->normal[1] = tmp[1]; v->normal[2] = tmp[2];

            tmp[0] = 0; tmp[1] = 0;
            if (uv0) cgltf_accessor_read_float(uv0, i, tmp, 2);
            v->uv[0] = tmp[0]; v->uv[1] = tmp[1];
        }
        vcount += prim_vcount;

        /* Indices (default to sequential if absent). */
        uint32_t prim_icount = prim->indices ? (uint32_t)prim->indices->count : prim_vcount;
        if (icount + prim_icount > icap) {
            uint32_t ncap = icap ? icap * 2 : 512;
            while (ncap < icount + prim_icount) ncap *= 2;
            uint32_t* ni = (uint32_t*)realloc(indices, ncap * sizeof *ni);
            if (!ni) { oom = true; break; }
            indices = ni; icap = ncap;
        }
        for (uint32_t i = 0; i < prim_icount; ++i) {
            uint32_t idx = prim->indices
                ? (uint32_t)cgltf_accessor_read_index(prim->indices, i)
                : i;
            indices[icount + i] = base + idx;
        }
        icount += prim_icount;
    }

    if (oom) {
        snprintf(err, err_sz, "gltf: out of memory");
        free(verts); free(indices);
        cgltf_free(cg);
        return false;
    }
    if (icount == 0 || vcount == 0) {
        snprintf(err, err_sz, "gltf: no triangle geometry");
        free(verts); free(indices);
        cgltf_free(cg);
        return false;
    }

    bool ok = ts_gpu_upload_mesh(gpu, verts, vcount, indices, icount, &out->mesh);
    free(verts); free(indices);
    if (!ok) {
        snprintf(err, err_sz, "gltf: GPU mesh upload failed");
        cgltf_free(cg);
        return false;
    }
    out->has_mesh = true;

    /* Skinning info. */
    out->skinned = cg->skins_count > 0;
    out->joint_count = out->skinned ? (uint32_t)cg->skins[0].joints_count : 0;

    /* Base color from the first material. */
    if (cg->materials_count > 0 && cg->materials[0].has_pbr_metallic_roughness) {
        const cgltf_float* bc = cg->materials[0].pbr_metallic_roughness.base_color_factor;
        out->base_color[0] = bc[0];
        out->base_color[1] = bc[1];
        out->base_color[2] = bc[2];
        out->base_color[3] = bc[3];
    }

    /* Animation clips. */
    out->clip_count = (uint32_t)cg->animations_count;
    if (out->clip_count > 0) {
        out->clips = TS_ARENA_ARR(arena, TsClipInfo, out->clip_count);
        for (uint32_t c = 0; c < out->clip_count; ++c) {
            const cgltf_animation* anim = &cg->animations[c];
            char namebuf[32];
            const char* name = anim->name;
            if (!name) {
                snprintf(namebuf, sizeof namebuf, "clip%u", c);
                name = namebuf;
            }
            out->clips[c].name = arena_strdup(arena, name);

            float duration = 0.0f;
            for (size_t s = 0; s < anim->samplers_count; ++s) {
                const cgltf_accessor* in = anim->samplers[s].input;
                if (!in || in->count == 0) continue;
                float end = 0.0f;
                if (cgltf_accessor_read_float(in, in->count - 1, &end, 1) && end > duration)
                    duration = end;
            }
            out->clips[c].duration = duration;
        }
    }

    cgltf_free(cg);
    return true;
}
