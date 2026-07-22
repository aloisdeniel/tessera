package com.tessera.flutter_tessera

import android.content.Context
import android.view.Choreographer
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import io.flutter.plugin.common.BinaryMessenger
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel
import io.flutter.plugin.platform.PlatformView
import java.io.File

/**
 * One embedded Tessera engine hosted in a SurfaceView.
 *
 * The engine renders offscreen (Vulkan/SPIR-V, via the C bridge) and the JNI
 * layer blits the pixels into this view's Surface. Drives the render loop on the
 * main thread (Choreographer), so channel handlers (pick) stay serialized with
 * the tick — matching the macOS/iOS design.
 *
 * NOTE: reference implementation for Android; not run in the authoring
 * environment. The shaders it renders ARE verified on Vulkan.
 */
internal class TesseraPlatformView(
    context: Context,
    viewId: Int,
    messenger: BinaryMessenger,
) : PlatformView, MethodChannel.MethodCallHandler, SurfaceHolder.Callback {

    private val surfaceView = SurfaceView(context)
    private val channel = MethodChannel(messenger, "flutter_tessera/view_$viewId")
    private val density = context.resources.displayMetrics.density
    private val assetDir: String = extractAssets(context)

    private var handle: Long = 0L
    private var running = false
    private var lastNanos = 0L

    init {
        surfaceView.holder.addCallback(this)
        channel.setMethodCallHandler(this)
    }

    override fun getView(): View = surfaceView

    override fun dispose() {
        stopLoop()
        channel.setMethodCallHandler(null)
        if (handle != 0L) {
            TesseraNative.nativeDestroy(handle)
            handle = 0L
        }
    }

    // ---- Surface lifecycle ----
    override fun surfaceCreated(holder: SurfaceHolder) {
        val w = surfaceView.width.coerceAtLeast(1)
        val h = surfaceView.height.coerceAtLeast(1)
        handle = TesseraNative.nativeCreate(holder.surface, w, h, density, assetDir)
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        if (handle != 0L) TesseraNative.nativeResize(handle, width, height, density)
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        stopLoop()
    }

    // ---- method channel ----
    override fun onMethodCall(call: MethodCall, result: MethodChannel.Result) {
        when (call.method) {
            "handle" ->
                result.success(if (handle != 0L) TesseraNative.nativeEngineHandle(handle) else 0L)
            "start" -> {
                startLoop()
                result.success(null)
            }
            "pick" -> {
                val x = (call.argument<Double>("x") ?: 0.0).toFloat()
                val y = (call.argument<Double>("y") ?: 0.0).toFloat()
                result.success(pick(x, y))
            }
            else -> result.notImplemented()
        }
    }

    private fun pick(x: Float, y: Float): Map<String, Any> {
        val ints = IntArray(4)
        val flts = FloatArray(3)
        var hit = false
        if (handle != 0L) hit = TesseraNative.nativePick(handle, x, y, ints, flts)[0]
        return mapOf(
            "hit" to hit,
            "hitTile" to (ints[0] != 0),
            "tileX" to ints[1],
            "tileY" to ints[2],
            "tileDistance" to flts[0].toDouble(),
            "hitEntity" to (ints[3] != 0),
            "entity" to flts[2].toLong(),
            "entityDistance" to flts[1].toDouble(),
        )
    }

    // ---- render loop (main thread) ----
    private val frameCallback = object : Choreographer.FrameCallback {
        override fun doFrame(frameTimeNanos: Long) {
            if (!running || handle == 0L) return
            val dt = if (lastNanos == 0L) 0.0 else (frameTimeNanos - lastNanos) / 1e9
            lastNanos = frameTimeNanos
            TesseraNative.nativeRenderFrame(handle, dt.coerceIn(0.0, 0.1))
            Choreographer.getInstance().postFrameCallback(this)
        }
    }

    private fun startLoop() {
        if (running) return
        running = true
        lastNanos = 0L
        Choreographer.getInstance().postFrameCallback(frameCallback)
    }

    private fun stopLoop() {
        running = false
        Choreographer.getInstance().removeFrameCallback(frameCallback)
    }

    /** Copy bundled shaders/ out of the APK to a filesystem dir the engine can
     *  fopen(); returns the parent dir (which contains shaders/). */
    private fun extractAssets(context: Context): String {
        val outRoot = File(context.filesDir, "tessera")
        val am = context.assets
        try {
            val names = am.list("shaders") ?: emptyArray()
            val outDir = File(outRoot, "shaders")
            outDir.mkdirs()
            for (n in names) {
                val dst = File(outDir, n)
                am.open("shaders/$n").use { input ->
                    dst.outputStream().use { input.copyTo(it) }
                }
            }
        } catch (_: Exception) {
            // Leave whatever was extracted; the engine will report missing shaders.
        }
        return outRoot.absolutePath
    }
}
