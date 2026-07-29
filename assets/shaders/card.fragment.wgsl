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

struct CardUniform {
    model: mat4x4<f32>,
    tint: vec4<f32>,
    uv_visible: vec4<f32>,
    uv_hidden: vec4<f32>,
    uv_back: vec4<f32>,
    params: vec4<f32>,
}

@group(3) @binding(0) var<uniform> frame: FrameUniform;
var<private> v_face_1: f32;
@group(2) @binding(0) var tex_visible_tx: texture_2d<f32>;
@group(2) @binding(1) var tex_visible_sp: sampler;
var<private> v_uv_1: vec2<f32>;
@group(3) @binding(1) var<uniform> card: CardUniform;
@group(2) @binding(2) var tex_hidden_tx: texture_2d<f32>;
@group(2) @binding(3) var tex_hidden_sp: sampler;
@group(2) @binding(4) var tex_back_tx: texture_2d<f32>;
@group(2) @binding(5) var tex_back_sp: sampler;
var<private> v_normal_1: vec3<f32>;
var<private> world_pos_1: vec3<f32>;
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

fn srgb_to_linear_u0028_vf3_u003b(c_1: ptr<function, vec3<f32>>) -> vec3<f32> {
    let _e44 = (*c_1);
    return pow(max(_e44, vec3(0f)), vec3<f32>(2.2f, 2.2f, 2.2f));
}

fn remap_u0028_vf2_u003b_vf4_u003b(uv: ptr<function, vec2<f32>>, rect: ptr<function, vec4<f32>>) -> vec2<f32> {
    let _e45 = (*rect);
    let _e47 = (*uv);
    let _e48 = (*rect);
    let _e50 = (*rect);
    return (_e45.xy + (_e47 * (_e48.zw - _e50.xy)));
}

fn main_1() {
    var vis: vec4<f32>;
    var param_1: vec2<f32>;
    var param_2: vec4<f32>;
    var hid: vec4<f32>;
    var param_3: vec2<f32>;
    var param_4: vec4<f32>;
    var texel: vec4<f32>;
    var param_5: vec2<f32>;
    var param_6: vec4<f32>;
    var n_1: vec3<f32>;
    var L: vec3<f32>;
    var V: vec3<f32>;
    var albedo: vec3<f32>;
    var param_7: vec3<f32>;
    var ndl_2: f32;
    var ramp: f32;
    var param_8: f32;
    var ambient: vec3<f32>;
    var param_9: vec3<f32>;
    var direct: vec3<f32>;
    var point: vec3<f32>;
    var param_10: vec3<f32>;
    var param_11: vec3<f32>;
    var lit: vec3<f32>;
    var rim: f32;
    var param_12: vec3<f32>;

    let _e69 = v_face_1;
    if (_e69 < 0.5f) {
        let _e71 = v_uv_1;
        param_1 = _e71;
        let _e73 = card.uv_visible;
        param_2 = _e73;
        let _e74 = remap_u0028_vf2_u003b_vf4_u003b((&param_1), (&param_2));
        let _e75 = textureSampleLevel(tex_visible_tx, tex_visible_sp, _e74, 0f);
        vis = _e75;
        let _e76 = v_uv_1;
        param_3 = _e76;
        let _e78 = card.uv_hidden;
        param_4 = _e78;
        let _e79 = remap_u0028_vf2_u003b_vf4_u003b((&param_3), (&param_4));
        let _e80 = textureSampleLevel(tex_hidden_tx, tex_hidden_sp, _e79, 0f);
        hid = _e80;
        let _e81 = vis;
        let _e82 = hid;
        let _e85 = card.params[0u];
        texel = mix(_e81, _e82, vec4(clamp(_e85, 0f, 1f)));
    } else {
        let _e89 = v_face_1;
        if (_e89 < 1.5f) {
            let _e91 = v_uv_1;
            param_5 = _e91;
            let _e93 = card.uv_back;
            param_6 = _e93;
            let _e94 = remap_u0028_vf2_u003b_vf4_u003b((&param_5), (&param_6));
            let _e95 = textureSampleLevel(tex_back_tx, tex_back_sp, _e94, 0f);
            texel = _e95;
        } else {
            texel = vec4<f32>(1f, 1f, 1f, 1f);
        }
    }
    let _e97 = card.tint;
    let _e98 = texel;
    texel = (_e98 * _e97);
    let _e100 = v_normal_1;
    n_1 = normalize(_e100);
    let _e103 = frame.light_dir;
    L = normalize(-(_e103.xyz));
    let _e108 = frame.camera_pos;
    let _e110 = world_pos_1;
    V = normalize((_e108.xyz - _e110));
    let _e113 = texel;
    param_7 = _e113.xyz;
    let _e115 = srgb_to_linear_u0028_vf3_u003b((&param_7));
    albedo = _e115;
    let _e116 = n_1;
    let _e117 = L;
    ndl_2 = max(dot(_e116, _e117), 0f);
    let _e120 = ndl_2;
    param_8 = _e120;
    let _e121 = cel_ramp_u0028_f1_u003b((&param_8));
    ramp = _e121;
    let _e123 = frame.ambient;
    param_9 = _e123.xyz;
    let _e125 = srgb_to_linear_u0028_vf3_u003b((&param_9));
    ambient = _e125;
    let _e127 = frame.light_color;
    let _e131 = frame.light_color[3u];
    let _e133 = ramp;
    direct = ((_e127.xyz * _e131) * _e133);
    let _e135 = world_pos_1;
    param_10 = _e135;
    let _e136 = n_1;
    param_11 = _e136;
    let _e137 = point_light_sum_u0028_vf3_u003b_vf3_u003b((&param_10), (&param_11));
    point = _e137;
    let _e138 = albedo;
    let _e139 = ambient;
    let _e140 = direct;
    let _e142 = point;
    lit = (_e138 * ((_e139 + _e140) + _e142));
    let _e145 = n_1;
    let _e146 = V;
    rim = pow((1f - max(dot(_e145, _e146), 0f)), 4f);
    let _e151 = rim;
    let _e154 = frame.light_color;
    let _e157 = lit;
    lit = (_e157 + (_e154.xyz * (_e151 * 0.1f)));
    let _e159 = lit;
    param_12 = _e159;
    let _e160 = linear_to_srgb_u0028_vf3_u003b((&param_12));
    let _e162 = texel[3u];
    frag = vec4<f32>(_e160.x, _e160.y, _e160.z, _e162);
    return;
}

@fragment 
fn main(@location(3) v_face: f32, @location(2) v_uv: vec2<f32>, @location(1) v_normal: vec3<f32>, @location(0) world_pos: vec3<f32>) -> @location(0) vec4<f32> {
    v_face_1 = v_face;
    v_uv_1 = v_uv;
    v_normal_1 = v_normal;
    world_pos_1 = world_pos;
    main_1();
    let _e9 = frag;
    return _e9;
}
