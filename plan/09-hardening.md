# Milestone 9 — Hardening, performance, docs & release

**Goal:** Turn the working renderer into something dependable: no leaks, bounded
memory, predictable performance, a clean error model, and documentation + a
polished sample so a new host can bind it in an afternoon.

## Deliverables
- Memory & resource audit (ASan/valgrind clean; GPU-object accounting).
- Robust error handling everywhere (no crashes on bad input from FFI).
- Performance budget met on a mid-range mobile target with a realistic board.
- Full API documentation + a getting-started guide + the reference sample game.
- Test suite (unit + a few integration/golden-image tests) in CI.

## Tasks
1. **Memory audit**:
   - Run all examples under ASan/UBSan (desktop) and valgrind; fix leaks.
   - Track GPU resource counts; assert zero live objects after `destroy`.
   - Arena/pool high-water marks logged; bound per-snapshot allocation.
2. **Fuzz the FFI boundary**: feed malformed/oversized states and defs (bad ids,
   huge counts, NaNs, null arrays) — the engine must reject gracefully via
   `last_error`, never crash. Add these as tests.
3. **Error model finalization**: consistent return conventions (0/invalid id +
   `last_error`), documented per function; optional severity in the log callback.
4. **Performance**:
   - Batch/instance tiles and entities; minimize pipeline/state changes.
   - Frustum cull; skip work when `tessera_is_idle` and camera is static (only
     redraw on change / animation, or render at reduced rate).
   - Profile on device; set and verify a budget (e.g. board of N tiles + M
     entities at 60fps on a mid Android/iPhone). Document limits.
5. **Robustness of transitions**: stress rapid `set_state` (retarget storms),
   id churn, empty states, single-tile boards, huge stacks on one tile.
6. **Docs**:
   - `tessera.h` fully commented (Doxygen-style).
   - `docs/getting-started.md`, `docs/state-model.md`, `docs/definitions.md`,
     `docs/ffi.md` (Dart + Lua), `docs/assets.md` (glTF/atlas authoring rules).
   - Threading contract and platform notes.
7. **Reference sample**: a small but complete turn-based demo (grid, several unit
   types, moves/attacks with effects, camera focus changes) usable as a template.
8. **Golden-image tests**: render fixed scenes headless and compare to reference
   images (tolerance) to catch visual regressions in CI (desktop).
9. **Versioning & release**: semver, `TESSERA_VERSION` macro + `tessera_version()`,
   changelog, tagged build artifacts per platform.

## Public API added
```c
TESSERA_API uint32_t    tessera_version(void);       // packed major.minor.patch
TESSERA_API const char* tessera_version_string(void);
```

## Acceptance criteria
- ASan/valgrind clean across all examples; zero GPU objects after destroy.
- Malformed FFI input never crashes; always reported via `last_error`.
- Performance budget met and documented on the chosen mobile targets.
- A newcomer can build, bind (Dart or Lua), and run the sample from the docs
  alone.
- Golden-image and unit tests pass in CI on macOS + Linux.

## Risks / notes
- Golden-image tests are backend/driver-sensitive — use tolerant comparison and
  run them only on a controlled CI runner/backend.
- Keep the "redraw only when needed" optimization behind correctness: animations,
  particles, and camera motion must still force redraws.
```
