// shadow.vert — Vulkan GLSL port of assets/shaders/shadow.vertex.msl.
// Depth-only shadow-map pass. SDL_GPU SPIR-V: vertex uniform buffers at set=1.
// Only location 0 is declared, so any vertex layout with pos at attribute 0
// (static mesh, dice, card) can share this shader via the pipeline pitch.
#version 450

layout(std140, set = 1, binding = 0) uniform ShadowFrameUniform {
    mat4 light_vp;
} frame;

layout(std140, set = 1, binding = 1) uniform ObjectUniform {
    mat4 model;
    vec4 tint;
    vec4 uv_rect;
} obj;

layout(location = 0) in vec3 in_pos;

void main() {
    gl_Position = frame.light_vp * (obj.model * vec4(in_pos, 1.0));
}
