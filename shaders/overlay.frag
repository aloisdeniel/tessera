// overlay.frag — Vulkan GLSL port of assets/shaders/overlay.fragment.msl.
// Tile-overlay decal: a tinted sprite quad, or a procedural filled disc / ring
// (both with a soft rim so the edge doesn't shimmer).
#version 450

layout(set = 2, binding = 0) uniform sampler2D tex;

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec2 v_local;
layout(location = 2) in vec4 v_tint;
layout(location = 3) in float v_shape;

layout(location = 0) out vec4 frag;

void main() {
    if (v_shape < 0.5) {                        // SPRITE: textured quad
        frag = texture(tex, v_uv) * v_tint;
        return;
    }
    vec2 d = v_local - vec2(0.5, 0.5);
    float r = length(d) * 2.0;                  // 0 centre, 1 at tile edge
    float a;
    if (v_shape < 1.5) {                        // DISC: filled, soft rim
        a = smoothstep(0.92, 0.80, r);
    } else {                                    // RING: soft band near the rim
        a = smoothstep(0.98, 0.90, r) * smoothstep(0.64, 0.74, r);
    }
    frag = vec4(v_tint.rgb, v_tint.a * a);
}
