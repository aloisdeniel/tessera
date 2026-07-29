struct CardUniform {
    model: mat4x4<f32>,
    tint: vec4<f32>,
    uv_visible: vec4<f32>,
    uv_hidden: vec4<f32>,
    uv_back: vec4<f32>,
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
    @location(0) member: vec3<f32>,
    @location(1) member_1: vec3<f32>,
    @location(2) member_2: vec2<f32>,
    @location(3) member_3: f32,
}

@group(1) @binding(1) var<uniform> card: CardUniform;
var<private> in_pos_1: vec3<f32>;
var<private> unnamed: gl_PerVertex = gl_PerVertex(vec4<f32>(0f, 0f, 0f, 1f), 1f, array<f32, 1>(), array<f32, 1>());
@group(1) @binding(0) var<uniform> frame: FrameUniform;
var<private> world_pos: vec3<f32>;
var<private> v_normal: vec3<f32>;
var<private> in_normal_1: vec3<f32>;
var<private> v_uv: vec2<f32>;
var<private> in_uv_1: vec2<f32>;
var<private> v_face: f32;
var<private> in_face_1: f32;

fn main_1() {
    var world: vec4<f32>;

    let _e17 = card.model;
    let _e18 = in_pos_1;
    world = (_e17 * vec4<f32>(_e18.x, _e18.y, _e18.z, 1f));
    let _e25 = frame.view_proj;
    let _e26 = world;
    unnamed.gl_Position = (_e25 * _e26);
    let _e29 = world;
    world_pos = _e29.xyz;
    let _e32 = card.model;
    let _e33 = in_normal_1;
    v_normal = normalize((_e32 * vec4<f32>(_e33.x, _e33.y, _e33.z, 0f)).xyz);
    let _e41 = in_uv_1;
    v_uv = _e41;
    let _e42 = in_face_1;
    v_face = _e42;
    return;
}

@vertex 
fn main(@location(0) in_pos: vec3<f32>, @location(1) in_normal: vec3<f32>, @location(2) in_uv: vec2<f32>, @location(3) in_face: f32) -> VertexOutput {
    in_pos_1 = in_pos;
    in_normal_1 = in_normal;
    in_uv_1 = in_uv;
    in_face_1 = in_face;
    main_1();
    let _e14 = unnamed.gl_Position;
    let _e15 = world_pos;
    let _e16 = v_normal;
    let _e17 = v_uv;
    let _e18 = v_face;
    return VertexOutput(_e14, _e15, _e16, _e17, _e18);
}
