/*
 * state.h — immutable snapshots + thread-safe handoff (M3).
 *
 * A snapshot is a single heap allocation backing all arrays, so it frees as one
 * unit. set_state deep-copies the caller's TesseraState (off-lock), then swaps
 * it into `pending` under the engine mutex. The tick loop promotes it.
 */
#ifndef TESSERA_STATE_H
#define TESSERA_STATE_H

#include "core/core.h"
#include "tessera.h"

typedef struct {
    TesseraTilePlacement*   tiles;    size_t tile_count;
    TesseraEntityPlacement* entities; size_t entity_count;
    TesseraEffectPlacement* effects;  size_t effect_count;
    TesseraCamera camera;
    uint64_t      epoch;
    void*         block;   /* single backing allocation (may be NULL if empty) */
} TsSnapshot;

/* Deep-copy a caller snapshot into one owned allocation. `reg` (optional) is
 * used to drop placements referencing unknown def ids (reported via log). */
TsSnapshot* ts_snapshot_copy(const TesseraState* src);
void        ts_snapshot_free(TsSnapshot* s);

struct TsStateStore {
    TsSnapshot* current;   /* logical "from" for animations */
    TsSnapshot* target;    /* last promoted snapshot         */
    TsSnapshot* pending;   /* set by set_state, not yet promoted */
    bool        has_pending;
};

struct TsStateStore* ts_state_create(void);
void ts_state_destroy(struct TsStateStore* s);

/* Publish a freshly-copied snapshot (takes ownership). Call under the engine
 * state mutex. Replaces any not-yet-promoted pending snapshot. */
void ts_state_publish_pending(struct TsStateStore* s, TsSnapshot* snap);

/* Promote pending->target if present. Returns true when a promotion happened,
 * yielding the previous target in *out_prev (may be NULL) and the new one in
 * *out_new. Call on the tick thread. Ownership stays with the store. */
bool ts_state_promote(struct TsStateStore* s, TsSnapshot** out_prev, TsSnapshot** out_new);

#endif /* TESSERA_STATE_H */
