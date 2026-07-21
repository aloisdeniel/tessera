/*
 * skeleton.h — runtime skeleton + animation clips for GPU skinning (M5).
 *
 * A skinned entity def owns one TsSkinData (arena-allocated): a joint hierarchy
 * with bind-pose local transforms + inverse-bind matrices, and a set of clips.
 * Sampling a clip produces a local pose; the skeleton walk turns that into the
 * joint palette (world_joint * inverse_bind) uploaded to the skinned shader.
 */
#ifndef TESSERA_SKELETON_H
#define TESSERA_SKELETON_H

#include "core/core.h"
#include "core/tmath.h"

#define TS_MAX_JOINTS 64   /* asset constraint; matches the shader palette size */

typedef struct {
    int    parent;         /* index into joints[], -1 for a root */
    mat4   inverse_bind;   /* inverse bind matrix */
    vec3   t;              /* bind-pose local translation */
    versor r;              /* bind-pose local rotation    */
    vec3   s;              /* bind-pose local scale        */
} TsJoint;

typedef struct {
    TsJoint  joints[TS_MAX_JOINTS];
    uint32_t joint_count;
} TsSkeleton;

typedef enum { TS_PATH_T = 0, TS_PATH_R = 1, TS_PATH_S = 2 } TsChannelPath;
typedef enum { TS_INTERP_LINEAR = 0, TS_INTERP_STEP = 1 } TsInterp;

typedef struct {
    uint32_t      joint;       /* target joint index */
    TsChannelPath path;
    TsInterp      interp;
    float*        times;       /* key_count floats (arena) */
    float*        values;      /* key_count * comps floats (arena) */
    uint32_t      key_count;
    uint32_t      comps;       /* 3 for T/S, 4 for R */
} TsChannel;

typedef struct {
    char*      name;
    float      duration;
    TsChannel* channels;
    uint32_t   channel_count;
} TsClip;

typedef struct {
    TsSkeleton skeleton;
    TsClip*    clips;
    uint32_t   clip_count;
} TsSkinData;

/* A single joint's local pose, used while sampling/blending. */
typedef struct { vec3 t; versor r; vec3 s; } TsJointPose;

/* Fill `pose` (joint_count entries) with the skeleton's bind-pose locals. */
void ts_skeleton_bind_pose(const TsSkeleton* sk, TsJointPose* pose);

/* Sample `clip` at `time` (looped or clamped) over the bind pose into `pose`. */
void ts_clip_sample(const TsSkeleton* sk, const TsClip* clip, float time,
                    bool loop, TsJointPose* pose);

/* Blend two local poses by t in [0,1] (a->b): lerp T/S, slerp R. In-place-safe
 * only if out != a,b. */
void ts_pose_blend(const TsJointPose* a, const TsJointPose* b, float t,
                   uint32_t joint_count, TsJointPose* out);

/* Walk the hierarchy: compute skinning matrices (world_joint * inverse_bind)
 * for each joint from a local pose. `out` holds joint_count mat4. */
void ts_skeleton_skinning(const TsSkeleton* sk, const TsJointPose* pose, mat4* out);

/* Normalise a time into [0,duration) (loop) or clamp to [0,duration]. */
float ts_clip_wrap_time(float time, float duration, bool loop);

#endif /* TESSERA_SKELETON_H */
