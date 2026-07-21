# Asset authoring rules

Tessera loads assets from **bytes or a filesystem path** (`TesseraBytes`); there
are no hidden filesystem assumptions, which is what lets the same loaders run
from an Android `AAssetManager` or an iOS bundle (see `docs/platforms.md`). Two
asset kinds exist: **atlas images** and **glTF/GLB models**.

## Atlas images

`tessera_register_atlas` decodes image bytes with `stb_image` and uploads an
RGBA8 texture (forced to 4 components). Any format `stb_image` supports works —
in practice PNG and JPEG are the reliable choices; PNG is recommended for
crisp sprite atlases with alpha. Tiles and particles reference the atlas id and
sample sub-regions via normalized `TesseraRect` UVs.

## glTF / GLB models (entities)

Entity models are imported with **cgltf**. The importer flattens the **first
mesh's triangle primitives** into interleaved vertices, uploads a GPU mesh, and
— when a skin is present — extracts a runtime skeleton plus the animation clips.

### Supported subset

- **Triangles only.** Non-triangle primitives are skipped.
- Vertex attributes read: **`POSITION`**, **`NORMAL`**, **`TEXCOORD_0`**, and —
  for skinning — **`JOINTS_0`** and **`WEIGHTS_0`** (index 0 only; higher sets
  are ignored).
- **One base color.** The material's `pbrMetallicRoughness.baseColorFactor` of
  the first material is used as a flat base color; there is no full PBR texture
  pipeline. Use the entity def's `atlas` override or the base color for tinting.
- **Skeletal animation.** Skinned meshes yield a skeleton (joint hierarchy,
  inverse-bind matrices, bind-pose locals) and TRS animation clips. Clip names
  are exposed via `tessera_entity_def_anim_name`.

### Hard limits

- **64 joints max** (`TS_MAX_JOINTS`, in `src/anim/skeleton.h`). This matches
  the shader's joint-palette size. A skin with more than 64 joints is rejected
  and the import fails — split or retarget the rig to stay within the cap.

### Buffer packaging

- **External `.bin` buffers are unsupported.** Buffer data must be embedded:
  use **GLB** (recommended) or a glTF with base64 **data-URI** buffers. A model
  that references a separate `.bin` file fails to load ("external buffers
  unsupported").

### Failure behavior

If model bytes cannot be read or the glTF import fails, the entity def falls
back to the shared **unit-cube mesh** and logs a warning rather than aborting —
so a bad or missing model degrades gracefully instead of crashing.

## Quick checklist for a skinned character

1. Export as **GLB** (embeds geometry + buffers in one file).
2. Triangulate the mesh; include `POSITION`, `NORMAL`, `TEXCOORD_0`.
3. Keep the skeleton at **≤ 64 joints**.
4. Include `JOINTS_0` / `WEIGHTS_0`; weights are renormalized on import.
5. Name your animation clips (idle / move / spawn / despawn) and wire their
   indices into the entity def's clip-role fields.
