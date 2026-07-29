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
    @location(0) member: vec3<f32>,
    @location(1) member_1: vec3<f32>,
    @location(2) member_2: vec2<f32>,
    @location(3) member_3: vec4<f32>,
}

@group(1) @binding(1) var<uniform> obj: ObjectUniform;
var<private> in_pos_1: vec3<f32>;
var<private> unnamed: gl_PerVertex = gl_PerVertex(vec4<f32>(0f, 0f, 0f, 1f), 1f, array<f32, 1>(), array<f32, 1>());
@group(1) @binding(0) var<uniform> frame: FrameUniform;
var<private> world_pos: vec3<f32>;
var<private> v_normal: vec3<f32>;
var<private> in_normal_1: vec3<f32>;
var<private> v_uv: vec2<f32>;
var<private> in_uv_1: vec2<f32>;
var<private> v_tint: vec4<f32>;

fn main_1() {
    var world: vec4<f32>;

    let _e18 = obj.model;
    let _e19 = in_pos_1;
    world = (_e18 * vec4<f32>(_e19.x, _e19.y, _e19.z, 1f));
    let _e26 = frame.view_proj;
    let _e27 = world;
    unnamed.gl_Position = (_e26 * _e27);
    let _e30 = world;
    world_pos = _e30.xyz;
    let _e33 = obj.model;
    let _e34 = in_normal_1;
    v_normal = normalize((_e33 * vec4<f32>(_e34.x, _e34.y, _e34.z, 0f)).xyz);
    let _e43 = obj.uv_rect;
    let _e45 = in_uv_1;
    let _e47 = obj.uv_rect;
    let _e50 = obj.uv_rect;
    v_uv = (_e43.xy + (_e45 * (_e47.zw - _e50.xy)));
    let _e56 = obj.tint;
    v_tint = _e56;
    return;
}

@vertex 
fn main(@location(0) in_pos: vec3<f32>, @location(1) in_normal: vec3<f32>, @location(2) in_uv: vec2<f32>) -> VertexOutput {
    in_pos_1 = in_pos;
    in_normal_1 = in_normal;
    in_uv_1 = in_uv;
    main_1();
    let _e12 = unnamed.gl_Position;
    let _e13 = world_pos;
    let _e14 = v_normal;
    let _e15 = v_uv;
    let _e16 = v_tint;
    return VertexOutput(_e12, _e13, _e14, _e15, _e16);
}
