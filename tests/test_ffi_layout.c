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
#include <string.h>

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

/* ---- cards ---- */
SIZE_IS(TesseraCardDef, 92);
OFF_IS(TesseraCardDef, visible_atlas, 0);
OFF_IS(TesseraCardDef, visible_uv, 4);
OFF_IS(TesseraCardDef, hidden_atlas, 20);
OFF_IS(TesseraCardDef, hidden_uv, 24);
OFF_IS(TesseraCardDef, back_atlas, 40);
OFF_IS(TesseraCardDef, back_uv, 44);
OFF_IS(TesseraCardDef, width, 60);
OFF_IS(TesseraCardDef, height, 64);
OFF_IS(TesseraCardDef, thickness, 68);
OFF_IS(TesseraCardDef, corner_radius, 72);
OFF_IS(TesseraCardDef, tint, 76);

/* ======================================================================= *
 *  Immutable state
 * ======================================================================= */
SIZE_IS(TesseraCoord, 8);
OFF_IS(TesseraCoord, x, 0);
OFF_IS(TesseraCoord, y, 4);

SIZE_IS(TesseraCoordF, 8);
OFF_IS(TesseraCoordF, x, 0);
OFF_IS(TesseraCoordF, y, 4);

SIZE_IS(TesseraTilePlacement, 24);
OFF_IS(TesseraTilePlacement, coord, 0);
OFF_IS(TesseraTilePlacement, tile_def, 8);
OFF_IS(TesseraTilePlacement, variant, 12);
OFF_IS(TesseraTilePlacement, id, 16);

SIZE_IS(TesseraEntityPlacement, 48);
OFF_IS(TesseraEntityPlacement, id, 0);
OFF_IS(TesseraEntityPlacement, def, 8);
OFF_IS(TesseraEntityPlacement, coord, 12);
OFF_IS(TesseraEntityPlacement, facing, 20);
OFF_IS(TesseraEntityPlacement, anim, 24);
OFF_IS(TesseraEntityPlacement, path, 32);
OFF_IS(TesseraEntityPlacement, path_count, 40);

SIZE_IS(TesseraEffectPlacement, 40);
OFF_IS(TesseraEffectPlacement, id, 0);
OFF_IS(TesseraEffectPlacement, def, 8);
OFF_IS(TesseraEffectPlacement, coord, 12);
OFF_IS(TesseraEffectPlacement, attach_entity_id, 24);
OFF_IS(TesseraEffectPlacement, attach_card_id, 32);

SIZE_IS(TesseraDicePlacement, 40);
OFF_IS(TesseraDicePlacement, id, 0);
OFF_IS(TesseraDicePlacement, def, 8);
OFF_IS(TesseraDicePlacement, face, 12);
OFF_IS(TesseraDicePlacement, position, 16);
OFF_IS(TesseraDicePlacement, seed, 28);
OFF_IS(TesseraDicePlacement, throw_s, 32);

SIZE_IS(TesseraCardPlacement, 88);
OFF_IS(TesseraCardPlacement, id, 0);
OFF_IS(TesseraCardPlacement, def, 8);
OFF_IS(TesseraCardPlacement, position, 12);
OFF_IS(TesseraCardPlacement, orientation, 24);
OFF_IS(TesseraCardPlacement, hidden, 40);
OFF_IS(TesseraCardPlacement, hand, 48);
OFF_IS(TesseraCardPlacement, hand_slot, 56);
OFF_IS(TesseraCardPlacement, source_draw, 64);
OFF_IS(TesseraCardPlacement, path, 72);
OFF_IS(TesseraCardPlacement, path_count, 80);

SIZE_IS(TesseraCardDrawPlacement, 48);
OFF_IS(TesseraCardDrawPlacement, id, 0);
OFF_IS(TesseraCardDrawPlacement, def, 8);
OFF_IS(TesseraCardDrawPlacement, position, 12);
OFF_IS(TesseraCardDrawPlacement, orientation, 24);
OFF_IS(TesseraCardDrawPlacement, count, 40);
OFF_IS(TesseraCardDrawPlacement, top_hidden, 44);

SIZE_IS(TesseraHandPlacement, 56);
OFF_IS(TesseraHandPlacement, id, 0);
OFF_IS(TesseraHandPlacement, position, 8);
OFF_IS(TesseraHandPlacement, orientation, 20);
OFF_IS(TesseraHandPlacement, spread_deg, 36);
OFF_IS(TesseraHandPlacement, radius, 40);
OFF_IS(TesseraHandPlacement, card_spacing, 44);
OFF_IS(TesseraHandPlacement, selected_card, 48);

SIZE_IS(TesseraOverlayPlacement, 68);
OFF_IS(TesseraOverlayPlacement, coord, 0);
OFF_IS(TesseraOverlayPlacement, shape, 8);
OFF_IS(TesseraOverlayPlacement, atlas, 12);
OFF_IS(TesseraOverlayPlacement, uv, 16);
OFF_IS(TesseraOverlayPlacement, tint, 32);
OFF_IS(TesseraOverlayPlacement, pulse_s, 48);
OFF_IS(TesseraOverlayPlacement, pulse_alpha_min, 52);
OFF_IS(TesseraOverlayPlacement, pulse_alpha_max, 56);
OFF_IS(TesseraOverlayPlacement, pulse_scale_min, 60);
OFF_IS(TesseraOverlayPlacement, pulse_scale_max, 64);

SIZE_IS(TesseraLabelPlacement, 128);
OFF_IS(TesseraLabelPlacement, id, 0);
OFF_IS(TesseraLabelPlacement, font, 8);
OFF_IS(TesseraLabelPlacement, text, 12);
OFF_IS(TesseraLabelPlacement, anchor, 76);
OFF_IS(TesseraLabelPlacement, anchor_id, 80);
OFF_IS(TesseraLabelPlacement, position, 88);
OFF_IS(TesseraLabelPlacement, size, 100);
OFF_IS(TesseraLabelPlacement, color, 104);
OFF_IS(TesseraLabelPlacement, billboard, 120);

SIZE_IS(TesseraHighlightPlacement, 48);
OFF_IS(TesseraHighlightPlacement, target_id, 0);
OFF_IS(TesseraHighlightPlacement, kind, 8);
OFF_IS(TesseraHighlightPlacement, style, 12);
OFF_IS(TesseraHighlightPlacement, color, 16);
OFF_IS(TesseraHighlightPlacement, thickness, 32);
OFF_IS(TesseraHighlightPlacement, pulse_s, 36);
OFF_IS(TesseraHighlightPlacement, pulse_min, 40);
OFF_IS(TesseraHighlightPlacement, pulse_max, 44);

SIZE_IS(TesseraCamera, 96);
OFF_IS(TesseraCamera, mode, 0);
OFF_IS(TesseraCamera, focus, 4);
OFF_IS(TesseraCamera, distance, 12);
OFF_IS(TesseraCamera, yaw, 16);
OFF_IS(TesseraCamera, pitch, 20);
OFF_IS(TesseraCamera, fov, 24);
OFF_IS(TesseraCamera, position, 28);
OFF_IS(TesseraCamera, orientation, 40);
OFF_IS(TesseraCamera, target, 56);
OFF_IS(TesseraCamera, target_id, 72);
OFF_IS(TesseraCamera, focus_card_id, 80);
OFF_IS(TesseraCamera, fit_padding, 88);

SIZE_IS(TesseraPointLightPlacement, 40);
OFF_IS(TesseraPointLightPlacement, id, 0);
OFF_IS(TesseraPointLightPlacement, position, 8);
OFF_IS(TesseraPointLightPlacement, color, 20);
OFF_IS(TesseraPointLightPlacement, intensity, 32);
OFF_IS(TesseraPointLightPlacement, radius, 36);

SIZE_IS(TesseraWorldModelPlacement, 48);
OFF_IS(TesseraWorldModelPlacement, id, 0);
OFF_IS(TesseraWorldModelPlacement, def, 8);
OFF_IS(TesseraWorldModelPlacement, position, 12);
OFF_IS(TesseraWorldModelPlacement, orientation, 24);
OFF_IS(TesseraWorldModelPlacement, scale, 40);

SIZE_IS(TesseraState, 296);
OFF_IS(TesseraState, tiles, 0);
OFF_IS(TesseraState, tile_count, 8);
OFF_IS(TesseraState, entities, 16);
OFF_IS(TesseraState, entity_count, 24);
OFF_IS(TesseraState, effects, 32);
OFF_IS(TesseraState, effect_count, 40);
OFF_IS(TesseraState, camera, 48);
OFF_IS(TesseraState, epoch, 144);
OFF_IS(TesseraState, cards, 152);
OFF_IS(TesseraState, card_count, 160);
OFF_IS(TesseraState, card_draws, 168);
OFF_IS(TesseraState, card_draw_count, 176);
OFF_IS(TesseraState, hands, 184);
OFF_IS(TesseraState, hand_count, 192);
OFF_IS(TesseraState, dice, 200);
OFF_IS(TesseraState, dice_count, 208);
OFF_IS(TesseraState, overlays, 216);
OFF_IS(TesseraState, overlay_count, 224);
OFF_IS(TesseraState, labels, 232);
OFF_IS(TesseraState, label_count, 240);
OFF_IS(TesseraState, highlights, 248);
OFF_IS(TesseraState, highlight_count, 256);
OFF_IS(TesseraState, point_lights, 264);
OFF_IS(TesseraState, point_light_count, 272);
OFF_IS(TesseraState, world_models, 280);
OFF_IS(TesseraState, world_model_count, 288);

/* ======================================================================= *
 *  Engine events
 * ======================================================================= */
SIZE_IS(TesseraEvent, 40);
OFF_IS(TesseraEvent, time, 0);
OFF_IS(TesseraEvent, subject_id, 8);
OFF_IS(TesseraEvent, type, 16);
OFF_IS(TesseraEvent, subject, 20);
OFF_IS(TesseraEvent, coord, 24);
OFF_IS(TesseraEvent, value, 32);
OFF_IS(TesseraEvent, reserved, 36);

/* ======================================================================= *
 *  Picking
 * ======================================================================= */
SIZE_IS(TesseraPick, 112);
OFF_IS(TesseraPick, hit_tile, 0);
OFF_IS(TesseraPick, tile, 4);
OFF_IS(TesseraPick, tile_distance, 12);
OFF_IS(TesseraPick, hit_entity, 16);
OFF_IS(TesseraPick, entity, 24);
OFF_IS(TesseraPick, entity_distance, 32);
OFF_IS(TesseraPick, ray_origin, 36);
OFF_IS(TesseraPick, ray_dir, 48);
OFF_IS(TesseraPick, point, 60);
OFF_IS(TesseraPick, hit_dice, 72);
OFF_IS(TesseraPick, dice, 80);
OFF_IS(TesseraPick, dice_distance, 88);
OFF_IS(TesseraPick, hit_card, 92);
OFF_IS(TesseraPick, card, 96);
OFF_IS(TesseraPick, card_distance, 104);

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
SIZE_IS(TesseraOverlayShape, 4);
SIZE_IS(TesseraLabelAnchor, 4);
SIZE_IS(TesseraHighlightKind, 4);
SIZE_IS(TesseraHighlightStyle, 4);
SIZE_IS(TesseraEventType, 4);
SIZE_IS(TesseraEventSubject, 4);

/* Scalar handle types the bindings depend on. */
SIZE_IS(TesseraDefId, 4);
SIZE_IS(TesseraEntityId, 8);
SIZE_IS(TesseraTileId, 8);

/* ======================================================================= *
 *  Machine-readable dump (`--dump`) — the canonical layout reference that
 *  tools/check_ffi_bindings.py diffs the Lua/Dart binding structs against.
 *  One line per record:
 *    struct <name> size=<n> align=<n>
 *    field <struct> <name> off=<n> size=<n>
 *    scalar <name> size=<n>
 * ======================================================================= */
#define D_STRUCT(T)   printf("struct %s size=%zu align=%zu\n", #T, sizeof(T), _Alignof(T))
#define D_FIELD(T, f) printf("field %s %s off=%zu size=%zu\n", #T, #f, \
                             offsetof(T, f), sizeof(((T*)0)->f))
#define D_SCALAR(T)   printf("scalar %s size=%zu\n", #T, sizeof(T))

static void dump_layout(void) {
    D_STRUCT(TesseraConfig);
    D_FIELD(TesseraConfig, native_window);
    D_FIELD(TesseraConfig, width);
    D_FIELD(TesseraConfig, height);
    D_FIELD(TesseraConfig, pixel_density);
    D_FIELD(TesseraConfig, engine_driven_loop);
    D_FIELD(TesseraConfig, debug);
    D_FIELD(TesseraConfig, log);
    D_FIELD(TesseraConfig, log_userdata);

    D_STRUCT(TesseraBytes);
    D_FIELD(TesseraBytes, data);
    D_FIELD(TesseraBytes, size);
    D_FIELD(TesseraBytes, path);
    D_FIELD(TesseraBytes, debug_name);

    D_STRUCT(TesseraRect);
    D_FIELD(TesseraRect, u0);
    D_FIELD(TesseraRect, v0);
    D_FIELD(TesseraRect, u1);
    D_FIELD(TesseraRect, v1);

    D_STRUCT(TesseraTileDef);
    D_FIELD(TesseraTileDef, atlas);
    D_FIELD(TesseraTileDef, top);
    D_FIELD(TesseraTileDef, side);
    D_FIELD(TesseraTileDef, bottom);
    D_FIELD(TesseraTileDef, tint);
    D_FIELD(TesseraTileDef, thickness);

    D_STRUCT(TesseraEntityDef);
    D_FIELD(TesseraEntityDef, gltf);
    D_FIELD(TesseraEntityDef, atlas);
    D_FIELD(TesseraEntityDef, scale);
    D_FIELD(TesseraEntityDef, pivot);
    D_FIELD(TesseraEntityDef, default_anim);
    D_FIELD(TesseraEntityDef, move_anim);
    D_FIELD(TesseraEntityDef, spawn_anim);
    D_FIELD(TesseraEntityDef, despawn_anim);
    D_FIELD(TesseraEntityDef, on_spawn_effect);
    D_FIELD(TesseraEntityDef, on_despawn_effect);

    D_STRUCT(TesseraParticleSpec);
    D_FIELD(TesseraParticleSpec, atlas);
    D_FIELD(TesseraParticleSpec, sprite);
    D_FIELD(TesseraParticleSpec, mode);
    D_FIELD(TesseraParticleSpec, count);
    D_FIELD(TesseraParticleSpec, lifetime_s);
    D_FIELD(TesseraParticleSpec, lifetime_var);
    D_FIELD(TesseraParticleSpec, speed);
    D_FIELD(TesseraParticleSpec, speed_var);
    D_FIELD(TesseraParticleSpec, spread_deg);
    D_FIELD(TesseraParticleSpec, gravity);
    D_FIELD(TesseraParticleSpec, size_start);
    D_FIELD(TesseraParticleSpec, size_end);
    D_FIELD(TesseraParticleSpec, color_start);
    D_FIELD(TesseraParticleSpec, color_end);
    D_FIELD(TesseraParticleSpec, blend);
    D_FIELD(TesseraParticleSpec, duration_s);

    D_STRUCT(TesseraEffectDef);
    D_FIELD(TesseraEffectDef, on_add);
    D_FIELD(TesseraEffectDef, on_remove);

    D_STRUCT(TesseraDiceFace);
    D_FIELD(TesseraDiceFace, sprite);

    D_STRUCT(TesseraDiceDef);
    D_FIELD(TesseraDiceDef, faces);
    D_FIELD(TesseraDiceDef, face_count);
    D_FIELD(TesseraDiceDef, size);
    D_FIELD(TesseraDiceDef, tint);

    D_STRUCT(TesseraCardDef);
    D_FIELD(TesseraCardDef, visible_atlas);
    D_FIELD(TesseraCardDef, visible_uv);
    D_FIELD(TesseraCardDef, hidden_atlas);
    D_FIELD(TesseraCardDef, hidden_uv);
    D_FIELD(TesseraCardDef, back_atlas);
    D_FIELD(TesseraCardDef, back_uv);
    D_FIELD(TesseraCardDef, width);
    D_FIELD(TesseraCardDef, height);
    D_FIELD(TesseraCardDef, thickness);
    D_FIELD(TesseraCardDef, corner_radius);
    D_FIELD(TesseraCardDef, tint);

    D_STRUCT(TesseraCoord);
    D_FIELD(TesseraCoord, x);
    D_FIELD(TesseraCoord, y);

    D_STRUCT(TesseraCoordF);
    D_FIELD(TesseraCoordF, x);
    D_FIELD(TesseraCoordF, y);

    D_STRUCT(TesseraTilePlacement);
    D_FIELD(TesseraTilePlacement, coord);
    D_FIELD(TesseraTilePlacement, tile_def);
    D_FIELD(TesseraTilePlacement, variant);
    D_FIELD(TesseraTilePlacement, id);

    D_STRUCT(TesseraEntityPlacement);
    D_FIELD(TesseraEntityPlacement, id);
    D_FIELD(TesseraEntityPlacement, def);
    D_FIELD(TesseraEntityPlacement, coord);
    D_FIELD(TesseraEntityPlacement, facing);
    D_FIELD(TesseraEntityPlacement, anim);
    D_FIELD(TesseraEntityPlacement, path);
    D_FIELD(TesseraEntityPlacement, path_count);

    D_STRUCT(TesseraEffectPlacement);
    D_FIELD(TesseraEffectPlacement, id);
    D_FIELD(TesseraEffectPlacement, def);
    D_FIELD(TesseraEffectPlacement, coord);
    D_FIELD(TesseraEffectPlacement, attach_entity_id);
    D_FIELD(TesseraEffectPlacement, attach_card_id);

    D_STRUCT(TesseraDicePlacement);
    D_FIELD(TesseraDicePlacement, id);
    D_FIELD(TesseraDicePlacement, def);
    D_FIELD(TesseraDicePlacement, face);
    D_FIELD(TesseraDicePlacement, position);
    D_FIELD(TesseraDicePlacement, seed);
    D_FIELD(TesseraDicePlacement, throw_s);

    D_STRUCT(TesseraCardPlacement);
    D_FIELD(TesseraCardPlacement, id);
    D_FIELD(TesseraCardPlacement, def);
    D_FIELD(TesseraCardPlacement, position);
    D_FIELD(TesseraCardPlacement, orientation);
    D_FIELD(TesseraCardPlacement, hidden);
    D_FIELD(TesseraCardPlacement, hand);
    D_FIELD(TesseraCardPlacement, hand_slot);
    D_FIELD(TesseraCardPlacement, source_draw);
    D_FIELD(TesseraCardPlacement, path);
    D_FIELD(TesseraCardPlacement, path_count);

    D_STRUCT(TesseraCardDrawPlacement);
    D_FIELD(TesseraCardDrawPlacement, id);
    D_FIELD(TesseraCardDrawPlacement, def);
    D_FIELD(TesseraCardDrawPlacement, position);
    D_FIELD(TesseraCardDrawPlacement, orientation);
    D_FIELD(TesseraCardDrawPlacement, count);
    D_FIELD(TesseraCardDrawPlacement, top_hidden);

    D_STRUCT(TesseraHandPlacement);
    D_FIELD(TesseraHandPlacement, id);
    D_FIELD(TesseraHandPlacement, position);
    D_FIELD(TesseraHandPlacement, orientation);
    D_FIELD(TesseraHandPlacement, spread_deg);
    D_FIELD(TesseraHandPlacement, radius);
    D_FIELD(TesseraHandPlacement, card_spacing);
    D_FIELD(TesseraHandPlacement, selected_card);

    D_STRUCT(TesseraOverlayPlacement);
    D_FIELD(TesseraOverlayPlacement, coord);
    D_FIELD(TesseraOverlayPlacement, shape);
    D_FIELD(TesseraOverlayPlacement, atlas);
    D_FIELD(TesseraOverlayPlacement, uv);
    D_FIELD(TesseraOverlayPlacement, tint);
    D_FIELD(TesseraOverlayPlacement, pulse_s);
    D_FIELD(TesseraOverlayPlacement, pulse_alpha_min);
    D_FIELD(TesseraOverlayPlacement, pulse_alpha_max);
    D_FIELD(TesseraOverlayPlacement, pulse_scale_min);
    D_FIELD(TesseraOverlayPlacement, pulse_scale_max);

    D_STRUCT(TesseraLabelPlacement);
    D_FIELD(TesseraLabelPlacement, id);
    D_FIELD(TesseraLabelPlacement, font);
    D_FIELD(TesseraLabelPlacement, text);
    D_FIELD(TesseraLabelPlacement, anchor);
    D_FIELD(TesseraLabelPlacement, anchor_id);
    D_FIELD(TesseraLabelPlacement, position);
    D_FIELD(TesseraLabelPlacement, size);
    D_FIELD(TesseraLabelPlacement, color);
    D_FIELD(TesseraLabelPlacement, billboard);

    D_STRUCT(TesseraHighlightPlacement);
    D_FIELD(TesseraHighlightPlacement, target_id);
    D_FIELD(TesseraHighlightPlacement, kind);
    D_FIELD(TesseraHighlightPlacement, style);
    D_FIELD(TesseraHighlightPlacement, color);
    D_FIELD(TesseraHighlightPlacement, thickness);
    D_FIELD(TesseraHighlightPlacement, pulse_s);
    D_FIELD(TesseraHighlightPlacement, pulse_min);
    D_FIELD(TesseraHighlightPlacement, pulse_max);

    D_STRUCT(TesseraCamera);
    D_FIELD(TesseraCamera, mode);
    D_FIELD(TesseraCamera, focus);
    D_FIELD(TesseraCamera, distance);
    D_FIELD(TesseraCamera, yaw);
    D_FIELD(TesseraCamera, pitch);
    D_FIELD(TesseraCamera, fov);
    D_FIELD(TesseraCamera, position);
    D_FIELD(TesseraCamera, orientation);
    D_FIELD(TesseraCamera, target);
    D_FIELD(TesseraCamera, target_id);
    D_FIELD(TesseraCamera, focus_card_id);
    D_FIELD(TesseraCamera, fit_padding);

    D_STRUCT(TesseraState);
    D_FIELD(TesseraState, tiles);
    D_FIELD(TesseraState, tile_count);
    D_FIELD(TesseraState, entities);
    D_FIELD(TesseraState, entity_count);
    D_FIELD(TesseraState, effects);
    D_FIELD(TesseraState, effect_count);
    D_FIELD(TesseraState, camera);
    D_FIELD(TesseraState, epoch);
    D_FIELD(TesseraState, cards);
    D_FIELD(TesseraState, card_count);
    D_FIELD(TesseraState, card_draws);
    D_FIELD(TesseraState, card_draw_count);
    D_FIELD(TesseraState, hands);
    D_FIELD(TesseraState, hand_count);
    D_FIELD(TesseraState, dice);
    D_FIELD(TesseraState, dice_count);
    D_FIELD(TesseraState, overlays);
    D_FIELD(TesseraState, overlay_count);
    D_FIELD(TesseraState, labels);
    D_FIELD(TesseraState, label_count);
    D_FIELD(TesseraState, highlights);
    D_FIELD(TesseraState, highlight_count);
    D_FIELD(TesseraState, point_lights);
    D_FIELD(TesseraState, point_light_count);
    D_FIELD(TesseraState, world_models);
    D_FIELD(TesseraState, world_model_count);

    D_STRUCT(TesseraPointLightPlacement);
    D_FIELD(TesseraPointLightPlacement, id);
    D_FIELD(TesseraPointLightPlacement, position);
    D_FIELD(TesseraPointLightPlacement, color);
    D_FIELD(TesseraPointLightPlacement, intensity);
    D_FIELD(TesseraPointLightPlacement, radius);

    D_STRUCT(TesseraWorldModelPlacement);
    D_FIELD(TesseraWorldModelPlacement, id);
    D_FIELD(TesseraWorldModelPlacement, def);
    D_FIELD(TesseraWorldModelPlacement, position);
    D_FIELD(TesseraWorldModelPlacement, orientation);
    D_FIELD(TesseraWorldModelPlacement, scale);

    D_STRUCT(TesseraEvent);
    D_FIELD(TesseraEvent, time);
    D_FIELD(TesseraEvent, subject_id);
    D_FIELD(TesseraEvent, type);
    D_FIELD(TesseraEvent, subject);
    D_FIELD(TesseraEvent, coord);
    D_FIELD(TesseraEvent, value);
    D_FIELD(TesseraEvent, reserved);

    D_STRUCT(TesseraPick);
    D_FIELD(TesseraPick, hit_tile);
    D_FIELD(TesseraPick, tile);
    D_FIELD(TesseraPick, tile_distance);
    D_FIELD(TesseraPick, hit_entity);
    D_FIELD(TesseraPick, entity);
    D_FIELD(TesseraPick, entity_distance);
    D_FIELD(TesseraPick, ray_origin);
    D_FIELD(TesseraPick, ray_dir);
    D_FIELD(TesseraPick, point);
    D_FIELD(TesseraPick, hit_dice);
    D_FIELD(TesseraPick, dice);
    D_FIELD(TesseraPick, dice_distance);
    D_FIELD(TesseraPick, hit_card);
    D_FIELD(TesseraPick, card);
    D_FIELD(TesseraPick, card_distance);

    D_STRUCT(TesseraScreenPos);
    D_FIELD(TesseraScreenPos, onscreen);
    D_FIELD(TesseraScreenPos, x);
    D_FIELD(TesseraScreenPos, y);
    D_FIELD(TesseraScreenPos, depth);
    D_FIELD(TesseraScreenPos, world);

    D_STRUCT(TesseraTiming);
    D_FIELD(TesseraTiming, move_s);
    D_FIELD(TesseraTiming, add_s);
    D_FIELD(TesseraTiming, remove_s);
    D_FIELD(TesseraTiming, tile_s);
    D_FIELD(TesseraTiming, reflow_s);
    D_FIELD(TesseraTiming, camera_s);
    D_FIELD(TesseraTiming, speed_multiplier);

    D_STRUCT(TesseraQuality);
    D_FIELD(TesseraQuality, shadows);
    D_FIELD(TesseraQuality, msaa);
    D_FIELD(TesseraQuality, render_scale);

    D_STRUCT(TesseraLight);
    D_FIELD(TesseraLight, dir);
    D_FIELD(TesseraLight, color);
    D_FIELD(TesseraLight, intensity);
    D_FIELD(TesseraLight, ambient);

    D_STRUCT(TesseraFocus);
    D_FIELD(TesseraFocus, enabled);
    D_FIELD(TesseraFocus, focus_distance);
    D_FIELD(TesseraFocus, focus_range);
    D_FIELD(TesseraFocus, blur_strength);

    D_SCALAR(TesseraDefId);
    D_SCALAR(TesseraEntityId);
    D_SCALAR(TesseraTileId);
    D_SCALAR(TesseraCardId);
    D_SCALAR(TesseraCardDrawId);
    D_SCALAR(TesseraHandId);
    D_SCALAR(TesseraDiceId);
    D_SCALAR(TesseraOpId);
    D_SCALAR(TesseraLogLevel);
    D_SCALAR(TesseraEmitMode);
    D_SCALAR(TesseraBlendMode);
    D_SCALAR(TesseraShadowMode);
    D_SCALAR(TesseraProjection);
    D_SCALAR(TesseraOverlayShape);
    D_SCALAR(TesseraLabelId);
    D_SCALAR(TesseraLabelAnchor);
    D_SCALAR(TesseraHighlightKind);
    D_SCALAR(TesseraHighlightStyle);
    D_SCALAR(TesseraEventType);
    D_SCALAR(TesseraEventSubject);
}

/* ======================================================================= *
 *  Runtime dump — the canonical reference table for binding authors.
 * ======================================================================= */
#define P_SIZE(T)     printf("  sizeof(%-24s) = %3zu  align %zu\n", #T, sizeof(T), _Alignof(T))
#define P_OFF(T, f)   printf("    %-20s @ %3zu\n", #f, offsetof(T, f))

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "--dump") == 0) {
        dump_layout();
        return 0;
    }
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

    P_SIZE(TesseraCardDef);
    P_OFF(TesseraCardDef, visible_atlas);
    P_OFF(TesseraCardDef, hidden_atlas);
    P_OFF(TesseraCardDef, back_atlas);
    P_OFF(TesseraCardDef, width);
    P_OFF(TesseraCardDef, tint);

    P_SIZE(TesseraDicePlacement);
    P_OFF(TesseraDicePlacement, id);
    P_OFF(TesseraDicePlacement, position);

    P_SIZE(TesseraCardPlacement);
    P_OFF(TesseraCardPlacement, id);
    P_OFF(TesseraCardPlacement, orientation);
    P_OFF(TesseraCardPlacement, hand);

    P_SIZE(TesseraCardDrawPlacement);
    P_OFF(TesseraCardDrawPlacement, count);

    P_SIZE(TesseraHandPlacement);
    P_OFF(TesseraHandPlacement, spread_deg);

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
    P_OFF(TesseraEffectPlacement, attach_card_id);

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
