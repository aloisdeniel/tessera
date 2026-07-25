// mask.frag — Vulkan GLSL port of assets/shaders/mask.fragment.msl.
// Flat silhouette-mask pass (selection highlights): every covered pixel
// becomes 1. Paired with the position-only shadow vertex shaders.
#version 450

layout(location = 0) out vec4 frag;

void main() {
    frag = vec4(1.0);
}
