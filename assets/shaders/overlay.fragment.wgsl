var<private> v_shape_1: f32;
var<private> frag: vec4<f32>;
@group(2) @binding(0) var tex_tx: texture_2d<f32>;
@group(2) @binding(1) var tex_sp: sampler;
var<private> v_uv_1: vec2<f32>;
var<private> v_tint_1: vec4<f32>;
var<private> v_local_1: vec2<f32>;

fn main_1() {
    var d: vec2<f32>;
    var r: f32;
    var a: f32;

    let _e22 = v_shape_1;
    if (_e22 < 0.5f) {
        let _e24 = v_uv_1;
        let _e25 = textureSampleLevel(tex_tx, tex_sp, _e24, 0f);
        let _e26 = v_tint_1;
        frag = (_e25 * _e26);
        return;
    }
    let _e28 = v_local_1;
    d = (_e28 - vec2<f32>(0.5f, 0.5f));
    let _e30 = d;
    r = (length(_e30) * 2f);
    let _e33 = v_shape_1;
    if (_e33 < 1.5f) {
        let _e35 = r;
        a = smoothstep(0.92f, 0.8f, _e35);
    } else {
        let _e37 = r;
        let _e39 = r;
        a = (smoothstep(0.98f, 0.9f, _e37) * smoothstep(0.64f, 0.74f, _e39));
    }
    let _e42 = v_tint_1;
    let _e43 = _e42.xyz;
    let _e45 = v_tint_1[3u];
    let _e46 = a;
    frag = vec4<f32>(_e43.x, _e43.y, _e43.z, (_e45 * _e46));
    return;
}

@fragment 
fn main(@location(3) v_shape: f32, @location(0) v_uv: vec2<f32>, @location(2) v_tint: vec4<f32>, @location(1) v_local: vec2<f32>) -> @location(0) vec4<f32> {
    v_shape_1 = v_shape;
    v_uv_1 = v_uv;
    v_tint_1 = v_tint;
    v_local_1 = v_local;
    main_1();
    let _e9 = frag;
    return _e9;
}
