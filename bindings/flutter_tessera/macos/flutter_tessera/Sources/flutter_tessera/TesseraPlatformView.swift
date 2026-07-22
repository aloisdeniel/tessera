// TesseraPlatformView.swift — one embedded Tessera engine hosted in an NSView.
//
// An NSView subclass (returned directly by the factory, so Flutter retains it)
// that owns the engine (via the C bridge, on the main thread) and the render
// loop. Presentation is zero-copy: the engine renders to the SDL window's
// swapchain, and we reparent that swapchain's CAMetalLayer-backed metal view
// into this view, so frames scan out straight into the Flutter surface with no
// CPU round-trip or blit. Exposes a per-view method channel:
//   • handle → Int   the engine pointer for Dart FFI (Tessera.fromHandle)
//   • start          begin the render loop (after Dart finishes setup)
//   • pick {x,y} → { hitTile, tileX, tileY, tileDistance, hitEntity, entity,
//                    entityDistance }
//
// NOTE: reference implementation for the macOS build; not compiled in this
// environment (no Flutter macOS toolchain here). The Dart side and the C bridge
// ARE verified; this Swift glue is expected to need a first on-device pass.
import Cocoa
import FlutterMacOS
import QuartzCore
// Under Swift Package Manager the C bridge is its own module (CTessera). Under
// CocoaPods the whole plugin is a single module and the bridge's public header
// (tessera_bridge.h) is visible to Swift via the pod umbrella with no import.
#if canImport(CTessera)
import CTessera
#endif

final class TesseraPlatformView: NSView {
    private let channel: FlutterMethodChannel
    private var bridge: OpaquePointer?      // FTessera*
    // The SDL-created metal view (backed by the engine's swapchain CAMetalLayer),
    // reparented out of the hidden SDL window into this view.
    private var metalView: NSView?
    private var bufW = 0
    private var bufH = 0
    private var lastTime: CFTimeInterval = 0
    private var timer: Timer?

    init(viewId: Int64, messenger: FlutterBinaryMessenger, frame: CGRect) {
        channel = FlutterMethodChannel(
            name: "flutter_tessera/view_\(viewId)", binaryMessenger: messenger)
        super.init(frame: frame)

        // A plain layer-backed container; the reparented metal view is our only
        // subview and does the actual drawing.
        wantsLayer = true

        let (w, h) = drawablePixelSize()
        // asset_dir nil: macOS uses the engine's compile-time asset path (dev).
        bridge = ftessera_create(Int32(w), Int32(h), Float(scale()), nil)

        // Pull the engine's swapchain metal view into our hierarchy so its
        // CAMetalLayer composites in the Flutter window (zero-copy present).
        attachMetalView()

        channel.setMethodCallHandler { [weak self] call, result in
            self?.handle(call, result)
        }
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) not supported") }

    /// Find the SDL-created metal view in the hidden SDL window and reparent it
    /// into this view. Its CAMetalLayer is the engine's swapchain target.
    private func attachMetalView() {
        guard let bridge = bridge,
              let winPtr = ftessera_native_window(bridge) else {
            NSLog("flutter_tessera: no native window to reparent")
            return
        }
        let nsWindow = Unmanaged<NSWindow>.fromOpaque(winPtr).takeUnretainedValue()
        let tag = Int(ftessera_metal_view_tag(bridge))
        guard tag != 0, let mv = nsWindow.contentView?.viewWithTag(tag) else {
            NSLog("flutter_tessera: metal view (tag \(tag)) not found")
            return
        }
        mv.frame = bounds
        mv.autoresizingMask = [.width, .height]
        addSubview(mv)
        metalView = mv
    }

    // ---- method channel ----
    private func handle(_ call: FlutterMethodCall, _ result: @escaping FlutterResult) {
        switch call.method {
        case "handle":
            result(bridge != nil ? Int(ftessera_engine_handle(bridge)) : 0)
        case "start":
            start()
            result(nil)
        case "pick":
            guard let args = call.arguments as? [String: Any],
                  let x = args["x"] as? Double, let y = args["y"] as? Double else {
                result(FlutterError(code: "bad_args", message: "pick needs x,y", details: nil))
                return
            }
            result(pick(x, y))
        default:
            result(FlutterMethodNotImplemented)
        }
    }

    private func start() {
        guard timer == nil, bridge != nil else { return }
        lastTime = CACurrentMediaTime()
        // Drive on the main run loop so the render loop, channel handlers, and
        // AppKit all stay on the main thread — no cross-thread engine access.
        let t = Timer(timeInterval: 1.0 / 60.0, repeats: true) { [weak self] _ in
            self?.renderFrame()
        }
        RunLoop.main.add(t, forMode: .common)
        timer = t
    }

    private func renderFrame() {
        guard let bridge = bridge else { return }
        let (w, h) = drawablePixelSize()
        if w == 0 || h == 0 { return }

        if w != bufW || h != bufH {
            bufW = w; bufH = h
            ftessera_resize(bridge, Int32(w), Int32(h), Float(scale()))
            // Keep the reparented view filling us. It was attached before Flutter
            // sized the platform view, so its frame must be updated here — a zero
            // frame means the layer composites nothing.
            metalView?.frame = bounds
            // SDL's own drawable-size auto-update runs off SDL window events,
            // which we don't pump, so size the reparented layer ourselves.
            if let layer = metalView?.layer as? CAMetalLayer {
                layer.contentsScale = scale()
                layer.drawableSize = CGSize(width: w, height: h)
            }
        }

        let now = CACurrentMediaTime()
        let dt = min(0.1, now - lastTime)
        lastTime = now

        // Advance + render + present straight to the swapchain. No CPU copy.
        ftessera_present(bridge, dt)
    }

    private func pick(_ x: Double, _ y: Double) -> [String: Any] {
        var out = FTesseraPick()
        var hit = false
        if let bridge = bridge {
            hit = ftessera_pick(bridge, Float(x), Float(y), &out)
        }
        return [
            "hit": hit,
            "hitTile": out.hit_tile != 0,
            "tileX": Int(out.tile_x),
            "tileY": Int(out.tile_y),
            "tileDistance": Double(out.tile_distance),
            "hitEntity": out.hit_entity != 0,
            "entity": Int(bitPattern: UInt(out.entity)),
            "entityDistance": Double(out.entity_distance),
        ]
    }

    // ---- helpers ----
    private func scale() -> CGFloat { window?.backingScaleFactor ?? 2.0 }

    private func drawablePixelSize() -> (Int, Int) {
        let s = scale()
        let b = bounds
        return (Int((b.width * s).rounded()), Int((b.height * s).rounded()))
    }

    deinit {
        timer?.invalidate()
        channel.setMethodCallHandler(nil)
        // ftessera_destroy tears down the swapchain, which removes the metal view
        // from this view and releases SDL's reference to it.
        if let bridge = bridge { ftessera_destroy(bridge) }
    }
}
