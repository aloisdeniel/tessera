var<private> frag: vec4<f32>;

fn main_1() {
    frag = vec4<f32>(1f, 1f, 1f, 1f);
    return;
}

@fragment 
fn main() -> @location(0) vec4<f32> {
    main_1();
    let _e1 = frag;
    return _e1;
}
