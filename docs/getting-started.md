# Getting started with Tessera

Tessera is a small, embeddable 3D renderer in C + SDL3 for turn-based,
grid-based games. You register immutable **definitions** (tile / entity / effect
types), then push immutable **state** snapshots; the engine deep-copies each
snapshot, diffs it against the previous one, and animates the transitions.

## Build (macOS / Linux)

```sh
brew install sdl3                 # macOS; on Linux install libsdl3-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
./build/example_hello             # opens a window with the demo board
./build/example_hello --shot out.png   # headless render to a PNG
```

The library is `libtessera` (shared + static). The one public header is
`include/tessera.h`.

## Minimal usage

```c
#include "tessera.h"

TesseraConfig cfg = { .width = 1280, .height = 720, .pixel_density = 1.0f };
TesseraEngine* e = tessera_create(&cfg);

// 1. register definitions (once)
TesseraTileDef grass = { .thickness = 0.25f, .tint = {0.5f,0.8f,0.4f,1.0f} };
TesseraDefId grass_id = tessera_register_tile_def(e, &grass);

TesseraEntityDef unit = { .scale = 1.0f };          // cube fallback if no gltf
TesseraDefId unit_id = tessera_register_entity_def(e, &unit);

// 2. push state (repeatedly, each turn)
TesseraTilePlacement tiles[] = {
    { .coord = {0,0}, .tile_def = grass_id },
    { .coord = {1,0}, .tile_def = grass_id },
};
TesseraEntityPlacement ents[] = {
    { .id = 1, .def = unit_id, .coord = {0,0} },
};
TesseraState st = {
    .tiles = tiles, .tile_count = 2,
    .entities = ents, .entity_count = 1,
    .camera = { .focus = {0,0}, .distance = 8, .yaw = 0.6f, .pitch = 0.7f, .fov = 0.9f },
};
tessera_set_state(e, &st);        // caller may free tiles/ents immediately after

// 3. drive the frame loop
while (running) {
    tessera_tick(e, dt_seconds);  // promotes state, advances tweens, renders
}

tessera_destroy(e);
```

Move the unit next turn by pushing a new state with the same entity `id` at a
new `coord` — the engine animates the hop and reflows any tile that changed
occupancy. See `docs/state-model.md` for the diff rules.

## Threading

`tessera_set_state` is thread-safe (it deep-copies off-lock and publishes under
a mutex). GPU calls (`tessera_tick`, `tessera_resize`, `tessera_register_*`)
must run on the render thread. `tessera_last_error` is any-thread.
