struct FrameUniform {
    view_proj: mat4x4<f32>,
    light_dir: vec4<f32>,
    ambient: vec4<f32>,
    light_color: vec4<f32>,
    camera_pos: vec4<f32>,
    light_vp: mat4x4<f32>,
    shadow_params: vec4<f32>,
    point_count: vec4<f32>,
    point_pos: array<vec4<f32>, 8>,
    point_color: array<vec4<f32>, 8>,
}

@group(3) @binding(0) var<uniform> frame: FrameUniform;
@group(2) @binding(2) var shadow_map_tx: texture_2d<f32>; //!nofilter
@group(2) @binding(3) var shadow_map_sp: sampler; //!nofilter
var<private> v_normal_1: vec3<f32>;
var<private> world_pos_1: vec3<f32>;
@group(2) @binding(0) var tex_tx: texture_2d<f32>;
@group(2) @binding(1) var tex_sp: sampler;
var<private> v_uv_1: vec2<f32>;
var<private> v_tint_1: vec4<f32>;
var<private> frag: vec4<f32>;

fn linear_to_srgb_u0028_vf3_u003b(c: ptr<function, vec3<f32>>) -> vec3<f32> {
    let _e44 = (*c);
    return pow(max(_e44, vec3(0f)), vec3<f32>(0.45454547f, 0.45454547f, 0.45454547f));
}

fn cel_ramp_u0028_f1_u003b(ndl: ptr<function, f32>) -> f32 {
    var scaled: f32;
    var lower: f32;
    var frac: f32;
    var soft: f32;

    let _e48 = (*ndl);
    scaled = (_e48 * 3f);
    let _e50 = scaled;
    lower = floor(_e50);
    let _e52 = scaled;
    let _e53 = lower;
    frac = (_e52 - _e53);
    let _e55 = frac;
    soft = smoothstep(0.35f, 0.65f, _e55);
    let _e57 = lower;
    let _e58 = soft;
    return ((_e57 + _e58) / 3f);
}

fn point_light_sum_u0028_vf3_u003b_vf3_u003b(wp: ptr<function, vec3<f32>>, n: ptr<function, vec3<f32>>) -> vec3<f32> {
    var sum: vec3<f32>;
    var count: i32;
    var i: i32;
    var toL: vec3<f32>;
    var dist: f32;
    var radius: f32;
    var att: f32;
    var ndl_1: f32;
    var param: f32;

    sum = vec3<f32>(0f, 0f, 0f);
    let _e56 = frame.point_count[0u];
    count = i32(min(_e56, 8f));
    i = 0i;
    loop {
        let _e59 = i;
        let _e60 = count;
        if (_e59 < _e60) {
            let _e62 = i;
            let _e65 = frame.point_pos[_e62];
            let _e67 = (*wp);
            toL = (_e65.xyz - _e67);
            let _e69 = toL;
            dist = length(_e69);
            let _e71 = i;
            let _e75 = frame.point_pos[_e71][3u];
            radius = max(_e75, 0.001f);
            let _e77 = dist;
            let _e78 = radius;
            if (_e77 >= _e78) {
                continue;
            }
            let _e80 = dist;
            let _e81 = radius;
            att = (1f - (_e80 / _e81));
            let _e84 = att;
            let _e85 = att;
            att = (_e85 * _e84);
            let _e87 = (*n);
            let _e88 = toL;
            let _e89 = dist;
            ndl_1 = max(dot(_e87, (_e88 / vec3(max(_e89, 0.0001f)))), 0f);
            let _e95 = i;
            let _e98 = frame.point_color[_e95];
            let _e100 = i;
            let _e104 = frame.point_color[_e100][3u];
            let _e106 = ndl_1;
            param = _e106;
            let _e107 = cel_ramp_u0028_f1_u003b((&param));
            let _e109 = att;
            let _e111 = sum;
            sum = (_e111 + (((_e98.xyz * _e104) * _e107) * _e109));
            continue;
        } else {
            break;
        }
        continuing {
            let _e113 = i;
            i = (_e113 + 1i);
        }
    }
    let _e115 = sum;
    return _e115;
}

fn shadow_visibility_u0028_vf3_u003b_f1_u003b(wp_1: ptr<function, vec3<f32>>, ndl_2: ptr<function, f32>) -> f32 {
    var lp: vec4<f32>;
    var p: vec3<f32>;
    var uv: vec2<f32>;
    var bias: f32;
    var texel: f32;
    var ref_: f32;
    var lit: f32;
    var dy: i32;
    var dx: i32;
    var d: f32;
    var phi_219_: bool;
    var phi_226_: bool;
    var phi_233_: bool;
    var phi_241_: bool;
    var phi_248_: bool;

    let _e57 = frame.shadow_params[0u];
    if (_e57 < 0.5f) {
        return 1f;
    }
    let _e60 = frame.light_vp;
    let _e61 = (*wp_1);
    lp = (_e60 * vec4<f32>(_e61.x, _e61.y, _e61.z, 1f));
    let _e67 = lp;
    let _e70 = lp[3u];
    p = (_e67.xyz / vec3(_e70));
    let _e74 = p[0u];
    let _e78 = p[1u];
    uv = vec2<f32>((0.5f + (0.5f * _e74)), (0.5f - (0.5f * _e78)));
    let _e83 = uv[0u];
    let _e84 = (_e83 <= 0f);
    phi_219_ = _e84;
    if !(_e84) {
        let _e87 = uv[0u];
        phi_219_ = (_e87 >= 1f);
    }
    let _e90 = phi_219_;
    phi_226_ = _e90;
    if !(_e90) {
        let _e93 = uv[1u];
        phi_226_ = (_e93 <= 0f);
    }
    let _e96 = phi_226_;
    phi_233_ = _e96;
    if !(_e96) {
        let _e99 = uv[1u];
        phi_233_ = (_e99 >= 1f);
    }
    let _e102 = phi_233_;
    phi_241_ = _e102;
    if !(_e102) {
        let _e105 = p[2u];
        phi_241_ = (_e105 <= 0f);
    }
    let _e108 = phi_241_;
    phi_248_ = _e108;
    if !(_e108) {
        let _e111 = p[2u];
        phi_248_ = (_e111 >= 1f);
    }
    let _e114 = phi_248_;
    if _e114 {
        return 1f;
    }
    let _e117 = frame.shadow_params[2u];
    let _e120 = frame.shadow_params[3u];
    let _e121 = (*ndl_2);
    bias = max(_e117, (_e120 * (1f - _e121)));
    let _e127 = frame.shadow_params[1u];
    texel = _e127;
    let _e129 = p[2u];
    let _e130 = bias;
    ref_ = (_e129 - _e130);
    lit = 0f;
    dy = -1i;
    loop {
        let _e132 = dy;
        if (_e132 <= 1i) {
            dx = -1i;
            loop {
                let _e134 = dx;
                if (_e134 <= 1i) {
                    let _e136 = uv;
                    let _e137 = dx;
                    let _e139 = dy;
                    let _e142 = texel;
                    let _e145 = textureSampleLevel(shadow_map_tx, shadow_map_sp, (_e136 + (vec2<f32>(f32(_e137), f32(_e139)) * _e142)), 0f);
                    d = _e145.x;
                    let _e147 = ref_;
                    let _e148 = d;
                    let _e151 = lit;
                    lit = (_e151 + select(0f, 1f, (_e147 <= _e148)));
                    continue;
                } else {
                    break;
                }
                continuing {
                    let _e153 = dx;
                    dx = (_e153 + 1i);
                }
            }
            continue;
        } else {
            break;
        }
        continuing {
            let _e155 = dy;
            dy = (_e155 + 1i);
        }
    }
    let _e157 = lit;
    return (_e157 / 9f);
}

fn srgb_to_linear_u0028_vf3_u003b(c_1: ptr<function, vec3<f32>>) -> vec3<f32> {
    let _e44 = (*c_1);
    return pow(max(_e44, vec3(0f)), vec3<f32>(2.2f, 2.2f, 2.2f));
}

fn main_1() {
    var n_1: vec3<f32>;
    var L: vec3<f32>;
    var V: vec3<f32>;
    var texel_1: vec4<f32>;
    var albedo: vec3<f32>;
    var param_1: vec3<f32>;
    var ndl_3: f32;
    var ramp: f32;
    var param_2: f32;
    var vis: f32;
    var param_3: vec3<f32>;
    var param_4: f32;
    var ambient: vec3<f32>;
    var param_5: vec3<f32>;
    var direct: vec3<f32>;
    var point: vec3<f32>;
    var param_6: vec3<f32>;
    var param_7: vec3<f32>;
    var lit_1: vec3<f32>;
    var rim: f32;
    var param_8: vec3<f32>;

    let _e64 = v_normal_1;
    n_1 = normalize(_e64);
    let _e67 = frame.light_dir;
    L = normalize(-(_e67.xyz));
    let _e72 = frame.camera_pos;
    let _e74 = world_pos_1;
    V = normalize((_e72.xyz - _e74));
    let _e77 = v_uv_1;
    let _e78 = textureSampleLevel(tex_tx, tex_sp, _e77, 0f);
    let _e79 = v_tint_1;
    texel_1 = (_e78 * _e79);
    let _e81 = texel_1;
    param_1 = _e81.xyz;
    let _e83 = srgb_to_linear_u0028_vf3_u003b((&param_1));
    albedo = _e83;
    let _e84 = n_1;
    let _e85 = L;
    ndl_3 = max(dot(_e84, _e85), 0f);
    let _e88 = ndl_3;
    param_2 = _e88;
    let _e89 = cel_ramp_u0028_f1_u003b((&param_2));
    ramp = _e89;
    let _e90 = world_pos_1;
    param_3 = _e90;
    let _e91 = ndl_3;
    param_4 = _e91;
    let _e92 = shadow_visibility_u0028_vf3_u003b_f1_u003b((&param_3), (&param_4));
    vis = _e92;
    let _e94 = frame.ambient;
    param_5 = _e94.xyz;
    let _e96 = srgb_to_linear_u0028_vf3_u003b((&param_5));
    ambient = _e96;
    let _e98 = frame.light_color;
    let _e102 = frame.light_color[3u];
    let _e104 = ramp;
    let _e106 = vis;
    direct = (((_e98.xyz * _e102) * _e104) * _e106);
    let _e108 = world_pos_1;
    param_6 = _e108;
    let _e109 = n_1;
    param_7 = _e109;
    let _e110 = point_light_sum_u0028_vf3_u003b_vf3_u003b((&param_6), (&param_7));
    point = _e110;
    let _e111 = albedo;
    let _e112 = ambient;
    let _e113 = direct;
    let _e115 = point;
    lit_1 = (_e111 * ((_e112 + _e113) + _e115));
    let _e118 = n_1;
    let _e119 = V;
    rim = pow((1f - max(dot(_e118, _e119), 0f)), 4f);
    let _e124 = rim;
    let _e127 = frame.light_color;
    let _e130 = lit_1;
    lit_1 = (_e130 + (_e127.xyz * (_e124 * 0.12f)));
    let _e132 = lit_1;
    param_8 = _e132;
    let _e133 = linear_to_srgb_u0028_vf3_u003b((&param_8));
    let _e135 = texel_1[3u];
    frag = vec4<f32>(_e133.x, _e133.y, _e133.z, _e135);
    return;
}

@fragment 
fn main(@location(1) v_normal: vec3<f32>, @location(0) world_pos: vec3<f32>, @location(2) v_uv: vec2<f32>, @location(3) v_tint: vec4<f32>) -> @location(0) vec4<f32> {
    v_normal_1 = v_normal;
    world_pos_1 = world_pos;
    v_uv_1 = v_uv;
    v_tint_1 = v_tint;
    main_1();
    let _e9 = frag;
    return _e9;
}
