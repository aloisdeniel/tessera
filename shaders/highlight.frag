// highlight.frag — Vulkan GLSL port of assets/shaders/highlight.fragment.msl.
// Fragment: sampled texture at set=2 (the silhouette mask), uniforms at set=3.
#version 450

layout(std140, set = 3, binding = 0) uniform HighlightUniform {
    vec4 color;    // rgb outline/glow color, a = base opacity (fade)
    vec4 params;   // x thickness px, y texel_x, z texel_y, w intensity
    vec4 params2;  // x style: 0 = crisp outline, 1 = soft additive glow
} u;

layout(set = 2, binding = 0) uniform sampler2D maskTex;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 frag;

void main() {
    vec2 texel = vec2(u.params.y, u.params.z);
    float r = max(u.params.x, 1.0);
    float center = texture(maskTex, v_uv).r;
    float alpha;

    if (u.params2.x < 0.5) {
        // OUTLINE: max over two rings of taps dilates the silhouette by r
        // pixels; subtracting the center keeps only the band outside the
        // object, so the rim hugs the shape and the interior stays untouched.
        const int N = 16;
        float m = center;
        for (int i = 0; i < N; ++i) {
            float a = (2.0 * 3.14159265) * (float(i) / float(N));
            vec2 dir = vec2(cos(a), sin(a));
            m = max(m, texture(maskTex, v_uv + dir * r * texel).r);
            m = max(m, texture(maskTex, v_uv + dir * (0.5 * r) * texel).r);
        }
        alpha = clamp(m - center, 0.0, 1.0);
    } else {
        // GLOW: golden-angle spiral disc blur over 2r pixels — brightest on
        // the object, falling off into a halo around it.
        const int N = 24;
        const float golden = 2.39996323;
        float sum = center;
        float total = 1.0;
        for (int i = 0; i < N; ++i) {
            float t = (float(i) + 0.5) / float(N);
            float a = golden * float(i);
            vec2 off = vec2(cos(a), sin(a)) * (2.0 * r * sqrt(t)) * texel;
            float wgt = 1.0 - 0.7 * sqrt(t);
            sum += texture(maskTex, v_uv + off).r * wgt;
            total += wgt;
        }
        alpha = sum / total;
    }
    frag = vec4(u.color.rgb, u.color.a * u.params.w * alpha);
}
