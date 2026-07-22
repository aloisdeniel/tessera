/* drawlist.c — builds the per-frame draw list.
 *
 * M0/M1 stage: renders a built-in demo board straight from the registry's
 * shared meshes so the pipeline is provable before state exists. Milestone 3
 * replaces this file with a version that walks the live TesseraState and the
 * layout solver. The single symbol `ts_scene_build_drawlist` is the contract. */
#include "engine.h"
#include "orchestration/orch.h"
#include "dice/dice.h"
#include <string.h>

static void push_tile(TsDrawItem* it, const TsMesh* mesh, SDL_GPUTexture* tex,
                      int x, int z, float r, float g, float b) {
    memset(it, 0, sizeof *it);
    vec3 world; ts_grid_to_world(x, z, world);
    mat4 m; glm_translate_make(m, world);
    glm_mat4_copy(m, it->model);
    it->mesh = mesh;
    it->texture = tex;
    it->tint[0] = r; it->tint[1] = g; it->tint[2] = b; it->tint[3] = 1.0f;
    it->uv_rect[0] = 0.0f; it->uv_rect[1] = 0.0f;
    it->uv_rect[2] = 1.0f; it->uv_rect[3] = 1.0f;
}

/* Build the base scene (live orchestrator, or the built-in demo board before any
 * state is pushed) into `arena`; returns count with *base pointing at it. */
static size_t build_base(TesseraEngine* e, TsArena* arena, TsDrawItem** base) {
    /* Once a state has been pushed, the orchestrator owns the scene. */
    if (e->orch && ts_orch_has_content(e->orch))
        return ts_orch_build_drawlist(e->orch, e, arena, base);

    const int N = 6;
    size_t max_items = (size_t)(N * N) + 3;
    TsDrawItem* items = TS_ARENA_ARR(arena, TsDrawItem, max_items);
    if (!items) { *base = NULL; return 0; }

    SDL_GPUTexture* white = e->registry.white.texture;
    size_t k = 0;

    for (int z = 0; z < N; ++z) {
        for (int x = 0; x < N; ++x) {
            bool light = ((x + z) & 1) == 0;
            float shade = light ? 0.85f : 0.55f;
            push_tile(&items[k++], &e->registry.tile_mesh, white,
                      x - N / 2, z - N / 2, shade, shade * 0.95f, shade * 0.8f);
        }
    }

    /* a few entities standing on tiles */
    int spots[3][2] = {{0, 0}, {2, -1}, {-2, 1}};
    float cols[3][3] = {{0.9f, 0.3f, 0.3f}, {0.3f, 0.6f, 0.9f}, {0.4f, 0.9f, 0.5f}};
    for (int i = 0; i < 3; ++i) {
        push_tile(&items[k], &e->registry.cube_mesh, white,
                  spots[i][0], spots[i][1], cols[i][0], cols[i][1], cols[i][2]);
        k++;
    }
    *base = items;
    return k;
}

size_t ts_scene_build_drawlist(TesseraEngine* e, TsArena* arena, TsDrawItem** out) {
    TsDrawItem* base = NULL;
    size_t base_count = build_base(e, arena, &base);

    /* Thrown dice live outside the state snapshot; append them to whatever the
     * base scene produced so they draw over the board with the same pipeline. */
    size_t dice_count = e->dice ? ts_dice_drawitem_count(e->dice) : 0;
    if (dice_count == 0) { *out = base; return base_count; }

    TsDrawItem* all = TS_ARENA_ARR(arena, TsDrawItem, base_count + dice_count);
    if (!all) { *out = base; return base_count; }
    if (base_count) memcpy(all, base, base_count * sizeof(TsDrawItem));
    size_t nd = ts_dice_build_drawlist(e->dice, e, all + base_count);
    *out = all;
    return base_count + nd;
}
