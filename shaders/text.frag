// text.frag — Vulkan GLSL port of assets/shaders/text.fragment.msl.
// Glyph quad: the font atlas is white with alpha = coverage, so the sample
// times the label tint is the finished fragment.
#version 450

layout(set = 2, binding = 0) uniform sampler2D tex;

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_tint;

layout(location = 0) out vec4 frag;

void main() {
    vec4 s = texture(tex, v_uv);
    frag = vec4(v_tint.rgb * s.rgb, v_tint.a * s.a);
}
