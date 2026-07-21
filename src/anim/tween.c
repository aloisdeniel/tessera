/* tween.c — easing curves + the generic scalar tween engine (M4). */
#include "anim/anim.h"

float ts_ease(TsEasing e, float t) {
    t = ts_clampf(t, 0.0f, 1.0f);
    switch (e) {
    case TS_EASE_LINEAR:
        return t;
    case TS_EASE_IN_CUBIC:
        return t * t * t;
    case TS_EASE_OUT_CUBIC: {
        float u = 1.0f - t;
        return 1.0f - u * u * u;
    }
    case TS_EASE_IN_OUT_CUBIC:
        if (t < 0.5f) {
            return 4.0f * t * t * t;
        } else {
            float u = -2.0f * t + 2.0f;
            return 1.0f - (u * u * u) * 0.5f;
        }
    case TS_EASE_OUT_BACK: {
        const float c1 = 1.70158f;
        const float c3 = c1 + 1.0f;
        float u = t - 1.0f;
        return 1.0f + c3 * u * u * u + c1 * u * u;
    }
    case TS_EASE_OUT_BOUNCE: {
        const float n1 = 7.5625f;
        const float d1 = 2.75f;
        if (t < 1.0f / d1) {
            return n1 * t * t;
        } else if (t < 2.0f / d1) {
            t -= 1.5f / d1;
            return n1 * t * t + 0.75f;
        } else if (t < 2.5f / d1) {
            t -= 2.25f / d1;
            return n1 * t * t + 0.9375f;
        } else {
            t -= 2.625f / d1;
            return n1 * t * t + 0.984375f;
        }
    }
    default:
        return t;
    }
}

void ts_tween_start(TsTween* tw, float duration, float delay, TsEasing ease) {
    tw->elapsed = 0.0f;
    tw->delay = delay < 0.0f ? 0.0f : delay;
    tw->duration = duration;
    tw->ease = ease;
    /* Instantaneous tweens are already done; nothing to animate. */
    tw->active = duration > 0.0f;
}

bool ts_tween_advance(TsTween* tw, float dt) {
    tw->elapsed += dt;
    bool animating = !ts_tween_done(tw);
    tw->active = animating;
    return animating;
}

float ts_tween_value01(const TsTween* tw) {
    if (tw->duration <= 0.0f) {
        return 1.0f;
    }
    float local = tw->elapsed - tw->delay;
    if (local <= 0.0f) {
        return 0.0f;
    }
    float t = ts_clampf(local / tw->duration, 0.0f, 1.0f);
    return ts_ease(tw->ease, t);
}

bool ts_tween_done(const TsTween* tw) {
    if (tw->duration <= 0.0f) {
        return tw->elapsed >= tw->delay;
    }
    return tw->elapsed >= tw->delay + tw->duration;
}

void ts_tween_vec3(const TsTween* tw, vec3 a, vec3 b, vec3 out) {
    glm_vec3_lerp(a, b, ts_tween_value01(tw), out);
}

void ts_tween_quat(const TsTween* tw, versor a, versor b, versor out) {
    glm_quat_slerp(a, b, ts_tween_value01(tw), out);
}

float ts_tween_scalar(const TsTween* tw, float a, float b) {
    return ts_lerpf(a, b, ts_tween_value01(tw));
}

float ts_arc(float p) {
    return 4.0f * p * (1.0f - p);
}
