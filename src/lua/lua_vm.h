/*
 * lua_vm.h — embedded Lua 5.4 game VM (internal API).
 *
 * The public tessera_lua_* wrappers (src/tessera.c) forward here. TsLua owns
 * the lua_State, the TSAB bundle blobs, the queued host events and the FIFO
 * of converted states awaiting sequential playback; ts_lua_advance (declared
 * in engine.h, called from the tick path) drives everything on the tick
 * thread. All memory is plain heap — never the frame arena.
 */
#ifndef TESSERA_LUA_VM_H
#define TESSERA_LUA_VM_H

#include "tessera.h"

typedef struct TsLua TsLua;
typedef struct lua_State lua_State;

TsLua* ts_lua_create(TesseraEngine* e);
void   ts_lua_destroy(TsLua* lua);

/* Backing implementations of the public tessera_lua_* API (same contracts;
 * see include/tessera.h). Errors land in the engine's last-error slot. */
bool ts_lua_load_bundle(TsLua* lua, const TesseraBytes* bundle);
bool ts_lua_load_game(TsLua* lua, const TesseraBytes* script);
bool ts_lua_queue_event(TsLua* lua, const char* name, const double* args, size_t arg_count);

/* ---- shared between lua_vm.c and lua_state_read.c -------------------- */

/* One converted state queued for playback: a heap TesseraState whose arrays
 * (and per-placement paths) are individually malloc'd. */
typedef struct TsLuaPending {
    TesseraState         st;
    struct TsLuaPending* next;
} TsLuaPending;

void ts_lua_pending_free(TsLuaPending* p);

/* Protected state-table converter (lua_state_read.c). lua_CFunction contract:
 * arg 1 = the state table, arg 2 = lightuserdata TsLuaPending** out. Raises a
 * lua error on wrong types (the caller pcalls it and frees *out on failure). */
int ts_lua_read_state(lua_State* L);

/* Table field readers shared with the def-registration glue (lua_state_read.c).
 * Each reads `key` out of the table at `idx`; a missing/nil field yields the
 * default (or leaves `out` untouched); a present field of the wrong type or
 * arity raises a lua error. */
float    ts_luax_fld_num(lua_State* L, int idx, const char* key, float def);
int64_t  ts_luax_fld_int(lua_State* L, int idx, const char* key, int64_t def);
bool     ts_luax_fld_bool(lua_State* L, int idx, const char* key, bool def);
/* Array of exactly `n` numbers (positions {x,y,z}, quaternions {x,y,z,w}). */
void     ts_luax_fld_vec(lua_State* L, int idx, const char* key, float* out, int n);
/* RGBA color array: {r,g,b,a} or {r,g,b} (alpha then defaults to 1). */
void     ts_luax_fld_color(lua_State* L, int idx, const char* key, float out[4]);
/* TesseraRect as {u0,v0,u1,v1} (array) or named keys {u0=,v0=,u1=,v1=}. */
void     ts_luax_fld_rect(lua_State* L, int idx, const char* key, TesseraRect* r);
/* Enum as an integer or the lowercase suffix string (names[value]); NULL-
 * terminated `names`. */
uint32_t ts_luax_fld_enum(lua_State* L, int idx, const char* key,
                          const char* const* names, uint32_t def);

#endif /* TESSERA_LUA_VM_H */
