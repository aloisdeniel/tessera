// dof.frag — Vulkan GLSL port of assets/shaders/dof.fragment.msl.
// Fragment: sampled textures at set=2 (scene, depth), uniform buffer at set=3.
#version 450

layout(std140, set = 3, binding = 0) uniform DofUniform {
    float znear, zfar;        // camera clip planes (for linear-depth reconstruct)
    float focus_dist;         // world distance to the sharp plane
    float focus_range;        // half-depth kept fully sharp
    float blur_px;            // max blur radius (pixels) at full CoC
    float texel_x, texel_y;   // 1/width, 1/height
    float ortho;              // 1 = orthographic depth (linear), 0 = perspective
} u;

layout(set = 2, binding = 0) uniform sampler2D scene;
layout(set = 2, binding = 1) uniform sampler2D depthTex;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 frag;

// Reconstruct eye-space distance from a 0..1 depth (SDL_GPU / *_rh_zo).
float linear_depth(float d, float n, float f, float ortho) {
    if (ortho > 0.5) return n + d * (f - n);   // ortho depth is already linear
    return (n * f) / (f + d * (n - f));
}

void main() {
    vec3 col = texture(scene, v_uv).rgb;

    float d = texture(depthTex, v_uv).r;
    float lz = linear_depth(d, u.znear, u.zfar, u.ortho);

    float dist = abs(lz - u.focus_dist);
    float coc = clamp((dist - u.focus_range) / max(u.focus_range, 0.5), 0.0, 1.0);
    float radius = coc * u.blur_px;
    if (radius < 0.75) { frag = vec4(col, 1.0); return; }

    // 22-tap golden-angle spiral disc blur, radius scaled by CoC.
    const int N = 22;
    const float golden = 2.39996323;
    vec3 sum = col;
    float total = 1.0;
    for (int i = 0; i < N; ++i) {
        float a = golden * float(i);
        float r = radius * sqrt((float(i) + 0.5) / float(N));
        vec2 off = vec2(cos(a), sin(a)) * r * vec2(u.texel_x, u.texel_y);
        sum += texture(scene, v_uv + off).rgb;
        total += 1.0;
    }
    frag = vec4(sum / total, 1.0);
}
