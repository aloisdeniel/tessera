/*
 * anim.h — easing + the generic tween engine (M4). Skeleton/clip runtime for
 * skeletal animation (M5) is declared here too but simulated later.
 */
#ifndef TESSERA_ANIM_H
#define TESSERA_ANIM_H

#include "core/core.h"
#include "core/tmath.h"

typedef enum {
    TS_EASE_LINEAR = 0,
    TS_EASE_IN_CUBIC,
    TS_EASE_OUT_CUBIC,
    TS_EASE_IN_OUT_CUBIC,
    TS_EASE_OUT_BACK,     /* slight overshoot */
    TS_EASE_OUT_BOUNCE
} TsEasing;

/* Apply an easing curve to a normalized t in [0,1]. */
float ts_ease(TsEasing e, float t);

/* A scalar tween with delay + duration. */
typedef struct {
    float    elapsed;    /* seconds since start (incl. delay) */
    float    delay;
    float    duration;
    TsEasing ease;
    bool     active;
} TsTween;

void  ts_tween_start(TsTween* tw, float duration, float delay, TsEasing ease);
/* Advance by dt; returns true while still animating (post-delay, pre-complete). */
bool  ts_tween_advance(TsTween* tw, float dt);
/* Eased progress in [0,1] (0 during delay, 1 when complete). */
float ts_tween_value01(const TsTween* tw);
bool  ts_tween_done(const TsTween* tw);

/* Convenience interpolators driven by a tween's eased value. */
void ts_tween_vec3(const TsTween* tw, vec3 a, vec3 b, vec3 out);
void ts_tween_quat(const TsTween* tw, versor a, versor b, versor out);
float ts_tween_scalar(const TsTween* tw, float a, float b);

/* Vertical hop arc height factor for a move at eased progress p in [0,1]:
 * returns 4*h*p*(1-p) style parabola weight (peaks at p=0.5). */
float ts_arc(float p);

#endif /* TESSERA_ANIM_H */
