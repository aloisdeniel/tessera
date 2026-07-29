struct gl_PerVertex {
    @builtin(position) gl_Position: vec4<f32>,
    gl_PointSize: f32,
    gl_ClipDistance: array<f32, 1>,
    gl_CullDistance: array<f32, 1>,
}

struct VertexOutput {
    @builtin(position) gl_Position: vec4<f32>,
    @location(0) member: vec2<f32>,
}

var<private> gl_VertexIndex_1: i32;
var<private> unnamed: gl_PerVertex = gl_PerVertex(vec4<f32>(0f, 0f, 0f, 1f), 1f, array<f32, 1>(), array<f32, 1>());
var<private> v_uv: vec2<f32>;

fn main_1() {
    var vid: u32;
    var p: vec2<f32>;

    let _e13 = gl_VertexIndex_1;
    vid = bitcast<u32>(_e13);
    let _e15 = vid;
    let _e20 = vid;
    p = vec2<f32>(f32(((_e15 << bitcast<u32>(1i)) & 2u)), f32((_e20 & 2u)));
    let _e24 = p;
    let _e27 = ((_e24 * 2f) - vec2(1f));
    unnamed.gl_Position = vec4<f32>(_e27.x, _e27.y, 0f, 1f);
    let _e33 = p[0u];
    let _e35 = p[1u];
    v_uv = vec2<f32>(_e33, (1f - _e35));
    return;
}

@vertex 
fn main(@builtin(vertex_index) gl_VertexIndex: u32) -> VertexOutput {
    gl_VertexIndex_1 = i32(gl_VertexIndex);
    main_1();
    let _e6 = unnamed.gl_Position;
    let _e7 = v_uv;
    return VertexOutput(_e6, _e7);
}
