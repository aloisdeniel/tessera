struct OverlayUniform {
    model: mat4x4<f32>,
    tint: vec4<f32>,
    uv_rect: vec4<f32>,
    params: vec4<f32>,
}

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
    @location(1) member: vec2<f32>,
    @location(0) member_1: vec2<f32>,
    @location(2) member_2: vec4<f32>,
    @location(3) member_3: f32,
}

@group(1) @binding(1) var<uniform> obj: OverlayUniform;
var<private> in_pos_1: vec3<f32>;
var<private> unnamed: gl_PerVertex = gl_PerVertex(vec4<f32>(0f, 0f, 0f, 1f), 1f, array<f32, 1>(), array<f32, 1>());
@group(1) @binding(0) var<uniform> frame: FrameUniform;
var<private> v_local: vec2<f32>;
var<private> in_uv_1: vec2<f32>;
var<private> v_uv: vec2<f32>;
var<private> v_tint: vec4<f32>;
var<private> v_shape: f32;
var<private> in_normal_1: vec3<f32>;

fn main_1() {
    var world: vec4<f32>;

    let _e19 = obj.model;
    let _e20 = in_pos_1;
    world = (_e19 * vec4<f32>(_e20.x, _e20.y, _e20.z, 1f));
    let _e27 = frame.view_proj;
    let _e28 = world;
    unnamed.gl_Position = (_e27 * _e28);
    let _e31 = in_uv_1;
    v_local = _e31;
    let _e33 = obj.uv_rect;
    let _e35 = in_uv_1;
    let _e37 = obj.uv_rect;
    let _e40 = obj.uv_rect;
    v_uv = (_e33.xy + (_e35 * (_e37.zw - _e40.xy)));
    let _e46 = obj.tint;
    v_tint = _e46;
    let _e49 = obj.params[0u];
    v_shape = _e49;
    return;
}

@vertex 
fn main(@location(0) in_pos: vec3<f32>, @location(2) in_uv: vec2<f32>, @location(1) in_normal: vec3<f32>) -> VertexOutput {
    in_pos_1 = in_pos;
    in_uv_1 = in_uv;
    in_normal_1 = in_normal;
    main_1();
    let _e12 = unnamed.gl_Position;
    let _e13 = v_local;
    let _e14 = v_uv;
    let _e15 = v_tint;
    let _e16 = v_shape;
    return VertexOutput(_e12, _e13, _e14, _e15, _e16);
}
