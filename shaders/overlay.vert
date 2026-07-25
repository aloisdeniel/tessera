// overlay.vert — Vulkan GLSL port of assets/shaders/overlay.vertex.msl.
#version 450

layout(std140, set = 1, binding = 0) uniform FrameUniform {
    mat4 view_proj;
    vec4 light_dir;
    vec4 ambient;
    vec4 light_color;
    vec4 camera_pos;
} frame;

layout(std140, set = 1, binding = 1) uniform OverlayUniform {
    mat4 model;
    vec4 tint;      // rgba; a already includes fade + pulse
    vec4 uv_rect;   // SPRITE atlas remap (u0,v0,u1,v1)
    vec4 params;    // x = shape (0 sprite, 1 disc, 2 ring)
} obj;

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;   // unused, kept for a shared vertex layout
layout(location = 2) in vec2 in_uv;

layout(location = 0) out vec2 v_uv;      // atlas-remapped (sprite)
layout(location = 1) out vec2 v_local;   // raw 0..1 quad uv (procedural shapes)
layout(location = 2) out vec4 v_tint;
layout(location = 3) out float v_shape;

void main() {
    vec4 world = obj.model * vec4(in_pos, 1.0);
    gl_Position = frame.view_proj * world;
    v_local = in_uv;
    v_uv    = obj.uv_rect.xy + in_uv * (obj.uv_rect.zw - obj.uv_rect.xy);
    v_tint  = obj.tint;
    v_shape = obj.params.x;
}
