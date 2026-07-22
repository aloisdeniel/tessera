# Definitions: tiles, entities, effects & atlases

Before pushing any state, the host registers immutable **definitions**. Each
registration returns a `TesseraDefId` (a `uint32`; `0` means invalid/failure —
check `tessera_last_error` on `0`). Definitions are referenced by id from the
placements inside a `TesseraState`. Registration uploads GPU resources, so it
must run on the render thread.

```c
TesseraDefId tessera_register_atlas     (TesseraEngine*, const TesseraBytes*  image);
TesseraDefId tessera_register_tile_def  (TesseraEngine*, const TesseraTileDef* def);
TesseraDefId tessera_register_entity_def(TesseraEngine*, const TesseraEntityDef* def);
TesseraDefId tessera_register_effect_def(TesseraEngine*, const TesseraEffectDef* def);
TesseraDefId tessera_register_dice_def  (TesseraEngine*, const TesseraDiceDef*   def);
```

(Dice are registered like other defs but thrown imperatively rather than placed
in a `TesseraState` — see [Dice](#dice--procedural-polyhedral-dice-with-per-face-sprites).)

## `TesseraBytes` — a borrowed byte span

Every asset input (atlas image, glTF model) is a `TesseraBytes`:

| Field | Meaning |
|---|---|
| `data` | pointer to an in-memory buffer, or `NULL` to load from `path` |
| `size` | byte length of `data` |
| `path` | optional filesystem path, used when `data == NULL` |
| `debug_name` | optional label for diagnostics |

The span is **not retained** past the registration call — the engine copies or
decodes what it needs and returns, so the caller may free the bytes immediately.

## Atlases

`tessera_register_atlas` decodes an image (via `stb_image`) and uploads it as a
texture. Tile and particle definitions reference an atlas id and sample sub-
regions of it through normalized UV rectangles (`TesseraRect { u0,v0,u1,v1 }` in
`[0,1]` space). An atlas id of `0` in a def means "untextured / white".

## `TesseraTileDef`

A tile is an extruded quad with independent top / side / bottom UVs.

| Field | Meaning |
|---|---|
| `atlas` | atlas id to sample (`0` = untextured white) |
| `top`, `side`, `bottom` | `TesseraRect` UV regions for each face |
| `tint[4]` | RGBA multiply color |
| `thickness` | relative tile height (default `0.25`) |

Tiles are keyed by grid `coord` when diffing states (see `docs/state-model.md`).

## `TesseraEntityDef`

An entity is a 3D model placed on the board. If no glTF bytes/path are supplied
the engine substitutes a shared **unit-cube fallback mesh** (no error), so an
entity works out of the box for prototyping.

| Field | Meaning |
|---|---|
| `gltf` | `TesseraBytes` of the model (glTF/GLB); empty = cube fallback |
| `atlas` | optional texture override (`0` = use the material from the glTF) |
| `scale` | uniform scale (default `1.0`) |
| `pivot[3]` | feet-anchor offset in model space |
| `default_anim` / `move_anim` / `spawn_anim` / `despawn_anim` | clip roles; `-1` = unset |
| `on_spawn_effect` / `on_despawn_effect` | linked effect def ids played on add/remove; `0` = none |

### glTF entity path & animation clip roles

When `gltf` carries bytes or a path, the model is imported (geometry, and — for
skinned models — a skeleton and its animation clips). The clips discovered in
the file can be inspected after registration:

```c
uint32_t    tessera_entity_def_anim_count(TesseraEngine*, TesseraDefId def);
const char* tessera_entity_def_anim_name (TesseraEngine*, TesseraDefId def, uint32_t index);
```

The four **clip-role** fields (`default_anim`, `move_anim`, `spawn_anim`,
`despawn_anim`) are indices into that clip list (or `-1` for unset). The engine
uses them to auto-select which clip plays during a transition: the idle/default
pose, a movement hop, a spawn-in, and a despawn-out. A per-placement
`TesseraEntityPlacement.anim` index can override the active clip for a specific
entity in a state.

See `docs/assets.md` for the supported glTF subset and the 64-joint cap.

## `TesseraEffectDef` & `TesseraParticleSpec`

An effect def bundles two particle specs — one played when the effect is added
to a state, one when it is removed:

```c
typedef struct { TesseraParticleSpec on_add; TesseraParticleSpec on_remove; } TesseraEffectDef;
```

`TesseraParticleSpec` fields:

| Field | Meaning |
|---|---|
| `atlas` + `sprite` | particle sprite sheet id and UV region (`0` = white/dot) |
| `mode` | `TESSERA_EMIT_BURST` or `TESSERA_EMIT_CONTINUOUS` |
| `count` | burst count, or particles/second if continuous |
| `lifetime_s`, `lifetime_var` | particle lifetime and its random variance |
| `speed`, `speed_var` | initial speed and variance |
| `spread_deg` | emission-cone half-angle |
| `gravity` | world-space +Y acceleration |
| `size_start`, `size_end` | sprite size over life |
| `color_start[4]`, `color_end[4]` | RGBA over life |
| `blend` | `TESSERA_BLEND_ALPHA` or `TESSERA_BLEND_ADD` |
| `duration_s` | `0` = one-shot burst; `>0` = continuous emission window |

Effects appear in a state through `TesseraEffectPlacement`, anchored to a tile
`coord` or attached to an entity via `attach_entity_id` (`0` = tile-anchored).
An entity def's `on_spawn_effect` / `on_despawn_effect` fire the linked effect
automatically when that entity is added or removed.

## Dice — procedural polyhedral dice with per-face sprites

Dice are a self-contained subsystem: register a **dice def** from a set of face
sprites, then throw dice into the scene imperatively (they live *outside* the
`TesseraState` snapshot).

```c
typedef struct { TesseraBytes sprite; } TesseraDiceFace;
typedef struct {
    const TesseraDiceFace* faces;      /* face_count sprites (index 0..N-1) */
    size_t                 face_count; /* >= 2                              */
    float                  size;       /* model diameter, world units (<=0 => 1) */
    float                  tint[4];    /* sprite multiply (all-zero => white)    */
} TesseraDiceDef;

TesseraDefId tessera_register_dice_def   (TesseraEngine*, const TesseraDiceDef*);
uint32_t     tessera_dice_def_face_count (TesseraEngine*, TesseraDefId);
```

From the face count the engine generates a matching convex model and packs the
sprites into one atlas, UV-mapped so each sprite is **centred on and fills its
face**:

| Faces | Model |
|---|---|
| `2` | a two-sided token / coin (the two discs are the faces) |
| `4` | a **tetrahedron** (d4) |
| `6` | a **cube** (d6) |
| `8` | an **octahedron** (d8) |
| `12` | a **dodecahedron** (d12) — pentagonal faces |
| `20` | an **icosahedron** (d20) |
| `N` (any other) | an N-gonal **barrel** — N rectangular side faces, plain end caps |

The face counts `4/6/8/12/20` produce true **Platonic solids** (each generated
from its exact vertex/face table); the cube fills each square face edge-to-edge,
the other regular solids centre the sprite on the face's circumscribed circle.
The solids are **chamfered** — each textured face is inset slightly and the gaps
along the edges and corners are filled with small body-coloured bevel facets
(the body colour is sampled from the sprites' borders), so the dice have
softened edges instead of razor-sharp ones. `size` is the bounding-sphere
diameter for the regular solids (so a cube's `size` is its long diagonal, not
its face-to-face width).
Every face's sprite tangent frame is chosen orientation-preserving (`u × v =
-n`), so numerals read upright and un-mirrored on all faces. Each face also gets
a *rest orientation* (the rotation that turns that face to point `+Y`), so any
face can be made to land face-up. Sprites are encoded images (PNG/JPG/TGA/… —
anything `stb_image` decodes), passed as `TesseraBytes`; a face whose sprite
fails to decode renders as a blank plate.

### Throwing dice

```c
typedef struct {
    TesseraDiceId id;          /* host-chosen stable handle (re-throws reuse it) */
    TesseraDefId  def;
    uint32_t      face;        /* face to land up (clamped to N)                 */
    float         position[3]; /* rest position of the die centre (world space)  */
    uint32_t      seed;        /* varies the tumble (picks a precomputed path)    */
    float         throw_s;     /* tumble duration (<=0 => default)               */
} TesseraDiceThrow;

void     tessera_add_dice    (TesseraEngine*, const TesseraDiceThrow*);
void     tessera_remove_dice (TesseraEngine*, TesseraDiceId);  /* fade + shrink out */
void     tessera_clear_dice  (TesseraEngine*);
uint32_t tessera_dice_count  (TesseraEngine*);
bool     tessera_dice_face   (TesseraEngine*, TesseraDiceId, uint32_t* out_face);
bool     tessera_dice_all_idle(TesseraEngine*);
```

A thrown die spawns airborne and tumbles along a precomputed trajectory whose
spin winds down to *exactly* the target face's rest orientation, so it always
settles on the requested face — at the given floating `position` (nothing
constrains it to a tile). `position` is a true world point, so dice can hover
above the board. The vertical axis is integrated as real physics — gravity pulls
the die down and its velocity reverses (losing energy to restitution) each time
it hits the rest height, so it bounces lower and quicker until it settles — while
the horizontal slide and the tumble carry their launch momentum *through* the
bounces and bleed off under friction, the spin easing to a stop (exactly on the
target face) only at the very end. The **d4** is oriented the way a real
tetrahedral die is read — it rests on a face with the chosen face's value shown
upright at the top apex — rather than lying flat-face-up. Removing a die fades
and shrinks it out, then culls it.

Because dice are imperative (not part of `tessera_set_state`), drive them from
the render / tick thread — the same thread as `tessera_tick` / `set_timing` —
not concurrently with the tick. `tessera_is_idle` also returns `false` while any
die is still tumbling or fading. See `examples/dice` for a full showcase.
