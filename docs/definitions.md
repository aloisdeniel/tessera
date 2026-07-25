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
TesseraDefId tessera_register_card_def  (TesseraEngine*, const TesseraCardDef*   def);
TesseraDefId tessera_register_font      (TesseraEngine*, const TesseraBytes* ttf, float pixel_height);
```

(Dice and cards are registered like other defs, then **placed through
`tessera_set_state`** — see [Dice](#dice--procedural-polyhedral-dice-with-per-face-sprites)
and [Cards](#cards--flat-textured-cards-piles--hands). Everything is
state-driven; there is no imperative placement API.)

`tessera_register_font` reads a TrueType/OpenType file (bytes or path, like an
atlas) and bakes ASCII + Latin-1 glyphs at `pixel_height` texels (`<= 0` ⇒ 48)
into one GPU atlas. The returned id is referenced by
`TesseraLabelPlacement.font` — see **Text labels** in `state-model.md` for
placing world-anchored 3D text through `TesseraState.labels`.

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
sprites, then place dice through `tessera_set_state` (a `TesseraDicePlacement`
array on the state). A die that appears is thrown; one that vanishes fades out.

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

### Placing dice

Dice are placed through `tessera_set_state`, via a `TesseraDicePlacement` array
on the state (alongside tiles/entities). A die that newly appears (by `id`) is
thrown; one that vanishes from the next state fades out; changing
`def`/`face`/`seed`/`position` re-throws it.

```c
typedef struct {
    TesseraDiceId id;          /* stable handle (diff key; changes re-throw)     */
    TesseraDefId  def;
    uint32_t      face;        /* face to land up (clamped to N)                 */
    float         position[3]; /* rest position of the die centre (world space)  */
    uint32_t      seed;        /* varies the tumble (picks a precomputed path)    */
    float         throw_s;     /* tumble duration (<=0 => default)               */
} TesseraDicePlacement;

/* read-only queries */
uint32_t tessera_dice_count  (TesseraEngine*);
bool     tessera_dice_face   (TesseraEngine*, TesseraDiceId, uint32_t* out_face);
bool     tessera_dice_all_idle(TesseraEngine*);
```

A placed die spawns airborne and tumbles along a precomputed trajectory whose
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

`tessera_is_idle` returns `false` while any die is still tumbling or fading. See
`examples/dice` for a full showcase.

## Cards — flat textured cards, piles & hands

A **card def** names three textures — the real front (`visible`), a concealing
front (`hidden`, shown so onlookers can't deduce a card even while it's in view),
and the `back` — each as a registered atlas id plus a sub-rect (so the shared
hidden/back cost nothing to reuse). The engine builds a thin rounded slab a
little smaller than 2×3 tiles.

```c
typedef struct {
    TesseraDefId visible_atlas; TesseraRect visible_uv;  /* the real front face   */
    TesseraDefId hidden_atlas;  TesseraRect hidden_uv;   /* concealing front face */
    TesseraDefId back_atlas;    TesseraRect back_uv;      /* the reverse face      */
    float width;         /* across the short edge (<=0 => ~1.84) */
    float height;        /* along the long edge   (<=0 => ~2.76) */
    float thickness;     /* single-card thickness (<=0 => 0.03)  */
    float corner_radius; /* rounded corners       (<=0 => 0.12)  */
    float tint[4];
} TesseraCardDef;
```

Cards, piles and hands are all placed through `tessera_set_state` and animate on
diff. All orientations are quaternions (`xyzw`; all-zero ⇒ identity).

```c
typedef struct {
    TesseraCardId id; TesseraDefId def;
    float position[3]; float orientation[4]; /* identity => flat, front up */
    bool  hidden;                            /* crossfades when toggled     */
    TesseraHandId hand;                      /* 0 => free; else fanned      */
    uint32_t hand_slot;                      /* order within the hand fan   */
    TesseraCardDrawId source_draw;           /* 0 => none; deal-from-pile   */
    const float* path; uint32_t path_count;  /* multi-step move (free card) */
} TesseraCardPlacement;

typedef struct {
    TesseraCardDrawId id; TesseraDefId def;
    float position[3]; float orientation[4];
    uint32_t count;      /* pile thickness (tweens on change)            */
    bool top_hidden;     /* top face shows the hidden (vs visible) front */
} TesseraCardDrawPlacement;

typedef struct {
    TesseraHandId id;
    float position[3]; float orientation[4]; /* identity => fronts face +Z */
    float spread_deg;   /* total fan angle   (<=0 => default) */
    float radius;       /* fan arc radius    (<=0 => default) */
    float card_spacing; /* lateral spacing   (<=0 => default) */
} TesseraHandPlacement;
```

Behaviour, all driven by the state diff:

- **Flip** — toggling `hidden` crossfades the front between the visible and
  hidden textures. A card's `position`/`orientation` change **tweens** like an
  entity.
- **Pile (`TesseraCardDrawPlacement`)** — one slab whose **thickness tracks
  `count`** (tweens when it changes), resting on the ground at `position`. The
  top face shows the top card (visible, or the hidden front when `top_hidden`);
  the bottom face always shows the def's **hidden** texture.
- **Hand (`TesseraHandPlacement`)** — a world-space anchor that **overrides the
  positions** of the cards whose `hand` equals its id, fanning them in an arc
  that follows the hand's transform (cards tween into their fan slots; `hand_slot`
  orders them). A card with `hand == 0` keeps its own placement.
- **Deal from a pile (`source_draw`)** — when a card **first appears** and its
  `source_draw` names a pile present in the same state, it spawns resting on top
  of that pile and slides (and flips, if the pile top and the card differ) to its
  target instead of fading in from nowhere. Ignored on later frames and when the
  named pile is absent.
- **Multi-step move (`path` / `path_count`)** — for a **free** card (ignored
  while `hand != 0`), a `path` of more than one position (each 3 floats) makes
  the card tween *through* those points in order instead of straight to
  `position`; the last entry must equal `position`. The whole move takes
  `2 × move_s` (twice a single move), split evenly across the steps. `NULL`/count
  `0`/`1` = a plain single move at the normal `move_s`. The array is copied by
  `tessera_set_state`.

`tessera_is_idle` returns `false` while any card is moving, flipping or a pile is
resizing. See `examples/cards` for a full showcase (flat cards, a flip, a moving
card, a fanned hand and a growing pile).
