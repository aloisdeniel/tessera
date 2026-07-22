// card.frag — Vulkan GLSL port of assets/shaders/card.fragment.msl.
// SDL_GPU SPIR-V: fragment sampled textures at set=2, uniform buffers at set=3.
#version 450

layout(std140, set = 3, binding = 0) uniform FrameUniform {
    mat4 view_proj;
    vec4 light_dir;
    vec4 ambient;
    vec4 light_color;
    vec4 camera_pos;
} frame;

layout(std140, set = 3, binding = 1) uniform CardUniform {
    mat4 model;
    vec4 tint;
    vec4 uv_visible;
    vec4 uv_hidden;
    vec4 uv_back;
    vec4 params;    // x = mix (0 visible .. 1 hidden)
} card;

layout(set = 2, binding = 0) uniform sampler2D tex_visible;
layout(set = 2, binding = 1) uniform sampler2D tex_hidden;
layout(set = 2, binding = 2) uniform sampler2D tex_back;

layout(location = 0) in vec3 world_pos;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec2 v_uv;
layout(location = 3) in float v_face;

layout(location = 0) out vec4 frag;

vec3 srgb_to_linear(vec3 c) { return pow(max(c, 0.0), vec3(2.2)); }
vec3 linear_to_srgb(vec3 c) { return pow(max(c, 0.0), vec3(1.0 / 2.2)); }

float cel_ramp(float ndl) {
    const float bands = 3.0;
    float scaled = ndl * bands;
    float lower = floor(scaled);
    float frac = scaled - lower;
    float soft = smoothstep(0.35, 0.65, frac);
    return (lower + soft) / bands;
}

vec2 remap(vec2 uv, vec4 rect) {
    return rect.xy + uv * (rect.zw - rect.xy);
}

void main() {
    vec4 texel;
    if (v_face < 0.5) {
        vec4 vis = texture(tex_visible, remap(v_uv, card.uv_visible));
        vec4 hid = texture(tex_hidden, remap(v_uv, card.uv_hidden));
        texel = mix(vis, hid, clamp(card.params.x, 0.0, 1.0));
    } else if (v_face < 1.5) {
        texel = texture(tex_back, remap(v_uv, card.uv_back));
    } else {
        texel = vec4(1.0);
    }
    texel *= card.tint;

    vec3 n = normalize(v_normal);
    vec3 L = normalize(-frame.light_dir.xyz);
    vec3 V = normalize(frame.camera_pos.xyz - world_pos);
    vec3 albedo = srgb_to_linear(texel.rgb);

    float ndl = max(dot(n, L), 0.0);
    float ramp = cel_ramp(ndl);
    vec3 ambient = srgb_to_linear(frame.ambient.rgb);
    vec3 direct = frame.light_color.rgb * frame.light_color.a * ramp;
    vec3 lit = albedo * (ambient + direct);

    float rim = pow(1.0 - max(dot(n, V), 0.0), 4.0);
    lit += rim * 0.10 * frame.light_color.rgb;

    frag = vec4(linear_to_srgb(lit), texel.a);
}
