var<private> v_uv_1: vec2<f32>;
var<private> frag: vec4<f32>;
var<private> v_opacity_1: f32;

fn main_1() {
    var d: vec2<f32>;
    var r: f32;
    var a: f32;

    let _e12 = v_uv_1;
    d = (_e12 - vec2<f32>(0.5f, 0.5f));
    let _e14 = d;
    r = (length(_e14) * 2f);
    let _e17 = r;
    a = smoothstep(1f, 0.3f, _e17);
    let _e19 = a;
    let _e20 = v_opacity_1;
    frag = vec4<f32>(0f, 0f, 0f, (_e19 * _e20));
    return;
}

@fragment 
fn main(@location(0) v_uv: vec2<f32>, @location(1) v_opacity: f32) -> @location(0) vec4<f32> {
    v_uv_1 = v_uv;
    v_opacity_1 = v_opacity;
    main_1();
    let _e5 = frag;
    return _e5;
}
