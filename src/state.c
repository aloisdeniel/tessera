/* state.c — immutable snapshots + thread-safe handoff (M3). */
#include "state.h"

#include <stdlib.h>
#include <string.h>

/* Round `n` up to the next multiple of `align` (align must be a power of 2). */
static size_t ts_align_up(size_t n, size_t align) {
    return (n + (align - 1)) & ~(align - 1);
}

TsSnapshot* ts_snapshot_copy(const TesseraState* src) {
    if (!src) return NULL;

    TsSnapshot* s = (TsSnapshot*)calloc(1, sizeof *s);
    if (!s) return NULL;

    s->camera = src->camera;
    s->epoch  = src->epoch;

    /* Treat NULL arrays as empty regardless of the reported count. */
    size_t nt = src->tiles    ? src->tile_count   : 0;
    size_t ne = src->entities ? src->entity_count : 0;
    size_t nf = src->effects  ? src->effect_count : 0;

    size_t bytes_t = nt * sizeof(TesseraTilePlacement);
    size_t bytes_e = ne * sizeof(TesseraEntityPlacement);
    size_t bytes_f = nf * sizeof(TesseraEffectPlacement);

    /* Pack the three arrays into a single allocation, keeping each segment
     * aligned for its element type. */
    size_t off_t = 0;
    size_t off_e = ts_align_up(off_t + bytes_t, _Alignof(TesseraEntityPlacement));
    size_t off_f = ts_align_up(off_e + bytes_e, _Alignof(TesseraEffectPlacement));
    size_t total = off_f + bytes_f;

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
        }
        if (nf) {
            s->effects = (TesseraEffectPlacement*)(base + off_f);
            memcpy(s->effects, src->effects, bytes_f);
            s->effect_count = nf;
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
