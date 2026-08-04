/*
 * lua_state_read.c — Lua state-table -> TesseraState converter.
 *
 * Mirrors ts_snapshot_copy / src/serialize.c: every placement array is walked
 * explicitly, field by field, in declaration order. Field names match the C
 * struct fields (snake_case) with the documented shorthands: `x`/`y` for a
 * TesseraCoord `coord`, `entity`/`card` for attach_entity_id/attach_card_id,
 * `slot` for hand_slot and `target` for a highlight's target_id. Unknown keys
 * are ignored silently; wrong types raise a lua error (the caller pcalls the
 * converter, reports the error and drops the state). Enum-typed fields accept
 * an integer or the lowercase suffix of the C enum constant ("orbit", "disc",
 * "glow", ...). Also home to the ts_luax_fld_* table readers shared with the
 * def-registration glue in lua_vm.c.
 */
#include "lua/lua_vm.h"

#include "lua.h"
#include "lauxlib.h"

#include <stdlib.h>
#include <string.h>

/* ---- shared field readers --------------------------------------------- */

float ts_luax_fld_num(lua_State* L, int idx, const char* key, float def) {
    idx = lua_absindex(L, idx);
    float v = def;
    if (lua_getfield(L, idx, key) != LUA_TNIL) {
        if (!lua_isnumber(L, -1))
            luaL_error(L, "field '%s' must be a number", key);
        v = (float)lua_tonumber(L, -1);
    }
    lua_pop(L, 1);
    return v;
}

int64_t ts_luax_fld_int(lua_State* L, int idx, const char* key, int64_t def) {
    idx = lua_absindex(L, idx);
    int64_t v = def;
    if (lua_getfield(L, idx, key) != LUA_TNIL) {
        int isint = 0;
        v = (int64_t)lua_tointegerx(L, -1, &isint);
        if (!isint)
            luaL_error(L, "field '%s' must be an integer", key);
    }
    lua_pop(L, 1);
    return v;
}

bool ts_luax_fld_bool(lua_State* L, int idx, const char* key, bool def) {
    idx = lua_absindex(L, idx);
    bool v = def;
    int t = lua_getfield(L, idx, key);
    if (t != LUA_TNIL) {
        if (t != LUA_TBOOLEAN)
            luaL_error(L, "field '%s' must be a boolean", key);
        v = lua_toboolean(L, -1) != 0;
    }
    lua_pop(L, 1);
    return v;
}

/* Read `want` numbers out of the array at stack top into out. */
static void read_num_array(lua_State* L, const char* key, float* out, int want) {
    if ((int)lua_rawlen(L, -1) != want)
        luaL_error(L, "field '%s' must be an array of %d numbers", key, want);
    for (int i = 0; i < want; ++i) {
        if (lua_rawgeti(L, -1, i + 1) != LUA_TNUMBER)
            luaL_error(L, "field '%s'[%d] must be a number", key, i + 1);
        out[i] = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);
    }
}

void ts_luax_fld_vec(lua_State* L, int idx, const char* key, float* out, int n) {
    idx = lua_absindex(L, idx);
    int t = lua_getfield(L, idx, key);
    if (t != LUA_TNIL) {
        if (t != LUA_TTABLE)
            luaL_error(L, "field '%s' must be an array of %d numbers", key, n);
        read_num_array(L, key, out, n);
    }
    lua_pop(L, 1);
}

void ts_luax_fld_color(lua_State* L, int idx, const char* key, float out[4]) {
    idx = lua_absindex(L, idx);
    int t = lua_getfield(L, idx, key);
    if (t != LUA_TNIL) {
        if (t != LUA_TTABLE)
            luaL_error(L, "field '%s' must be a color array {r,g,b[,a]}", key);
        if (lua_rawlen(L, -1) == 3) {
            read_num_array(L, key, out, 3);
            out[3] = 1.0f;
        } else {
            read_num_array(L, key, out, 4);
        }
    }
    lua_pop(L, 1);
}

void ts_luax_fld_rect(lua_State* L, int idx, const char* key, TesseraRect* r) {
    idx = lua_absindex(L, idx);
    int t = lua_getfield(L, idx, key);
    if (t != LUA_TNIL) {
        if (t != LUA_TTABLE)
            luaL_error(L, "field '%s' must be a rect {u0,v0,u1,v1}", key);
        if (lua_getfield(L, -1, "u0") != LUA_TNIL) {      /* named keys */
            lua_pop(L, 1);
            r->u0 = ts_luax_fld_num(L, -1, "u0", 0);
            r->v0 = ts_luax_fld_num(L, -1, "v0", 0);
            r->u1 = ts_luax_fld_num(L, -1, "u1", 0);
            r->v1 = ts_luax_fld_num(L, -1, "v1", 0);
        } else {                                          /* array form */
            lua_pop(L, 1);
            float v[4];
            read_num_array(L, key, v, 4);
            r->u0 = v[0]; r->v0 = v[1]; r->u1 = v[2]; r->v1 = v[3];
        }
    }
    lua_pop(L, 1);
}

uint32_t ts_luax_fld_enum(lua_State* L, int idx, const char* key,
                          const char* const* names, uint32_t def) {
    idx = lua_absindex(L, idx);
    uint32_t v = def;
    int t = lua_getfield(L, idx, key);
    if (t == LUA_TSTRING) {
        const char* s = lua_tostring(L, -1);
        uint32_t i = 0;
        for (; names[i]; ++i)
            if (strcmp(names[i], s) == 0) { v = i; break; }
        if (!names[i])
            luaL_error(L, "field '%s': unknown name '%s'", key, s);
    } else if (t != LUA_TNIL) {
        int isint = 0;
        v = (uint32_t)lua_tointegerx(L, -1, &isint);
        if (!isint)
            luaL_error(L, "field '%s' must be an integer or a name string", key);
    }
    lua_pop(L, 1);
    return v;
}

/* ---- converter internals ----------------------------------------------- */

/* Enum name tables (index == enum value, lowercase C constant suffix). */
static const char* const CAMERA_MODES[] = {
    "orbit", "manual", "target", "focus_tile", "focus_entity", "focus_dice",
    "focus_draw", "focus_card", "focus_hand", NULL
};
static const char* const OVERLAY_SHAPES[]   = { "sprite", "disc", "ring", NULL };
static const char* const LABEL_ANCHORS[]    = { "world", "entity", "tile", "dice", "card", "draw", NULL };
static const char* const HIGHLIGHT_KINDS[]  = { "entity", "tile", "dice", "card", NULL };
static const char* const HIGHLIGHT_STYLES[] = { "outline", "glow", NULL };

/* Fetch the array field `key` from the state table (arg 1). Returns the
 * element count and leaves the array on the stack top; 0 elements (or a nil
 * field) push nothing. Allocates count*elem zeroed bytes into *out. */
static size_t begin_array(lua_State* L, const char* key, size_t elem, void** out) {
    *out = NULL;
    int t = lua_getfield(L, 1, key);
    if (t == LUA_TNIL) { lua_pop(L, 1); return 0; }
    if (t != LUA_TTABLE)
        luaL_error(L, "'%s' must be an array of tables", key);
    size_t n = lua_rawlen(L, -1);
    if (!n) { lua_pop(L, 1); return 0; }
    *out = calloc(n, elem);
    if (!*out)
        luaL_error(L, "'%s': out of memory", key);
    return n;
}

/* Push element i (1-based) of the array at the top; must be a table. */
static void elem_table(lua_State* L, const char* key, size_t i) {
    if (lua_rawgeti(L, -1, (lua_Integer)i) != LUA_TTABLE)
        luaL_error(L, "%s[%d] must be a table", key, (int)i);
}

/* Read a board coord from the table at `idx`: named {x=,y=} (either may be
 * omitted => 0) or a two-element array {x, y}. */
static TesseraCoord read_coord(lua_State* L, int idx, const char* what) {
    idx = lua_absindex(L, idx);
    TesseraCoord c = { 0, 0 };
    if (lua_rawlen(L, idx) == 2) {  /* array form */
        float v[2];
        lua_pushvalue(L, idx);      /* read_num_array wants the array on top */
        read_num_array(L, what, v, 2);
        lua_pop(L, 1);
        c.x = (int32_t)v[0];
        c.y = (int32_t)v[1];
    } else {
        c.x = (int32_t)ts_luax_fld_int(L, idx, "x", 0);
        c.y = (int32_t)ts_luax_fld_int(L, idx, "y", 0);
    }
    return c;
}

static void read_camera(lua_State* L, TesseraCamera* cam) {
    int t = lua_getfield(L, 1, "camera");
    if (t == LUA_TNIL) { lua_pop(L, 1); return; }
    if (t != LUA_TTABLE)
        luaL_error(L, "'camera' must be a table");
    int c = lua_gettop(L);
    cam->mode = ts_luax_fld_enum(L, c, "mode", CAMERA_MODES, 0);
    if (lua_getfield(L, c, "focus") != LUA_TNIL) {   /* {x,y} floats, named or array */
        if (!lua_istable(L, -1))
            luaL_error(L, "field 'focus' must be {x, y}");
        lua_getfield(L, -1, "x");
        bool named = !lua_isnil(L, -1);
        lua_pop(L, 1);
        if (named) {
            cam->focus.x = ts_luax_fld_num(L, -1, "x", 0);
            cam->focus.y = ts_luax_fld_num(L, -1, "y", 0);
        } else {
            float v[2];
            read_num_array(L, "focus", v, 2);
            cam->focus.x = v[0];
            cam->focus.y = v[1];
        }
    }
    lua_pop(L, 1);
    cam->distance = ts_luax_fld_num(L, c, "distance", 0);
    cam->yaw      = ts_luax_fld_num(L, c, "yaw", 0);
    cam->pitch    = ts_luax_fld_num(L, c, "pitch", 0);
    cam->fov      = ts_luax_fld_num(L, c, "fov", 0);
    ts_luax_fld_vec(L, c, "position", cam->position, 3);
    ts_luax_fld_vec(L, c, "orientation", cam->orientation, 4);
    ts_luax_fld_vec(L, c, "target", cam->target, 3);
    cam->target_id     = (uint64_t)ts_luax_fld_int(L, c, "target_id", 0);
    cam->focus_card_id = (uint64_t)ts_luax_fld_int(L, c, "focus_card_id", 0);
    cam->fit_padding   = ts_luax_fld_num(L, c, "fit_padding", 0);
    lua_pop(L, 1);
}

static void read_tiles(lua_State* L, TesseraState* st) {
    void* arr = NULL;
    size_t n = begin_array(L, "tiles", sizeof(TesseraTilePlacement), &arr);
    if (!n) return;
    TesseraTilePlacement* v = (TesseraTilePlacement*)arr;
    st->tiles = v;
    st->tile_count = n;
    for (size_t i = 1; i <= n; ++i, ++v) {
        elem_table(L, "tiles", i);
        v->coord    = read_coord(L, -1, "tiles");
        v->tile_def = (TesseraDefId)ts_luax_fld_int(L, -1, "def", 0);
        v->variant  = (uint32_t)ts_luax_fld_int(L, -1, "variant", 0);
        v->id       = (TesseraTileId)ts_luax_fld_int(L, -1, "id", 0);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

static void read_entities(lua_State* L, TesseraState* st) {
    void* arr = NULL;
    size_t n = begin_array(L, "entities", sizeof(TesseraEntityPlacement), &arr);
    if (!n) return;
    TesseraEntityPlacement* v = (TesseraEntityPlacement*)arr;
    st->entities = v;
    st->entity_count = n;
    for (size_t i = 1; i <= n; ++i, ++v) {
        elem_table(L, "entities", i);
        v->id     = (TesseraEntityId)ts_luax_fld_int(L, -1, "id", 0);
        v->def    = (TesseraDefId)ts_luax_fld_int(L, -1, "def", 0);
        v->coord  = read_coord(L, -1, "entities");
        v->facing = (uint16_t)ts_luax_fld_int(L, -1, "facing", 0);
        v->anim   = (uint32_t)ts_luax_fld_int(L, -1, "anim", 0);
        int t = lua_getfield(L, -1, "path");
        if (t != LUA_TNIL) {                     /* path = {{x,y}, ...} */
            if (t != LUA_TTABLE)
                luaL_error(L, "field 'path' must be an array of coords");
            size_t steps = lua_rawlen(L, -1);
            if (steps) {
                TesseraCoord* path = (TesseraCoord*)calloc(steps, sizeof *path);
                if (!path)
                    luaL_error(L, "'path': out of memory");
                v->path = path;                  /* owned; freed in pending_free */
                v->path_count = (uint32_t)steps;
                for (size_t s = 1; s <= steps; ++s) {
                    elem_table(L, "path", s);
                    path[s - 1] = read_coord(L, -1, "path");
                    lua_pop(L, 1);
                }
            }
        }
        lua_pop(L, 2);                           /* path + element */
    }
    lua_pop(L, 1);
}

static void read_effects(lua_State* L, TesseraState* st) {
    void* arr = NULL;
    size_t n = begin_array(L, "effects", sizeof(TesseraEffectPlacement), &arr);
    if (!n) return;
    TesseraEffectPlacement* v = (TesseraEffectPlacement*)arr;
    st->effects = v;
    st->effect_count = n;
    for (size_t i = 1; i <= n; ++i, ++v) {
        elem_table(L, "effects", i);
        v->id    = (TesseraEntityId)ts_luax_fld_int(L, -1, "id", 0);
        v->def   = (TesseraDefId)ts_luax_fld_int(L, -1, "def", 0);
        v->coord = read_coord(L, -1, "effects");
        v->attach_entity_id = (TesseraEntityId)ts_luax_fld_int(L, -1, "entity", 0);
        v->attach_card_id   = (TesseraCardId)ts_luax_fld_int(L, -1, "card", 0);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

static void read_cards(lua_State* L, TesseraState* st) {
    void* arr = NULL;
    size_t n = begin_array(L, "cards", sizeof(TesseraCardPlacement), &arr);
    if (!n) return;
    TesseraCardPlacement* v = (TesseraCardPlacement*)arr;
    st->cards = v;
    st->card_count = n;
    for (size_t i = 1; i <= n; ++i, ++v) {
        elem_table(L, "cards", i);
        v->id  = (TesseraCardId)ts_luax_fld_int(L, -1, "id", 0);
        v->def = (TesseraDefId)ts_luax_fld_int(L, -1, "def", 0);
        ts_luax_fld_vec(L, -1, "position", v->position, 3);
        ts_luax_fld_vec(L, -1, "orientation", v->orientation, 4);
        v->hidden      = ts_luax_fld_bool(L, -1, "hidden", false);
        v->hand        = (TesseraHandId)ts_luax_fld_int(L, -1, "hand", 0);
        v->hand_slot   = (uint32_t)ts_luax_fld_int(L, -1, "slot", 0);
        v->source_draw = (TesseraCardDrawId)ts_luax_fld_int(L, -1, "source_draw", 0);
        int t = lua_getfield(L, -1, "path");
        if (t != LUA_TNIL) {                     /* path = {{x,y,z}, ...} */
            if (t != LUA_TTABLE)
                luaL_error(L, "field 'path' must be an array of positions");
            size_t steps = lua_rawlen(L, -1);
            if (steps) {
                /* nmemb = steps so calloc's overflow-checked multiply guards
                 * a pathological rawlen (mirrors read_entities). */
                float* path = (float*)calloc(steps, 3 * sizeof *path);
                if (!path)
                    luaL_error(L, "'path': out of memory");
                v->path = path;                  /* owned; freed in pending_free */
                v->path_count = (uint32_t)steps;
                for (size_t s = 1; s <= steps; ++s) {
                    if (lua_rawgeti(L, -1, (lua_Integer)s) != LUA_TTABLE)
                        luaL_error(L, "path[%d] must be {x, y, z}", (int)s);
                    read_num_array(L, "path", path + (s - 1) * 3, 3);
                    lua_pop(L, 1);
                }
            }
        }
        lua_pop(L, 2);                           /* path + element */
    }
    lua_pop(L, 1);
}

static void read_draws(lua_State* L, TesseraState* st) {
    void* arr = NULL;
    size_t n = begin_array(L, "draws", sizeof(TesseraCardDrawPlacement), &arr);
    if (!n) return;
    TesseraCardDrawPlacement* v = (TesseraCardDrawPlacement*)arr;
    st->card_draws = v;
    st->card_draw_count = n;
    for (size_t i = 1; i <= n; ++i, ++v) {
        elem_table(L, "draws", i);
        v->id  = (TesseraCardDrawId)ts_luax_fld_int(L, -1, "id", 0);
        v->def = (TesseraDefId)ts_luax_fld_int(L, -1, "def", 0);
        ts_luax_fld_vec(L, -1, "position", v->position, 3);
        ts_luax_fld_vec(L, -1, "orientation", v->orientation, 4);
        v->count      = (uint32_t)ts_luax_fld_int(L, -1, "count", 0);
        v->top_hidden = ts_luax_fld_bool(L, -1, "top_hidden", false);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

static void read_hands(lua_State* L, TesseraState* st) {
    void* arr = NULL;
    size_t n = begin_array(L, "hands", sizeof(TesseraHandPlacement), &arr);
    if (!n) return;
    TesseraHandPlacement* v = (TesseraHandPlacement*)arr;
    st->hands = v;
    st->hand_count = n;
    for (size_t i = 1; i <= n; ++i, ++v) {
        elem_table(L, "hands", i);
        v->id = (TesseraHandId)ts_luax_fld_int(L, -1, "id", 0);
        ts_luax_fld_vec(L, -1, "position", v->position, 3);
        ts_luax_fld_vec(L, -1, "orientation", v->orientation, 4);
        v->spread_deg    = ts_luax_fld_num(L, -1, "spread_deg", 0);
        v->radius        = ts_luax_fld_num(L, -1, "radius", 0);
        v->card_spacing  = ts_luax_fld_num(L, -1, "card_spacing", 0);
        v->selected_card = (TesseraCardId)ts_luax_fld_int(L, -1, "selected_card", 0);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

static void read_dice(lua_State* L, TesseraState* st) {
    void* arr = NULL;
    size_t n = begin_array(L, "dice", sizeof(TesseraDicePlacement), &arr);
    if (!n) return;
    TesseraDicePlacement* v = (TesseraDicePlacement*)arr;
    st->dice = v;
    st->dice_count = n;
    for (size_t i = 1; i <= n; ++i, ++v) {
        elem_table(L, "dice", i);
        v->id   = (TesseraDiceId)ts_luax_fld_int(L, -1, "id", 0);
        v->def  = (TesseraDefId)ts_luax_fld_int(L, -1, "def", 0);
        v->face = (uint32_t)ts_luax_fld_int(L, -1, "face", 0);
        ts_luax_fld_vec(L, -1, "position", v->position, 3);
        v->seed    = (uint32_t)ts_luax_fld_int(L, -1, "seed", 0);
        v->throw_s = ts_luax_fld_num(L, -1, "throw_s", 0);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

static void read_overlays(lua_State* L, TesseraState* st) {
    void* arr = NULL;
    size_t n = begin_array(L, "overlays", sizeof(TesseraOverlayPlacement), &arr);
    if (!n) return;
    TesseraOverlayPlacement* v = (TesseraOverlayPlacement*)arr;
    st->overlays = v;
    st->overlay_count = n;
    for (size_t i = 1; i <= n; ++i, ++v) {
        elem_table(L, "overlays", i);
        v->coord = read_coord(L, -1, "overlays");
        v->shape = ts_luax_fld_enum(L, -1, "shape", OVERLAY_SHAPES, 0);
        v->atlas = (TesseraDefId)ts_luax_fld_int(L, -1, "atlas", 0);
        ts_luax_fld_rect(L, -1, "uv", &v->uv);
        ts_luax_fld_color(L, -1, "tint", v->tint);
        v->pulse_s         = ts_luax_fld_num(L, -1, "pulse_s", 0);
        v->pulse_alpha_min = ts_luax_fld_num(L, -1, "pulse_alpha_min", 0);
        v->pulse_alpha_max = ts_luax_fld_num(L, -1, "pulse_alpha_max", 0);
        v->pulse_scale_min = ts_luax_fld_num(L, -1, "pulse_scale_min", 0);
        v->pulse_scale_max = ts_luax_fld_num(L, -1, "pulse_scale_max", 0);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

static void read_labels(lua_State* L, TesseraState* st) {
    void* arr = NULL;
    size_t n = begin_array(L, "labels", sizeof(TesseraLabelPlacement), &arr);
    if (!n) return;
    TesseraLabelPlacement* v = (TesseraLabelPlacement*)arr;
    st->labels = v;
    st->label_count = n;
    for (size_t i = 1; i <= n; ++i, ++v) {
        elem_table(L, "labels", i);
        v->id   = (TesseraLabelId)ts_luax_fld_int(L, -1, "id", 0);
        v->font = (TesseraDefId)ts_luax_fld_int(L, -1, "font", 0);
        int t = lua_getfield(L, -1, "text");
        if (t != LUA_TNIL) {                     /* clamp to the cap, NUL-safe */
            if (t != LUA_TSTRING)
                luaL_error(L, "field 'text' must be a string");
            size_t len = 0;
            const char* s = lua_tolstring(L, -1, &len);
            if (len > TESSERA_LABEL_TEXT_CAP - 1) len = TESSERA_LABEL_TEXT_CAP - 1;
            memcpy(v->text, s, len);
            v->text[len] = 0;
        }
        lua_pop(L, 1);
        v->anchor    = ts_luax_fld_enum(L, -1, "anchor", LABEL_ANCHORS, 0);
        v->anchor_id = (uint64_t)ts_luax_fld_int(L, -1, "anchor_id", 0);
        ts_luax_fld_vec(L, -1, "position", v->position, 3);
        v->size = ts_luax_fld_num(L, -1, "size", 0);
        ts_luax_fld_color(L, -1, "color", v->color);
        v->billboard = ts_luax_fld_bool(L, -1, "billboard", false);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

static void read_highlights(lua_State* L, TesseraState* st) {
    void* arr = NULL;
    size_t n = begin_array(L, "highlights", sizeof(TesseraHighlightPlacement), &arr);
    if (!n) return;
    TesseraHighlightPlacement* v = (TesseraHighlightPlacement*)arr;
    st->highlights = v;
    st->highlight_count = n;
    for (size_t i = 1; i <= n; ++i, ++v) {
        elem_table(L, "highlights", i);
        v->target_id = (uint64_t)ts_luax_fld_int(L, -1, "target", 0);
        v->kind      = ts_luax_fld_enum(L, -1, "kind", HIGHLIGHT_KINDS, 0);
        v->style     = ts_luax_fld_enum(L, -1, "style", HIGHLIGHT_STYLES, 0);
        ts_luax_fld_color(L, -1, "color", v->color);
        v->thickness = ts_luax_fld_num(L, -1, "thickness", 0);
        v->pulse_s   = ts_luax_fld_num(L, -1, "pulse_s", 0);
        v->pulse_min = ts_luax_fld_num(L, -1, "pulse_min", 0);
        v->pulse_max = ts_luax_fld_num(L, -1, "pulse_max", 0);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

static void read_point_lights(lua_State* L, TesseraState* st) {
    void* arr = NULL;
    size_t n = begin_array(L, "point_lights", sizeof(TesseraPointLightPlacement), &arr);
    if (!n) return;
    TesseraPointLightPlacement* v = (TesseraPointLightPlacement*)arr;
    st->point_lights = v;
    st->point_light_count = n;
    for (size_t i = 1; i <= n; ++i, ++v) {
        elem_table(L, "point_lights", i);
        v->id = (TesseraPointLightId)ts_luax_fld_int(L, -1, "id", 0);
        ts_luax_fld_vec(L, -1, "position", v->position, 3);
        ts_luax_fld_vec(L, -1, "color", v->color, 3);
        v->intensity = ts_luax_fld_num(L, -1, "intensity", 0);
        v->radius    = ts_luax_fld_num(L, -1, "radius", 0);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

static void read_world_models(lua_State* L, TesseraState* st) {
    void* arr = NULL;
    size_t n = begin_array(L, "world_models", sizeof(TesseraWorldModelPlacement), &arr);
    if (!n) return;
    TesseraWorldModelPlacement* v = (TesseraWorldModelPlacement*)arr;
    st->world_models = v;
    st->world_model_count = n;
    for (size_t i = 1; i <= n; ++i, ++v) {
        elem_table(L, "world_models", i);
        v->id  = (TesseraWorldModelId)ts_luax_fld_int(L, -1, "id", 0);
        v->def = (TesseraDefId)ts_luax_fld_int(L, -1, "def", 0);
        ts_luax_fld_vec(L, -1, "position", v->position, 3);
        ts_luax_fld_vec(L, -1, "orientation", v->orientation, 4);
        v->scale = ts_luax_fld_num(L, -1, "scale", 0);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

/* ---- entry points ------------------------------------------------------ */

void ts_lua_pending_free(TsLuaPending* p) {
    if (!p) return;
    TesseraState* st = &p->st;
    for (size_t i = 0; i < st->entity_count; ++i)
        free((void*)st->entities[i].path);
    for (size_t i = 0; i < st->card_count; ++i)
        free((void*)st->cards[i].path);
    free((void*)st->tiles);
    free((void*)st->entities);
    free((void*)st->effects);
    free((void*)st->cards);
    free((void*)st->card_draws);
    free((void*)st->hands);
    free((void*)st->dice);
    free((void*)st->overlays);
    free((void*)st->labels);
    free((void*)st->highlights);
    free((void*)st->point_lights);
    free((void*)st->world_models);
    free(p);
}

/* Protected converter: arg 1 = state table, arg 2 = TsLuaPending** out. The
 * node is written to *out BEFORE any field is read, so the caller can free a
 * partially built state when a type error aborts the conversion. */
int ts_lua_read_state(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    TsLuaPending** out = (TsLuaPending**)lua_touserdata(L, 2);
    TsLuaPending* p = (TsLuaPending*)calloc(1, sizeof *p);
    if (!p)
        return luaL_error(L, "state: out of memory");
    *out = p;

    read_camera(L, &p->st.camera);
    p->st.epoch = (uint64_t)ts_luax_fld_int(L, 1, "epoch", 0);
    read_tiles(L, &p->st);
    read_entities(L, &p->st);
    read_effects(L, &p->st);
    read_cards(L, &p->st);
    read_draws(L, &p->st);
    read_hands(L, &p->st);
    read_dice(L, &p->st);
    read_overlays(L, &p->st);
    read_labels(L, &p->st);
    read_highlights(L, &p->st);
    read_point_lights(L, &p->st);
    read_world_models(L, &p->st);
    return 0;
}
