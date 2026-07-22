// dof.vert — Vulkan GLSL port of assets/shaders/dof.vertex.msl.
// Fullscreen triangle from the vertex id — no vertex buffer bound.
#version 450

layout(location = 0) out vec2 v_uv;

void main() {
    uint vid = uint(gl_VertexIndex);
    vec2 p = vec2(float((vid << 1) & 2u), float(vid & 2u));  // (0,0) (2,0) (0,2)
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);             // covers [-1,3]
    v_uv = vec2(p.x, 1.0 - p.y);                             // texture origin top-left
}
