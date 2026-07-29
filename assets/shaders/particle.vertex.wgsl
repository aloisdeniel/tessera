struct gl_PerVertex {
    @builtin(position) gl_Position: vec4<f32>,
    gl_PointSize: f32,
    gl_ClipDistance: array<f32, 1>,
    gl_CullDistance: array<f32, 1>,
}

struct FrameUniform {
    view_proj: mat4x4<f32>,
    light_dir: vec4<f32>,
    ambient: vec4<f32>,
    light_color: vec4<f32>,
    camera_pos: vec4<f32>,
}

struct VertexOutput {
    @builtin(position) gl_Position: vec4<f32>,
    @location(0) member: vec2<f32>,
    @location(1) member_1: vec4<f32>,
}

var<private> unnamed: gl_PerVertex = gl_PerVertex(vec4<f32>(0f, 0f, 0f, 1f), 1f, array<f32, 1>(), array<f32, 1>());
@group(1) @binding(0) var<uniform> frame: FrameUniform;
var<private> in_pos_1: vec3<f32>;
var<private> v_uv: vec2<f32>;
var<private> in_uv_1: vec2<f32>;
var<private> v_color: vec4<f32>;
var<private> in_color_1: vec4<f32>;

fn main_1() {
    let _e11 = frame.view_proj;
    let _e12 = in_pos_1;
    unnamed.gl_Position = (_e11 * vec4<f32>(_e12.x, _e12.y, _e12.z, 1f));
    let _e19 = in_uv_1;
    v_uv = _e19;
    let _e20 = in_color_1;
    v_color = _e20;
    return;
}

@vertex 
fn main(@location(0) in_pos: vec3<f32>, @location(1) in_uv: vec2<f32>, @location(2) in_color: vec4<f32>) -> VertexOutput {
    in_pos_1 = in_pos;
    in_uv_1 = in_uv;
    in_color_1 = in_color;
    main_1();
    let _e10 = unnamed.gl_Position;
    let _e11 = v_uv;
    let _e12 = v_color;
    return VertexOutput(_e10, _e11, _e12);
}
