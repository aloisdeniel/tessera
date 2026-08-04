/* test_lua.c — embedded Lua game VM.
 *
 * Covers: TSAB bundle parsing (good container, bad magic, truncation),
 * tessera_lua_event before a game is loaded, bad scripts (syntax error /
 * chunk not returning a function), and a full game round-trip: the chunk
 * registers an atlas + tile def from a bundle asset, the "start" event
 * returns TWO states that must play sequentially (each awaiting the previous
 * operation's settle), then a host event drives one more state. Runs
 * headless (render_rgba) and reaches into the state store internals to
 * assert what each promoted snapshot contained. */
#include "tessera.h"
#include "engine.h"
#include "state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

static void log_fn(void* ud, int level, const char* msg) {
    (void)ud;
    if (level >= TESSERA_LOG_ERROR) printf("[engine] %s\n", msg);
}

static void tick(TesseraEngine* e, unsigned char* buf) {
    tessera_render_rgba(e, 1.0 / 60.0, 64, 64, buf, 64 * 64 * 4);
}

/* Uncompressed 32-bit TGA (stb decodes it) — same helper as test_cards.c. */
static unsigned char* tga32(const unsigned char* rgba, int w, int h, int* len) {
    int sz = 18 + w * h * 4;
    unsigned char* d = (unsigned char*)malloc((size_t)sz);
    memset(d, 0, 18);
    d[2] = 2; d[12] = w & 0xFF; d[13] = (w >> 8) & 0xFF;
    d[14] = h & 0xFF; d[15] = (h >> 8) & 0xFF; d[16] = 32; d[17] = 0x28;
    for (int i = 0; i < w * h; ++i) {
        d[18+i*4+0] = rgba[i*4+2]; d[18+i*4+1] = rgba[i*4+1];
        d[18+i*4+2] = rgba[i*4+0]; d[18+i*4+3] = rgba[i*4+3];
    }
    *len = sz; return d;
}

/* ---- little TSAB writer (mirrors the format in docs/lua.md) ------------ */

typedef struct { unsigned char* buf; size_t len, cap; } Wr;

static void wr(Wr* w, const void* p, size_t n) {
    if (w->len + n > w->cap) {
        w->cap = (w->len + n) * 2 + 64;
        w->buf = (unsigned char*)realloc(w->buf, w->cap);
    }
    memcpy(w->buf + w->len, p, n);
    w->len += n;
}
static void wr_u16(Wr* w, unsigned v) { unsigned char b[2] = { v & 0xFF, (v >> 8) & 0xFF }; wr(w, b, 2); }
static void wr_u32(Wr* w, unsigned v) {
    unsigned char b[4] = { v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF };
    wr(w, b, 4);
}

static unsigned char* build_bundle(const char* const* names, const void* const* blobs,
                                   const size_t* sizes, size_t count, size_t* out_len) {
    Wr w = { NULL, 0, 0 };
    wr_u32(&w, 0x42415354u);          /* "TSAB" */
    wr_u32(&w, 1);                    /* version */
    wr_u32(&w, (unsigned)count);
    for (size_t i = 0; i < count; ++i) {
        wr_u16(&w, (unsigned)strlen(names[i]));
        wr(&w, names[i], strlen(names[i]));
        wr_u32(&w, (unsigned)sizes[i]);
        wr(&w, blobs[i], sizes[i]);
    }
    *out_len = w.len;
    return w.buf;
}

/* Tile count of the last promoted snapshot (what is being animated toward). */
static size_t target_tiles(TesseraEngine* e) {
    return (e->state && e->state->target) ? e->state->target->tile_count : 0;
}

/* The game script: top-level registers defs from the bundle, the game fn
 * answers "start" with TWO sequential states and "tap" with one more. */
static const char* GAME_SRC =
    "assert(#tessera.asset_names() == 2)\n"
    "local atlas = tessera.register_atlas(tessera.asset('atlas.tga'))\n"
    "assert(atlas ~= 0, 'atlas failed')\n"
    "assert(tessera.asset('note.txt') == 'hello', 'blob bytes differ')\n"
    "local tile = tessera.register_tile_def{ atlas = atlas,\n"
    "  top = {0,0,1,1}, side = {0,0,1,1}, bottom = {0,0,1,1}, tint = {1,1,1,1} }\n"
    "assert(tile ~= 0, 'tile def failed')\n"
    /* a d20: 20 retained face-string pushes exceed LUA_MINSTACK without the
     * luaL_checkstack reservation in l_register_dice_def */
    "local faces = {}\n"
    "for i = 1, 20 do faces[i] = tessera.asset('atlas.tga') end\n"
    "local die = tessera.register_dice_def{ faces = faces, size = 1 }\n"
    "assert(die ~= 0, 'dice def (20 faces) failed')\n"
    /* sandbox: text chunks load, precompiled bytecode is rejected */
    "assert(load('return 1')() == 1, 'text load broken')\n"
    "assert(not load(string.dump(function() end)), 'bytecode not rejected')\n"
    "local cam = { mode = 'orbit', focus = {0, 0}, distance = 10,\n"
    "              pitch = 0.9, fov = 0.9 }\n"
    "return function(event)\n"
    "  if event.name == 'start' then\n"
    "    return {\n"
    "      { tiles = { {x=0, y=0, def=tile, id=1} }, camera = cam },\n"
    "      { tiles = { {x=0, y=0, def=tile, id=1},\n"
    "                  {x=1, y=0, def=tile, id=2} }, camera = cam },\n"
    "    }\n"
    "  elseif event.name == 'tap' then\n"
    "    local tx = math.tointeger(event.args[1])\n"
    "    return { { tiles = { {x=0, y=0, def=tile, id=1},\n"
    "                         {x=1, y=0, def=tile, id=2},\n"
    "                         {x=tx, y=0, def=tile, id=3} }, camera = cam } }\n"
    "  end\n"
    "  return {}\n"
    "end\n";

int main(void) {
    TesseraConfig cfg = { .width = 64, .height = 64, .pixel_density = 1.0f, .log = log_fn };
    TesseraEngine* e = tessera_create(&cfg);
    if (!e) { printf("create NULL\n"); return 1; }
    if (strcmp(tessera_backend_name(e), "none") == 0) {
        printf("SKIP: no GPU backend (%s)\n", tessera_last_error(e));
        tessera_destroy(e);
        return 0;
    }
    unsigned char buf[64 * 64 * 4];

    /* Fast deterministic timings so transitions settle in a few ticks. */
    TesseraTiming tm = { .move_s = 0.1f, .add_s = 0.1f, .remove_s = 0.1f,
                         .tile_s = 0.1f, .reflow_s = 0.1f, .camera_s = 0.1f,
                         .speed_multiplier = 1.0f };
    tessera_set_timing(e, &tm);

    /* ---- event before any game is loaded -> false ---- */
    CHECK(!tessera_lua_event(e, "tap", NULL, 0));

    /* ---- bundle: bad magic ---- */
    {
        unsigned char junk[16] = { 'J','U','N','K', 1,0,0,0, 0,0,0,0, 0,0,0,0 };
        TesseraBytes by = { .data = junk, .size = sizeof junk };
        CHECK(!tessera_lua_load_bundle(e, &by));
        CHECK(tessera_last_error(e)[0] != 0);
    }

    /* ---- build the real bundle: a 16x16 atlas + a text blob ---- */
    unsigned char px[16 * 16 * 4];
    for (int i = 0; i < 16 * 16; ++i) {
        px[i*4+0] = 200; px[i*4+1] = 80; px[i*4+2] = 40; px[i*4+3] = 255;
    }
    int tga_len = 0;
    unsigned char* tga = tga32(px, 16, 16, &tga_len);
    const char* names[2] = { "atlas.tga", "note.txt" };
    const void* blobs[2] = { tga, "hello" };
    size_t sizes[2] = { (size_t)tga_len, 5 };
    size_t bundle_len = 0;
    unsigned char* bundle = build_bundle(names, blobs, sizes, 2, &bundle_len);
    free(tga);

    /* ---- bundle: truncations at several byte lengths -> false ---- */
    {
        size_t cuts[4] = { 8, 13, bundle_len / 2, bundle_len - 1 };
        for (int i = 0; i < 4; ++i) {
            TesseraBytes by = { .data = bundle, .size = cuts[i] };
            CHECK(!tessera_lua_load_bundle(e, &by));
        }
    }

    /* ---- bundle: the real thing parses ---- */
    {
        TesseraBytes by = { .data = bundle, .size = bundle_len };
        CHECK(tessera_lua_load_bundle(e, &by));
    }
    free(bundle);

    /* ---- bad scripts -> false + error set ---- */
    {
        const char* syntax = "this is not lua ((";
        TesseraBytes by = { .data = syntax, .size = strlen(syntax) };
        CHECK(!tessera_lua_load_game(e, &by));
        CHECK(tessera_last_error(e)[0] != 0);

        const char* notfn = "return 42";
        TesseraBytes by2 = { .data = notfn, .size = strlen(notfn) };
        CHECK(!tessera_lua_load_game(e, &by2));

        /* a runtime error at chunk top-level also fails the load */
        const char* boom = "error('top-level boom')";
        TesseraBytes by3 = { .data = boom, .size = strlen(boom) };
        CHECK(!tessera_lua_load_game(e, &by3));
    }
    CHECK(!tessera_lua_event(e, "tap", NULL, 0));   /* still no game */

    /* ---- the real game loads (top-level registration succeeds) ---- */
    e->error[0] = 0;                    /* drop errors from the bad-input runs */
    uint32_t defs_before = tessera_def_count(e);
    {
        TesseraBytes by = { .data = GAME_SRC, .size = strlen(GAME_SRC),
                            .debug_name = "test_game" };
        CHECK(tessera_lua_load_game(e, &by));
    }
    /* the chunk registered atlas + tile def + a 20-face dice def */
    CHECK(tessera_def_count(e) == defs_before + 3);
    CHECK(tessera_sound_count(e) == 0);
    CHECK(tessera_def_count(NULL) == 0);
    CHECK(tessera_sound_count(NULL) == 0);

    /* ---- "start": two states must play IN ORDER, second after the first
     *      op settles. Watch the promoted snapshot's tile count grow 1 -> 2
     *      and record the tick each op completed. ---- */
    TesseraOpId base = tessera_last_completed_operation(e);
    int first_done_tick = -1, second_done_tick = -1;
    size_t tiles_when_first_done = 0, tiles_when_second_done = 0;
    for (int t = 0; t < 300 && second_done_tick < 0; ++t) {
        tick(e, buf);
        TesseraOpId done = tessera_last_completed_operation(e);
        if (first_done_tick < 0 && done >= base + 1) {
            first_done_tick = t;
            tiles_when_first_done = target_tiles(e);
        }
        if (second_done_tick < 0 && done >= base + 2) {
            second_done_tick = t;
            tiles_when_second_done = target_tiles(e);
        }
    }
    CHECK(first_done_tick >= 0);
    CHECK(second_done_tick > first_done_tick);      /* strictly sequential */
    CHECK(tiles_when_first_done == 1);              /* beat 1: one tile     */
    CHECK(tiles_when_second_done == 2);             /* beat 2: second tile  */
    CHECK(tessera_last_error(e)[0] == 0);           /* clean run            */

    /* ---- host event: one more state, tile placed at args[1] ---- */
    double args[2] = { 3.0, 0.0 };
    CHECK(tessera_lua_event(e, "tap", args, 2));
    for (int t = 0; t < 300; ++t) {
        tick(e, buf);
        if (tessera_last_completed_operation(e) >= base + 3) break;
    }
    CHECK(tessera_last_completed_operation(e) >= base + 3);
    CHECK(target_tiles(e) == 3);
    {
        /* the new tile landed at (3, 0) with id 3 */
        bool found = false;
        const TsSnapshot* s = e->state->target;
        for (size_t i = 0; s && i < s->tile_count; ++i)
            if (s->tiles[i].id == 3 && s->tiles[i].coord.x == 3 &&
                s->tiles[i].coord.y == 0) found = true;
        CHECK(found);
    }
    CHECK(tessera_last_error(e)[0] == 0);

    /* ---- an unknown event name reaches the game fn and returns {} ---- */
    CHECK(tessera_lua_event(e, "noop", NULL, 0));
    for (int t = 0; t < 30; ++t) tick(e, buf);
    CHECK(tessera_last_completed_operation(e) == base + 3);   /* no new op */

    tessera_destroy(e);
    printf(g_fail ? "test_lua: %d FAILURES\n" : "test_lua: OK\n", g_fail);
    return g_fail ? 1 : 0;
}
