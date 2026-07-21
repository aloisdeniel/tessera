/* gltf.c — glTF/GLB import (M2 mesh, M5 skinning + animation).
 *
 * Parses model bytes with cgltf, flattens the first mesh's triangle primitives
 * into interleaved vertex data (static TesseraVertex, or TsSkinnedVertex when a
 * skin is present), uploads a GPU mesh, and — for skinned models — extracts a
 * runtime skeleton (joint hierarchy, inverse-bind, bind-pose locals) and the
 * animation clips (TRS channels) into the caller's arena. */
#include "assets/assets.h"
#include "anim/skeleton.h"
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

/* Decompose a node's local transform into TRS. */
static void node_local_trs(const cgltf_node* n, vec3 t, versor r, vec3 s) {
    glm_vec3_zero(t); glm_quat_identity(r); glm_vec3_one(s);
    if (n->has_matrix) {
        mat4 m;
        memcpy(m, n->matrix, 16 * sizeof(float));
        t[0] = m[3][0]; t[1] = m[3][1]; t[2] = m[3][2];
        s[0] = glm_vec3_norm(m[0]);
        s[1] = glm_vec3_norm(m[1]);
        s[2] = glm_vec3_norm(m[2]);
        /* A mirrored basis (det < 0) can't be a proper rotation; fold the flip
         * into a negative X scale so glm_mat4_quat gets an orthonormal rotation. */
        float det = glm_mat4_det((vec4*)m);
        if (det < 0.0f) s[0] = -s[0];
        mat4 rot; glm_mat4_identity(rot);
        for (int c = 0; c < 3; ++c) {
            float inv = fabsf(s[c]) > 1e-6f ? 1.0f / s[c] : 0.0f;
            rot[c][0] = m[c][0] * inv; rot[c][1] = m[c][1] * inv; rot[c][2] = m[c][2] * inv;
        }
        glm_mat4_quat(rot, r);
    } else {
        if (n->has_translation) glm_vec3_copy((float*)n->translation, t);
        if (n->has_rotation) { r[0] = n->rotation[0]; r[1] = n->rotation[1];
                               r[2] = n->rotation[2]; r[3] = n->rotation[3]; }
        if (n->has_scale) glm_vec3_copy((float*)n->scale, s);
    }
}

/* Index of `node` within skin->joints, or -1. */
static int joint_index_of(const cgltf_skin* skin, const cgltf_node* node) {
    if (!node) return -1;
    for (size_t i = 0; i < skin->joints_count; ++i)
        if (skin->joints[i] == node) return (int)i;
    return -1;
}

/* Extract the runtime skeleton from skin[0] into the arena. Returns false on an
 * unsupported rig (too many joints). */
static bool extract_skeleton(const cgltf_skin* skin, TsSkeleton* sk,
                             char* err, size_t err_sz) {
    if (skin->joints_count > TS_MAX_JOINTS) {
        snprintf(err, err_sz, "gltf: %zu joints exceeds cap %d",
                 skin->joints_count, TS_MAX_JOINTS);
        return false;
    }
    sk->joint_count = (uint32_t)skin->joints_count;
    for (uint32_t i = 0; i < sk->joint_count; ++i) {
        const cgltf_node* jn = skin->joints[i];
        TsJoint* j = &sk->joints[i];
        j->parent = joint_index_of(skin, jn->parent);
        node_local_trs(jn, j->t, j->r, j->s);
        glm_mat4_identity(j->inverse_bind);
        if (skin->inverse_bind_matrices) {
            float m[16];
            if (cgltf_accessor_read_float(skin->inverse_bind_matrices, i, m, 16))
                memcpy(j->inverse_bind, m, 16 * sizeof(float));
        }
    }
    return true;
}

static TsChannelPath map_path(cgltf_animation_path_type p, bool* ok) {
    *ok = true;
    switch (p) {
    case cgltf_animation_path_type_translation: return TS_PATH_T;
    case cgltf_animation_path_type_rotation:    return TS_PATH_R;
    case cgltf_animation_path_type_scale:       return TS_PATH_S;
    default: *ok = false; return TS_PATH_T;   /* weights/unsupported skipped */
    }
}

/* Extract all animation clips into the arena. */
static void extract_clips(const cgltf_data* cg, const cgltf_skin* skin,
                          TsArena* arena, TsSkinData* skin_data) {
    uint32_t nclips = (uint32_t)cg->animations_count;
    skin_data->clip_count = nclips;
    if (nclips == 0) { skin_data->clips = NULL; return; }
    skin_data->clips = TS_ARENA_ARR(arena, TsClip, nclips);

    for (uint32_t c = 0; c < nclips; ++c) {
        const cgltf_animation* anim = &cg->animations[c];
        TsClip* clip = &skin_data->clips[c];
        char namebuf[32];
        const char* name = anim->name;
        if (!name) { snprintf(namebuf, sizeof namebuf, "clip%u", c); name = namebuf; }
        clip->name = arena_strdup(arena, name);
        clip->duration = 0.0f;

        /* count usable channels (targeting a joint with a supported path) */
        uint32_t used = 0;
        for (size_t ch = 0; ch < anim->channels_count; ++ch) {
            bool ok; map_path(anim->channels[ch].target_path, &ok);
            if (ok && joint_index_of(skin, anim->channels[ch].target_node) >= 0) ++used;
        }
        /* Allocate for the upper-bound `used`, but the write pass below applies
         * stricter guards (null sampler, empty/zero-key accessors). Record the
         * ACTUAL number written as channel_count so ts_clip_sample never reads
         * an unwritten (uninitialised arena) TsChannel. */
        clip->channels = used ? TS_ARENA_ARR(arena, TsChannel, used) : NULL;

        uint32_t w = 0;
        for (size_t ci = 0; ci < anim->channels_count; ++ci) {
            const cgltf_animation_channel* src = &anim->channels[ci];
            bool ok; TsChannelPath path = map_path(src->target_path, &ok);
            int ji = joint_index_of(skin, src->target_node);
            if (!ok || ji < 0 || !src->sampler) continue;

            const cgltf_accessor* in = src->sampler->input;
            const cgltf_accessor* out = src->sampler->output;
            if (!in || !out || in->count == 0) continue;

            TsChannel* dst = &clip->channels[w++];
            dst->joint = (uint32_t)ji;
            dst->path = path;
            dst->interp = (src->sampler->interpolation == cgltf_interpolation_type_step)
                          ? TS_INTERP_STEP : TS_INTERP_LINEAR;
            dst->comps = (path == TS_PATH_R) ? 4 : 3;
            dst->key_count = (uint32_t)in->count;
            dst->times = TS_ARENA_ARR(arena, float, dst->key_count);
            dst->values = TS_ARENA_ARR(arena, float, dst->key_count * dst->comps);
            for (uint32_t k = 0; k < dst->key_count; ++k) {
                float tv = 0.0f;
                cgltf_accessor_read_float(in, k, &tv, 1);
                dst->times[k] = tv;
                if (tv > clip->duration) clip->duration = tv;
                cgltf_accessor_read_float(out, k, &dst->values[k * dst->comps], dst->comps);
            }
        }
        clip->channel_count = w;   /* actual channels written, not the estimate */
    }
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
    const cgltf_skin* skin = cg->skins_count > 0 ? &cg->skins[0] : NULL;
    bool skinned = skin != NULL;

    /* Growing local arrays (one of static/skinned is used per model). */
    TesseraVertex*   sverts = NULL;
    TsSkinnedVertex* kverts = NULL;
    uint32_t* indices = NULL;
    uint32_t vcount = 0, icount = 0, vcap = 0, icap = 0;
    bool oom = false;

    for (size_t p = 0; p < mesh->primitives_count && !oom; ++p) {
        const cgltf_primitive* prim = &mesh->primitives[p];
        if (prim->type != cgltf_primitive_type_triangles) continue;

        const cgltf_accessor *pos = NULL, *nrm = NULL, *uv0 = NULL, *jnt = NULL, *wgt = NULL;
        for (size_t a = 0; a < prim->attributes_count; ++a) {
            const cgltf_attribute* attr = &prim->attributes[a];
            if (attr->type == cgltf_attribute_type_position) pos = attr->data;
            else if (attr->type == cgltf_attribute_type_normal) nrm = attr->data;
            else if (attr->type == cgltf_attribute_type_texcoord && attr->index == 0) uv0 = attr->data;
            else if (attr->type == cgltf_attribute_type_joints && attr->index == 0) jnt = attr->data;
            else if (attr->type == cgltf_attribute_type_weights && attr->index == 0) wgt = attr->data;
        }
        if (!pos || pos->count == 0) continue;

        uint32_t base = vcount;
        uint32_t pvc = (uint32_t)pos->count;
        if (vcount + pvc > vcap) {
            uint32_t ncap = vcap ? vcap * 2 : 256;
            while (ncap < vcount + pvc) ncap *= 2;
            if (skinned) {
                TsSkinnedVertex* nv = (TsSkinnedVertex*)realloc(kverts, ncap * sizeof *nv);
                if (!nv) { oom = true; break; }
                kverts = nv;
            } else {
                TesseraVertex* nv = (TesseraVertex*)realloc(sverts, ncap * sizeof *nv);
                if (!nv) { oom = true; break; }
                sverts = nv;
            }
            vcap = ncap;
        }

        for (uint32_t i = 0; i < pvc; ++i) {
            float pv[3] = {0,0,0}, nv3[3] = {0,1,0}, uv[2] = {0,0};
            cgltf_accessor_read_float(pos, i, pv, 3);
            if (nrm) cgltf_accessor_read_float(nrm, i, nv3, 3);
            if (uv0) cgltf_accessor_read_float(uv0, i, uv, 2);
            if (skinned) {
                TsSkinnedVertex* v = &kverts[vcount + i];
                v->pos[0]=pv[0]; v->pos[1]=pv[1]; v->pos[2]=pv[2];
                v->normal[0]=nv3[0]; v->normal[1]=nv3[1]; v->normal[2]=nv3[2];
                v->uv[0]=uv[0]; v->uv[1]=uv[1];
                cgltf_uint ji[4] = {0,0,0,0};
                float wt[4] = {0,0,0,0};
                if (jnt) cgltf_accessor_read_uint(jnt, i, ji, 4);
                if (wgt) cgltf_accessor_read_float(wgt, i, wt, 4);
                for (int c = 0; c < 4; ++c)
                    v->joints[c] = (uint8_t)(ji[c] < TS_MAX_JOINTS ? ji[c] : 0);
                float wsum = wt[0]+wt[1]+wt[2]+wt[3];
                if (wsum <= 1e-6f) { wt[0] = 1.0f; wsum = 1.0f; }
                for (int c = 0; c < 4; ++c) v->weights[c] = wt[c] / wsum;
            } else {
                TesseraVertex* v = &sverts[vcount + i];
                v->pos[0]=pv[0]; v->pos[1]=pv[1]; v->pos[2]=pv[2];
                v->normal[0]=nv3[0]; v->normal[1]=nv3[1]; v->normal[2]=nv3[2];
                v->uv[0]=uv[0]; v->uv[1]=uv[1];
            }
        }
        vcount += pvc;

        uint32_t pic = prim->indices ? (uint32_t)prim->indices->count : pvc;
        if (icount + pic > icap) {
            uint32_t ncap = icap ? icap * 2 : 512;
            while (ncap < icount + pic) ncap *= 2;
            uint32_t* ni = (uint32_t*)realloc(indices, ncap * sizeof *ni);
            if (!ni) { oom = true; break; }
            indices = ni; icap = ncap;
        }
        for (uint32_t i = 0; i < pic; ++i) {
            uint32_t idx = prim->indices ? (uint32_t)cgltf_accessor_read_index(prim->indices, i) : i;
            indices[icount + i] = base + idx;
        }
        icount += pic;
    }

    if (oom) { snprintf(err, err_sz, "gltf: out of memory"); goto fail; }
    if (icount == 0 || vcount == 0) { snprintf(err, err_sz, "gltf: no triangle geometry"); goto fail; }

    bool ok;
    if (skinned)
        ok = ts_gpu_upload_mesh_raw(gpu, kverts, vcount, (uint32_t)sizeof(TsSkinnedVertex),
                                    indices, icount, &out->mesh);
    else
        ok = ts_gpu_upload_mesh(gpu, sverts, vcount, indices, icount, &out->mesh);
    if (!ok) { snprintf(err, err_sz, "gltf: GPU mesh upload failed"); goto fail; }
    out->has_mesh = true;

    out->skinned = skinned;
    out->joint_count = skinned ? (uint32_t)skin->joints_count : 0;

    if (skinned) {
        out->skin = TS_ARENA_NEW(arena, TsSkinData);
        memset(out->skin, 0, sizeof(TsSkinData));
        if (!extract_skeleton(skin, &out->skin->skeleton, err, err_sz)) goto fail_uploaded;
        extract_clips(cg, skin, arena, out->skin);
    }

    /* Base color from the first material. */
    if (cg->materials_count > 0 && cg->materials[0].has_pbr_metallic_roughness) {
        const cgltf_float* bc = cg->materials[0].pbr_metallic_roughness.base_color_factor;
        out->base_color[0]=bc[0]; out->base_color[1]=bc[1];
        out->base_color[2]=bc[2]; out->base_color[3]=bc[3];
    }

    /* Clip name/duration metadata for introspection. */
    out->clip_count = (uint32_t)cg->animations_count;
    if (out->clip_count > 0) {
        out->clips = TS_ARENA_ARR(arena, TsClipInfo, out->clip_count);
        for (uint32_t c = 0; c < out->clip_count; ++c) {
            const cgltf_animation* anim = &cg->animations[c];
            char namebuf[32];
            const char* name = anim->name;
            if (!name) { snprintf(namebuf, sizeof namebuf, "clip%u", c); name = namebuf; }
            out->clips[c].name = arena_strdup(arena, name);
            float dur = (skinned && out->skin && c < out->skin->clip_count)
                        ? out->skin->clips[c].duration : 0.0f;
            if (dur == 0.0f) {
                for (size_t s = 0; s < anim->samplers_count; ++s) {
                    const cgltf_accessor* in = anim->samplers[s].input;
                    if (!in || in->count == 0) continue;
                    float end = 0.0f;
                    if (cgltf_accessor_read_float(in, in->count - 1, &end, 1) && end > dur) dur = end;
                }
            }
            out->clips[c].duration = dur;
        }
    }

    free(sverts); free(kverts); free(indices);
    cgltf_free(cg);
    return true;

fail_uploaded:
    ts_gpu_free_mesh(gpu, &out->mesh);
    out->has_mesh = false;
fail:
    free(sverts); free(kverts); free(indices);
    cgltf_free(cg);
    return false;
}
