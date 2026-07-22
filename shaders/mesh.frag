// mesh.frag — Vulkan GLSL port of assets/shaders/mesh.fragment.msl.
// SDL_GPU SPIR-V: fragment sampled textures at set=2, uniform buffers at set=3.
#version 450

layout(std140, set = 3, binding = 0) uniform FrameUniform {
    mat4 view_proj;
    vec4 light_dir;    // xyz direction the light travels
    vec4 ambient;      // rgb ambient
    vec4 light_color;  // rgb color, a = intensity
    vec4 camera_pos;   // xyz eye position, w unused
} frame;

layout(set = 2, binding = 0) uniform sampler2D tex;

layout(location = 0) in vec3 world_pos;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec2 v_uv;
layout(location = 3) in vec4 v_tint;

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

void main() {
    vec3 n = normalize(v_normal);
    vec3 L = normalize(-frame.light_dir.xyz);
    vec3 V = normalize(frame.camera_pos.xyz - world_pos);

    vec4 texel = texture(tex, v_uv) * v_tint;
    vec3 albedo = srgb_to_linear(texel.rgb);

    float ndl = max(dot(n, L), 0.0);
    float ramp = cel_ramp(ndl);

    vec3 ambient = srgb_to_linear(frame.ambient.rgb);
    vec3 direct = frame.light_color.rgb * frame.light_color.a * ramp;
    vec3 lit = albedo * (ambient + direct);

    float rim = pow(1.0 - max(dot(n, V), 0.0), 4.0);
    lit += rim * 0.12 * frame.light_color.rgb;

    frag = vec4(linear_to_srgb(lit), texel.a);
}
