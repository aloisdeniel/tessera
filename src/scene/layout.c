/* layout.c — deterministic multi-entity tile layout solver.
 *
 * CONTRACT (M4 reflow depends on this): slot i is assigned to the i-th entity
 * of an id-sorted group. Given the same (n), the same slot offsets/scale are
 * always produced, so an entity keeps its slot when others don't change. */
#include "scene/scene.h"
#include <math.h>

void ts_layout_solve(uint32_t n, TsLayout* out) {
    out->count = n;
    if (n == 0) { out->scale = 1.0f; return; }
    if (n > TS_MAX_SLOTS) n = TS_MAX_SLOTS;

    if (n == 1) {
        out->offset[0][0] = 0.0f;
        out->offset[0][1] = 0.0f;
        out->scale = 1.0f;
        out->count = 1;
        return;
    }

    /* Choose a grid that fits n: cols = ceil(sqrt(n)). */
    uint32_t cols = (uint32_t)ceilf(sqrtf((float)n));
    uint32_t rows = (n + cols - 1) / cols;

    /* Cell footprint fraction of the tile; leave a margin. */
    const float margin = 0.12f;
    float span = 1.0f - 2.0f * margin;             /* usable width in tile units */
    float cell_w = span / (float)cols;
    float cell_h = span / (float)rows;
    float cell = cell_w < cell_h ? cell_w : cell_h;

    /* Scale each occupant so it fits its cell (relative to n==1 footprint). */
    out->scale = ts_clampf(cell * 0.9f, 0.18f, 1.0f);

    for (uint32_t i = 0; i < n; ++i) {
        uint32_t r = i / cols;
        uint32_t c = i % cols;
        /* entities on the last (possibly partial) row are centered */
        uint32_t row_items = (r == rows - 1) ? (n - r * cols) : cols;
        float x = ((float)c - (float)(row_items - 1) * 0.5f) * cell_w;
        float z = ((float)r - (float)(rows - 1) * 0.5f) * cell_h;
        out->offset[i][0] = x;
        out->offset[i][1] = z;
    }
    out->count = n;
}
