package com.tessera.flutter_tessera

import android.view.Surface

/** JNI entry points into libtessera.so (engine + bridge + jni). */
internal object TesseraNative {
    init {
        System.loadLibrary("tessera")
    }

    external fun nativeCreate(
        surface: Surface, w: Int, h: Int, density: Float, assetDir: String
    ): Long

    /** The TesseraEngine* address, for Dart FFI (Tessera.fromHandle). */
    external fun nativeEngineHandle(handle: Long): Long

    external fun nativeResize(handle: Long, w: Int, h: Int, density: Float)

    external fun nativeRenderFrame(handle: Long, dt: Double)

    /**
     * Ray-pick. Fills [outInts] = {hitTile, tileX, tileY, hitEntity} and
     * [outFloats] = {tileDistance, entityDistance, entityAsFloat}. Returns
     * {anyHit}.
     */
    external fun nativePick(
        handle: Long, x: Float, y: Float, outInts: IntArray, outFloats: FloatArray
    ): BooleanArray

    external fun nativeDestroy(handle: Long)
}
