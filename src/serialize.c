/* serialize.c — state <-> versioned little-endian blob, plus the replay
 * container (a timestamped sequence of state blobs).
 *
 * Pure data, engine-free. The wire walk below mirrors ts_snapshot_copy
 * (src/state.c): every field of every placement array — including the
 * entity/card multi-step move paths and the bounded label text — is written
 * explicitly, field by field, in declaration order. Nothing is memcpy'd as a
 * whole struct, so padding never leaks and the format is identical on every
 * host. serialize(deserialize(blob)) is byte-identical to `blob`.
 *
 * TRIPWIRE: the static asserts below pin the sizeof of TesseraState and of
 * every struct this file walks. Growing any of them (a new field or a new
 * state array) breaks this build on purpose — extend the wire walk in BOTH
 * tessera_state_serialize and tessera_state_deserialize (append-only; bump
 * TESSERA_STATE_BLOB_VERSION when old blobs need migrating), extend
 * ts_snapshot_copy, then update the assert. */
#include "tessera.h"

#include <stdlib.h>
#include <string.h>

#define TS_SER_MSG " changed — update the wire walk in src/serialize.c " \
                   "(serialize AND deserialize), then this assert"
_Static_assert(sizeof(TesseraState)              == 296, "TesseraState" TS_SER_MSG);
_Static_assert(sizeof(TesseraCamera)             ==  96, "TesseraCamera" TS_SER_MSG);
_Static_assert(sizeof(TesseraTilePlacement)      ==  24, "TesseraTilePlacement" TS_SER_MSG);
_Static_assert(sizeof(TesseraEntityPlacement)    ==  48, "TesseraEntityPlacement" TS_SER_MSG);
_Static_assert(sizeof(TesseraEffectPlacement)    ==  32, "TesseraEffectPlacement" TS_SER_MSG);
_Static_assert(sizeof(TesseraCardPlacement)      ==  88, "TesseraCardPlacement" TS_SER_MSG);
_Static_assert(sizeof(TesseraCardDrawPlacement)  ==  48, "TesseraCardDrawPlacement" TS_SER_MSG);
_Static_assert(sizeof(TesseraHandPlacement)      ==  56, "TesseraHandPlacement" TS_SER_MSG);
_Static_assert(sizeof(TesseraDicePlacement)      ==  40, "TesseraDicePlacement" TS_SER_MSG);
_Static_assert(sizeof(TesseraOverlayPlacement)   ==  68, "TesseraOverlayPlacement" TS_SER_MSG);
_Static_assert(sizeof(TesseraLabelPlacement)     == 128, "TesseraLabelPlacement" TS_SER_MSG);
_Static_assert(sizeof(TesseraHighlightPlacement) ==  48, "TesseraHighlightPlacement" TS_SER_MSG);
_Static_assert(sizeof(TesseraPointLightPlacement) == 40, "TesseraPointLightPlacement" TS_SER_MSG);
_Static_assert(sizeof(TesseraWorldModelPlacement) == 48, "TesseraWorldModelPlacement" TS_SER_MSG);

/* Blob header: magic u32, version u32, total size u64, epoch u64. */
#define TS_BLOB_HDR 24u

/* Serialized (wire) element sizes — NOT the in-memory sizeofs. */
#define TS_SER_CAMERA   88u
#define TS_SER_TILE     24u
#define TS_SER_ENTITY   30u   /* + path_count * 8  */
#define TS_SER_EFFECT   28u
#define TS_SER_CARD     65u   /* + path_count * 12 */
#define TS_SER_DRAW     45u
#define TS_SER_HAND     56u
#define TS_SER_DICE     36u
#define TS_SER_OVERLAY  68u
#define TS_SER_LABEL    121u
#define TS_SER_HL       48u
#define TS_SER_PLIGHT   40u
#define TS_SER_WMODEL   44u

/* ---- little-endian writer -------------------------------------------- */
typedef struct {
    uint8_t* buf;   /* NULL => measuring only */
    size_t   cap;
    size_t   off;   /* always advances; final value = required size */
} TsWr;

static void wr_bytes(TsWr* w, const void* p, size_t n) {
    if (w->buf && w->off + n <= w->cap) memcpy(w->buf + w->off, p, n);
    w->off += n;
}
static void wr_u8(TsWr* w, uint8_t v) { wr_bytes(w, &v, 1); }
static void wr_u16(TsWr* w, uint16_t v) {
    uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    wr_bytes(w, b, 2);
}
static void wr_u32(TsWr* w, uint32_t v) {
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8),
                     (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    wr_bytes(w, b, 4);
}
static void wr_u64(TsWr* w, uint64_t v) {
    wr_u32(w, (uint32_t)v);
    wr_u32(w, (uint32_t)(v >> 32));
}
static void wr_i32(TsWr* w, int32_t v)  { wr_u32(w, (uint32_t)v); }
static void wr_f32(TsWr* w, float v) {
    uint32_t u; memcpy(&u, &v, 4); wr_u32(w, u);
}
static void wr_coord(TsWr* w, TesseraCoord c) { wr_i32(w, c.x); wr_i32(w, c.y); }
static void wr_f32s(TsWr* w, const float* v, size_t n) {
    for (size_t i = 0; i < n; ++i) wr_f32(w, v[i]);
}

/* ---- bounds-checked little-endian reader ------------------------------ */
typedef struct {
    const uint8_t* p;
    size_t len, off;
    bool ok;            /* sticky: any out-of-bounds read poisons the parse */
} TsRd;

static bool rd_need(TsRd* r, size_t n) {
    if (!r->ok || n > r->len - r->off) { r->ok = false; return false; }
    return true;
}
static void rd_skip(TsRd* r, size_t n) { if (rd_need(r, n)) r->off += n; }
static void rd_bytes(TsRd* r, void* dst, size_t n) {
    if (!rd_need(r, n)) { memset(dst, 0, n); return; }
    memcpy(dst, r->p + r->off, n);
    r->off += n;
}
static uint8_t rd_u8(TsRd* r) {
    if (!rd_need(r, 1)) return 0;
    return r->p[r->off++];
}
static uint16_t rd_u16(TsRd* r) {
    if (!rd_need(r, 2)) return 0;
    uint16_t v = (uint16_t)(r->p[r->off] | (r->p[r->off + 1] << 8));
    r->off += 2;
    return v;
}
static uint32_t rd_u32(TsRd* r) {
    if (!rd_need(r, 4)) return 0;
    uint32_t v = (uint32_t)r->p[r->off]
               | ((uint32_t)r->p[r->off + 1] << 8)
               | ((uint32_t)r->p[r->off + 2] << 16)
               | ((uint32_t)r->p[r->off + 3] << 24);
    r->off += 4;
    return v;
}
static uint64_t rd_u64(TsRd* r) {
    uint64_t lo = rd_u32(r);
    uint64_t hi = rd_u32(r);
    return lo | (hi << 32);
}
static int32_t rd_i32(TsRd* r) { return (int32_t)rd_u32(r); }
static float rd_f32(TsRd* r) {
    uint32_t u = rd_u32(r);
    float v; memcpy(&v, &u, 4);
    return v;
}
static TesseraCoord rd_coord(TsRd* r) {
    TesseraCoord c; c.x = rd_i32(r); c.y = rd_i32(r);
    return c;
}
static void rd_f32s(TsRd* r, float* v, size_t n) {
    for (size_t i = 0; i < n; ++i) v[i] = rd_f32(r);
}

/* An array count read from the wire: reject anything whose fixed part alone
 * would not fit in the remaining bytes (also prevents size_t overflow). */
static size_t rd_count(TsRd* r, size_t elem_wire_size) {
    uint64_t n = rd_u64(r);
    if (!r->ok) return 0;
    if (n > (uint64_t)((r->len - r->off) / elem_wire_size)) { r->ok = false; return 0; }
    return (size_t)n;
}

/* ---- serialize --------------------------------------------------------- */
static void wr_camera(TsWr* w, const TesseraCamera* c) {
    wr_u32(w, c->mode);
    wr_f32(w, c->focus.x); wr_f32(w, c->focus.y);
    wr_f32(w, c->distance); wr_f32(w, c->yaw); wr_f32(w, c->pitch); wr_f32(w, c->fov);
    wr_f32s(w, c->position, 3);
    wr_f32s(w, c->orientation, 4);
    wr_f32s(w, c->target, 3);
    wr_u64(w, c->target_id);
    wr_u64(w, c->focus_card_id);
    wr_f32(w, c->fit_padding);
}

size_t tessera_state_serialize(const TesseraState* state, void* buf, size_t cap) {
    if (!state) return 0;
    TsWr w = { (uint8_t*)buf, buf ? cap : 0, 0 };

    /* header (total size is patched in at the end) */
    wr_u32(&w, TESSERA_STATE_BLOB_MAGIC);
    wr_u32(&w, TESSERA_STATE_BLOB_VERSION);
    wr_u64(&w, 0);                 /* total size placeholder */
    wr_u64(&w, state->epoch);      /* caller sequence number */

    wr_camera(&w, &state->camera);

    /* NULL arrays are empty regardless of the reported count, exactly like
     * ts_snapshot_copy. */
    size_t nt  = state->tiles      ? state->tile_count      : 0;
    size_t ne  = state->entities   ? state->entity_count    : 0;
    size_t nf  = state->effects    ? state->effect_count    : 0;
    size_t nc  = state->cards      ? state->card_count      : 0;
    size_t ncd = state->card_draws ? state->card_draw_count : 0;
    size_t nh  = state->hands      ? state->hand_count      : 0;
    size_t ndi = state->dice       ? state->dice_count      : 0;
    size_t nov = state->overlays   ? state->overlay_count   : 0;
    size_t nlb = state->labels     ? state->label_count     : 0;
    size_t nhl = state->highlights ? state->highlight_count : 0;
    size_t npl = state->point_lights ? state->point_light_count : 0;
    size_t nwm = state->world_models ? state->world_model_count : 0;

    wr_u64(&w, nt);
    for (size_t i = 0; i < nt; ++i) {
        const TesseraTilePlacement* t = &state->tiles[i];
        wr_coord(&w, t->coord);
        wr_u32(&w, t->tile_def);
        wr_u32(&w, t->variant);
        wr_u64(&w, t->id);
    }

    wr_u64(&w, ne);
    for (size_t i = 0; i < ne; ++i) {
        const TesseraEntityPlacement* p = &state->entities[i];
        uint32_t pc = p->path ? p->path_count : 0;
        wr_u64(&w, p->id);
        wr_u32(&w, p->def);
        wr_coord(&w, p->coord);
        wr_u16(&w, p->facing);
        wr_u32(&w, p->anim);
        wr_u32(&w, pc);
        for (uint32_t k = 0; k < pc; ++k) wr_coord(&w, p->path[k]);
    }

    wr_u64(&w, nf);
    for (size_t i = 0; i < nf; ++i) {
        const TesseraEffectPlacement* p = &state->effects[i];
        wr_u64(&w, p->id);
        wr_u32(&w, p->def);
        wr_coord(&w, p->coord);
        wr_u64(&w, p->attach_entity_id);
    }

    wr_u64(&w, nc);
    for (size_t i = 0; i < nc; ++i) {
        const TesseraCardPlacement* p = &state->cards[i];
        uint32_t pc = p->path ? p->path_count : 0;
        wr_u64(&w, p->id);
        wr_u32(&w, p->def);
        wr_f32s(&w, p->position, 3);
        wr_f32s(&w, p->orientation, 4);
        wr_u8(&w, p->hidden ? 1 : 0);
        wr_u64(&w, p->hand);
        wr_u32(&w, p->hand_slot);
        wr_u64(&w, p->source_draw);
        wr_u32(&w, pc);
        wr_f32s(&w, p->path, (size_t)pc * 3);
    }

    wr_u64(&w, ncd);
    for (size_t i = 0; i < ncd; ++i) {
        const TesseraCardDrawPlacement* p = &state->card_draws[i];
        wr_u64(&w, p->id);
        wr_u32(&w, p->def);
        wr_f32s(&w, p->position, 3);
        wr_f32s(&w, p->orientation, 4);
        wr_u32(&w, p->count);
        wr_u8(&w, p->top_hidden ? 1 : 0);
    }

    wr_u64(&w, nh);
    for (size_t i = 0; i < nh; ++i) {
        const TesseraHandPlacement* p = &state->hands[i];
        wr_u64(&w, p->id);
        wr_f32s(&w, p->position, 3);
        wr_f32s(&w, p->orientation, 4);
        wr_f32(&w, p->spread_deg);
        wr_f32(&w, p->radius);
        wr_f32(&w, p->card_spacing);
        wr_u64(&w, p->selected_card);
    }

    wr_u64(&w, ndi);
    for (size_t i = 0; i < ndi; ++i) {
        const TesseraDicePlacement* p = &state->dice[i];
        wr_u64(&w, p->id);
        wr_u32(&w, p->def);
        wr_u32(&w, p->face);
        wr_f32s(&w, p->position, 3);
        wr_u32(&w, p->seed);
        wr_f32(&w, p->throw_s);
    }

    wr_u64(&w, nov);
    for (size_t i = 0; i < nov; ++i) {
        const TesseraOverlayPlacement* p = &state->overlays[i];
        wr_coord(&w, p->coord);
        wr_u32(&w, p->shape);
        wr_u32(&w, p->atlas);
        wr_f32(&w, p->uv.u0); wr_f32(&w, p->uv.v0);
        wr_f32(&w, p->uv.u1); wr_f32(&w, p->uv.v1);
        wr_f32s(&w, p->tint, 4);
        wr_f32(&w, p->pulse_s);
        wr_f32(&w, p->pulse_alpha_min); wr_f32(&w, p->pulse_alpha_max);
        wr_f32(&w, p->pulse_scale_min); wr_f32(&w, p->pulse_scale_max);
    }

    wr_u64(&w, nlb);
    for (size_t i = 0; i < nlb; ++i) {
        const TesseraLabelPlacement* p = &state->labels[i];
        char text[TESSERA_LABEL_TEXT_CAP];
        memcpy(text, p->text, TESSERA_LABEL_TEXT_CAP);
        text[TESSERA_LABEL_TEXT_CAP - 1] = 0;   /* same guarantee as snapshot copy */
        wr_u64(&w, p->id);
        wr_u32(&w, p->font);
        wr_bytes(&w, text, TESSERA_LABEL_TEXT_CAP);
        wr_u32(&w, p->anchor);
        wr_u64(&w, p->anchor_id);
        wr_f32s(&w, p->position, 3);
        wr_f32(&w, p->size);
        wr_f32s(&w, p->color, 4);
        wr_u8(&w, p->billboard ? 1 : 0);
    }

    wr_u64(&w, nhl);
    for (size_t i = 0; i < nhl; ++i) {
        const TesseraHighlightPlacement* p = &state->highlights[i];
        wr_u64(&w, p->target_id);
        wr_u32(&w, p->kind);
        wr_u32(&w, p->style);
        wr_f32s(&w, p->color, 4);
        wr_f32(&w, p->thickness);
        wr_f32(&w, p->pulse_s);
        wr_f32(&w, p->pulse_min); wr_f32(&w, p->pulse_max);
    }

    wr_u64(&w, npl);
    for (size_t i = 0; i < npl; ++i) {
        const TesseraPointLightPlacement* p = &state->point_lights[i];
        wr_u64(&w, p->id);
        wr_f32s(&w, p->position, 3);
        wr_f32s(&w, p->color, 3);
        wr_f32(&w, p->intensity);
        wr_f32(&w, p->radius);
    }

    wr_u64(&w, nwm);
    for (size_t i = 0; i < nwm; ++i) {
        const TesseraWorldModelPlacement* p = &state->world_models[i];
        wr_u64(&w, p->id);
        wr_u32(&w, p->def);
        wr_f32s(&w, p->position, 3);
        wr_f32s(&w, p->orientation, 4);
        wr_f32(&w, p->scale);
    }

    /* patch the total size into the header */
    size_t total = w.off;
    if (w.buf && w.cap >= TS_BLOB_HDR) {
        TsWr patch = { w.buf, w.cap, 8 };
        wr_u64(&patch, (uint64_t)total);
    }
    return total;
}

/* ---- deserialize ------------------------------------------------------- */
static size_t ts_ser_align_up(size_t n, size_t align) {
    return (n + (align - 1)) & ~(align - 1);
}

/* Pass 1: validate the whole blob and tally array + path element counts.
 * Returns false on any structural problem. */
typedef struct {
    size_t nt, ne, nf, nc, ncd, nh, ndi, nov, nlb, nhl, npl, nwm;
    size_t ep_total;   /* summed entity path coords */
    size_t cp_total;   /* summed card path steps    */
} TsSerCounts;

static bool ts_ser_scan(TsRd* r, TsSerCounts* c) {
    rd_skip(r, TS_SER_CAMERA);

    c->nt = rd_count(r, TS_SER_TILE);
    rd_skip(r, c->nt * TS_SER_TILE);

    c->ne = rd_count(r, TS_SER_ENTITY);
    for (size_t i = 0; i < c->ne && r->ok; ++i) {
        rd_skip(r, TS_SER_ENTITY - 4);
        uint32_t pc = rd_u32(r);
        if (!r->ok || pc > (r->len - r->off) / 8) { r->ok = false; break; }
        c->ep_total += pc;
        rd_skip(r, (size_t)pc * 8);
    }

    c->nf = rd_count(r, TS_SER_EFFECT);
    rd_skip(r, c->nf * TS_SER_EFFECT);

    c->nc = rd_count(r, TS_SER_CARD);
    for (size_t i = 0; i < c->nc && r->ok; ++i) {
        rd_skip(r, TS_SER_CARD - 4);
        uint32_t pc = rd_u32(r);
        if (!r->ok || pc > (r->len - r->off) / 12) { r->ok = false; break; }
        c->cp_total += pc;
        rd_skip(r, (size_t)pc * 12);
    }

    c->ncd = rd_count(r, TS_SER_DRAW);
    rd_skip(r, c->ncd * TS_SER_DRAW);
    c->nh = rd_count(r, TS_SER_HAND);
    rd_skip(r, c->nh * TS_SER_HAND);
    c->ndi = rd_count(r, TS_SER_DICE);
    rd_skip(r, c->ndi * TS_SER_DICE);
    c->nov = rd_count(r, TS_SER_OVERLAY);
    rd_skip(r, c->nov * TS_SER_OVERLAY);
    c->nlb = rd_count(r, TS_SER_LABEL);
    rd_skip(r, c->nlb * TS_SER_LABEL);
    c->nhl = rd_count(r, TS_SER_HL);
    rd_skip(r, c->nhl * TS_SER_HL);
    c->npl = rd_count(r, TS_SER_PLIGHT);
    rd_skip(r, c->npl * TS_SER_PLIGHT);
    c->nwm = rd_count(r, TS_SER_WMODEL);
    rd_skip(r, c->nwm * TS_SER_WMODEL);

    /* every byte must be accounted for */
    return r->ok && r->off == r->len;
}

TesseraState* tessera_state_deserialize(const void* blob, size_t len) {
    if (!blob || len < TS_BLOB_HDR) return NULL;
    TsRd r = { (const uint8_t*)blob, len, 0, true };
    if (rd_u32(&r) != TESSERA_STATE_BLOB_MAGIC) return NULL;
    if (rd_u32(&r) != TESSERA_STATE_BLOB_VERSION) return NULL;
    uint64_t total = rd_u64(&r);
    uint64_t epoch = rd_u64(&r);
    if (!r.ok || total != (uint64_t)len) return NULL;

    TsSerCounts c = {0};
    TsRd scan = r;
    if (!ts_ser_scan(&scan, &c)) return NULL;

    /* One allocation: the TesseraState up front, then each array segment at
     * its natural alignment, path storage last (mirrors ts_snapshot_copy). */
    size_t off_t  = ts_ser_align_up(sizeof(TesseraState), _Alignof(TesseraTilePlacement));
    size_t off_e  = ts_ser_align_up(off_t  + c.nt  * sizeof(TesseraTilePlacement),
                                    _Alignof(TesseraEntityPlacement));
    size_t off_f  = ts_ser_align_up(off_e  + c.ne  * sizeof(TesseraEntityPlacement),
                                    _Alignof(TesseraEffectPlacement));
    size_t off_c  = ts_ser_align_up(off_f  + c.nf  * sizeof(TesseraEffectPlacement),
                                    _Alignof(TesseraCardPlacement));
    size_t off_cd = ts_ser_align_up(off_c  + c.nc  * sizeof(TesseraCardPlacement),
                                    _Alignof(TesseraCardDrawPlacement));
    size_t off_h  = ts_ser_align_up(off_cd + c.ncd * sizeof(TesseraCardDrawPlacement),
                                    _Alignof(TesseraHandPlacement));
    size_t off_di = ts_ser_align_up(off_h  + c.nh  * sizeof(TesseraHandPlacement),
                                    _Alignof(TesseraDicePlacement));
    size_t off_ov = ts_ser_align_up(off_di + c.ndi * sizeof(TesseraDicePlacement),
                                    _Alignof(TesseraOverlayPlacement));
    size_t off_lb = ts_ser_align_up(off_ov + c.nov * sizeof(TesseraOverlayPlacement),
                                    _Alignof(TesseraLabelPlacement));
    size_t off_hl = ts_ser_align_up(off_lb + c.nlb * sizeof(TesseraLabelPlacement),
                                    _Alignof(TesseraHighlightPlacement));
    size_t off_pl = ts_ser_align_up(off_hl + c.nhl * sizeof(TesseraHighlightPlacement),
                                    _Alignof(TesseraPointLightPlacement));
    size_t off_wm = ts_ser_align_up(off_pl + c.npl * sizeof(TesseraPointLightPlacement),
                                    _Alignof(TesseraWorldModelPlacement));
    size_t off_ep = ts_ser_align_up(off_wm + c.nwm * sizeof(TesseraWorldModelPlacement),
                                    _Alignof(TesseraCoord));
    size_t off_cp = ts_ser_align_up(off_ep + c.ep_total * sizeof(TesseraCoord),
                                    _Alignof(float));
    size_t alloc  = off_cp + c.cp_total * 3 * sizeof(float);

    uint8_t* base = (uint8_t*)calloc(1, alloc);
    if (!base) return NULL;
    TesseraState* st = (TesseraState*)base;
    st->epoch = epoch;

    /* camera */
    TesseraCamera* cam = &st->camera;
    cam->mode = rd_u32(&r);
    cam->focus.x = rd_f32(&r); cam->focus.y = rd_f32(&r);
    cam->distance = rd_f32(&r); cam->yaw = rd_f32(&r);
    cam->pitch = rd_f32(&r); cam->fov = rd_f32(&r);
    rd_f32s(&r, cam->position, 3);
    rd_f32s(&r, cam->orientation, 4);
    rd_f32s(&r, cam->target, 3);
    cam->target_id = rd_u64(&r);
    cam->focus_card_id = rd_u64(&r);
    cam->fit_padding = rd_f32(&r);

    rd_u64(&r);   /* tile count (validated in scan) */
    if (c.nt) {
        TesseraTilePlacement* a = (TesseraTilePlacement*)(base + off_t);
        for (size_t i = 0; i < c.nt; ++i) {
            a[i].coord = rd_coord(&r);
            a[i].tile_def = rd_u32(&r);
            a[i].variant = rd_u32(&r);
            a[i].id = rd_u64(&r);
        }
        st->tiles = a; st->tile_count = c.nt;
    }

    rd_u64(&r);
    if (c.ne) {
        TesseraEntityPlacement* a = (TesseraEntityPlacement*)(base + off_e);
        TesseraCoord* pw = (TesseraCoord*)(base + off_ep);
        for (size_t i = 0; i < c.ne; ++i) {
            a[i].id = rd_u64(&r);
            a[i].def = rd_u32(&r);
            a[i].coord = rd_coord(&r);
            a[i].facing = rd_u16(&r);
            a[i].anim = rd_u32(&r);
            uint32_t pc = rd_u32(&r);
            if (pc) {
                for (uint32_t k = 0; k < pc; ++k) pw[k] = rd_coord(&r);
                a[i].path = pw;
                a[i].path_count = pc;
                pw += pc;
            }
        }
        st->entities = a; st->entity_count = c.ne;
    }

    rd_u64(&r);
    if (c.nf) {
        TesseraEffectPlacement* a = (TesseraEffectPlacement*)(base + off_f);
        for (size_t i = 0; i < c.nf; ++i) {
            a[i].id = rd_u64(&r);
            a[i].def = rd_u32(&r);
            a[i].coord = rd_coord(&r);
            a[i].attach_entity_id = rd_u64(&r);
        }
        st->effects = a; st->effect_count = c.nf;
    }

    rd_u64(&r);
    if (c.nc) {
        TesseraCardPlacement* a = (TesseraCardPlacement*)(base + off_c);
        float* pw = (float*)(base + off_cp);
        for (size_t i = 0; i < c.nc; ++i) {
            a[i].id = rd_u64(&r);
            a[i].def = rd_u32(&r);
            rd_f32s(&r, a[i].position, 3);
            rd_f32s(&r, a[i].orientation, 4);
            a[i].hidden = rd_u8(&r) != 0;
            a[i].hand = rd_u64(&r);
            a[i].hand_slot = rd_u32(&r);
            a[i].source_draw = rd_u64(&r);
            uint32_t pc = rd_u32(&r);
            if (pc) {
                rd_f32s(&r, pw, (size_t)pc * 3);
                a[i].path = pw;
                a[i].path_count = pc;
                pw += (size_t)pc * 3;
            }
        }
        st->cards = a; st->card_count = c.nc;
    }

    rd_u64(&r);
    if (c.ncd) {
        TesseraCardDrawPlacement* a = (TesseraCardDrawPlacement*)(base + off_cd);
        for (size_t i = 0; i < c.ncd; ++i) {
            a[i].id = rd_u64(&r);
            a[i].def = rd_u32(&r);
            rd_f32s(&r, a[i].position, 3);
            rd_f32s(&r, a[i].orientation, 4);
            a[i].count = rd_u32(&r);
            a[i].top_hidden = rd_u8(&r) != 0;
        }
        st->card_draws = a; st->card_draw_count = c.ncd;
    }

    rd_u64(&r);
    if (c.nh) {
        TesseraHandPlacement* a = (TesseraHandPlacement*)(base + off_h);
        for (size_t i = 0; i < c.nh; ++i) {
            a[i].id = rd_u64(&r);
            rd_f32s(&r, a[i].position, 3);
            rd_f32s(&r, a[i].orientation, 4);
            a[i].spread_deg = rd_f32(&r);
            a[i].radius = rd_f32(&r);
            a[i].card_spacing = rd_f32(&r);
            a[i].selected_card = rd_u64(&r);
        }
        st->hands = a; st->hand_count = c.nh;
    }

    rd_u64(&r);
    if (c.ndi) {
        TesseraDicePlacement* a = (TesseraDicePlacement*)(base + off_di);
        for (size_t i = 0; i < c.ndi; ++i) {
            a[i].id = rd_u64(&r);
            a[i].def = rd_u32(&r);
            a[i].face = rd_u32(&r);
            rd_f32s(&r, a[i].position, 3);
            a[i].seed = rd_u32(&r);
            a[i].throw_s = rd_f32(&r);
        }
        st->dice = a; st->dice_count = c.ndi;
    }

    rd_u64(&r);
    if (c.nov) {
        TesseraOverlayPlacement* a = (TesseraOverlayPlacement*)(base + off_ov);
        for (size_t i = 0; i < c.nov; ++i) {
            a[i].coord = rd_coord(&r);
            a[i].shape = rd_u32(&r);
            a[i].atlas = rd_u32(&r);
            a[i].uv.u0 = rd_f32(&r); a[i].uv.v0 = rd_f32(&r);
            a[i].uv.u1 = rd_f32(&r); a[i].uv.v1 = rd_f32(&r);
            rd_f32s(&r, a[i].tint, 4);
            a[i].pulse_s = rd_f32(&r);
            a[i].pulse_alpha_min = rd_f32(&r); a[i].pulse_alpha_max = rd_f32(&r);
            a[i].pulse_scale_min = rd_f32(&r); a[i].pulse_scale_max = rd_f32(&r);
        }
        st->overlays = a; st->overlay_count = c.nov;
    }

    rd_u64(&r);
    if (c.nlb) {
        TesseraLabelPlacement* a = (TesseraLabelPlacement*)(base + off_lb);
        for (size_t i = 0; i < c.nlb; ++i) {
            a[i].id = rd_u64(&r);
            a[i].font = rd_u32(&r);
            rd_bytes(&r, a[i].text, TESSERA_LABEL_TEXT_CAP);
            a[i].text[TESSERA_LABEL_TEXT_CAP - 1] = 0;
            a[i].anchor = rd_u32(&r);
            a[i].anchor_id = rd_u64(&r);
            rd_f32s(&r, a[i].position, 3);
            a[i].size = rd_f32(&r);
            rd_f32s(&r, a[i].color, 4);
            a[i].billboard = rd_u8(&r) != 0;
        }
        st->labels = a; st->label_count = c.nlb;
    }

    rd_u64(&r);
    if (c.nhl) {
        TesseraHighlightPlacement* a = (TesseraHighlightPlacement*)(base + off_hl);
        for (size_t i = 0; i < c.nhl; ++i) {
            a[i].target_id = rd_u64(&r);
            a[i].kind = rd_u32(&r);
            a[i].style = rd_u32(&r);
            rd_f32s(&r, a[i].color, 4);
            a[i].thickness = rd_f32(&r);
            a[i].pulse_s = rd_f32(&r);
            a[i].pulse_min = rd_f32(&r); a[i].pulse_max = rd_f32(&r);
        }
        st->highlights = a; st->highlight_count = c.nhl;
    }

    rd_u64(&r);
    if (c.npl) {
        TesseraPointLightPlacement* a = (TesseraPointLightPlacement*)(base + off_pl);
        for (size_t i = 0; i < c.npl; ++i) {
            a[i].id = rd_u64(&r);
            rd_f32s(&r, a[i].position, 3);
            rd_f32s(&r, a[i].color, 3);
            a[i].intensity = rd_f32(&r);
            a[i].radius = rd_f32(&r);
        }
        st->point_lights = a; st->point_light_count = c.npl;
    }

    rd_u64(&r);
    if (c.nwm) {
        TesseraWorldModelPlacement* a = (TesseraWorldModelPlacement*)(base + off_wm);
        for (size_t i = 0; i < c.nwm; ++i) {
            a[i].id = rd_u64(&r);
            a[i].def = rd_u32(&r);
            rd_f32s(&r, a[i].position, 3);
            rd_f32s(&r, a[i].orientation, 4);
            a[i].scale = rd_f32(&r);
        }
        st->world_models = a; st->world_model_count = c.nwm;
    }

    if (!r.ok || r.off != r.len) {   /* cannot happen after a good scan */
        free(base);
        return NULL;
    }
    return st;
}

void tessera_state_free(TesseraState* state) {
    free(state);   /* the state and its arrays are one allocation */
}

/* ---- replay container -------------------------------------------------- */
#define TS_REPLAY_HDR 24u   /* magic u32, version u32, total u64, count u32, reserved u32 */

typedef struct {
    uint64_t ts_ms;
    uint8_t* blob;
    size_t   size;
} TsReplayRec;

struct TesseraReplay {
    TsReplayRec* recs;
    uint32_t     count, cap;
};

TesseraReplay* tessera_replay_create(void) {
    return (TesseraReplay*)calloc(1, sizeof(TesseraReplay));
}

void tessera_replay_free(TesseraReplay* r) {
    if (!r) return;
    for (uint32_t i = 0; i < r->count; ++i) free(r->recs[i].blob);
    free(r->recs);
    free(r);
}

static bool ts_replay_push(TesseraReplay* r, uint64_t ts_ms, uint8_t* blob, size_t size) {
    if (r->count == r->cap) {
        uint32_t ncap = r->cap ? r->cap * 2 : 8;
        TsReplayRec* nr = (TsReplayRec*)realloc(r->recs, ncap * sizeof *nr);
        if (!nr) return false;
        r->recs = nr;
        r->cap = ncap;
    }
    r->recs[r->count++] = (TsReplayRec){ ts_ms, blob, size };
    return true;
}

bool tessera_replay_append(TesseraReplay* r, uint64_t timestamp_ms,
                           const TesseraState* state) {
    if (!r || !state) return false;
    size_t n = tessera_state_serialize(state, NULL, 0);
    if (!n) return false;
    uint8_t* blob = (uint8_t*)malloc(n);
    if (!blob) return false;
    tessera_state_serialize(state, blob, n);
    if (!ts_replay_push(r, timestamp_ms, blob, n)) { free(blob); return false; }
    return true;
}

uint32_t tessera_replay_count(const TesseraReplay* r) {
    return r ? r->count : 0;
}

TesseraState* tessera_replay_get(const TesseraReplay* r, uint32_t index,
                                 uint64_t* out_timestamp_ms) {
    if (!r || index >= r->count) return NULL;
    const TsReplayRec* rec = &r->recs[index];
    TesseraState* st = tessera_state_deserialize(rec->blob, rec->size);
    if (st && out_timestamp_ms) *out_timestamp_ms = rec->ts_ms;
    return st;
}

size_t tessera_replay_serialize(const TesseraReplay* r, void* buf, size_t cap) {
    if (!r) return 0;
    TsWr w = { (uint8_t*)buf, buf ? cap : 0, 0 };
    wr_u32(&w, TESSERA_REPLAY_MAGIC);
    wr_u32(&w, TESSERA_REPLAY_VERSION);
    wr_u64(&w, 0);                 /* total size placeholder */
    wr_u32(&w, r->count);
    wr_u32(&w, 0);                 /* reserved */
    for (uint32_t i = 0; i < r->count; ++i) {
        wr_u64(&w, r->recs[i].ts_ms);
        wr_u64(&w, (uint64_t)r->recs[i].size);
        wr_bytes(&w, r->recs[i].blob, r->recs[i].size);
    }
    size_t total = w.off;
    if (w.buf && w.cap >= TS_REPLAY_HDR) {
        TsWr patch = { w.buf, w.cap, 8 };
        wr_u64(&patch, (uint64_t)total);
    }
    return total;
}

TesseraReplay* tessera_replay_open(const void* data, size_t len) {
    if (!data || len < TS_REPLAY_HDR) return NULL;
    TsRd r = { (const uint8_t*)data, len, 0, true };
    if (rd_u32(&r) != TESSERA_REPLAY_MAGIC) return NULL;
    if (rd_u32(&r) != TESSERA_REPLAY_VERSION) return NULL;
    uint64_t total = rd_u64(&r);
    uint32_t count = rd_u32(&r);
    rd_u32(&r);   /* reserved */
    if (!r.ok || total != (uint64_t)len) return NULL;
    /* each record needs at least its 16-byte prefix */
    if (count > (len - TS_REPLAY_HDR) / 16) return NULL;

    TesseraReplay* rp = tessera_replay_create();
    if (!rp) return NULL;
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t ts = rd_u64(&r);
        uint64_t sz = rd_u64(&r);
        if (!r.ok || sz > (uint64_t)(r.len - r.off)) { tessera_replay_free(rp); return NULL; }
        uint8_t* blob = (uint8_t*)malloc(sz ? (size_t)sz : 1);
        if (!blob) { tessera_replay_free(rp); return NULL; }
        rd_bytes(&r, blob, (size_t)sz);
        if (!ts_replay_push(rp, ts, blob, (size_t)sz)) {
            free(blob);
            tessera_replay_free(rp);
            return NULL;
        }
    }
    if (!r.ok || r.off != r.len) { tessera_replay_free(rp); return NULL; }
    return rp;
}
