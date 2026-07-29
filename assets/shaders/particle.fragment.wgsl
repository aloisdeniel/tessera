@group(2) @binding(0) var tex_tx: texture_2d<f32>;
@group(2) @binding(1) var tex_sp: sampler;
var<private> v_uv_1: vec2<f32>;
var<private> frag: vec4<f32>;
var<private> v_color_1: vec4<f32>;

fn main_1() {
    var t: vec4<f32>;

    let _e7 = v_uv_1;
    let _e8 = textureSampleLevel(tex_tx, tex_sp, _e7, 0f);
    t = _e8;
    let _e9 = t;
    let _e10 = v_color_1;
    frag = (_e9 * _e10);
    return;
}

@fragment 
fn main(@location(0) v_uv: vec2<f32>, @location(1) v_color: vec4<f32>) -> @location(0) vec4<f32> {
    v_uv_1 = v_uv;
    v_color_1 = v_color;
    main_1();
    let _e5 = frag;
    return _e5;
}
