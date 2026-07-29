struct DofUniform {
    znear: f32,
    zfar: f32,
    focus_dist: f32,
    focus_range: f32,
    blur_px: f32,
    texel_x: f32,
    texel_y: f32,
    ortho: f32,
}

@group(2) @binding(0) var scene_tx: texture_2d<f32>;
@group(2) @binding(1) var scene_sp: sampler;
var<private> v_uv_1: vec2<f32>;
@group(2) @binding(2) var depthTex_tx: texture_2d<f32>; //!nofilter
@group(2) @binding(3) var depthTex_sp: sampler; //!nofilter
@group(3) @binding(0) var<uniform> u: DofUniform;
var<private> frag: vec4<f32>;

fn linear_depth_u0028_f1_u003b_f1_u003b_f1_u003b_f1_u003b(d: ptr<function, f32>, n: ptr<function, f32>, f: ptr<function, f32>, ortho: ptr<function, f32>) -> f32 {
    let _e27 = (*ortho);
    if (_e27 > 0.5f) {
        let _e29 = (*n);
        let _e30 = (*d);
        let _e31 = (*f);
        let _e32 = (*n);
        return (_e29 + (_e30 * (_e31 - _e32)));
    }
    let _e36 = (*n);
    let _e37 = (*f);
    let _e39 = (*f);
    let _e40 = (*d);
    let _e41 = (*n);
    let _e42 = (*f);
    return ((_e36 * _e37) / (_e39 + (_e40 * (_e41 - _e42))));
}

fn main_1() {
    var col: vec3<f32>;
    var d_1: f32;
    var lz: f32;
    var param: f32;
    var param_1: f32;
    var param_2: f32;
    var param_3: f32;
    var dist: f32;
    var coc: f32;
    var radius: f32;
    var sum: vec3<f32>;
    var total: f32;
    var i: i32;
    var a: f32;
    var r: f32;
    var off: vec2<f32>;

    let _e39 = v_uv_1;
    let _e40 = textureSampleLevel(scene_tx, scene_sp, _e39, 0f);
    col = _e40.xyz;
    let _e42 = v_uv_1;
    let _e43 = textureSampleLevel(depthTex_tx, depthTex_sp, _e42, 0f);
    d_1 = _e43.x;
    let _e45 = d_1;
    param = _e45;
    let _e47 = u.znear;
    param_1 = _e47;
    let _e49 = u.zfar;
    param_2 = _e49;
    let _e51 = u.ortho;
    param_3 = _e51;
    let _e52 = linear_depth_u0028_f1_u003b_f1_u003b_f1_u003b_f1_u003b((&param), (&param_1), (&param_2), (&param_3));
    lz = _e52;
    let _e53 = lz;
    let _e55 = u.focus_dist;
    dist = abs((_e53 - _e55));
    let _e58 = dist;
    let _e60 = u.focus_range;
    let _e63 = u.focus_range;
    coc = clamp(((_e58 - _e60) / max(_e63, 0.5f)), 0f, 1f);
    let _e67 = coc;
    let _e69 = u.blur_px;
    radius = (_e67 * _e69);
    let _e71 = radius;
    if (_e71 < 0.75f) {
        let _e73 = col;
        frag = vec4<f32>(_e73.x, _e73.y, _e73.z, 1f);
        return;
    }
    let _e78 = col;
    sum = _e78;
    total = 1f;
    i = 0i;
    loop {
        let _e79 = i;
        if (_e79 < 22i) {
            let _e81 = i;
            a = (2.3999631f * f32(_e81));
            let _e84 = radius;
            let _e85 = i;
            r = (_e84 * sqrt(((f32(_e85) + 0.5f) / 22f)));
            let _e91 = a;
            let _e93 = a;
            let _e96 = r;
            let _e99 = u.texel_x;
            let _e101 = u.texel_y;
            off = ((vec2<f32>(cos(_e91), sin(_e93)) * _e96) * vec2<f32>(_e99, _e101));
            let _e104 = v_uv_1;
            let _e105 = off;
            let _e107 = textureSampleLevel(scene_tx, scene_sp, (_e104 + _e105), 0f);
            let _e109 = sum;
            sum = (_e109 + _e107.xyz);
            let _e111 = total;
            total = (_e111 + 1f);
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
    let _e116 = total;
    let _e118 = (_e115 / vec3(_e116));
    frag = vec4<f32>(_e118.x, _e118.y, _e118.z, 1f);
    return;
}

@fragment 
fn main(@location(0) v_uv: vec2<f32>) -> @location(0) vec4<f32> {
    v_uv_1 = v_uv;
    main_1();
    let _e3 = frag;
    return _e3;
}
