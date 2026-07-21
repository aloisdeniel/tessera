# Milestone 2 — Asset loading & the definition registry

**Goal:** Load real 3D models (glTF) and textures, upload them to the GPU, and
expose the **definition registration API** so hosts can declare tile, entity, and
effect types. After this, the engine can draw registered models by def id (still
static placement, wired fully in M3).

## Deliverables
- glTF loader (cgltf) producing engine mesh/material data (static geometry now;
  skinning data parsed but animated in M5).
- Texture loading (stb_image) + a **sprite-sheet / atlas** abstraction for tile
  top/side/bottom faces and entity textures.
- GPU resource manager: dedupe + lifetime of meshes, textures, samplers.
- Definition registry: `register_tile_def`, `register_entity_def`,
  `register_effect_def` returning ids; validation + error reporting.

## Model of a definition
- **TileDef**: shares a base mesh (the thin box). Distinguished by sprite regions
  into an atlas for `top`, `side`, `bottom`, plus optional tint. Many tile types
  reuse one mesh + one atlas — cheap to register.
- **EntityDef**: a glTF model path/bytes, default material, a table of named
  animations (parsed here, played in M5), default scale, and an anchor/pivot
  (feet at `y=0`).
- **EffectDef**: two particle-effect descriptors — `on_add` and `on_remove`
  (data-only spec now; simulated/rendered in M6).

## Tasks
1. **Asset input**: accept either a filesystem path or an in-memory buffer
   (`{const void* data; size_t size;}`) so mobile can feed bundled assets. All
   loaders take bytes; a small VFS resolves paths → bytes.
2. **glTF import** (`assets/gltf.c`): parse with cgltf, flatten primitives into
   `TesseraVertex` arrays + index arrays, read base-color texture + factors,
   collect skin/joint + animation channels into engine structs (stored, not yet
   played). Triangulate/validate; report unsupported features gracefully.
3. **Texture upload** (`assets/texture.c`): decode via stb_image, create GPU
   texture + mipmaps, upload through a transfer buffer + copy pass. Cache by
   content hash to dedupe.
4. **Atlas** (`assets/atlas.c`): a sprite sheet = one texture + named UV rects.
   TileDef references rects by name/index for each face; the mesh UVs are
   remapped per face at draw time via per-object uniforms (face→rect offsets).
5. **GPU resource manager** (`gpu/resources.c`): ref-counted meshes/textures/
   samplers keyed by id; freed on `tessera_destroy`. Slot-map handles.
6. **Registry** (`registry.c`): store defs in generation-tagged slot maps;
   validate inputs (nonzero mesh, valid atlas rects); return `TesseraDefId`
   (0 = failure, sets last-error). Registration is allowed before or after first
   state (thread-safe; guarded).
7. **Draw-by-def path**: extend the M1 renderer to draw an instance given
   `(def_id, model_matrix)`, pulling mesh/material/atlas from the registry.
8. **examples/defs**: register 2 tile types + 1 entity model + place a few by
   hand; confirm they render.

## Public API added
```c
typedef struct { const void* data; size_t size; const char* debug_name; } TesseraBytes;

typedef struct {                    // sprite region into an atlas
    float u0, v0, u1, v1;
} TesseraRect;

typedef struct {
    TesseraDefId atlas;             // registered atlas/texture id
    TesseraRect  top, side, bottom;
    float        tint[4];
    float        thickness;         // relative tile height
} TesseraTileDef;

typedef struct {
    TesseraBytes gltf;              // model bytes
    float        scale;
    float        pivot[3];          // feet anchor
    // named animations discovered inside gltf; queried later
} TesseraEntityDef;

typedef struct {
    // particle spec parsed here, simulated in M6
    TesseraParticleSpec on_add;
    TesseraParticleSpec on_remove;
} TesseraEffectDef;

TESSERA_API TesseraDefId tessera_register_atlas(TesseraEngine*, const TesseraBytes* image);
TESSERA_API TesseraDefId tessera_register_tile_def(TesseraEngine*,   const TesseraTileDef*);
TESSERA_API TesseraDefId tessera_register_entity_def(TesseraEngine*, const TesseraEntityDef*);
TESSERA_API TesseraDefId tessera_register_effect_def(TesseraEngine*, const TesseraEffectDef*);
```

## Acceptance criteria
- A glTF entity model and 2 tile types render from registered defs.
- Textures/atlas sample correctly per face; duplicate assets are deduped.
- Registering an invalid def returns 0 and sets a useful `tessera_last_error`.
- No GPU leaks across register → destroy.

## Risks / notes
- glTF is large; support a **subset** explicitly (triangles, single base-color
  texture, one UV set, skins ≤ N joints) and reject the rest with clear errors.
- Decide up front: entity animations must be authored into the glTF; document the
  expected rig/naming convention for the sample assets.
