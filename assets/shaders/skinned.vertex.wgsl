struct JointUniform {
    joints: array<mat4x4<f32>, 64>,
}

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

var<private> in_weights_1: vec4<f32>;
@group(1) @binding(2) var<uniform> sk: JointUniform;
var<private> in_joints_1: vec4<u32>;
var<private> in_pos_1: vec3<f32>;
var<private> in_normal_1: vec3<f32>;
@group(1) @binding(1) var<uniform> obj: ObjectUniform;
var<private> unnamed: gl_PerVertex = gl_PerVertex(vec4<f32>(0f, 0f, 0f, 1f), 1f, array<f32, 1>(), array<f32, 1>());
@group(1) @binding(0) var<uniform> frame: FrameUniform;
var<private> world_pos: vec3<f32>;
var<private> v_normal: vec3<f32>;
var<private> v_uv: vec2<f32>;
var<private> in_uv_1: vec2<f32>;
var<private> v_tint: vec4<f32>;

fn main_1() {
    var wsum: f32;
    var skin: mat4x4<f32>;
    var spos: vec4<f32>;
    var snorm_: vec3<f32>;
    var world: vec4<f32>;

    let _e35 = in_weights_1[0u];
    let _e37 = in_weights_1[1u];
    let _e40 = in_weights_1[2u];
    let _e43 = in_weights_1[3u];
    wsum = (((_e35 + _e37) + _e40) + _e43);
    let _e45 = wsum;
    if (_e45 < 0.0001f) {
        skin = mat4x4<f32>(vec4<f32>(1f, 0f, 0f, 0f), vec4<f32>(0f, 1f, 0f, 0f), vec4<f32>(0f, 0f, 1f, 0f), vec4<f32>(0f, 0f, 0f, 1f));
    } else {
        let _e48 = in_joints_1[0u];
        let _e51 = sk.joints[_e48];
        let _e53 = in_weights_1[0u];
        let _e54 = (_e51 * _e53);
        let _e56 = in_joints_1[1u];
        let _e59 = sk.joints[_e56];
        let _e61 = in_weights_1[1u];
        let _e62 = (_e59 * _e61);
        let _e75 = mat4x4<f32>((_e54[0] + _e62[0]), (_e54[1] + _e62[1]), (_e54[2] + _e62[2]), (_e54[3] + _e62[3]));
        let _e77 = in_joints_1[2u];
        let _e80 = sk.joints[_e77];
        let _e82 = in_weights_1[2u];
        let _e83 = (_e80 * _e82);
        let _e96 = mat4x4<f32>((_e75[0] + _e83[0]), (_e75[1] + _e83[1]), (_e75[2] + _e83[2]), (_e75[3] + _e83[3]));
        let _e98 = in_joints_1[3u];
        let _e101 = sk.joints[_e98];
        let _e103 = in_weights_1[3u];
        let _e104 = (_e101 * _e103);
        skin = mat4x4<f32>((_e96[0] + _e104[0]), (_e96[1] + _e104[1]), (_e96[2] + _e104[2]), (_e96[3] + _e104[3]));
    }
    let _e118 = skin;
    let _e119 = in_pos_1;
    spos = (_e118 * vec4<f32>(_e119.x, _e119.y, _e119.z, 1f));
    let _e125 = skin;
    let _e126 = in_normal_1;
    snorm_ = (_e125 * vec4<f32>(_e126.x, _e126.y, _e126.z, 0f)).xyz;
    let _e134 = obj.model;
    let _e135 = spos;
    world = (_e134 * _e135);
    let _e138 = frame.view_proj;
    let _e139 = world;
    unnamed.gl_Position = (_e138 * _e139);
    let _e142 = world;
    world_pos = _e142.xyz;
    let _e145 = obj.model;
    let _e146 = snorm_;
    v_normal = normalize((_e145 * vec4<f32>(_e146.x, _e146.y, _e146.z, 0f)).xyz);
    let _e155 = obj.uv_rect;
    let _e157 = in_uv_1;
    let _e159 = obj.uv_rect;
    let _e162 = obj.uv_rect;
    v_uv = (_e155.xy + (_e157 * (_e159.zw - _e162.xy)));
    let _e168 = obj.tint;
    v_tint = _e168;
    return;
}

@vertex 
fn main(@location(4) in_weights: vec4<f32>, @location(3) in_joints: vec4<u32>, @location(0) in_pos: vec3<f32>, @location(1) in_normal: vec3<f32>, @location(2) in_uv: vec2<f32>) -> VertexOutput {
    in_weights_1 = in_weights;
    in_joints_1 = in_joints;
    in_pos_1 = in_pos;
    in_normal_1 = in_normal;
    in_uv_1 = in_uv;
    main_1();
    let _e16 = unnamed.gl_Position;
    let _e17 = world_pos;
    let _e18 = v_normal;
    let _e19 = v_uv;
    let _e20 = v_tint;
    return VertexOutput(_e16, _e17, _e18, _e19, _e20);
}
