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

var<private> unnamed: gl_PerVertex = gl_PerVertex(vec4<f32>(0f, 0f, 0f, 1f), 1f, array<f32, 1>(), array<f32, 1>());
@group(1) @binding(0) var<uniform> frame: ShadowFrameUniform;
@group(1) @binding(1) var<uniform> obj: ObjectUniform;
var<private> in_pos_1: vec3<f32>;

fn main_1() {
    let _e8 = frame.light_vp;
    let _e10 = obj.model;
    let _e11 = in_pos_1;
    unnamed.gl_Position = (_e8 * (_e10 * vec4<f32>(_e11.x, _e11.y, _e11.z, 1f)));
    return;
}

@vertex 
fn main(@location(0) in_pos: vec3<f32>) -> @builtin(position) vec4<f32> {
    in_pos_1 = in_pos;
    main_1();
    let _e4 = unnamed.gl_Position;
    return _e4;
}
