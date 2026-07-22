// particle.frag — Vulkan GLSL port of assets/shaders/particle.fragment.msl.
#version 450

layout(set = 2, binding = 0) uniform sampler2D tex;

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;

layout(location = 0) out vec4 frag;

void main() {
    vec4 t = texture(tex, v_uv);
    frag = t * v_color;   // color carries premultiplied lifetime fade
}
