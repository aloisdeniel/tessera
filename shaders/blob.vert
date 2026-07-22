// blob.vert — Vulkan GLSL port of assets/shaders/blob.vertex.msl.
#version 450

layout(std140, set = 1, binding = 0) uniform FrameUniform {
    mat4 view_proj;
    vec4 light_dir;
    vec4 ambient;
    vec4 light_color;
    vec4 camera_pos;
} frame;

layout(std140, set = 1, binding = 1) uniform ObjectUniform {
    mat4 model;
    vec4 tint;      // rgb unused, a = shadow opacity
    vec4 uv_rect;
} obj;

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;   // unused, kept for a shared vertex layout
layout(location = 2) in vec2 in_uv;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out float v_opacity;

void main() {
    vec4 world = obj.model * vec4(in_pos, 1.0);
    gl_Position = frame.view_proj * world;
    v_uv = in_uv;
    v_opacity = obj.tint.a;
}
