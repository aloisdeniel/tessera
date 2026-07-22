// mesh.vert — Vulkan GLSL port of assets/shaders/mesh.vertex.msl.
// SDL_GPU SPIR-V binding convention: vertex uniform buffers at set=1.
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
    vec4 tint;
    vec4 uv_rect;   // u0,v0,u1,v1
} obj;

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;

layout(location = 0) out vec3 world_pos;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec2 v_uv;
layout(location = 3) out vec4 v_tint;

void main() {
    vec4 world = obj.model * vec4(in_pos, 1.0);
    gl_Position = frame.view_proj * world;
    world_pos = world.xyz;
    v_normal = normalize((obj.model * vec4(in_normal, 0.0)).xyz);
    v_uv = obj.uv_rect.xy + in_uv * (obj.uv_rect.zw - obj.uv_rect.xy);
    v_tint = obj.tint;
}
