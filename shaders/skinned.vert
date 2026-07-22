// skinned.vert — Vulkan GLSL port of assets/shaders/skinned.vertex.msl.
// Uses the mesh fragment shader. Vertex uniforms at set=1 (frame, obj, joints).
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
    vec4 uv_rect;
} obj;

// Joint palette (skinning matrices = world_joint * inverse_bind), capped at 64.
layout(std140, set = 1, binding = 2) uniform JointUniform {
    mat4 joints[64];
} sk;

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in uvec4 in_joints;
layout(location = 4) in vec4 in_weights;

layout(location = 0) out vec3 world_pos;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec2 v_uv;
layout(location = 3) out vec4 v_tint;

void main() {
    float wsum = in_weights.x + in_weights.y + in_weights.z + in_weights.w;
    mat4 skin;
    if (wsum < 1e-4) {
        skin = mat4(1.0);   // unrigged vertex: identity
    } else {
        skin = sk.joints[in_joints.x] * in_weights.x
             + sk.joints[in_joints.y] * in_weights.y
             + sk.joints[in_joints.z] * in_weights.z
             + sk.joints[in_joints.w] * in_weights.w;
    }

    vec4 spos = skin * vec4(in_pos, 1.0);
    vec3 snorm = (skin * vec4(in_normal, 0.0)).xyz;
    vec4 world = obj.model * spos;

    gl_Position = frame.view_proj * world;
    world_pos = world.xyz;
    v_normal = normalize((obj.model * vec4(snorm, 0.0)).xyz);
    v_uv = obj.uv_rect.xy + in_uv * (obj.uv_rect.zw - obj.uv_rect.xy);
    v_tint = obj.tint;
}
