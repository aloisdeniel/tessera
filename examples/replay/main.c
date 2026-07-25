/* examples/replay — record a short game as a replay file, then play it back.
 *
 * Phase 1 (record): pushes a handful of evolving states (a pawn walking a
 * multi-step path with a highlight + overlay trail, a die throw) through a
 * live engine, snapshotting each state into a TesseraReplay with its
 * timestamp, and writes the serialized container to disk.
 *
 * Phase 2 (replay): re-reads the file with tessera_replay_open and replays
 * every record through tessera_set_state on a FRESH engine — the renderer
 * re-animates the transitions between the recorded snapshots.
 *
 *   example_replay [--file game.tsr] [--shots prefix]   # headless captures
 *
 * With --shots, each replayed record is captured to <prefix>NN.png. */
#include "tessera.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void logfn(void* ud, int level, const char* msg) {
    (void)ud;
    if (level >= TESSERA_LOG_WARN) fprintf(stderr, "[tessera] %s\n", msg);
}

/* Def ids depend only on registration order, so recording and replaying
 * engines that register identically hand out identical ids. */
static TesseraDefId g_tile_def, g_entity_def;

static TesseraEngine* make_engine(void) {
    TesseraConfig cfg = { .width = 640, .height = 400, .pixel_density = 1.0f,
                          .log = logfn };
    TesseraEngine* e = tessera_create(&cfg);
    if (!e) return NULL;
    if (strcmp(tessera_backend_name(e), "none") == 0) {
        fprintf(stderr, "no GPU backend: %s\n", tessera_last_error(e));
        tessera_destroy(e);
        return NULL;
    }
    TesseraTileDef tdef = { .thickness = 0.25f, .tint = {0.5f, 0.62f, 0.42f, 1.0f} };
    TesseraEntityDef edef = { .scale = 1.0f };
    g_tile_def = tessera_register_tile_def(e, &tdef);
    g_entity_def = tessera_register_entity_def(e, &edef);
    if (!g_tile_def || !g_entity_def) {
        fprintf(stderr, "def registration failed: %s\n", tessera_last_error(e));
        tessera_destroy(e);
        return NULL;
    }
    return e;
}

/* Build one frame of the little scripted game into caller-owned arrays. */
#define BOARD 4
typedef struct {
    TesseraTilePlacement tiles[BOARD * BOARD];
    TesseraEntityPlacement ent;
    TesseraCoord path[3];
    TesseraOverlayPlacement overlay;
    TesseraHighlightPlacement hl;
} Frame;

static void build_state(Frame* f, TesseraState* st, int step) {
    memset(f, 0, sizeof *f);
    memset(st, 0, sizeof *st);
    int ti = 0;
    for (int y = 0; y < BOARD; ++y)
        for (int x = 0; x < BOARD; ++x)
            f->tiles[ti++] = (TesseraTilePlacement){ .coord = {x, y}, .tile_def = g_tile_def };

    static const TesseraCoord stops[4] = { {0, 0}, {1, 1}, {3, 1}, {3, 3} };
    f->ent = (TesseraEntityPlacement){ .id = 1, .def = g_entity_def, .coord = stops[step] };
    if (step == 2) {   /* multi-step walk through the middle of the board */
        f->path[0] = (TesseraCoord){1, 1};
        f->path[1] = (TesseraCoord){2, 1};
        f->path[2] = (TesseraCoord){3, 1};
        f->ent.path = f->path;
        f->ent.path_count = 3;
    }
    f->overlay = (TesseraOverlayPlacement){
        .coord = stops[step], .shape = TESSERA_OVERLAY_RING,
        .tint = {1.0f, 0.85f, 0.2f, 0.9f} };
    f->hl = (TesseraHighlightPlacement){
        .target_id = 1, .kind = TESSERA_HIGHLIGHT_ENTITY,
        .style = TESSERA_HIGHLIGHT_OUTLINE, .color = {1.0f, 0.85f, 0.2f, 1.0f} };

    st->tiles = f->tiles;      st->tile_count = BOARD * BOARD;
    st->entities = &f->ent;    st->entity_count = 1;
    st->overlays = &f->overlay; st->overlay_count = 1;
    st->highlights = &f->hl;   st->highlight_count = 1;
    st->camera = (TesseraCamera){ .focus = {1.5f, 1.5f}, .distance = 9.0f,
                                  .yaw = 0.6f, .pitch = 0.85f, .fov = 0.9f };
    st->epoch = (uint64_t)step;
}

static void settle(TesseraEngine* e) {
    for (int i = 0; i < 600 && !tessera_is_idle(e); ++i) tessera_tick(e, 1.0 / 60.0);
}

int main(int argc, char** argv) {
    const char* file = "replay_demo.tsr";
    const char* shots = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--file") && i + 1 < argc) file = argv[++i];
        else if (!strcmp(argv[i], "--shots") && i + 1 < argc) shots = argv[++i];
    }

    /* ---- phase 1: play the moves live and record each pushed state ------- */
    TesseraEngine* e = make_engine();
    if (!e) return 1;

    TesseraReplay* rec = tessera_replay_create();
    uint64_t clock_ms = 0;
    for (int step = 0; step < 4; ++step) {
        Frame f;
        TesseraState st;
        build_state(&f, &st, step);
        tessera_set_state(e, &st);
        tessera_replay_append(rec, clock_ms, &st);
        settle(e);
        clock_ms += 800;
    }
    tessera_destroy(e);

    size_t n = tessera_replay_serialize(rec, NULL, 0);
    void* buf = malloc(n);
    tessera_replay_serialize(rec, buf, n);
    tessera_replay_free(rec);

    FILE* out = fopen(file, "wb");
    if (!out) { fprintf(stderr, "cannot write %s\n", file); free(buf); return 1; }
    fwrite(buf, 1, n, out);
    fclose(out);
    free(buf);
    printf("recorded 4 states -> %s (%zu bytes)\n", file, n);

    /* ---- phase 2: load the file back and replay it on a fresh engine ----- */
    FILE* in = fopen(file, "rb");
    fseek(in, 0, SEEK_END);
    long len = ftell(in);
    fseek(in, 0, SEEK_SET);
    void* data = malloc((size_t)len);
    if (fread(data, 1, (size_t)len, in) != (size_t)len) {
        fprintf(stderr, "short read on %s\n", file);
        fclose(in); free(data);
        return 1;
    }
    fclose(in);

    TesseraReplay* play = tessera_replay_open(data, (size_t)len);
    free(data);
    if (!play) { fprintf(stderr, "%s is not a valid replay\n", file); return 1; }

    e = make_engine();
    if (!e) { tessera_replay_free(play); return 1; }
    uint32_t count = tessera_replay_count(play);
    printf("replaying %u records\n", count);
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t ts = 0;
        TesseraState* st = tessera_replay_get(play, i, &ts);
        if (!st) { fprintf(stderr, "record %u is corrupt\n", i); continue; }
        tessera_set_state(e, st);
        tessera_state_free(st);
        settle(e);
        printf("  [%u] t=%llums epoch=%u replayed\n", i, (unsigned long long)ts,
               (unsigned)i);
        if (shots) {
            char path[512];
            snprintf(path, sizeof path, "%s%02u.png", shots, i);
            if (tessera_capture_png(e, 640, 400, path))
                printf("        captured %s\n", path);
        }
    }
    tessera_replay_free(play);
    tessera_destroy(e);
    printf("replay done\n");
    return 0;
}
