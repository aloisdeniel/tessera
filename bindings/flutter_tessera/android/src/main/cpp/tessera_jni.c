/*
 * tessera_jni.c — JNI bridge for the Android plugin.
 *
 * Reuses the shared C bridge (tessera_bridge.c): the engine renders offscreen
 * with tessera_render_rgba (Vulkan/SPIR-V on Android), and this layer blits the
 * RGBA pixels into the platform view's Android Surface via ANativeWindow. Assets
 * (SPIR-V shaders) are extracted from the APK to a filesystem dir by the Kotlin
 * side and passed as asset_dir.
 *
 * NOTE: reference implementation for the Android build; not compiled in this
 * environment (no NDK/emulator here). The SPIR-V shaders it renders ARE verified
 * on a real Vulkan device (MoltenVK). The main integration risk is bringing up
 * SDL_GPU headlessly inside a Flutter Activity — see the plugin README.
 */
#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <stdlib.h>
#include <string.h>

#include "tessera_bridge.h"

typedef struct {
    FTessera*      engine;
    ANativeWindow* win;      /* output surface we blit into */
    uint8_t*       buf;      /* RGBA scratch, w*h*4 */
    int            w, h;
} AndroidView;

#define JNI(ret, name) JNIEXPORT ret JNICALL \
    Java_com_tessera_flutter_1tessera_TesseraNative_##name

JNI(jlong, nativeCreate)(JNIEnv* env, jclass clazz, jobject surface,
                         jint w, jint h, jfloat density, jstring assetDir) {
    (void)clazz;
    AndroidView* v = (AndroidView*)calloc(1, sizeof *v);
    if (!v) return 0;
    v->win = ANativeWindow_fromSurface(env, surface);
    v->w = w; v->h = h;

    const char* dir = assetDir ? (*env)->GetStringUTFChars(env, assetDir, NULL) : NULL;
    v->engine = ftessera_create(w, h, density, dir);
    if (dir) (*env)->ReleaseStringUTFChars(env, assetDir, dir);

    if (!v->engine) {
        if (v->win) ANativeWindow_release(v->win);
        free(v);
        return 0;
    }
    return (jlong)(uintptr_t)v;
}

JNI(jlong, nativeEngineHandle)(JNIEnv* env, jclass clazz, jlong h) {
    (void)env; (void)clazz;
    AndroidView* v = (AndroidView*)(uintptr_t)h;
    return v ? (jlong)ftessera_engine_handle(v->engine) : 0;
}

JNI(void, nativeResize)(JNIEnv* env, jclass clazz, jlong h,
                        jint w, jint hh, jfloat density) {
    (void)env; (void)clazz;
    AndroidView* v = (AndroidView*)(uintptr_t)h;
    if (!v) return;
    v->w = w; v->h = hh;
    ftessera_resize(v->engine, w, hh, density);
}

JNI(void, nativeRenderFrame)(JNIEnv* env, jclass clazz, jlong h, jdouble dt) {
    (void)env; (void)clazz;
    AndroidView* v = (AndroidView*)(uintptr_t)h;
    if (!v || !v->win || v->w <= 0 || v->h <= 0) return;

    size_t need = (size_t)v->w * v->h * 4;
    if (!v->buf) v->buf = (uint8_t*)malloc(need);
    if (!v->buf) return;

    if (!ftessera_render_rgba(v->engine, dt, v->w, v->h, v->buf, need)) return;

    ANativeWindow_setBuffersGeometry(v->win, v->w, v->h, WINDOW_FORMAT_RGBA_8888);
    ANativeWindow_Buffer out;
    if (ANativeWindow_lock(v->win, &out, NULL) != 0) return;
    /* Copy row by row to honour the surface stride (out.stride is in pixels). */
    for (int y = 0; y < out.height && y < v->h; ++y) {
        uint8_t* dst = (uint8_t*)out.bits + (size_t)y * out.stride * 4;
        const uint8_t* src = v->buf + (size_t)y * v->w * 4;
        int row = (out.width < v->w ? out.width : v->w) * 4;
        memcpy(dst, src, (size_t)row);
    }
    ANativeWindow_unlockAndPost(v->win);
}

JNI(jbooleanArray, nativePick)(JNIEnv* env, jclass clazz, jlong h,
                               jfloat x, jfloat y, jintArray outInts,
                               jfloatArray outFloats) {
    (void)clazz;
    AndroidView* v = (AndroidView*)(uintptr_t)h;
    FTesseraPick pk;
    memset(&pk, 0, sizeof pk);
    jboolean hit = JNI_FALSE;
    if (v) hit = ftessera_pick(v->engine, x, y, &pk) ? JNI_TRUE : JNI_FALSE;

    jint ints[4] = { pk.hit_tile, pk.tile_x, pk.tile_y, pk.hit_entity };
    jfloat flts[3] = { pk.tile_distance, pk.entity_distance, (jfloat)pk.entity };
    (*env)->SetIntArrayRegion(env, outInts, 0, 4, ints);
    (*env)->SetFloatArrayRegion(env, outFloats, 0, 3, flts);

    jbooleanArray res = (*env)->NewBooleanArray(env, 1);
    (*env)->SetBooleanArrayRegion(env, res, 0, 1, &hit);
    return res;
}

JNI(void, nativeDestroy)(JNIEnv* env, jclass clazz, jlong h) {
    (void)env; (void)clazz;
    AndroidView* v = (AndroidView*)(uintptr_t)h;
    if (!v) return;
    if (v->engine) ftessera_destroy(v->engine);
    if (v->win) ANativeWindow_release(v->win);
    free(v->buf);
    free(v);
}
