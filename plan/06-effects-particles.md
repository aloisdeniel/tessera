# Milestone 6 — Effects & particle system

**Goal:** Implement effects — visual-only elements with no physical persistence.
Adding an effect to a tile plays its `on_add` particle burst; removing it plays
`on_remove`. Build a lightweight particle system driven by the `EffectDef` specs
registered in M2, integrated with the M4 diff (EFFECT_ADD / EFFECT_REMOVE).

## Deliverables
- A data-driven particle spec (`TesseraParticleSpec`) — enough for common
  table-top VFX (bursts, sparkles, smoke, rising motes).
- A CPU-simulated, GPU-rendered particle system (billboarded quads, additive/alpha
  blend, soft depth).
- Effect lifecycle wired to state diff: spawn on add, spawn on remove, auto-expire.
- Optional attach point so effects can ride an entity (e.g. buff aura) vs a tile.

## Particle spec (registered as part of EffectDef)
```c
typedef enum { TESSERA_EMIT_BURST, TESSERA_EMIT_CONTINUOUS } TesseraEmitMode;

typedef struct {
    TesseraDefId    atlas;        // sprite sheet for particles (0 = white quad)
    TesseraRect     sprite;       // region (or animated over life)
    TesseraEmitMode mode;
    uint32_t        count;        // burst count / rate per sec
    float           lifetime_s, lifetime_var;
    float           speed, speed_var;
    float           spread_deg;   // cone half-angle
    float           gravity;      // world-space accel (y)
    float           size_start, size_end;
    float           color_start[4], color_end[4];  // RGBA lerp over life
    int             blend;        // additive vs alpha
    float           duration_s;   // 0 => one-shot burst; >0 continuous window
} TesseraParticleSpec;
```

## Tasks
1. **Emitter runtime** (`fx/emitter.c`): instantiate a live emitter from a spec at
   a world position (tile center or entity anchor). Track age; stop after
   `duration`; retire when all particles dead.
2. **Simulation** (`fx/particles.c`): per-particle `{ pos, vel, age, life, seed }`;
   integrate (gravity, drag), fade size/color over normalized age. Pool-allocated,
   fixed max per system; recycle. Deterministic-ish RNG seeded per particle.
3. **Rendering** (`fx/particle_render.c`): billboard quads facing the camera,
   built into a dynamic vertex buffer each frame; one pipeline with additive and
   one with alpha blend, depth-test on / depth-write off. Sort translucent if
   needed (or rely on additive to be order-independent).
4. **Diff integration**: `EFFECT_ADD` → spawn `on_add` emitter at the effect's
   coord; `EFFECT_REMOVE` → spawn `on_remove` emitter, then drop the effect
   instance. Effects never persist as geometry — only their emitters live, and
   only until particles expire.
5. **Attachment**: effect coord resolves to a tile center by default; allow an
   optional `attach_entity_id` in `TesseraEffectPlacement` so an effect follows a
   moving entity.
6. **Effect during transitions**: allow entity add/remove to optionally trigger an
   associated effect (e.g. poof on despawn) via a linked EffectDef on the
   EntityDef.
7. **examples/fx**: add/remove effects on tiles and confirm bursts; a continuous
   aura on a moving entity.

## Public API added
```c
// TesseraEffectPlacement (from M3) gains:
//   TesseraEntityId attach_entity_id;  // 0 = anchored to tile coord
// TesseraEntityDef may reference on_spawn/on_despawn effect defs.
```
No new top-level functions required — effects flow through `set_state`.

## Acceptance criteria
- Adding an effect to a tile plays its burst; removing plays the removal burst;
  both auto-clean when particles die.
- An effect attached to a moving entity follows it.
- Hundreds of particles across several emitters stay within frame budget.
- No leaks when effects are added/removed rapidly.

## Risks / notes
- Translucency ordering can look wrong with mixed emitters; prefer additive for
  bright VFX and keep alpha effects simple, or add a coarse back-to-front sort.
- Cap total live particles globally to bound memory and fill-rate on mobile.
