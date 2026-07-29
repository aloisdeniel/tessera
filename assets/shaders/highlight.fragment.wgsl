struct HighlightUniform {
    color: vec4<f32>,
    params: vec4<f32>,
    params2_: vec4<f32>,
}

@group(3) @binding(0) var<uniform> u: HighlightUniform;
@group(2) @binding(0) var maskTex_tx: texture_2d<f32>;
@group(2) @binding(1) var maskTex_sp: sampler;
var<private> v_uv_1: vec2<f32>;
var<private> frag: vec4<f32>;

fn main_1() {
    var texel: vec2<f32>;
    var r: f32;
    var center: f32;
    var m: f32;
    var i: i32;
    var a: f32;
    var dir: vec2<f32>;
    var alpha: f32;
    var sum: f32;
    var total: f32;
    var i_1: i32;
    var t: f32;
    var a_1: f32;
    var off: vec2<f32>;
    var wgt: f32;

    let _e40 = u.params[1u];
    let _e43 = u.params[2u];
    texel = vec2<f32>(_e40, _e43);
    let _e47 = u.params[0u];
    r = max(_e47, 1f);
    let _e49 = v_uv_1;
    let _e50 = textureSampleLevel(maskTex_tx, maskTex_sp, _e49, 0f);
    center = _e50.x;
    let _e54 = u.params2_[0u];
    if (_e54 < 0.5f) {
        let _e56 = center;
        m = _e56;
        i = 0i;
        loop {
            let _e57 = i;
            if (_e57 < 16i) {
                let _e59 = i;
                a = (6.2831855f * (f32(_e59) / 16f));
                let _e63 = a;
                let _e65 = a;
                dir = vec2<f32>(cos(_e63), sin(_e65));
                let _e68 = m;
                let _e69 = v_uv_1;
                let _e70 = dir;
                let _e71 = r;
                let _e73 = texel;
                let _e76 = textureSampleLevel(maskTex_tx, maskTex_sp, (_e69 + ((_e70 * _e71) * _e73)), 0f);
                m = max(_e68, _e76.x);
                let _e79 = m;
                let _e80 = v_uv_1;
                let _e81 = dir;
                let _e82 = r;
                let _e85 = texel;
                let _e88 = textureSampleLevel(maskTex_tx, maskTex_sp, (_e80 + ((_e81 * (0.5f * _e82)) * _e85)), 0f);
                m = max(_e79, _e88.x);
                continue;
            } else {
                break;
            }
            continuing {
                let _e91 = i;
                i = (_e91 + 1i);
            }
        }
        let _e93 = m;
        let _e94 = center;
        alpha = clamp((_e93 - _e94), 0f, 1f);
    } else {
        let _e97 = center;
        sum = _e97;
        total = 1f;
        i_1 = 0i;
        loop {
            let _e98 = i_1;
            if (_e98 < 24i) {
                let _e100 = i_1;
                t = ((f32(_e100) + 0.5f) / 24f);
                let _e104 = i_1;
                a_1 = (2.3999631f * f32(_e104));
                let _e107 = a_1;
                let _e109 = a_1;
                let _e112 = r;
                let _e114 = t;
                let _e118 = texel;
                off = ((vec2<f32>(cos(_e107), sin(_e109)) * ((2f * _e112) * sqrt(_e114))) * _e118);
                let _e120 = t;
                wgt = (1f - (0.7f * sqrt(_e120)));
                let _e124 = v_uv_1;
                let _e125 = off;
                let _e127 = textureSampleLevel(maskTex_tx, maskTex_sp, (_e124 + _e125), 0f);
                let _e129 = wgt;
                let _e131 = sum;
                sum = (_e131 + (_e127.x * _e129));
                let _e133 = wgt;
                let _e134 = total;
                total = (_e134 + _e133);
                continue;
            } else {
                break;
            }
            continuing {
                let _e136 = i_1;
                i_1 = (_e136 + 1i);
            }
        }
        let _e138 = sum;
        let _e139 = total;
        alpha = (_e138 / _e139);
    }
    let _e142 = u.color;
    let _e143 = _e142.xyz;
    let _e146 = u.color[3u];
    let _e149 = u.params[3u];
    let _e151 = alpha;
    frag = vec4<f32>(_e143.x, _e143.y, _e143.z, ((_e146 * _e149) * _e151));
    return;
}

@fragment 
fn main(@location(0) v_uv: vec2<f32>) -> @location(0) vec4<f32> {
    v_uv_1 = v_uv;
    main_1();
    let _e3 = frag;
    return _e3;
}
