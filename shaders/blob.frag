// blob.frag — Vulkan GLSL port of assets/shaders/blob.fragment.msl.
// Soft radial blob shadow: a dark disc that fades to nothing at the rim.
#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 1) in float v_opacity;

layout(location = 0) out vec4 frag;

void main() {
    vec2 d = v_uv - vec2(0.5, 0.5);
    float r = length(d) * 2.0;           // 0 at centre, 1 at quad edge
    float a = smoothstep(1.0, 0.30, r);  // solid core, soft falloff
    frag = vec4(0.0, 0.0, 0.0, a * v_opacity);
}
