/* test_ffi_layout.c — canonical FFI layout reference for tessera.h.
 *
 * This test locks the size of every public struct and the offset of every
 * field via _Static_assert (compile-time), and ALSO prints them at runtime so
 * binding authors (Dart dart:ffi, LuaJIT FFI, ...) can cross-check their own
 * struct definitions against the exact C ABI.
 *
 * The numbers below describe the layout produced by the target ABI (LP64:
 * pointer/size_t = 8 bytes, enum/int = 4 bytes, bool = 1, float = 4,
 * uint16 = 2, uint32 = 4, uint64 = 8). If tessera.h ever changes a struct,
 * this test fails to compile — that is the point. Keep bindings/dart and
 * bindings/lua in sync with whatever this file asserts.
 */
#include "tessera.h"
#include <stddef.h>
#include <stdio.h>

/* ---- helpers ---------------------------------------------------------- */
#define SA(cond, msg) _Static_assert(cond, msg)
#define SIZE_IS(T, n)   SA(sizeof(T) == (n), #T " size must be " #n)
#define OFF_IS(T, f, n) SA(offsetof(T, f) == (n), #T "." #f " offset must be " #n)

/* ======================================================================= *
 *  Lifecycle / config
 * ======================================================================= */
SIZE_IS(TesseraConfig, 40);
OFF_IS(TesseraConfig, native_window, 0);
OFF_IS(TesseraConfig, width, 8);
OFF_IS(TesseraConfig, height, 12);
OFF_IS(TesseraConfig, pixel_density, 16);
OFF_IS(TesseraConfig, engine_driven_loop, 20);
OFF_IS(TesseraConfig, debug, 21);
OFF_IS(TesseraConfig, log, 24);
OFF_IS(TesseraConfig, log_userdata, 32);

/* ======================================================================= *
 *  Assets & definitions
 * ======================================================================= */
SIZE_IS(TesseraBytes, 32);
OFF_IS(TesseraBytes, data, 0);
OFF_IS(TesseraBytes, size, 8);
OFF_IS(TesseraBytes, path, 16);
OFF_IS(TesseraBytes, debug_name, 24);

SIZE_IS(TesseraRect, 16);
OFF_IS(TesseraRect, u0, 0);
OFF_IS(TesseraRect, v0, 4);
OFF_IS(TesseraRect, u1, 8);
OFF_IS(TesseraRect, v1, 12);

SIZE_IS(TesseraTileDef, 72);
OFF_IS(TesseraTileDef, atlas, 0);
OFF_IS(TesseraTileDef, top, 4);
OFF_IS(TesseraTileDef, side, 20);
OFF_IS(TesseraTileDef, bottom, 36);
OFF_IS(TesseraTileDef, tint, 52);
OFF_IS(TesseraTileDef, thickness, 68);

SIZE_IS(TesseraEntityDef, 80);
OFF_IS(TesseraEntityDef, gltf, 0);
OFF_IS(TesseraEntityDef, atlas, 32);
OFF_IS(TesseraEntityDef, scale, 36);
OFF_IS(TesseraEntityDef, pivot, 40);
OFF_IS(TesseraEntityDef, default_anim, 52);
OFF_IS(TesseraEntityDef, move_anim, 56);
OFF_IS(TesseraEntityDef, spawn_anim, 60);
OFF_IS(TesseraEntityDef, despawn_anim, 64);
OFF_IS(TesseraEntityDef, on_spawn_effect, 68);
OFF_IS(TesseraEntityDef, on_despawn_effect, 72);

SIZE_IS(TesseraParticleSpec, 100);
OFF_IS(TesseraParticleSpec, atlas, 0);
OFF_IS(TesseraParticleSpec, sprite, 4);
OFF_IS(TesseraParticleSpec, mode, 20);
OFF_IS(TesseraParticleSpec, count, 24);
OFF_IS(TesseraParticleSpec, lifetime_s, 28);
OFF_IS(TesseraParticleSpec, lifetime_var, 32);
OFF_IS(TesseraParticleSpec, speed, 36);
OFF_IS(TesseraParticleSpec, speed_var, 40);
OFF_IS(TesseraParticleSpec, spread_deg, 44);
OFF_IS(TesseraParticleSpec, gravity, 48);
OFF_IS(TesseraParticleSpec, size_start, 52);
OFF_IS(TesseraParticleSpec, size_end, 56);
OFF_IS(TesseraParticleSpec, color_start, 60);
OFF_IS(TesseraParticleSpec, color_end, 76);
OFF_IS(TesseraParticleSpec, blend, 92);
OFF_IS(TesseraParticleSpec, duration_s, 96);

SIZE_IS(TesseraEffectDef, 200);
OFF_IS(TesseraEffectDef, on_add, 0);
OFF_IS(TesseraEffectDef, on_remove, 100);

/* ---- dice ---- */
SIZE_IS(TesseraDiceFace, 32);
OFF_IS(TesseraDiceFace, sprite, 0);

SIZE_IS(TesseraDiceDef, 40);
OFF_IS(TesseraDiceDef, faces, 0);
OFF_IS(TesseraDiceDef, face_count, 8);
OFF_IS(TesseraDiceDef, size, 16);
OFF_IS(TesseraDiceDef, tint, 20);

SIZE_IS(TesseraDiceThrow, 40);
OFF_IS(TesseraDiceThrow, id, 0);
OFF_IS(TesseraDiceThrow, def, 8);
OFF_IS(TesseraDiceThrow, face, 12);
OFF_IS(TesseraDiceThrow, position, 16);
OFF_IS(TesseraDiceThrow, seed, 28);
OFF_IS(TesseraDiceThrow, throw_s, 32);

/* ======================================================================= *
 *  Immutable state
 * ======================================================================= */
SIZE_IS(TesseraCoord, 8);
OFF_IS(TesseraCoord, x, 0);
OFF_IS(TesseraCoord, y, 4);

SIZE_IS(TesseraTilePlacement, 24);
OFF_IS(TesseraTilePlacement, coord, 0);
OFF_IS(TesseraTilePlacement, tile_def, 8);
OFF_IS(TesseraTilePlacement, variant, 12);
OFF_IS(TesseraTilePlacement, id, 16);

SIZE_IS(TesseraEntityPlacement, 32);
OFF_IS(TesseraEntityPlacement, id, 0);
OFF_IS(TesseraEntityPlacement, def, 8);
OFF_IS(TesseraEntityPlacement, coord, 12);
OFF_IS(TesseraEntityPlacement, facing, 20);
OFF_IS(TesseraEntityPlacement, anim, 24);

SIZE_IS(TesseraEffectPlacement, 32);
OFF_IS(TesseraEffectPlacement, id, 0);
OFF_IS(TesseraEffectPlacement, def, 8);
OFF_IS(TesseraEffectPlacement, coord, 12);
OFF_IS(TesseraEffectPlacement, attach_entity_id, 24);

SIZE_IS(TesseraCamera, 24);
OFF_IS(TesseraCamera, focus, 0);
OFF_IS(TesseraCamera, distance, 8);
OFF_IS(TesseraCamera, yaw, 12);
OFF_IS(TesseraCamera, pitch, 16);
OFF_IS(TesseraCamera, fov, 20);

SIZE_IS(TesseraState, 80);
OFF_IS(TesseraState, tiles, 0);
OFF_IS(TesseraState, tile_count, 8);
OFF_IS(TesseraState, entities, 16);
OFF_IS(TesseraState, entity_count, 24);
OFF_IS(TesseraState, effects, 32);
OFF_IS(TesseraState, effect_count, 40);
OFF_IS(TesseraState, camera, 48);
OFF_IS(TesseraState, epoch, 72);

/* ======================================================================= *
 *  Picking
 * ======================================================================= */
SIZE_IS(TesseraPick, 72);
OFF_IS(TesseraPick, hit_tile, 0);
OFF_IS(TesseraPick, tile, 4);
OFF_IS(TesseraPick, tile_distance, 12);
OFF_IS(TesseraPick, hit_entity, 16);
OFF_IS(TesseraPick, entity, 24);
OFF_IS(TesseraPick, entity_distance, 32);
OFF_IS(TesseraPick, ray_origin, 36);
OFF_IS(TesseraPick, ray_dir, 48);
OFF_IS(TesseraPick, point, 60);

SIZE_IS(TesseraScreenPos, 28);
OFF_IS(TesseraScreenPos, onscreen, 0);
OFF_IS(TesseraScreenPos, x, 4);
OFF_IS(TesseraScreenPos, y, 8);
OFF_IS(TesseraScreenPos, depth, 12);
OFF_IS(TesseraScreenPos, world, 16);

/* ======================================================================= *
 *  Timing / quality / lighting
 * ======================================================================= */
SIZE_IS(TesseraTiming, 28);
OFF_IS(TesseraTiming, move_s, 0);
OFF_IS(TesseraTiming, add_s, 4);
OFF_IS(TesseraTiming, remove_s, 8);
OFF_IS(TesseraTiming, tile_s, 12);
OFF_IS(TesseraTiming, reflow_s, 16);
OFF_IS(TesseraTiming, camera_s, 20);
OFF_IS(TesseraTiming, speed_multiplier, 24);

SIZE_IS(TesseraQuality, 12);
OFF_IS(TesseraQuality, shadows, 0);
OFF_IS(TesseraQuality, msaa, 4);
OFF_IS(TesseraQuality, render_scale, 8);

SIZE_IS(TesseraLight, 40);
OFF_IS(TesseraLight, dir, 0);
OFF_IS(TesseraLight, color, 12);
OFF_IS(TesseraLight, intensity, 24);
OFF_IS(TesseraLight, ambient, 28);

SIZE_IS(TesseraFocus, 16);
OFF_IS(TesseraFocus, enabled, 0);
OFF_IS(TesseraFocus, focus_distance, 4);
OFF_IS(TesseraFocus, focus_range, 8);
OFF_IS(TesseraFocus, blur_strength, 12);

/* Enums are plain int (4 bytes) in this ABI. */
SIZE_IS(TesseraEmitMode, 4);
SIZE_IS(TesseraBlendMode, 4);
SIZE_IS(TesseraShadowMode, 4);
SIZE_IS(TesseraProjection, 4);
SIZE_IS(TesseraLogLevel, 4);

/* Scalar handle types the bindings depend on. */
SIZE_IS(TesseraDefId, 4);
SIZE_IS(TesseraEntityId, 8);
SIZE_IS(TesseraTileId, 8);

/* ======================================================================= *
 *  Runtime dump — the canonical reference table for binding authors.
 * ======================================================================= */
#define P_SIZE(T)     printf("  sizeof(%-24s) = %3zu  align %zu\n", #T, sizeof(T), _Alignof(T))
#define P_OFF(T, f)   printf("    %-20s @ %3zu\n", #f, offsetof(T, f))

int main(void) {
    printf("== Tessera FFI struct layout (target ABI) ==\n\n");

    P_SIZE(TesseraConfig);
    P_OFF(TesseraConfig, native_window);
    P_OFF(TesseraConfig, width);
    P_OFF(TesseraConfig, height);
    P_OFF(TesseraConfig, pixel_density);
    P_OFF(TesseraConfig, engine_driven_loop);
    P_OFF(TesseraConfig, debug);
    P_OFF(TesseraConfig, log);
    P_OFF(TesseraConfig, log_userdata);

    P_SIZE(TesseraBytes);
    P_OFF(TesseraBytes, data);
    P_OFF(TesseraBytes, size);
    P_OFF(TesseraBytes, path);
    P_OFF(TesseraBytes, debug_name);

    P_SIZE(TesseraRect);
    P_OFF(TesseraRect, u0);
    P_OFF(TesseraRect, v0);
    P_OFF(TesseraRect, u1);
    P_OFF(TesseraRect, v1);

    P_SIZE(TesseraTileDef);
    P_OFF(TesseraTileDef, atlas);
    P_OFF(TesseraTileDef, top);
    P_OFF(TesseraTileDef, side);
    P_OFF(TesseraTileDef, bottom);
    P_OFF(TesseraTileDef, tint);
    P_OFF(TesseraTileDef, thickness);

    P_SIZE(TesseraEntityDef);
    P_OFF(TesseraEntityDef, gltf);
    P_OFF(TesseraEntityDef, atlas);
    P_OFF(TesseraEntityDef, scale);
    P_OFF(TesseraEntityDef, pivot);
    P_OFF(TesseraEntityDef, default_anim);
    P_OFF(TesseraEntityDef, move_anim);
    P_OFF(TesseraEntityDef, spawn_anim);
    P_OFF(TesseraEntityDef, despawn_anim);
    P_OFF(TesseraEntityDef, on_spawn_effect);
    P_OFF(TesseraEntityDef, on_despawn_effect);

    P_SIZE(TesseraParticleSpec);
    P_OFF(TesseraParticleSpec, atlas);
    P_OFF(TesseraParticleSpec, sprite);
    P_OFF(TesseraParticleSpec, mode);
    P_OFF(TesseraParticleSpec, count);
    P_OFF(TesseraParticleSpec, lifetime_s);
    P_OFF(TesseraParticleSpec, lifetime_var);
    P_OFF(TesseraParticleSpec, speed);
    P_OFF(TesseraParticleSpec, speed_var);
    P_OFF(TesseraParticleSpec, spread_deg);
    P_OFF(TesseraParticleSpec, gravity);
    P_OFF(TesseraParticleSpec, size_start);
    P_OFF(TesseraParticleSpec, size_end);
    P_OFF(TesseraParticleSpec, color_start);
    P_OFF(TesseraParticleSpec, color_end);
    P_OFF(TesseraParticleSpec, blend);
    P_OFF(TesseraParticleSpec, duration_s);

    P_SIZE(TesseraEffectDef);
    P_OFF(TesseraEffectDef, on_add);
    P_OFF(TesseraEffectDef, on_remove);

    P_SIZE(TesseraDiceFace);
    P_SIZE(TesseraDiceDef);
    P_OFF(TesseraDiceDef, faces);
    P_OFF(TesseraDiceDef, face_count);
    P_OFF(TesseraDiceDef, size);
    P_OFF(TesseraDiceDef, tint);
    P_SIZE(TesseraDiceThrow);
    P_OFF(TesseraDiceThrow, id);
    P_OFF(TesseraDiceThrow, def);
    P_OFF(TesseraDiceThrow, face);
    P_OFF(TesseraDiceThrow, position);
    P_OFF(TesseraDiceThrow, seed);
    P_OFF(TesseraDiceThrow, throw_s);

    P_SIZE(TesseraCoord);
    P_OFF(TesseraCoord, x);
    P_OFF(TesseraCoord, y);

    P_SIZE(TesseraTilePlacement);
    P_OFF(TesseraTilePlacement, coord);
    P_OFF(TesseraTilePlacement, tile_def);
    P_OFF(TesseraTilePlacement, variant);
    P_OFF(TesseraTilePlacement, id);

    P_SIZE(TesseraEntityPlacement);
    P_OFF(TesseraEntityPlacement, id);
    P_OFF(TesseraEntityPlacement, def);
    P_OFF(TesseraEntityPlacement, coord);
    P_OFF(TesseraEntityPlacement, facing);
    P_OFF(TesseraEntityPlacement, anim);

    P_SIZE(TesseraEffectPlacement);
    P_OFF(TesseraEffectPlacement, id);
    P_OFF(TesseraEffectPlacement, def);
    P_OFF(TesseraEffectPlacement, coord);
    P_OFF(TesseraEffectPlacement, attach_entity_id);

    P_SIZE(TesseraCamera);
    P_OFF(TesseraCamera, focus);
    P_OFF(TesseraCamera, distance);
    P_OFF(TesseraCamera, yaw);
    P_OFF(TesseraCamera, pitch);
    P_OFF(TesseraCamera, fov);

    P_SIZE(TesseraState);
    P_OFF(TesseraState, tiles);
    P_OFF(TesseraState, tile_count);
    P_OFF(TesseraState, entities);
    P_OFF(TesseraState, entity_count);
    P_OFF(TesseraState, effects);
    P_OFF(TesseraState, effect_count);
    P_OFF(TesseraState, camera);
    P_OFF(TesseraState, epoch);

    P_SIZE(TesseraTiming);
    P_OFF(TesseraTiming, move_s);
    P_OFF(TesseraTiming, speed_multiplier);

    P_SIZE(TesseraQuality);
    P_OFF(TesseraQuality, shadows);
    P_OFF(TesseraQuality, msaa);
    P_OFF(TesseraQuality, render_scale);

    P_SIZE(TesseraLight);
    P_OFF(TesseraLight, dir);
    P_OFF(TesseraLight, color);
    P_OFF(TesseraLight, intensity);
    P_OFF(TesseraLight, ambient);

    printf("\n  sizeof(TesseraDefId)    = %zu\n", sizeof(TesseraDefId));
    printf("  sizeof(TesseraEntityId) = %zu\n", sizeof(TesseraEntityId));
    printf("  sizeof(enum)            = %zu\n", sizeof(TesseraEmitMode));

    printf("\nall FFI layout static-asserts passed at compile time\n");
    return 0;
}
