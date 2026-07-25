// text.vert — Vulkan GLSL port of assets/shaders/text.vertex.msl.
#version 450

layout(std140, set = 1, binding = 0) uniform FrameUniform {
    mat4 view_proj;
    vec4 light_dir;
    vec4 ambient;
    vec4 light_color;
    vec4 camera_pos;
} frame;

layout(std140, set = 1, binding = 1) uniform TextUniform {
    mat4 model;     // maps the unit quad onto one glyph rect in world space
    vec4 tint;      // label color; a already includes fade + crossfade
    vec4 uv_rect;   // glyph sub-rect of the font atlas (u0,v0,u1,v1)
} obj;

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;   // unused, kept for a shared vertex layout
layout(location = 2) in vec2 in_uv;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_tint;

void main() {
    vec4 world = obj.model * vec4(in_pos, 1.0);
    gl_Position = frame.view_proj * world;
    v_uv   = obj.uv_rect.xy + in_uv * (obj.uv_rect.zw - obj.uv_rect.xy);
    v_tint = obj.tint;
}
