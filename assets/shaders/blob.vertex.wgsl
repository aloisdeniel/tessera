struct ObjectUniform {
    model: mat4x4<f32>,
    tint: vec4<f32>,
    uv_rect: vec4<f32>,
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
    @location(0) member: vec2<f32>,
    @location(1) member_1: f32,
}

@group(1) @binding(1) var<uniform> obj: ObjectUniform;
var<private> in_pos_1: vec3<f32>;
var<private> unnamed: gl_PerVertex = gl_PerVertex(vec4<f32>(0f, 0f, 0f, 1f), 1f, array<f32, 1>(), array<f32, 1>());
@group(1) @binding(0) var<uniform> frame: FrameUniform;
var<private> v_uv: vec2<f32>;
var<private> in_uv_1: vec2<f32>;
var<private> v_opacity: f32;
var<private> in_normal_1: vec3<f32>;

fn main_1() {
    var world: vec4<f32>;

    let _e15 = obj.model;
    let _e16 = in_pos_1;
    world = (_e15 * vec4<f32>(_e16.x, _e16.y, _e16.z, 1f));
    let _e23 = frame.view_proj;
    let _e24 = world;
    unnamed.gl_Position = (_e23 * _e24);
    let _e27 = in_uv_1;
    v_uv = _e27;
    let _e30 = obj.tint[3u];
    v_opacity = _e30;
    return;
}

@vertex 
fn main(@location(0) in_pos: vec3<f32>, @location(2) in_uv: vec2<f32>, @location(1) in_normal: vec3<f32>) -> VertexOutput {
    in_pos_1 = in_pos;
    in_uv_1 = in_uv;
    in_normal_1 = in_normal;
    main_1();
    let _e10 = unnamed.gl_Position;
    let _e11 = v_uv;
    let _e12 = v_opacity;
    return VertexOutput(_e10, _e11, _e12);
}
