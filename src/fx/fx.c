/* fx.c — CPU particle simulation + GPU billboard rendering (M6). */
#include "fx/fx.h"
#include "engine.h"
#include "state.h"
#include "orchestration/orch.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TS_FX_GLOBAL_CAP 4096u   /* max live particles across all emitters */

/* ------------------------------------------------------------------ rng */
static inline uint32_t xrng(uint32_t* s) {
    uint32_t x = *s ? *s : 0x9e3779b9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return x;
}
static inline float frand(uint32_t* s) { return (float)(xrng(s) & 0xFFFFFF) / (float)0xFFFFFF; }
static inline float frand_sym(uint32_t* s) { return frand(s) * 2.0f - 1.0f; }

/* ------------------------------------------------------------- lifecycle */
struct TsFx* ts_fx_create(void) {
    struct TsFx* fx = (struct TsFx*)calloc(1, sizeof *fx);
    if (fx) fx->global_cap = TS_FX_GLOBAL_CAP;
    return fx;
}

void ts_fx_destroy(struct TsFx* fx, TsGpu* gpu) {
    if (!fx) return;
    for (size_t i = 0; i < fx->count; ++i) free(fx->emitters[i].parts);
    free(fx->emitters);
    free(fx->scratch);
    if (gpu && gpu->device) {
        if (fx->vbo) SDL_ReleaseGPUBuffer(gpu->device, fx->vbo);
        if (fx->ibo) SDL_ReleaseGPUBuffer(gpu->device, fx->ibo);
    }
    free(fx);
}

static TsEmitter* fx_add_emitter(struct TsFx* fx) {
    if (fx->count == fx->cap) {
        size_t nc = fx->cap ? fx->cap * 2 : 8;
        TsEmitter* ne = (TsEmitter*)realloc(fx->emitters, nc * sizeof(TsEmitter));
        if (!ne) return NULL;
        fx->emitters = ne; fx->cap = nc;
    }
    TsEmitter* em = &fx->emitters[fx->count++];
    memset(em, 0, sizeof *em);
    return em;
}

/* Total live particles across all emitters (for the global cap). */
static size_t fx_live_particles(const struct TsFx* fx) {
    size_t n = 0;
    for (size_t i = 0; i < fx->count; ++i) n += fx->emitters[i].parts ? fx->emitters[i].part_count : 0;
    return n;
}

/* ------------------------------------------------------------- emission */
static void emit_one(TsEmitter* em, uint32_t* seed) {
    if (em->part_count == em->part_cap) {
        size_t nc = em->part_cap ? em->part_cap * 2 : 16;
        TsParticle* np = (TsParticle*)realloc(em->parts, nc * sizeof(TsParticle));
        if (!np) return;
        em->parts = np; em->part_cap = nc;
    }
    TsParticle* p = &em->parts[em->part_count++];
    const TesseraParticleSpec* s = &em->spec;

    /* cone around +Y with spread half-angle */
    float spread = s->spread_deg * (float)M_PI / 180.0f;
    float ct = cosf(spread);
    float u = frand(seed);
    float cos_theta = 1.0f - u * (1.0f - ct);
    float sin_theta = sqrtf(fmaxf(0.0f, 1.0f - cos_theta * cos_theta));
    float phi = frand(seed) * 2.0f * (float)M_PI;
    vec3 dir = { sin_theta * cosf(phi), cos_theta, sin_theta * sinf(phi) };

    float speed = s->speed + frand_sym(seed) * s->speed_var;
    p->vel[0] = dir[0] * speed;
    p->vel[1] = dir[1] * speed;
    p->vel[2] = dir[2] * speed;
    glm_vec3_copy(em->anchor, p->pos);
    p->age = 0.0f;
    p->life = fmaxf(0.05f, s->lifetime_s + frand_sym(seed) * s->lifetime_var);
    p->seed = xrng(seed);
}

void ts_fx_spawn(struct TsFx* fx, TesseraEngine* e, const TesseraParticleSpec* spec,
                 const vec3 anchor, TesseraEntityId attach, TesseraCardId attach_card) {
    if (!fx || !spec) return;
    if (spec->count == 0 && spec->mode == TESSERA_EMIT_BURST) return;

    TsEmitter* em = fx_add_emitter(fx);
    if (!em) return;
    em->spec = *spec;
    em->texture = ts_registry_particle_texture(&e->registry, spec->atlas);
    glm_vec3_copy((float*)anchor, em->anchor);
    em->attach = attach;
    em->attach_card = attach_card;
    em->age = 0.0f;
    em->emit_accum = 0.0f;
    em->additive = (spec->blend == TESSERA_BLEND_ADD);
    em->alive = true;
    em->parts = NULL; em->part_count = em->part_cap = 0;

    uint32_t seed = (uint32_t)(fx->count * 2654435761u) ^ (uint32_t)attach ^ 0x1234567u;
    if (spec->mode == TESSERA_EMIT_BURST) {
        size_t budget = fx->global_cap > fx_live_particles(fx) ? fx->global_cap - fx_live_particles(fx) : 0;
        uint32_t n = spec->count < budget ? spec->count : (uint32_t)budget;
        for (uint32_t i = 0; i < n; ++i) emit_one(em, &seed);
    }
}

/* ------------------------------------------------------------- advance */
void ts_fx_advance(TesseraEngine* e, float dt) {
    struct TsFx* fx = e->fx;
    if (!fx || dt <= 0.0f) return;

    for (size_t i = 0; i < fx->count;) {
        TsEmitter* em = &fx->emitters[i];

        /* follow an attached card (wins over the entity) or entity */
        if (em->attach_card && e->orch) {
            vec3 p;
            if (ts_orch_card_pos(e->orch, em->attach_card, p)) {
                p[1] += 0.05f; /* just clear of the displayed face */
                glm_vec3_copy(p, em->anchor);
            }
        } else if (em->attach && e->orch) {
            vec3 p;
            if (ts_orch_entity_pos(e->orch, em->attach, p)) {
                p[1] += 0.5f;  /* aura sits around the body, not the feet */
                glm_vec3_copy(p, em->anchor);
            }
        }

        em->age += dt;

        /* continuous emission within the duration window */
        if (em->spec.mode == TESSERA_EMIT_CONTINUOUS && em->age <= em->spec.duration_s) {
            uint32_t seed = em->parts && em->part_count ? em->parts[em->part_count - 1].seed
                                                        : (uint32_t)(0xABCDu + i);
            em->emit_accum += (float)em->spec.count * dt;
            size_t budget = fx->global_cap > fx_live_particles(fx) ? fx->global_cap - fx_live_particles(fx) : 0;
            while (em->emit_accum >= 1.0f && budget > 0) {
                emit_one(em, &seed);
                em->emit_accum -= 1.0f;
                --budget;
            }
            /* Don't let the backlog build up while the global cap is saturated,
             * otherwise freed slots would dump a catch-up burst instead of
             * resuming at the intended rate. Keep at most ~1 frame of slack. */
            if (em->emit_accum > 1.0f) em->emit_accum = 1.0f;
        }

        /* integrate particles, compacting dead ones out */
        size_t w = 0;
        for (size_t k = 0; k < em->part_count; ++k) {
            TsParticle* p = &em->parts[k];
            p->age += dt;
            if (p->age >= p->life) continue;
            p->vel[1] += em->spec.gravity * dt;
            p->pos[0] += p->vel[0] * dt;
            p->pos[1] += p->vel[1] * dt;
            p->pos[2] += p->vel[2] * dt;
            if (w != k) em->parts[w] = *p;
            ++w;
        }
        em->part_count = w;

        /* retire: past its emission window (or burst) and no particles left */
        bool emitting = (em->spec.mode == TESSERA_EMIT_CONTINUOUS) && (em->age <= em->spec.duration_s);
        if (!emitting && em->part_count == 0) {
            free(em->parts);
            fx->emitters[i] = fx->emitters[fx->count - 1];
            fx->count--;
            continue;
        }
        ++i;
    }
}

bool ts_fx_is_idle(const struct TsFx* fx) {
    return !fx || fx->count == 0;
}

/* ------------------------------------------------------- diff integration */
static bool snapshot_has_effect(const struct TsSnapshot* s, TesseraEntityId id) {
    if (!s) return false;
    for (size_t i = 0; i < s->effect_count; ++i)
        if (s->effects[i].id == id) return true;
    return false;
}
static bool snapshot_has_entity_id(const struct TsSnapshot* s, TesseraEntityId id) {
    if (!s) return false;
    for (size_t i = 0; i < s->entity_count; ++i)
        if (s->entities[i].id == id) return true;
    return false;
}

/* Resolve an effect placement's world anchor: its attach card's displayed
 * face, its attach entity, or the tile centre. */
static void effect_anchor(TesseraEngine* e, const TesseraEffectPlacement* ep, vec3 out) {
    if (ep->attach_card_id && e->orch &&
        ts_orch_card_pos(e->orch, ep->attach_card_id, out)) {
        out[1] += 0.05f; /* just clear of the displayed face */
        return;
    }
    if (ep->attach_entity_id && e->orch &&
        ts_orch_entity_pos(e->orch, ep->attach_entity_id, out))
        return;
    ts_grid_to_world(ep->coord.x, ep->coord.y, out);
}

void ts_fx_on_promote(TesseraEngine* e, const struct TsSnapshot* prev,
                      const struct TsSnapshot* next) {
    if (!e->fx) return;

    /* effects added -> on_add burst; effects removed -> on_remove burst */
    if (next) {
        for (size_t i = 0; i < next->effect_count; ++i) {
            const TesseraEffectPlacement* ep = &next->effects[i];
            if (snapshot_has_effect(prev, ep->id)) continue;
            TsDef* d = ts_registry_get(&e->registry, ep->def, TS_DEF_EFFECT);
            if (!d) continue;
            vec3 a; effect_anchor(e, ep, a);
            ts_fx_spawn(e->fx, e, &d->as.effect.spec.on_add, a,
                        ep->attach_entity_id, ep->attach_card_id);
        }
    }
    if (prev) {
        for (size_t i = 0; i < prev->effect_count; ++i) {
            const TesseraEffectPlacement* ep = &prev->effects[i];
            if (snapshot_has_effect(next, ep->id)) continue;
            TsDef* d = ts_registry_get(&e->registry, ep->def, TS_DEF_EFFECT);
            if (!d) continue;
            vec3 a; effect_anchor(e, ep, a);
            ts_fx_spawn(e->fx, e, &d->as.effect.spec.on_remove, a, 0, 0);
        }
    }

    /* entity spawn/despawn linked effects */
    if (next) {
        for (size_t i = 0; i < next->entity_count; ++i) {
            const TesseraEntityPlacement* np = &next->entities[i];
            if (snapshot_has_entity_id(prev, np->id)) continue;
            TsDef* d = ts_registry_get(&e->registry, np->def, TS_DEF_ENTITY);
            if (!d || !d->as.entity.spec.on_spawn_effect) continue;
            TsDef* ed = ts_registry_get(&e->registry, d->as.entity.spec.on_spawn_effect, TS_DEF_EFFECT);
            if (!ed) continue;
            vec3 a; ts_grid_to_world(np->coord.x, np->coord.y, a);
            ts_fx_spawn(e->fx, e, &ed->as.effect.spec.on_add, a, np->id, 0);
        }
    }
    if (prev) {
        for (size_t i = 0; i < prev->entity_count; ++i) {
            const TesseraEntityPlacement* pp = &prev->entities[i];
            if (snapshot_has_entity_id(next, pp->id)) continue;
            TsDef* d = ts_registry_get(&e->registry, pp->def, TS_DEF_ENTITY);
            if (!d || !d->as.entity.spec.on_despawn_effect) continue;
            TsDef* ed = ts_registry_get(&e->registry, d->as.entity.spec.on_despawn_effect, TS_DEF_EFFECT);
            if (!ed) continue;
            vec3 a; ts_grid_to_world(pp->coord.x, pp->coord.y, a);
            ts_fx_spawn(e->fx, e, &ed->as.effect.spec.on_remove, a, 0, 0);
        }
    }
}

/* --------------------------------------------------------------- render */
static void ensure_index_buffer(struct TsFx* fx, TsGpu* g) {
    size_t quads = fx->global_cap;
    size_t need_i = quads * 6;
    if (fx->ibo && fx->ibo_cap >= need_i) return;
    if (fx->ibo) SDL_ReleaseGPUBuffer(g->device, fx->ibo);

    uint32_t* idx = (uint32_t*)malloc(need_i * sizeof(uint32_t));
    for (size_t q = 0; q < quads; ++q) {
        uint32_t b = (uint32_t)(q * 4);
        uint32_t* o = &idx[q * 6];
        o[0] = b + 0; o[1] = b + 1; o[2] = b + 2;
        o[3] = b + 0; o[4] = b + 2; o[5] = b + 3;
    }
    SDL_GPUBufferCreateInfo ici = { .usage = SDL_GPU_BUFFERUSAGE_INDEX,
                                    .size = (uint32_t)(need_i * sizeof(uint32_t)) };
    fx->ibo = SDL_CreateGPUBuffer(g->device, &ici);
    fx->ibo_cap = need_i;

    SDL_GPUTransferBufferCreateInfo tci = { .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
                                            .size = (uint32_t)(need_i * sizeof(uint32_t)) };
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(g->device, &tci);
    void* m = SDL_MapGPUTransferBuffer(g->device, tb, false);
    memcpy(m, idx, need_i * sizeof(uint32_t));
    SDL_UnmapGPUTransferBuffer(g->device, tb);
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(g->device);
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTransferBufferLocation src = { .transfer_buffer = tb, .offset = 0 };
    SDL_GPUBufferRegion dst = { .buffer = fx->ibo, .offset = 0, .size = (uint32_t)(need_i * sizeof(uint32_t)) };
    SDL_UploadToGPUBuffer(cp, &src, &dst, false);
    SDL_EndGPUCopyPass(cp);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(g->device, tb);
    free(idx);
}

/* Append a particle's 4 verts to the scratch buffer. */
static void push_quad(TsParticleVertex* v, const TsParticle* p, const TesseraParticleSpec* s,
                      const vec3 right, const vec3 up) {
    float t = p->age / p->life;
    if (t < 0) t = 0; if (t > 1) t = 1;
    float size = ts_lerpf(s->size_start, s->size_end, t) * 0.5f;
    float col[4];
    for (int c = 0; c < 4; ++c) col[c] = ts_lerpf(s->color_start[c], s->color_end[c], t);

    vec3 r, u;
    glm_vec3_scale((float*)right, size, r);
    glm_vec3_scale((float*)up, size, u);

    float u0 = s->sprite.u0, v0 = s->sprite.v0, u1 = s->sprite.u1, v1 = s->sprite.v1;
    if (u0 == 0 && v0 == 0 && u1 == 0 && v1 == 0) { u1 = 1; v1 = 1; }

    /* corners: -r-u, +r-u, +r+u, -r+u */
    const float sx[4] = {-1, 1, 1, -1};
    const float sy[4] = {-1, -1, 1, 1};
    const float uu[4] = {u0, u1, u1, u0};
    const float vv[4] = {v1, v1, v0, v0};
    for (int i = 0; i < 4; ++i) {
        v[i].pos[0] = p->pos[0] + r[0] * sx[i] + u[0] * sy[i];
        v[i].pos[1] = p->pos[1] + r[1] * sx[i] + u[1] * sy[i];
        v[i].pos[2] = p->pos[2] + r[2] * sx[i] + u[2] * sy[i];
        v[i].uv[0] = uu[i]; v[i].uv[1] = vv[i];
        v[i].color[0] = col[0]; v[i].color[1] = col[1];
        v[i].color[2] = col[2]; v[i].color[3] = col[3];
    }
}

void ts_fx_prepare(TesseraEngine* e, SDL_GPUCommandBuffer* cmd) {
    struct TsFx* fx = e->fx;
    if (!fx || fx->count == 0) { if (fx) { fx->add_vcount = fx->alpha_vcount = 0; } return; }
    TsGpu* g = &e->gpu;

    size_t live = fx_live_particles(fx);
    if (live == 0) { fx->add_vcount = fx->alpha_vcount = 0; return; }
    if (live > fx->global_cap) live = fx->global_cap;

    /* camera billboard basis */
    vec3 fwd, right, up;
    glm_vec3_sub(e->camera.focus, e->camera.eye, fwd);
    glm_vec3_normalize(fwd);
    glm_vec3_cross(fwd, (vec3){0, 1, 0}, right);
    if (glm_vec3_norm(right) < 1e-4f) glm_vec3_copy((vec3){1, 0, 0}, right);
    glm_vec3_normalize(right);
    glm_vec3_cross(right, fwd, up);
    glm_vec3_normalize(up);

    size_t need_v = live * 4;
    if (fx->scratch_cap < need_v) {
        TsParticleVertex* ns = (TsParticleVertex*)realloc(fx->scratch, need_v * sizeof(TsParticleVertex));
        if (!ns) { fx->add_vcount = fx->alpha_vcount = 0; return; }  /* no stale redraw */
        fx->scratch = ns; fx->scratch_cap = need_v;
    }

    /* additive particles first, then alpha (two contiguous ranges) */
    uint32_t vi = 0;
    for (int pass = 0; pass < 2; ++pass) {
        bool want_add = (pass == 0);
        if (pass == 1) { fx->add_vcount = vi; fx->alpha_voffset = vi; }
        for (size_t ei = 0; ei < fx->count; ++ei) {
            TsEmitter* em = &fx->emitters[ei];
            if (em->additive != want_add) continue;
            for (size_t k = 0; k < em->part_count && vi + 4 <= need_v; ++k) {
                push_quad(&fx->scratch[vi], &em->parts[k], &em->spec, right, up);
                vi += 4;
            }
        }
    }
    fx->alpha_vcount = vi - fx->alpha_voffset;
    uint32_t total_v = vi;
    if (total_v == 0) return;

    ensure_index_buffer(fx, g);

    /* (re)create the dynamic vertex buffer if it must grow */
    if (!fx->vbo || fx->vbo_cap < total_v) {
        if (fx->vbo) SDL_ReleaseGPUBuffer(g->device, fx->vbo);
        size_t cap = fx->global_cap * 4;
        SDL_GPUBufferCreateInfo vci = { .usage = SDL_GPU_BUFFERUSAGE_VERTEX,
                                        .size = (uint32_t)(cap * sizeof(TsParticleVertex)) };
        fx->vbo = SDL_CreateGPUBuffer(g->device, &vci);
        fx->vbo_cap = cap;
    }

    /* upload verts via a copy pass on the frame's command buffer */
    uint32_t bytes = total_v * (uint32_t)sizeof(TsParticleVertex);
    SDL_GPUTransferBufferCreateInfo tci = { .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = bytes };
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(g->device, &tci);
    void* m = SDL_MapGPUTransferBuffer(g->device, tb, false);
    memcpy(m, fx->scratch, bytes);
    SDL_UnmapGPUTransferBuffer(g->device, tb);
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTransferBufferLocation src = { .transfer_buffer = tb, .offset = 0 };
    SDL_GPUBufferRegion dst = { .buffer = fx->vbo, .offset = 0, .size = bytes };
    SDL_UploadToGPUBuffer(cp, &src, &dst, false);
    SDL_EndGPUCopyPass(cp);
    SDL_ReleaseGPUTransferBuffer(g->device, tb);   /* released after submit completes */
}

static void draw_range(SDL_GPURenderPass* pass, TsGpu* g, struct TsFx* fx,
                       SDL_GPUGraphicsPipeline* pipe, SDL_GPUTexture* tex,
                       uint32_t voffset, uint32_t vcount) {
    if (vcount == 0) return;
    uint32_t quads = vcount / 4;
    SDL_BindGPUGraphicsPipeline(pass, pipe);
    SDL_GPUBufferBinding vb = { .buffer = fx->vbo, .offset = 0 };
    SDL_GPUBufferBinding ib = { .buffer = fx->ibo, .offset = 0 };
    SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
    SDL_BindGPUIndexBuffer(pass, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    SDL_GPUTextureSamplerBinding tsb = { .texture = tex, .sampler = g->linear_sampler };
    SDL_BindGPUFragmentSamplers(pass, 0, &tsb, 1);
    SDL_DrawGPUIndexedPrimitives(pass, quads * 6, 1, 0, (Sint32)voffset, 0);
}

void ts_fx_record(TesseraEngine* e, SDL_GPUCommandBuffer* cmd,
                  SDL_GPURenderPass* pass, const TsFrameUniform* fu) {
    struct TsFx* fx = e->fx;
    if (!fx || (fx->add_vcount == 0 && fx->alpha_vcount == 0)) return;
    TsGpu* g = &e->gpu;
    if (!g->particle_add || !g->particle_alpha || !fx->vbo || !fx->ibo) return;

    SDL_PushGPUVertexUniformData(cmd, 0, fu, sizeof *fu);

    /* One texture per group is a simplification: use the first live emitter's
     * atlas of each blend type. Table-top VFX usually share a sheet. */
    SDL_GPUTexture* add_tex = e->registry.particle_dot.texture;
    SDL_GPUTexture* alpha_tex = e->registry.particle_dot.texture;
    for (size_t i = 0; i < fx->count; ++i) {
        if (fx->emitters[i].additive) { add_tex = fx->emitters[i].texture; break; }
    }
    for (size_t i = 0; i < fx->count; ++i) {
        if (!fx->emitters[i].additive) { alpha_tex = fx->emitters[i].texture; break; }
    }

    draw_range(pass, g, fx, g->particle_add, add_tex, 0, fx->add_vcount);
    draw_range(pass, g, fx, g->particle_alpha, alpha_tex, fx->alpha_voffset, fx->alpha_vcount);
}
