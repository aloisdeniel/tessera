struct JointUniform {
    joints: array<mat4x4<f32>, 64>,
}

struct gl_PerVertex {
    @builtin(position) gl_Position: vec4<f32>,
    gl_PointSize: f32,
    gl_ClipDistance: array<f32, 1>,
    gl_CullDistance: array<f32, 1>,
}

struct ShadowFrameUniform {
    light_vp: mat4x4<f32>,
}

struct ObjectUniform {
    model: mat4x4<f32>,
    tint: vec4<f32>,
    uv_rect: vec4<f32>,
}

var<private> in_weights_1: vec4<f32>;
@group(1) @binding(2) var<uniform> sk: JointUniform;
var<private> in_joints_1: vec4<u32>;
var<private> unnamed: gl_PerVertex = gl_PerVertex(vec4<f32>(0f, 0f, 0f, 1f), 1f, array<f32, 1>(), array<f32, 1>());
@group(1) @binding(0) var<uniform> frame: ShadowFrameUniform;
@group(1) @binding(1) var<uniform> obj: ObjectUniform;
var<private> in_pos_1: vec3<f32>;

fn main_1() {
    var wsum: f32;
    var skin: mat4x4<f32>;

    let _e24 = in_weights_1[0u];
    let _e26 = in_weights_1[1u];
    let _e29 = in_weights_1[2u];
    let _e32 = in_weights_1[3u];
    wsum = (((_e24 + _e26) + _e29) + _e32);
    let _e34 = wsum;
    if (_e34 < 0.0001f) {
        skin = mat4x4<f32>(vec4<f32>(1f, 0f, 0f, 0f), vec4<f32>(0f, 1f, 0f, 0f), vec4<f32>(0f, 0f, 1f, 0f), vec4<f32>(0f, 0f, 0f, 1f));
    } else {
        let _e37 = in_joints_1[0u];
        let _e40 = sk.joints[_e37];
        let _e42 = in_weights_1[0u];
        let _e43 = (_e40 * _e42);
        let _e45 = in_joints_1[1u];
        let _e48 = sk.joints[_e45];
        let _e50 = in_weights_1[1u];
        let _e51 = (_e48 * _e50);
        let _e64 = mat4x4<f32>((_e43[0] + _e51[0]), (_e43[1] + _e51[1]), (_e43[2] + _e51[2]), (_e43[3] + _e51[3]));
        let _e66 = in_joints_1[2u];
        let _e69 = sk.joints[_e66];
        let _e71 = in_weights_1[2u];
        let _e72 = (_e69 * _e71);
        let _e85 = mat4x4<f32>((_e64[0] + _e72[0]), (_e64[1] + _e72[1]), (_e64[2] + _e72[2]), (_e64[3] + _e72[3]));
        let _e87 = in_joints_1[3u];
        let _e90 = sk.joints[_e87];
        let _e92 = in_weights_1[3u];
        let _e93 = (_e90 * _e92);
        skin = mat4x4<f32>((_e85[0] + _e93[0]), (_e85[1] + _e93[1]), (_e85[2] + _e93[2]), (_e85[3] + _e93[3]));
    }
    let _e108 = frame.light_vp;
    let _e110 = obj.model;
    let _e111 = skin;
    let _e112 = in_pos_1;
    unnamed.gl_Position = (_e108 * (_e110 * (_e111 * vec4<f32>(_e112.x, _e112.y, _e112.z, 1f))));
    return;
}

@vertex 
fn main(@location(4) in_weights: vec4<f32>, @location(3) in_joints: vec4<u32>, @location(0) in_pos: vec3<f32>) -> @builtin(position) vec4<f32> {
    in_weights_1 = in_weights;
    in_joints_1 = in_joints;
    in_pos_1 = in_pos;
    main_1();
    let _e8 = unnamed.gl_Position;
    return _e8;
}
