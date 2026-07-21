/* test_anim.c — headless unit tests for the skeletal-animation math in
 * src/anim/skeleton.h. No GPU is created; this exercises pure CPU math and is
 * safe to run in CI. Style mirrors tests/test_core.c (plain CHECK asserts;
 * main returns nonzero on failure). */
#include "anim/skeleton.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)
#define CHECKF(a, b) CHECK(fabsf((float)(a) - (float)(b)) < 1e-4f)

/* Compare a vec3 against expected components. */
static void check_vec3(const vec3 v, float x, float y, float z, const char* what) {
    if (fabsf(v[0]-x) > 1e-3f || fabsf(v[1]-y) > 1e-3f || fabsf(v[2]-z) > 1e-3f) {
        printf("FAIL vec3 %s = (%f,%f,%f), want (%f,%f,%f)\n",
               what, v[0], v[1], v[2], x, y, z);
        g_fail++;
    }
}

/* Transform a homogeneous point (x,y,z,1) by a mat4 and drop w. */
static void xform_point(mat4 m, float x, float y, float z, vec3 out) {
    vec4 p = {x, y, z, 1.0f}, r;
    glm_mat4_mulv(m, p, r);
    out[0] = r[0]; out[1] = r[1]; out[2] = r[2];
}

/* ------------------------------------------------------------------------- */
static void test_wrap_time(void) {
    /* looping wraps into [0,duration) */
    CHECKF(ts_clip_wrap_time(2.5f, 1.0f, true), 0.5f);
    CHECKF(ts_clip_wrap_time(0.25f, 1.0f, true), 0.25f);
    CHECKF(ts_clip_wrap_time(1.0f, 1.0f, true), 0.0f);   /* exact wrap */
    CHECKF(ts_clip_wrap_time(-0.25f, 1.0f, true), 0.75f);/* negative wraps up */

    /* clamping holds at the ends */
    CHECKF(ts_clip_wrap_time(2.5f, 1.0f, false), 1.0f);
    CHECKF(ts_clip_wrap_time(-3.0f, 1.0f, false), 0.0f);
    CHECKF(ts_clip_wrap_time(0.4f, 1.0f, false), 0.4f);

    /* zero / negative duration always returns 0 */
    CHECKF(ts_clip_wrap_time(5.0f, 0.0f, true), 0.0f);
    CHECKF(ts_clip_wrap_time(5.0f, 0.0f, false), 0.0f);
    CHECKF(ts_clip_wrap_time(5.0f, -1.0f, true), 0.0f);
}

/* Build the 2-joint skeleton described in the task:
 *   joint0: root at origin, identity inverse-bind.
 *   joint1: child of 0, bind-local translate(0,1,0), inverse-bind translate(0,-1,0).
 */
static void build_skeleton(TsSkeleton* sk) {
    memset(sk, 0, sizeof *sk);
    sk->joint_count = 2;

    TsJoint* j0 = &sk->joints[0];
    j0->parent = -1;
    glm_mat4_identity(j0->inverse_bind);
    glm_vec3_zero(j0->t);
    glm_quat_identity(j0->r);
    glm_vec3_one(j0->s);

    TsJoint* j1 = &sk->joints[1];
    j1->parent = 0;
    glm_mat4_identity(j1->inverse_bind);
    glm_translate(j1->inverse_bind, (vec3){0.0f, -1.0f, 0.0f});
    glm_vec3_copy((vec3){0.0f, 1.0f, 0.0f}, j1->t);
    glm_quat_identity(j1->r);
    glm_vec3_one(j1->s);
}

static void test_skinning_bind_pose(void) {
    TsSkeleton sk; build_skeleton(&sk);

    TsJointPose pose[2];
    ts_skeleton_bind_pose(&sk, pose);

    mat4 skin[2];
    ts_skeleton_skinning(&sk, pose, skin);

    /* Both skinning matrices must be identity: a bind-pose vertex is unmoved. */
    vec3 p;
    xform_point(skin[0], 0.0f, 0.5f, 0.0f, p);
    check_vec3(p, 0.0f, 0.5f, 0.0f, "bind joint0 lower vertex");
    xform_point(skin[1], 0.0f, 1.0f, 0.0f, p);   /* at the joint pivot */
    check_vec3(p, 0.0f, 1.0f, 0.0f, "bind joint1 pivot vertex");
    xform_point(skin[1], 0.0f, 2.0f, 0.0f, p);   /* an upper vertex */
    check_vec3(p, 0.0f, 2.0f, 0.0f, "bind joint1 upper vertex");
}

static void test_skinning_rotated_joint(void) {
    TsSkeleton sk; build_skeleton(&sk);

    /* Local pose = bind pose, then rotate joint1 by +90deg about Z. */
    TsJointPose pose[2];
    ts_skeleton_bind_pose(&sk, pose);
    glm_quatv(pose[1].r, GLM_PI_2f, (vec3){0.0f, 0.0f, 1.0f});

    mat4 skin[2];
    ts_skeleton_skinning(&sk, pose, skin);

    /* A vertex skinned by the *root* (joint0) never moves — joint1's rotation
     * doesn't touch it. */
    vec3 lower;
    xform_point(skin[0], 0.0f, 0.5f, 0.0f, lower);
    check_vec3(lower, 0.0f, 0.5f, 0.0f, "rotated: lower (root) vertex");

    /* The joint1 pivot (world (0,1,0) at bind) stays put — it is the rotation
     * center. */
    vec3 pivot;
    xform_point(skin[1], 0.0f, 1.0f, 0.0f, pivot);
    check_vec3(pivot, 0.0f, 1.0f, 0.0f, "rotated: joint1 pivot");

    /* An upper vertex (world (0,2,0) at bind) swings out: rotating the offset
     * (0,1,0) by +90deg about Z gives (-1,0,0), plus the pivot -> (-1,1,0). */
    vec3 upper;
    xform_point(skin[1], 0.0f, 2.0f, 0.0f, upper);
    check_vec3(upper, -1.0f, 1.0f, 0.0f, "rotated: joint1 upper vertex");

    /* Sanity: the upper vertex actually moved a meaningful distance. */
    CHECK(fabsf(upper[0] - 0.0f) > 0.5f);
}

/* Build a clip with a single rotation channel on joint1: key0 = identity,
 * key1 = +90deg about Z, over duration 1.0. */
static void test_clip_sample(void) {
    TsSkeleton sk; build_skeleton(&sk);

    static float times[2] = {0.0f, 1.0f};
    versor q0, q1;
    glm_quat_identity(q0);
    glm_quatv(q1, GLM_PI_2f, (vec3){0.0f, 0.0f, 1.0f});
    float values[8] = {
        q0[0], q0[1], q0[2], q0[3],
        q1[0], q1[1], q1[2], q1[3],
    };
    TsChannel ch;
    memset(&ch, 0, sizeof ch);
    ch.joint = 1;
    ch.path = TS_PATH_R;
    ch.interp = TS_INTERP_LINEAR;
    ch.times = times;
    ch.values = values;
    ch.key_count = 2;
    ch.comps = 4;

    TsClip clip;
    memset(&clip, 0, sizeof clip);
    clip.name = (char*)"spin";
    clip.duration = 1.0f;
    clip.channels = &ch;
    clip.channel_count = 1;

    TsJointPose pose[2];

    /* t = 0 -> identity rotation. Rotate the offset (0,1,0); it stays (0,1,0). */
    ts_clip_sample(&sk, &clip, 0.0f, false, pose);
    vec3 off = {0.0f, 1.0f, 0.0f}, r;
    glm_quat_rotatev(pose[1].r, off, r);
    check_vec3(r, 0.0f, 1.0f, 0.0f, "clip t=0 slerp endpoint");

    /* t = 1 (clamped) -> +90deg about Z. Offset (0,1,0) -> (-1,0,0). */
    ts_clip_sample(&sk, &clip, 1.0f, false, pose);
    glm_quat_rotatev(pose[1].r, off, r);
    check_vec3(r, -1.0f, 0.0f, 0.0f, "clip t=1 slerp endpoint");

    /* t = 0.5 -> +45deg about Z. Offset (0,1,0) -> (-sin45, cos45, 0). */
    ts_clip_sample(&sk, &clip, 0.5f, false, pose);
    glm_quat_rotatev(pose[1].r, off, r);
    check_vec3(r, -0.70710678f, 0.70710678f, 0.0f, "clip mid slerp");

    /* joint0 is untouched by the clip: it holds its bind-pose local. */
    check_vec3(pose[0].t, 0.0f, 0.0f, 0.0f, "clip leaves joint0 translation");
}

static void test_pose_blend(void) {
    TsJointPose a, b, out;
    glm_vec3_copy((vec3){0.0f, 0.0f, 0.0f}, a.t);
    glm_vec3_copy((vec3){1.0f, 1.0f, 1.0f}, a.s);
    glm_quat_identity(a.r);

    glm_vec3_copy((vec3){2.0f, 4.0f, 6.0f}, b.t);
    glm_vec3_copy((vec3){3.0f, 3.0f, 3.0f}, b.s);
    glm_quatv(b.r, GLM_PI_2f, (vec3){0.0f, 0.0f, 1.0f});

    vec3 off = {0.0f, 1.0f, 0.0f}, r;

    /* t = 0 -> exactly a */
    ts_pose_blend(&a, &b, 0.0f, 1, &out);
    check_vec3(out.t, 0.0f, 0.0f, 0.0f, "blend t=0 translation");
    check_vec3(out.s, 1.0f, 1.0f, 1.0f, "blend t=0 scale");
    glm_quat_rotatev(out.r, off, r);
    check_vec3(r, 0.0f, 1.0f, 0.0f, "blend t=0 rotation");

    /* t = 1 -> exactly b */
    ts_pose_blend(&a, &b, 1.0f, 1, &out);
    check_vec3(out.t, 2.0f, 4.0f, 6.0f, "blend t=1 translation");
    check_vec3(out.s, 3.0f, 3.0f, 3.0f, "blend t=1 scale");
    glm_quat_rotatev(out.r, off, r);
    check_vec3(r, -1.0f, 0.0f, 0.0f, "blend t=1 rotation");

    /* t = 0.5 -> linear T/S, slerped R (45deg). */
    ts_pose_blend(&a, &b, 0.5f, 1, &out);
    check_vec3(out.t, 1.0f, 2.0f, 3.0f, "blend mid translation (linear)");
    check_vec3(out.s, 2.0f, 2.0f, 2.0f, "blend mid scale (linear)");
    glm_quat_rotatev(out.r, off, r);
    check_vec3(r, -0.70710678f, 0.70710678f, 0.0f, "blend mid rotation (slerp)");
}

int main(void) {
    test_wrap_time();
    test_skinning_bind_pose();
    test_skinning_rotated_joint();
    test_clip_sample();
    test_pose_blend();

    if (g_fail == 0) { printf("all anim tests passed\n"); return 0; }
    printf("%d checks failed\n", g_fail);
    return 1;
}
