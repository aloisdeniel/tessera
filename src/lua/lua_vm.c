/*
 * lua_vm.c — embedded Lua 5.4 game VM.
 *
 * Owns a sandboxed lua_State (base/table/string/math/utf8 only, print
 * redirected to the engine log), the "TSAB" asset-bundle blobs, the queued
 * host events and the FIFO of converted states. The game script's chunk
 * returns `function(event) -> array of state tables`; ts_lua_advance (tick
 * thread, called right after the op settle in ts_engine_advance) delivers one
 * event at a time and plays the returned states sequentially — each state is
 * pushed through tessera_set_state only after the previous operation settled.
 *
 * Threading: the lua_State is only touched on the load thread (before ticking
 * starts) and the tick thread — never concurrently (see the public API docs).
 * The event queue is the one any-thread entry point and is mutex-protected.
 * All allocations are plain heap (Lua's default allocator + malloc), never
 * the frame arena.
 */
#include "engine.h"
#include "lua/lua_vm.h"

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* ---- internal types --------------------------------------------------- */

typedef struct {
    char*    name;   /* NUL-terminated entry name        */
    uint8_t* data;   /* owned copy of the blob bytes     */
    size_t   size;
} TsLuaBlob;

typedef struct TsLuaEvent {
    char*              name;
    double*            args;      /* NULL when arg_count == 0 */
    size_t             arg_count;
    struct TsLuaEvent* next;
} TsLuaEvent;

struct TsLua {
    TesseraEngine* eng;
    lua_State*     L;

    TsLuaBlob*     blobs;         /* bundle entries (by-name lookup)     */
    size_t         blob_count, blob_cap;

    int            game_ref;      /* registry ref of the game fn (LUA_NOREF) */

    SDL_Mutex*     ev_mutex;      /* guards the event queue (any-thread) */
    TsLuaEvent*    ev_head;
    TsLuaEvent*    ev_tail;

    TsLuaPending*  st_head;       /* converted states awaiting playback  */
    TsLuaPending*  st_tail;       /* (tick/load thread only)             */

    TesseraOpId    inflight;      /* op id of the last pushed state      */
    bool           has_inflight;
};

/* Registry key under which the TsLua* is stashed for the C module fns. */
static const char TS_LUA_SELF_KEY = 0;

static TsLua* lua_self(lua_State* L) {
    lua_rawgetp(L, LUA_REGISTRYINDEX, &TS_LUA_SELF_KEY);
    TsLua* lua = (TsLua*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return lua;
}

/* ---- byte-span resolution (data or path, like the registry) ----------- */

/* Resolve a TesseraBytes to (data, size). When data == NULL the path is
 * loaded via SDL; *out_owned then holds the allocation to SDL_free. */
static const void* resolve_bytes(const TesseraBytes* b, size_t* out_size,
                                 void** out_owned) {
    *out_owned = NULL;
    if (!b) return NULL;
    if (b->data) { *out_size = b->size; return b->data; }
    if (!b->path) return NULL;
    size_t n = 0;
    void* p = SDL_LoadFile(b->path, &n);
    if (!p) return NULL;
    *out_owned = p;
    *out_size = n;
    return p;
}

/* ---- asset bundle (TSAB v1, little-endian) ----------------------------- */

#define TS_TSAB_MAGIC   0x42415354u  /* bytes 'T','S','A','B' on disk */
#define TS_TSAB_VERSION 1u

static uint32_t rd_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd_u16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static TsLuaBlob* blob_find(TsLua* lua, const char* name) {
    for (size_t i = 0; i < lua->blob_count; ++i)
        if (strcmp(lua->blobs[i].name, name) == 0) return &lua->blobs[i];
    return NULL;
}

/* Add (or override, per the load_bundle contract) one named blob. Copies. */
static bool blob_put(TsLua* lua, const char* name, size_t name_len,
                     const uint8_t* data, size_t size) {
    char* n = (char*)malloc(name_len + 1);
    uint8_t* d = (uint8_t*)malloc(size ? size : 1);
    if (!n || !d) { free(n); free(d); return false; }
    memcpy(n, name, name_len);
    n[name_len] = 0;
    if (size) memcpy(d, data, size);

    TsLuaBlob* slot = blob_find(lua, n);
    if (slot) {
        free(slot->name); free(slot->data);
    } else {
        if (lua->blob_count == lua->blob_cap) {
            size_t cap = lua->blob_cap ? lua->blob_cap * 2 : 8;
            TsLuaBlob* grown = (TsLuaBlob*)realloc(lua->blobs, cap * sizeof *grown);
            if (!grown) { free(n); free(d); return false; }
            lua->blobs = grown;
            lua->blob_cap = cap;
        }
        slot = &lua->blobs[lua->blob_count++];
    }
    slot->name = n;
    slot->data = d;
    slot->size = size;
    return true;
}

bool ts_lua_load_bundle(TsLua* lua, const TesseraBytes* bundle) {
    TesseraEngine* e = lua->eng;
    size_t len = 0;
    void* owned = NULL;
    const uint8_t* p = (const uint8_t*)resolve_bytes(bundle, &len, &owned);
    if (!p) { ts_engine_set_error(e, "lua bundle: no bytes"); return false; }

    bool ok = false;
    if (len < 12) { ts_engine_set_error(e, "lua bundle: truncated header"); goto out; }
    if (rd_u32(p) != TS_TSAB_MAGIC) {
        ts_engine_set_error(e, "lua bundle: bad magic (not a TSAB container)");
        goto out;
    }
    if (rd_u32(p + 4) != TS_TSAB_VERSION) {
        ts_engine_set_error(e, "lua bundle: unsupported version %u", rd_u32(p + 4));
        goto out;
    }
    uint32_t count = rd_u32(p + 8);

    /* Strict validation pass over every entry BEFORE committing anything, so
     * a truncated container never partially applies. */
    size_t off = 12;
    for (uint32_t i = 0; i < count; ++i) {
        if (off + 2 > len) { ts_engine_set_error(e, "lua bundle: truncated at entry %u", i); goto out; }
        uint16_t name_len = rd_u16(p + off); off += 2;
        if (name_len == 0 || off + name_len > len) {
            ts_engine_set_error(e, "lua bundle: bad name at entry %u", i); goto out;
        }
        off += name_len;
        if (off + 4 > len) { ts_engine_set_error(e, "lua bundle: truncated at entry %u", i); goto out; }
        uint32_t blob_len = rd_u32(p + off); off += 4;
        if (blob_len > len - off) {
            ts_engine_set_error(e, "lua bundle: blob overruns container at entry %u", i); goto out;
        }
        off += blob_len;
    }
    if (off != len) { ts_engine_set_error(e, "lua bundle: %zu trailing bytes", len - off); goto out; }

    /* Commit pass. */
    off = 12;
    for (uint32_t i = 0; i < count; ++i) {
        uint16_t name_len = rd_u16(p + off); off += 2;
        const char* name = (const char*)(p + off); off += name_len;
        uint32_t blob_len = rd_u32(p + off); off += 4;
        if (!blob_put(lua, name, name_len, p + off, blob_len)) {
            ts_engine_set_error(e, "lua bundle: out of memory");
            goto out;
        }
        off += blob_len;
    }
    ok = true;
out:
    if (owned) SDL_free(owned);
    return ok;
}

/* ---- event queue (any-thread) ------------------------------------------ */

static void event_free(TsLuaEvent* ev) {
    if (!ev) return;
    free(ev->name);
    free(ev->args);
    free(ev);
}

bool ts_lua_queue_event(TsLua* lua, const char* name, const double* args,
                        size_t arg_count) {
    if (!lua || !name) return false;
    TsLuaEvent* ev = (TsLuaEvent*)calloc(1, sizeof *ev);
    if (!ev) return false;
    ev->name = (char*)malloc(strlen(name) + 1);
    if (!ev->name) { free(ev); return false; }
    strcpy(ev->name, name);
    if (args && arg_count) {
        ev->args = (double*)malloc(arg_count * sizeof *args);
        if (!ev->args) { event_free(ev); return false; }
        memcpy(ev->args, args, arg_count * sizeof *args);
        ev->arg_count = arg_count;
    }
    /* game_ref is written by ts_lua_load_game under this same mutex, so the
     * "no game loaded" gate must sit inside the critical section too. */
    SDL_LockMutex(lua->ev_mutex);
    if (lua->game_ref == LUA_NOREF) {
        SDL_UnlockMutex(lua->ev_mutex);
        event_free(ev);
        return false;
    }
    if (lua->ev_tail) lua->ev_tail->next = ev;
    else              lua->ev_head = ev;
    lua->ev_tail = ev;
    SDL_UnlockMutex(lua->ev_mutex);
    return true;
}

static TsLuaEvent* event_pop(TsLua* lua) {
    SDL_LockMutex(lua->ev_mutex);
    TsLuaEvent* ev = lua->ev_head;
    if (ev) {
        lua->ev_head = ev->next;
        if (!lua->ev_head) lua->ev_tail = NULL;
    }
    SDL_UnlockMutex(lua->ev_mutex);
    return ev;
}

static void queues_clear(TsLua* lua) {
    TsLuaEvent* ev;
    while ((ev = event_pop(lua)) != NULL) event_free(ev);
    while (lua->st_head) {
        TsLuaPending* p = lua->st_head;
        lua->st_head = p->next;
        ts_lua_pending_free(p);
    }
    lua->st_tail = NULL;
    lua->has_inflight = false;
}

/* ---- tessera.* module -------------------------------------------------- */

/* tessera.asset(name) -> binary string (error on unknown name) */
static int l_asset(lua_State* L) {
    TsLua* lua = lua_self(L);
    const char* name = luaL_checkstring(L, 1);
    TsLuaBlob* b = blob_find(lua, name);
    if (!b) return luaL_error(L, "unknown bundle asset '%s'", name);
    lua_pushlstring(L, (const char*)b->data, b->size);
    return 1;
}

/* tessera.asset_names() -> array of entry names */
static int l_asset_names(lua_State* L) {
    TsLua* lua = lua_self(L);
    lua_createtable(L, (int)lua->blob_count, 0);
    for (size_t i = 0; i < lua->blob_count; ++i) {
        lua_pushstring(L, lua->blobs[i].name);
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    return 1;
}

/* A TesseraBytes over the Lua string at `idx` (borrowed; valid for the call). */
static TesseraBytes arg_bytes(lua_State* L, int idx) {
    size_t n = 0;
    const char* s = luaL_checklstring(L, idx, &n);
    TesseraBytes b = { s, n, NULL, NULL };
    return b;
}

/* tessera.register_atlas(bytes) -> id (0 on failure, see tessera_last_error) */
static int l_register_atlas(lua_State* L) {
    TsLua* lua = lua_self(L);
    TesseraBytes b = arg_bytes(L, 1);
    lua_pushinteger(L, (lua_Integer)tessera_register_atlas(lua->eng, &b));
    return 1;
}

/* Read one TesseraParticleSpec sub-table field (nil => all-zero spec). */
static void fld_particle_spec(lua_State* L, int idx, const char* key,
                              TesseraParticleSpec* out) {
    memset(out, 0, sizeof *out);
    idx = lua_absindex(L, idx);
    int t = lua_getfield(L, idx, key);
    if (t == LUA_TNIL) { lua_pop(L, 1); return; }
    if (t != LUA_TTABLE) {
        luaL_error(L, "field '%s' must be a particle-spec table", key);
        return; /* unreachable */
    }
    int s = lua_gettop(L);
    out->atlas        = (TesseraDefId)ts_luax_fld_int(L, s, "atlas", 0);
    ts_luax_fld_rect(L, s, "sprite", &out->sprite);
    out->mode         = (TesseraEmitMode)ts_luax_fld_int(L, s, "mode", 0);
    out->count        = (uint32_t)ts_luax_fld_int(L, s, "count", 0);
    out->lifetime_s   = ts_luax_fld_num(L, s, "lifetime_s", 0);
    out->lifetime_var = ts_luax_fld_num(L, s, "lifetime_var", 0);
    out->speed        = ts_luax_fld_num(L, s, "speed", 0);
    out->speed_var    = ts_luax_fld_num(L, s, "speed_var", 0);
    out->spread_deg   = ts_luax_fld_num(L, s, "spread_deg", 0);
    out->gravity      = ts_luax_fld_num(L, s, "gravity", 0);
    out->size_start   = ts_luax_fld_num(L, s, "size_start", 0);
    out->size_end     = ts_luax_fld_num(L, s, "size_end", 0);
    ts_luax_fld_color(L, s, "color_start", out->color_start);
    ts_luax_fld_color(L, s, "color_end", out->color_end);
    out->blend        = (TesseraBlendMode)ts_luax_fld_int(L, s, "blend", 0);
    out->duration_s   = ts_luax_fld_num(L, s, "duration_s", 0);
    lua_pop(L, 1);
}

/* tessera.register_tile_def{atlas=, top=, side=, bottom=, tint=, thickness=} */
static int l_register_tile_def(lua_State* L) {
    TsLua* lua = lua_self(L);
    luaL_checktype(L, 1, LUA_TTABLE);
    TesseraTileDef d;
    memset(&d, 0, sizeof d);
    d.atlas = (TesseraDefId)ts_luax_fld_int(L, 1, "atlas", 0);
    ts_luax_fld_rect(L, 1, "top", &d.top);
    ts_luax_fld_rect(L, 1, "side", &d.side);
    ts_luax_fld_rect(L, 1, "bottom", &d.bottom);
    ts_luax_fld_color(L, 1, "tint", d.tint);
    d.thickness = ts_luax_fld_num(L, 1, "thickness", 0);
    lua_pushinteger(L, (lua_Integer)tessera_register_tile_def(lua->eng, &d));
    return 1;
}

/* tessera.register_entity_def{gltf=, atlas=, scale=, pivot=, *_anim=, on_*_effect=} */
static int l_register_entity_def(lua_State* L) {
    TsLua* lua = lua_self(L);
    luaL_checktype(L, 1, LUA_TTABLE);
    TesseraEntityDef d;
    memset(&d, 0, sizeof d);
    int t = lua_getfield(L, 1, "gltf");
    if (t == LUA_TSTRING) {
        d.gltf.data = lua_tolstring(L, -1, &d.gltf.size);
    } else if (t != LUA_TNIL) {
        return luaL_error(L, "field 'gltf' must be a binary string");
    }
    /* the gltf string stays on the stack so the bytes remain valid */
    d.atlas = (TesseraDefId)ts_luax_fld_int(L, 1, "atlas", 0);
    d.scale = ts_luax_fld_num(L, 1, "scale", 0);
    ts_luax_fld_vec(L, 1, "pivot", d.pivot, 3);
    d.default_anim = (int32_t)ts_luax_fld_int(L, 1, "default_anim", -1);
    d.move_anim    = (int32_t)ts_luax_fld_int(L, 1, "move_anim", -1);
    d.spawn_anim   = (int32_t)ts_luax_fld_int(L, 1, "spawn_anim", -1);
    d.despawn_anim = (int32_t)ts_luax_fld_int(L, 1, "despawn_anim", -1);
    d.on_spawn_effect   = (TesseraDefId)ts_luax_fld_int(L, 1, "on_spawn_effect", 0);
    d.on_despawn_effect = (TesseraDefId)ts_luax_fld_int(L, 1, "on_despawn_effect", 0);
    lua_pushinteger(L, (lua_Integer)tessera_register_entity_def(lua->eng, &d));
    return 1;
}

/* tessera.register_effect_def{on_add=, on_remove=} */
static int l_register_effect_def(lua_State* L) {
    TsLua* lua = lua_self(L);
    luaL_checktype(L, 1, LUA_TTABLE);
    TesseraEffectDef d;
    fld_particle_spec(L, 1, "on_add", &d.on_add);
    fld_particle_spec(L, 1, "on_remove", &d.on_remove);
    lua_pushinteger(L, (lua_Integer)tessera_register_effect_def(lua->eng, &d));
    return 1;
}

/* tessera.register_card_def{visible_atlas=, visible_uv=, hidden_atlas=, ...} */
static int l_register_card_def(lua_State* L) {
    TsLua* lua = lua_self(L);
    luaL_checktype(L, 1, LUA_TTABLE);
    TesseraCardDef d;
    memset(&d, 0, sizeof d);
    d.visible_atlas = (TesseraDefId)ts_luax_fld_int(L, 1, "visible_atlas", 0);
    ts_luax_fld_rect(L, 1, "visible_uv", &d.visible_uv);
    d.hidden_atlas = (TesseraDefId)ts_luax_fld_int(L, 1, "hidden_atlas", 0);
    ts_luax_fld_rect(L, 1, "hidden_uv", &d.hidden_uv);
    d.back_atlas = (TesseraDefId)ts_luax_fld_int(L, 1, "back_atlas", 0);
    ts_luax_fld_rect(L, 1, "back_uv", &d.back_uv);
    d.width         = ts_luax_fld_num(L, 1, "width", 0);
    d.height        = ts_luax_fld_num(L, 1, "height", 0);
    d.thickness     = ts_luax_fld_num(L, 1, "thickness", 0);
    d.corner_radius = ts_luax_fld_num(L, 1, "corner_radius", 0);
    ts_luax_fld_color(L, 1, "tint", d.tint);
    lua_pushinteger(L, (lua_Integer)tessera_register_card_def(lua->eng, &d));
    return 1;
}

/* tessera.register_dice_def{faces={bytes, ...}, size=, tint=} */
static int l_register_dice_def(lua_State* L) {
    TsLua* lua = lua_self(L);
    luaL_checktype(L, 1, LUA_TTABLE);
    TesseraDiceDef d;
    memset(&d, 0, sizeof d);
    /* scalars first: a type error here must not leak the faces array below */
    d.size = ts_luax_fld_num(L, 1, "size", 0);
    ts_luax_fld_color(L, 1, "tint", d.tint);
    if (lua_getfield(L, 1, "faces") != LUA_TTABLE)
        return luaL_error(L, "field 'faces' must be an array of sprite byte strings");
    int faces_idx = lua_gettop(L);
    size_t count = lua_rawlen(L, faces_idx);
    /* One retained string push per face below (the spans must stay valid), so
     * reserve stack space up front — a lua_CFunction is only guaranteed
     * LUA_MINSTACK free slots. Raised BEFORE the calloc so the error path
     * cannot leak `faces`. */
    if (count > (size_t)INT_MAX - 4)
        return luaL_error(L, "too many faces");
    luaL_checkstack(L, (int)count + 4, "dice faces");
    TesseraDiceFace* faces = NULL;
    if (count) {
        faces = (TesseraDiceFace*)calloc(count, sizeof *faces);
        if (!faces) return luaL_error(L, "out of memory");
    }
    for (size_t i = 0; i < count; ++i) {
        /* face strings stay on the stack so the byte spans remain valid */
        if (lua_rawgeti(L, faces_idx, (lua_Integer)i + 1) != LUA_TSTRING) {
            free(faces);
            return luaL_error(L, "faces[%d] must be a binary string", (int)i + 1);
        }
        faces[i].sprite.data = lua_tolstring(L, -1, &faces[i].sprite.size);
    }
    d.faces = faces;
    d.face_count = count;
    TesseraDefId id = tessera_register_dice_def(lua->eng, &d);
    free(faces);
    lua_pushinteger(L, (lua_Integer)id);
    return 1;
}

/* tessera.register_font(ttf_bytes, size_px) -> id */
static int l_register_font(lua_State* L) {
    TsLua* lua = lua_self(L);
    TesseraBytes b = arg_bytes(L, 1);
    float px = (float)luaL_optnumber(L, 2, 0);
    lua_pushinteger(L, (lua_Integer)tessera_register_font(lua->eng, &b, px));
    return 1;
}

/* tessera.register_sound(wav_bytes) -> id */
static int l_register_sound(lua_State* L) {
    TsLua* lua = lua_self(L);
    TesseraBytes b = arg_bytes(L, 1);
    lua_pushinteger(L, (lua_Integer)tessera_register_sound(lua->eng, &b));
    return 1;
}

/* tessera.play_sound(id [, volume]) */
static int l_play_sound(lua_State* L) {
    TsLua* lua = lua_self(L);
    TesseraSoundId id = (TesseraSoundId)luaL_checkinteger(L, 1);
    float gain = (float)luaL_optnumber(L, 2, 1.0);
    lua_pushboolean(L, tessera_play_sound(lua->eng, id, gain));
    return 1;
}

/* tessera.log(msg) — INFO log */
static int l_log(lua_State* L) {
    TsLua* lua = lua_self(L);
    TS_LOGI(&lua->eng->log, "lua: %s", luaL_checkstring(L, 1));
    return 0;
}

/* print(...) — redirected to the engine log (INFO), tab-separated. */
static int l_print(lua_State* L) {
    TsLua* lua = lua_self(L);
    luaL_Buffer buf;
    luaL_buffinit(L, &buf);
    int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i) {
        if (i > 1) luaL_addchar(&buf, '\t');
        luaL_tolstring(L, i, NULL);
        luaL_addvalue(&buf);
    }
    luaL_pushresult(&buf);
    TS_LOGI(&lua->eng->log, "lua: %s", lua_tostring(L, -1));
    lua_pop(L, 1);
    return 0;
}

static const luaL_Reg TS_LUA_MODULE[] = {
    { "asset",               l_asset },
    { "asset_names",         l_asset_names },
    { "register_atlas",      l_register_atlas },
    { "register_tile_def",   l_register_tile_def },
    { "register_entity_def", l_register_entity_def },
    { "register_effect_def", l_register_effect_def },
    { "register_card_def",   l_register_card_def },
    { "register_dice_def",   l_register_dice_def },
    { "register_font",       l_register_font },
    { "register_sound",      l_register_sound },
    { "play_sound",          l_play_sound },
    { "log",                 l_log },
    { NULL, NULL }
};

/* Enum constants exported on the tessera module. */
typedef struct { const char* name; lua_Integer value; } TsLuaConst;
static const TsLuaConst TS_LUA_CONSTS[] = {
    { "EMIT_BURST",          TESSERA_EMIT_BURST },
    { "EMIT_CONTINUOUS",     TESSERA_EMIT_CONTINUOUS },
    { "BLEND_ALPHA",         TESSERA_BLEND_ALPHA },
    { "BLEND_ADD",           TESSERA_BLEND_ADD },
    { "OVERLAY_SPRITE",      TESSERA_OVERLAY_SPRITE },
    { "OVERLAY_DISC",        TESSERA_OVERLAY_DISC },
    { "OVERLAY_RING",        TESSERA_OVERLAY_RING },
    { "LABEL_ANCHOR_WORLD",  TESSERA_LABEL_ANCHOR_WORLD },
    { "LABEL_ANCHOR_ENTITY", TESSERA_LABEL_ANCHOR_ENTITY },
    { "LABEL_ANCHOR_TILE",   TESSERA_LABEL_ANCHOR_TILE },
    { "LABEL_ANCHOR_DICE",   TESSERA_LABEL_ANCHOR_DICE },
    { "LABEL_ANCHOR_CARD",   TESSERA_LABEL_ANCHOR_CARD },
    { "LABEL_ANCHOR_DRAW",   TESSERA_LABEL_ANCHOR_DRAW },
    { "HIGHLIGHT_ENTITY",    TESSERA_HIGHLIGHT_ENTITY },
    { "HIGHLIGHT_TILE",      TESSERA_HIGHLIGHT_TILE },
    { "HIGHLIGHT_DICE",      TESSERA_HIGHLIGHT_DICE },
    { "HIGHLIGHT_CARD",      TESSERA_HIGHLIGHT_CARD },
    { "HIGHLIGHT_OUTLINE",   TESSERA_HIGHLIGHT_OUTLINE },
    { "HIGHLIGHT_GLOW",      TESSERA_HIGHLIGHT_GLOW },
    { "CAMERA_ORBIT",        TESSERA_CAMERA_ORBIT },
    { "CAMERA_MANUAL",       TESSERA_CAMERA_MANUAL },
    { "CAMERA_TARGET",       TESSERA_CAMERA_TARGET },
    { "CAMERA_FOCUS_TILE",   TESSERA_CAMERA_FOCUS_TILE },
    { "CAMERA_FOCUS_ENTITY", TESSERA_CAMERA_FOCUS_ENTITY },
    { "CAMERA_FOCUS_DICE",   TESSERA_CAMERA_FOCUS_DICE },
    { "CAMERA_FOCUS_DRAW",   TESSERA_CAMERA_FOCUS_DRAW },
    { "CAMERA_FOCUS_CARD",   TESSERA_CAMERA_FOCUS_CARD },
    { "CAMERA_FOCUS_HAND",   TESSERA_CAMERA_FOCUS_HAND },
    { NULL, 0 }
};

/* ---- VM lifecycle ------------------------------------------------------ */

/* load() with the chunk mode forced to "t": Lua does not verify precompiled
 * bytecode, so binary chunks must not be loadable from inside the sandbox.
 * Upvalue 1 is the original base load. */
static int l_load_text(lua_State* L) {
    int n = lua_gettop(L);                  /* chunk [,name [,mode [,env]]] */
    if (n < 2) { lua_settop(L, 2); n = 2; }
    if (n > 4) { lua_settop(L, 4); n = 4; }
    lua_pushvalue(L, lua_upvalueindex(1));  /* original base load */
    lua_pushvalue(L, 1);                    /* chunk */
    lua_pushvalue(L, 2);                    /* chunkname (or nil) */
    lua_pushliteral(L, "t");                /* mode: text only */
    if (n == 4) lua_pushvalue(L, 4);        /* env */
    lua_call(L, (n == 4) ? 4 : 3, LUA_MULTRET);
    return lua_gettop(L) - n;
}

static void open_sandbox(lua_State* L) {
    /* base, table, string, math, utf8 — no io/os/package/debug. */
    luaL_requiref(L, LUA_GNAME, luaopen_base, 1);       lua_pop(L, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1); lua_pop(L, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);lua_pop(L, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1); lua_pop(L, 1);
    luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1); lua_pop(L, 1);
    /* base leaks filesystem access through dofile/loadfile — remove them. */
    lua_pushnil(L); lua_setglobal(L, "dofile");
    lua_pushnil(L); lua_setglobal(L, "loadfile");
    /* Lua does not verify bytecode: restrict load() to text chunks. */
    lua_getglobal(L, "load");
    lua_pushcclosure(L, l_load_text, 1);
    lua_setglobal(L, "load");
}

TsLua* ts_lua_create(TesseraEngine* e) {
    TsLua* lua = (TsLua*)calloc(1, sizeof *lua);
    if (!lua) return NULL;
    lua->eng = e;
    lua->game_ref = LUA_NOREF;
    lua->ev_mutex = SDL_CreateMutex();
    lua->L = luaL_newstate();
    if (!lua->ev_mutex || !lua->L) { ts_lua_destroy(lua); return NULL; }

    lua_State* L = lua->L;
    /* stash self for the module C functions */
    lua_pushlightuserdata(L, lua);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &TS_LUA_SELF_KEY);

    open_sandbox(L);
    lua_pushcfunction(L, l_print);
    lua_setglobal(L, "print");

    /* global `tessera` module: functions + enum constants */
    luaL_newlib(L, TS_LUA_MODULE);
    for (const TsLuaConst* c = TS_LUA_CONSTS; c->name; ++c) {
        lua_pushinteger(L, c->value);
        lua_setfield(L, -2, c->name);
    }
    lua_setglobal(L, "tessera");
    return lua;
}

void ts_lua_destroy(TsLua* lua) {
    if (!lua) return;
    queues_clear(lua);
    if (lua->L) lua_close(lua->L);
    if (lua->ev_mutex) SDL_DestroyMutex(lua->ev_mutex);
    for (size_t i = 0; i < lua->blob_count; ++i) {
        free(lua->blobs[i].name);
        free(lua->blobs[i].data);
    }
    free(lua->blobs);
    free(lua);
}

/* ---- game script ------------------------------------------------------- */

bool ts_lua_load_game(TsLua* lua, const TesseraBytes* script) {
    TesseraEngine* e = lua->eng;
    lua_State* L = lua->L;
    size_t len = 0;
    void* owned = NULL;
    const char* src = (const char*)resolve_bytes(script, &len, &owned);
    if (!src) { ts_engine_set_error(e, "lua game: no bytes"); return false; }

    const char* chunkname = (script->debug_name && script->debug_name[0])
                          ? script->debug_name : "game";
    bool ok = false;
    if (luaL_loadbuffer(L, src, len, chunkname) != LUA_OK ||
        lua_pcall(L, 0, 1, 0) != LUA_OK) {
        const char* msg = lua_tostring(L, -1);
        TS_LOGE(&e->log, "lua: load failed: %s", msg ? msg : "?");
        ts_engine_set_error(e, "lua game: %s", msg ? msg : "load failed");
        lua_pop(L, 1);
        goto out;
    }
    if (!lua_isfunction(L, -1)) {
        ts_engine_set_error(e, "lua game: chunk must return a function(event)");
        lua_pop(L, 1);
        goto out;
    }
    /* Replace any previous game: drop its ref, queued events and states.
     * Pending states are tick/load-thread-only data (load_game is documented
     * not-concurrent-with-tick), so they need no lock. */
    while (lua->st_head) {
        TsLuaPending* p = lua->st_head;
        lua->st_head = p->next;
        ts_lua_pending_free(p);
    }
    lua->st_tail = NULL;
    lua->has_inflight = false;

    /* Build the synthetic "start" event before taking the lock (no allocation
     * inside the critical section). */
    TsLuaEvent* start_ev = (TsLuaEvent*)calloc(1, sizeof *start_ev);
    if (start_ev) {
        start_ev->name = (char*)malloc(sizeof "start");
        if (!start_ev->name) { free(start_ev); start_ev = NULL; }
        else memcpy(start_ev->name, "start", sizeof "start");
    }
    luaL_unref(L, LUA_REGISTRYINDEX, lua->game_ref);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);

    /* Publish the new game ref and install "start" as the sole queued event
     * in ONE ev_mutex hold: an any-thread tessera_lua_event racing this load
     * can neither race the game_ref write nor be delivered to the new game
     * before {name="start"}. Stale events are freed outside the lock. */
    SDL_LockMutex(lua->ev_mutex);
    lua->game_ref = ref;
    TsLuaEvent* stale = lua->ev_head;
    lua->ev_head = start_ev;            /* NULL on OOM: queue just empties */
    lua->ev_tail = start_ev;
    SDL_UnlockMutex(lua->ev_mutex);
    while (stale) {
        TsLuaEvent* next = stale->next;
        event_free(stale);
        stale = next;
    }
    ok = true;
out:
    if (owned) SDL_free(owned);
    return ok;
}

/* Deliver one event to the game function and append every returned state to
 * the playback FIFO. Lua errors are logged + set as last_error; the event is
 * dropped and playback continues. Tick thread (or load thread pre-tick). */
static void run_game_event(TsLua* lua, const TsLuaEvent* ev) {
    TesseraEngine* e = lua->eng;
    lua_State* L = lua->L;

    lua_rawgeti(L, LUA_REGISTRYINDEX, lua->game_ref);
    lua_createtable(L, 0, 2);
    lua_pushstring(L, ev->name);
    lua_setfield(L, -2, "name");
    if (ev->arg_count) {
        lua_createtable(L, (int)ev->arg_count, 0);
        for (size_t i = 0; i < ev->arg_count; ++i) {
            lua_pushnumber(L, ev->args[i]);
            lua_rawseti(L, -2, (lua_Integer)i + 1);
        }
        lua_setfield(L, -2, "args");
    }
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        const char* msg = lua_tostring(L, -1);
        TS_LOGE(&e->log, "lua: game error on '%s': %s", ev->name, msg ? msg : "?");
        ts_engine_set_error(e, "lua game error: %s", msg ? msg : "?");
        lua_pop(L, 1);
        return;
    }
    if (lua_isnil(L, -1)) { lua_pop(L, 1); return; }
    if (!lua_istable(L, -1)) {
        TS_LOGE(&e->log, "lua: game fn for '%s' must return an array of state tables", ev->name);
        ts_engine_set_error(e, "lua game error: return value is not a table");
        lua_pop(L, 1);
        return;
    }

    lua_Integer n = (lua_Integer)lua_rawlen(L, -1);
    for (lua_Integer i = 1; i <= n; ++i) {
        TsLuaPending* out = NULL;
        lua_pushcfunction(L, ts_lua_read_state);
        lua_rawgeti(L, -2, i);              /* the state table */
        lua_pushlightuserdata(L, &out);
        if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
            const char* msg = lua_tostring(L, -1);
            TS_LOGE(&e->log, "lua: bad state %d from '%s': %s",
                    (int)i, ev->name, msg ? msg : "?");
            ts_engine_set_error(e, "lua state error: %s", msg ? msg : "?");
            lua_pop(L, 1);
            ts_lua_pending_free(out);       /* partial build, if any */
            continue;
        }
        if (!out) continue;
        if (lua->st_tail) lua->st_tail->next = out;
        else              lua->st_head = out;
        lua->st_tail = out;
    }
    lua_pop(L, 1);                          /* the returned array */
}

/* ---- sequential playback (tick thread, after the op settle) ------------ */

void ts_lua_advance(TesseraEngine* e) {
    TsLua* lua = e->lua;
    if (!lua) return;

    /* Still waiting on the previously pushed state's transition? */
    if (lua->has_inflight) {
        if (!tessera_operation_completed(e, lua->inflight)) return;
        lua->has_inflight = false;
    }

    /* Nothing queued to play: deliver at most ONE pending event, whose
     * returned states land in the FIFO and start playing this same tick. */
    if (!lua->st_head && lua->game_ref != LUA_NOREF) {
        TsLuaEvent* ev = event_pop(lua);
        if (!ev) return;
        run_game_event(lua, ev);
        event_free(ev);
    }

    if (lua->st_head) {
        TsLuaPending* p = lua->st_head;
        lua->st_head = p->next;
        if (!lua->st_head) lua->st_tail = NULL;
        TesseraOpId id = tessera_set_state(e, &p->st);
        ts_lua_pending_free(p);
        if (id) { lua->inflight = id; lua->has_inflight = true; }
    }
}
