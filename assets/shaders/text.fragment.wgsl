@group(2) @binding(0) var tex_tx: texture_2d<f32>;
@group(2) @binding(1) var tex_sp: sampler;
var<private> v_uv_1: vec2<f32>;
var<private> frag: vec4<f32>;
var<private> v_tint_1: vec4<f32>;

fn main_1() {
    var s: vec4<f32>;

    let _e8 = v_uv_1;
    let _e9 = textureSampleLevel(tex_tx, tex_sp, _e8, 0f);
    s = _e9;
    let _e10 = v_tint_1;
    let _e12 = s;
    let _e14 = (_e10.xyz * _e12.xyz);
    let _e16 = v_tint_1[3u];
    let _e18 = s[3u];
    frag = vec4<f32>(_e14.x, _e14.y, _e14.z, (_e16 * _e18));
    return;
}

@fragment 
fn main(@location(0) v_uv: vec2<f32>, @location(1) v_tint: vec4<f32>) -> @location(0) vec4<f32> {
    v_uv_1 = v_uv;
    v_tint_1 = v_tint;
    main_1();
    let _e5 = frag;
    return _e5;
}
