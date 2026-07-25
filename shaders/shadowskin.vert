// shadowskin.vert — Vulkan GLSL port of assets/shaders/shadowskin.vertex.msl.
// Depth-only shadow-map pass for GPU-skinned meshes (palette in binding 2).
#version 450

layout(std140, set = 1, binding = 0) uniform ShadowFrameUniform {
    mat4 light_vp;
} frame;

layout(std140, set = 1, binding = 1) uniform ObjectUniform {
    mat4 model;
    vec4 tint;
    vec4 uv_rect;
} obj;

layout(std140, set = 1, binding = 2) uniform JointUniform {
    mat4 joints[64];
} sk;

layout(location = 0) in vec3 in_pos;
layout(location = 3) in uvec4 in_joints;
layout(location = 4) in vec4 in_weights;

void main() {
    float wsum = in_weights.x + in_weights.y + in_weights.z + in_weights.w;
    mat4 skin;
    if (wsum < 1e-4) {
        skin = mat4(1.0);
    } else {
        skin = sk.joints[in_joints.x] * in_weights.x
             + sk.joints[in_joints.y] * in_weights.y
             + sk.joints[in_joints.z] * in_weights.z
             + sk.joints[in_joints.w] * in_weights.w;
    }
    gl_Position = frame.light_vp * (obj.model * (skin * vec4(in_pos, 1.0)));
}
