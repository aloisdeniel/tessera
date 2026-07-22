// particle.vert — Vulkan GLSL port of assets/shaders/particle.vertex.msl.
// Particles arrive as camera-facing quads already expanded in world space on
// the CPU, so the vertex stage just projects them.
#version 450

layout(std140, set = 1, binding = 0) uniform FrameUniform {
    mat4 view_proj;
    vec4 light_dir;
    vec4 ambient;
    vec4 light_color;
    vec4 camera_pos;
} frame;

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;

void main() {
    gl_Position = frame.view_proj * vec4(in_pos, 1.0);
    v_uv = in_uv;
    v_color = in_color;
}
