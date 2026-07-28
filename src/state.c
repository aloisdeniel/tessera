/* state.c — immutable snapshots + thread-safe handoff (M3). */
#include "state.h"

#include <stdlib.h>
#include <string.h>

/* Round `n` up to the next multiple of `align` (align must be a power of 2). */
static size_t ts_align_up(size_t n, size_t align) {
    return (n + (align - 1)) & ~(align - 1);
}

/* NOTE: src/serialize.c walks the exact same structure to flatten a state to
 * a versioned blob (and pins every struct sizeof with static asserts). When a
 * new array/field is added here, extend the wire walk there too. */
TsSnapshot* ts_snapshot_copy(const TesseraState* src) {
    if (!src) return NULL;

    TsSnapshot* s = (TsSnapshot*)calloc(1, sizeof *s);
    if (!s) return NULL;

    s->camera = src->camera;
    s->epoch  = src->epoch;

    /* Treat NULL arrays as empty regardless of the reported count. */
    size_t nt  = src->tiles      ? src->tile_count      : 0;
    size_t ne  = src->entities   ? src->entity_count    : 0;
    size_t nf  = src->effects    ? src->effect_count    : 0;
    size_t nc  = src->cards      ? src->card_count      : 0;
    size_t ncd = src->card_draws ? src->card_draw_count : 0;
    size_t nh  = src->hands      ? src->hand_count      : 0;
    size_t ndi = src->dice       ? src->dice_count      : 0;
    size_t nov = src->overlays   ? src->overlay_count   : 0;
    size_t nlb = src->labels     ? src->label_count     : 0;
    size_t nhl = src->highlights ? src->highlight_count : 0;
    size_t npl = src->point_lights ? src->point_light_count : 0;
    size_t nwm = src->world_models ? src->world_model_count : 0;

    /* Entity/card multi-step move paths are caller-owned pointer+count fields;
     * they must be deep-copied into the snapshot block too. Sum their sizes. */
    size_t bytes_ep = 0;   /* entity paths: TesseraCoord[] */
    for (size_t i = 0; i < ne; ++i)
        if (src->entities[i].path && src->entities[i].path_count)
            bytes_ep += (size_t)src->entities[i].path_count * sizeof(TesseraCoord);
    size_t bytes_cp = 0;   /* card paths: float[] (3 per step) */
    for (size_t i = 0; i < nc; ++i)
        if (src->cards[i].path && src->cards[i].path_count)
            bytes_cp += (size_t)src->cards[i].path_count * 3 * sizeof(float);

    size_t bytes_t  = nt  * sizeof(TesseraTilePlacement);
    size_t bytes_e  = ne  * sizeof(TesseraEntityPlacement);
    size_t bytes_f  = nf  * sizeof(TesseraEffectPlacement);
    size_t bytes_c  = nc  * sizeof(TesseraCardPlacement);
    size_t bytes_cd = ncd * sizeof(TesseraCardDrawPlacement);
    size_t bytes_h  = nh  * sizeof(TesseraHandPlacement);
    size_t bytes_di = ndi * sizeof(TesseraDicePlacement);
    size_t bytes_ov = nov * sizeof(TesseraOverlayPlacement);
    size_t bytes_lb = nlb * sizeof(TesseraLabelPlacement);
    size_t bytes_hl = nhl * sizeof(TesseraHighlightPlacement);
    size_t bytes_pl = npl * sizeof(TesseraPointLightPlacement);
    size_t bytes_wm = nwm * sizeof(TesseraWorldModelPlacement);

    /* Pack every array into a single allocation, each segment aligned for its
     * element type. Path storage trails the placement arrays. */
    size_t off_t  = 0;
    size_t off_e  = ts_align_up(off_t  + bytes_t,  _Alignof(TesseraEntityPlacement));
    size_t off_f  = ts_align_up(off_e  + bytes_e,  _Alignof(TesseraEffectPlacement));
    size_t off_c  = ts_align_up(off_f  + bytes_f,  _Alignof(TesseraCardPlacement));
    size_t off_cd = ts_align_up(off_c  + bytes_c,  _Alignof(TesseraCardDrawPlacement));
    size_t off_h  = ts_align_up(off_cd + bytes_cd, _Alignof(TesseraHandPlacement));
    size_t off_di = ts_align_up(off_h  + bytes_h,  _Alignof(TesseraDicePlacement));
    size_t off_ov = ts_align_up(off_di + bytes_di, _Alignof(TesseraOverlayPlacement));
    size_t off_lb = ts_align_up(off_ov + bytes_ov, _Alignof(TesseraLabelPlacement));
    size_t off_hl = ts_align_up(off_lb + bytes_lb, _Alignof(TesseraHighlightPlacement));
    size_t off_pl = ts_align_up(off_hl + bytes_hl, _Alignof(TesseraPointLightPlacement));
    size_t off_wm = ts_align_up(off_pl + bytes_pl, _Alignof(TesseraWorldModelPlacement));
    size_t off_ep = ts_align_up(off_wm + bytes_wm, _Alignof(TesseraCoord));
    size_t off_cp = ts_align_up(off_ep + bytes_ep, _Alignof(float));
    size_t total  = off_cp + bytes_cp;

    if (total > 0) {
        s->block = malloc(total);
        if (!s->block) {
            free(s);
            return NULL;
        }
        uint8_t* base = (uint8_t*)s->block;

        if (nt) {
            s->tiles = (TesseraTilePlacement*)(base + off_t);
            memcpy(s->tiles, src->tiles, bytes_t);
            s->tile_count = nt;
        }
        if (ne) {
            s->entities = (TesseraEntityPlacement*)(base + off_e);
            memcpy(s->entities, src->entities, bytes_e);
            s->entity_count = ne;
            /* deep-copy each entity's move path and repoint into the block */
            uint8_t* pw = base + off_ep;
            for (size_t i = 0; i < ne; ++i) {
                if (src->entities[i].path && src->entities[i].path_count) {
                    size_t b = (size_t)src->entities[i].path_count * sizeof(TesseraCoord);
                    memcpy(pw, src->entities[i].path, b);
                    s->entities[i].path = (const TesseraCoord*)pw;
                    pw += b;
                } else {
                    s->entities[i].path = NULL;
                    s->entities[i].path_count = 0;
                }
            }
        }
        if (nf) {
            s->effects = (TesseraEffectPlacement*)(base + off_f);
            memcpy(s->effects, src->effects, bytes_f);
            s->effect_count = nf;
        }
        if (nc) {
            s->cards = (TesseraCardPlacement*)(base + off_c);
            memcpy(s->cards, src->cards, bytes_c);
            s->card_count = nc;
            /* deep-copy each card's move path and repoint into the block */
            uint8_t* pw = base + off_cp;
            for (size_t i = 0; i < nc; ++i) {
                if (src->cards[i].path && src->cards[i].path_count) {
                    size_t b = (size_t)src->cards[i].path_count * 3 * sizeof(float);
                    memcpy(pw, src->cards[i].path, b);
                    s->cards[i].path = (const float*)pw;
                    pw += b;
                } else {
                    s->cards[i].path = NULL;
                    s->cards[i].path_count = 0;
                }
            }
        }
        if (ncd) {
            s->card_draws = (TesseraCardDrawPlacement*)(base + off_cd);
            memcpy(s->card_draws, src->card_draws, bytes_cd);
            s->card_draw_count = ncd;
        }
        if (nh) {
            s->hands = (TesseraHandPlacement*)(base + off_h);
            memcpy(s->hands, src->hands, bytes_h);
            s->hand_count = nh;
        }
        if (ndi) {
            s->dice = (TesseraDicePlacement*)(base + off_di);
            memcpy(s->dice, src->dice, bytes_di);
            s->dice_count = ndi;
        }
        if (nov) {
            s->overlays = (TesseraOverlayPlacement*)(base + off_ov);
            memcpy(s->overlays, src->overlays, bytes_ov);
            s->overlay_count = nov;
        }
        if (nlb) {
            s->labels = (TesseraLabelPlacement*)(base + off_lb);
            memcpy(s->labels, src->labels, bytes_lb);
            s->label_count = nlb;
            /* text is an inline bounded array; force NUL termination */
            for (size_t i = 0; i < nlb; ++i)
                s->labels[i].text[TESSERA_LABEL_TEXT_CAP - 1] = 0;
        }
        if (nhl) {
            s->highlights = (TesseraHighlightPlacement*)(base + off_hl);
            memcpy(s->highlights, src->highlights, bytes_hl);
            s->highlight_count = nhl;
        }
        if (npl) {
            s->point_lights = (TesseraPointLightPlacement*)(base + off_pl);
            memcpy(s->point_lights, src->point_lights, bytes_pl);
            s->point_light_count = npl;
        }
        if (nwm) {
            s->world_models = (TesseraWorldModelPlacement*)(base + off_wm);
            memcpy(s->world_models, src->world_models, bytes_wm);
            s->world_model_count = nwm;
        }
    }

    return s;
}

void ts_snapshot_free(TsSnapshot* s) {
    if (!s) return;
    free(s->block);
    free(s);
}

struct TsStateStore* ts_state_create(void) {
    return (struct TsStateStore*)calloc(1, sizeof(struct TsStateStore));
}

void ts_state_destroy(struct TsStateStore* s) {
    if (!s) return;
    /* current/target/pending may alias (e.g. before any promotion). Free each
     * distinct pointer exactly once. */
    TsSnapshot* a = s->current;
    TsSnapshot* b = s->target;
    TsSnapshot* c = s->pending;
    if (a) ts_snapshot_free(a);
    if (b && b != a) ts_snapshot_free(b);
    if (c && c != a && c != b) ts_snapshot_free(c);
    free(s);
}

void ts_state_publish_pending(struct TsStateStore* s, TsSnapshot* snap) {
    if (s->pending) ts_snapshot_free(s->pending);
    s->pending = snap;
    s->has_pending = true;
}

bool ts_state_promote(struct TsStateStore* s, TsSnapshot** out_prev, TsSnapshot** out_new) {
    if (!s->has_pending) return false;

    TsSnapshot* old_current = s->current;
    s->current = s->target;   /* becomes 'prev' (may be NULL first time) */
    s->target  = s->pending;
    s->pending = NULL;
    s->has_pending = false;

    if (old_current && old_current != s->current) ts_snapshot_free(old_current);

    *out_prev = s->current;
    *out_new  = s->target;
    return true;
}
