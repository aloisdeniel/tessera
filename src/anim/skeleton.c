/* skeleton.c — clip sampling + skeleton skinning-matrix computation (M5). */
#include "anim/skeleton.h"
#include <string.h>

void ts_skeleton_bind_pose(const TsSkeleton* sk, TsJointPose* pose) {
    for (uint32_t i = 0; i < sk->joint_count; ++i) {
        glm_vec3_copy((float*)sk->joints[i].t, pose[i].t);
        glm_quat_copy((float*)sk->joints[i].r, pose[i].r);
        glm_vec3_copy((float*)sk->joints[i].s, pose[i].s);
    }
}

float ts_clip_wrap_time(float time, float duration, bool loop) {
    if (duration <= 0.0f) return 0.0f;
    if (loop) {
        float t = fmodf(time, duration);
        if (t < 0.0f) t += duration;
        return t;
    }
    return ts_clampf(time, 0.0f, duration);
}

/* Find the key index k such that times[k] <= t < times[k+1]; returns the lower
 * key and the interpolation factor in *frac. */
static uint32_t find_key(const float* times, uint32_t n, float t, float* frac) {
    if (n == 0) { *frac = 0.0f; return 0; }
    if (t <= times[0]) { *frac = 0.0f; return 0; }
    if (t >= times[n - 1]) { *frac = 0.0f; return n - 1; }
    /* linear scan is fine for the short clips we target */
    uint32_t k = 0;
    while (k + 1 < n && times[k + 1] <= t) ++k;
    if (k + 1 >= n) { *frac = 0.0f; return n - 1; }
    float span = times[k + 1] - times[k];
    *frac = span > 1e-8f ? (t - times[k]) / span : 0.0f;
    return k;
}

static void sample_channel(const TsChannel* ch, float t, TsJointPose* jp) {
    float frac;
    uint32_t k = find_key(ch->times, ch->key_count, t, &frac);
    if (ch->key_count == 0) return;
    uint32_t k1 = (k + 1 < ch->key_count) ? k + 1 : k;
    if (ch->interp == TS_INTERP_STEP) { frac = 0.0f; k1 = k; }

    const float* a = &ch->values[k * ch->comps];
    const float* b = &ch->values[k1 * ch->comps];

    if (ch->path == TS_PATH_R) {
        versor qa = {a[0], a[1], a[2], a[3]};
        versor qb = {b[0], b[1], b[2], b[3]};
        versor out;
        glm_quat_slerp(qa, qb, frac, out);
        glm_quat_copy(out, jp->r);
    } else {
        vec3 va = {a[0], a[1], a[2]};
        vec3 vb = {b[0], b[1], b[2]};
        vec3 out;
        glm_vec3_lerp(va, vb, frac, out);
        if (ch->path == TS_PATH_T) glm_vec3_copy(out, jp->t);
        else                       glm_vec3_copy(out, jp->s);
    }
}

void ts_clip_sample(const TsSkeleton* sk, const TsClip* clip, float time,
                    bool loop, TsJointPose* pose) {
    ts_skeleton_bind_pose(sk, pose);
    if (!clip) return;
    float t = ts_clip_wrap_time(time, clip->duration, loop);
    for (uint32_t c = 0; c < clip->channel_count; ++c) {
        const TsChannel* ch = &clip->channels[c];
        if (ch->joint >= sk->joint_count) continue;
        sample_channel(ch, t, &pose[ch->joint]);
    }
}

void ts_pose_blend(const TsJointPose* a, const TsJointPose* b, float t,
                   uint32_t joint_count, TsJointPose* out) {
    for (uint32_t i = 0; i < joint_count; ++i) {
        glm_vec3_lerp((float*)a[i].t, (float*)b[i].t, t, out[i].t);
        glm_vec3_lerp((float*)a[i].s, (float*)b[i].s, t, out[i].s);
        glm_quat_slerp((float*)a[i].r, (float*)b[i].r, t, out[i].r);
    }
}

void ts_skeleton_skinning(const TsSkeleton* sk, const TsJointPose* pose, mat4* out) {
    mat4 local[TS_MAX_JOINTS];
    mat4 world[TS_MAX_JOINTS];
    bool done[TS_MAX_JOINTS];
    uint32_t n = sk->joint_count;
    if (n > TS_MAX_JOINTS) n = TS_MAX_JOINTS;

    for (uint32_t i = 0; i < n; ++i) {
        ts_trs((float*)pose[i].t, (float*)pose[i].r, (float*)pose[i].s, local[i]);
        done[i] = false;
    }

    /* Resolve world matrices regardless of joint ordering: repeat until every
     * joint whose parent is ready has been computed (parents are a DAG/tree). */
    bool progressed = true;
    while (progressed) {
        progressed = false;
        for (uint32_t i = 0; i < n; ++i) {
            if (done[i]) continue;
            int p = sk->joints[i].parent;
            if (p < 0 || p >= (int)n) {
                glm_mat4_copy(local[i], world[i]);
                done[i] = true; progressed = true;
            } else if (done[p]) {
                glm_mat4_mul(world[p], local[i], world[i]);
                done[i] = true; progressed = true;
            }
        }
    }
    /* Any joint left (cyclic/bad parent) falls back to its local transform. */
    for (uint32_t i = 0; i < n; ++i) {
        if (!done[i]) glm_mat4_copy(local[i], world[i]);
        glm_mat4_mul(world[i], (vec4*)sk->joints[i].inverse_bind, out[i]);
    }
}
