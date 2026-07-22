// card.vert — Vulkan GLSL port of assets/shaders/card.vertex.msl.
// SDL_GPU SPIR-V binding convention: vertex uniform buffers at set=1.
#version 450

layout(std140, set = 1, binding = 0) uniform FrameUniform {
    mat4 view_proj;
    vec4 light_dir;
    vec4 ambient;
    vec4 light_color;
    vec4 camera_pos;
} frame;

layout(std140, set = 1, binding = 1) uniform CardUniform {
    mat4 model;
    vec4 tint;
    vec4 uv_visible;
    vec4 uv_hidden;
    vec4 uv_back;
    vec4 params;    // x = mix (0 visible .. 1 hidden)
} card;

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in float in_face;

layout(location = 0) out vec3 world_pos;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec2 v_uv;
layout(location = 3) out float v_face;

void main() {
    vec4 world = card.model * vec4(in_pos, 1.0);
    gl_Position = frame.view_proj * world;
    world_pos = world.xyz;
    v_normal = normalize((card.model * vec4(in_normal, 0.0)).xyz);
    v_uv = in_uv;
    v_face = in_face;
}
