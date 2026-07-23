// TesseraPlatformView.swift (iOS) — one embedded Tessera engine in a UIView.
//
// The UIKit counterpart of the macOS platform view: a container UIView that
// owns the engine (via the C bridge) and a CADisplayLink render loop on the main
// thread. Presentation is zero-copy — the engine renders to the SDL window's
// swapchain, and we reparent that swapchain's CAMetalLayer-backed metal view
// into the container, so frames scan out straight into the Flutter surface with
// no CPU round-trip or blit. Same per-view method channel (handle / start / pick).
//
// NOTE: reference implementation for the iOS build; not compiled in this
// environment (no iOS toolchain/device here). The C bridge and shaders ARE
// verified; the macOS sibling is verified end-to-end.
import Flutter
import UIKit
import QuartzCore
#if canImport(CTessera)
import CTessera
#endif

final class TesseraPlatformView: NSObject, FlutterPlatformView {
    private let container: UIView
    private let channel: FlutterMethodChannel
    private var bridge: OpaquePointer?      // FTessera*
    // The SDL-created metal view (backed by the engine's swapchain CAMetalLayer),
    // reparented out of the hidden SDL window into `container`.
    private var metalView: UIView?
    private var bufW = 0
    private var bufH = 0
    private var lastTime: CFTimeInterval = 0
    private var link: CADisplayLink?

    init(frame: CGRect, viewId: Int64, messenger: FlutterBinaryMessenger) {
        container = UIView(frame: frame)
        channel = FlutterMethodChannel(
            name: "flutter_tessera/view_\(viewId)", binaryMessenger: messenger)
        super.init()

        container.backgroundColor = .clear

        let (w, h) = drawablePixelSize()
        // Bundled shaders live next to the plugin resources; hand the engine the
        // directory that contains `shaders/`.
        let dir = TesseraPlatformView.assetDir()
        bridge = dir.withCString { ftessera_create(Int32(w), Int32(h), Float(scale()), $0) }
        if bridge == nil {
            let err = String(cString: ftessera_create_error())
            NSLog("flutter_tessera: engine create FAILED — \(err) (assetDir=\(dir))")
        }

        // Pull the engine's swapchain metal view into our hierarchy so its
        // CAMetalLayer composites in the Flutter window (zero-copy present).
        attachMetalView()

        channel.setMethodCallHandler { [weak self] call, result in
            self?.handle(call, result)
        }
    }

    func view() -> UIView { container }

    /// Find the SDL-created metal view in the hidden SDL window and reparent it
    /// into the container. Its CAMetalLayer is the engine's swapchain target.
    /// Returns true once attached. Idempotent; safe to retry.
    @discardableResult
    private func attachMetalView() -> Bool {
        if metalView != nil { return true }
        guard let bridge = bridge,
              let winPtr = ftessera_native_window(bridge) else {
            NSLog("flutter_tessera: no native window to reparent")
            return false
        }
        let uiWindow = Unmanaged<UIWindow>.fromOpaque(winPtr).takeUnretainedValue()
        let tag = Int(ftessera_metal_view_tag(bridge))
        // The SDL metal view is the hidden window's rootViewController.view. That
        // window is never made key-and-visible, so UIKit doesn't add the root
        // view into window.subviews — window.viewWithTag(...) can't reach it.
        // Go through the root view controller's view directly (then its subtree).
        var mv = uiWindow.viewWithTag(tag)
        if mv == nil, let root = uiWindow.rootViewController?.view {
            mv = (root.tag == tag) ? root : root.viewWithTag(tag)
        }
        guard tag != 0, let metal = mv else { return false }

        metal.frame = container.bounds
        metal.autoresizingMask = [.flexibleWidth, .flexibleHeight]
        // The SDL view feeds SDL's (unpumped) event queue; let touches fall
        // through to Flutter's gesture recognizers instead.
        metal.isUserInteractionEnabled = false
        container.addSubview(metal)
        metalView = metal
        return true
    }

    private func handle(_ call: FlutterMethodCall, _ result: @escaping FlutterResult) {
        switch call.method {
        case "handle":
            if let bridge = bridge {
                result(Int(ftessera_engine_handle(bridge)))
            } else {
                result(FlutterError(code: "no_engine",
                                    message: String(cString: ftessera_create_error()),
                                    details: nil))
            }
        case "start":
            start()
            result(nil)
        case "pick":
            guard let a = call.arguments as? [String: Any],
                  let x = a["x"] as? Double, let y = a["y"] as? Double else {
                result(FlutterError(code: "bad_args", message: "pick needs x,y", details: nil))
                return
            }
            result(pick(x, y))
        default:
            result(FlutterMethodNotImplemented)
        }
    }

    private func start() {
        guard link == nil, bridge != nil else { return }
        lastTime = CACurrentMediaTime()
        // A CADisplayLink retains its target and the run loop retains the link,
        // so targeting `self` directly would keep this view (and its engine)
        // alive forever — leaking on every teardown and, on hot restart, stacking
        // a second engine on the un-released old one. Drive it through a weak
        // proxy so `deinit` runs and tears the engine down.
        let l = CADisplayLink(target: WeakRenderProxy(self),
                              selector: #selector(WeakRenderProxy.tick))
        l.add(to: .main, forMode: .common)
        link = l
    }

    fileprivate func renderFrame() {
        guard let bridge = bridge else { return }

        // The metal view may not be reachable at init time; keep trying for a
        // short while (capped so a genuine failure logs once and stops).
        if metalView == nil && attachTries < 60 {
            attachTries += 1
            if !attachMetalView() && attachTries == 60 {
                NSLog("flutter_tessera: metal view (tag \(Int(ftessera_metal_view_tag(bridge)))) not found after \(attachTries) tries")
            }
        }

        let (w, h) = drawablePixelSize()
        if w == 0 || h == 0 { return }

        var sizeChanged = false
        if w != bufW || h != bufH {
            bufW = w; bufH = h
            sizeChanged = true
            ftessera_resize(bridge, Int32(w), Int32(h), Float(scale()))
            // Keep the reparented view filling the container. It was attached
            // before Flutter sized the platform view, so its frame must be
            // updated here (autoresizing alone doesn't fire reliably) — a zero
            // frame means the layer composites nothing and, on iOS, SDL's
            // layoutSubviews forces drawableSize to 0.
            metalView?.frame = container.bounds
            // SDL's own drawable-size auto-update runs off SDL window events,
            // which we don't pump, so size the reparented layer ourselves.
            if let layer = metalView?.layer as? CAMetalLayer {
                layer.contentsScale = scale()
                layer.drawableSize = CGSize(width: w, height: h)
            }
            logState(w: w, h: h)
        }

        let now = CACurrentMediaTime()
        let dt = min(0.1, now - lastTime)
        lastTime = now

        // Advance + render + present straight to the swapchain. No CPU copy.
        ftessera_present(bridge, dt)

        // Notify Dart AFTER present so the pending scene has been promoted this
        // tick — cameraFitDistance fits to live tiles, which don't exist until
        // the first tick. Sending before present would race the initial fit.
        if sizeChanged {
            channel.invokeMethod("resize", arguments: [
                "width": Double(container.bounds.width),
                "height": Double(container.bounds.height),
            ])
        }
        let err = String(cString: ftessera_last_error(bridge))
        if !err.isEmpty && err != lastLoggedError {
            lastLoggedError = err
            NSLog("flutter_tessera: present error — \(err)")
        }
    }

    private var lastLoggedError = ""
    private var attachTries = 0

    /// One log line per size change: confirms the metal view is attached and
    /// sized, and reports the drawable it presents into.
    private func logState(w: Int, h: Int) {
        let ds = (metalView?.layer as? CAMetalLayer)?.drawableSize ?? .zero
        NSLog("flutter_tessera: view=\(metalView != nil) container=\(container.bounds.size) "
            + "mvFrame=\(metalView?.frame.size ?? .zero) drawable=\(ds) px=\(w)x\(h)")
    }

    private func pick(_ x: Double, _ y: Double) -> [String: Any] {
        var out = FTesseraPick()
        var hit = false
        if let bridge = bridge { hit = ftessera_pick(bridge, Float(x), Float(y), &out) }
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

    private func scale() -> CGFloat { container.window?.screen.scale ?? UIScreen.main.scale }

    private func drawablePixelSize() -> (Int, Int) {
        let s = scale()
        let b = container.bounds
        return (Int((b.width * s).rounded()), Int((b.height * s).rounded()))
    }

    /// The directory that contains the bundled `shaders/` folder. Swift Package
    /// Manager copies `Resources/shaders` into the target's resource bundle
    /// (`Bundle.module`), so `shaders/` sits at its root; CocoaPods instead
    /// bundles the engine assets as `tessera_assets.bundle`.
    private static func assetDir() -> String {
        #if SWIFT_PACKAGE
        return Bundle.module.bundlePath
        #else
        let base = Bundle(for: TesseraPlatformView.self)
        if let url = base.url(forResource: "tessera_assets", withExtension: "bundle") {
            return url.path
        }
        return base.bundlePath
        #endif
    }

    deinit {
        link?.invalidate()
        channel.setMethodCallHandler(nil)
        // ftessera_destroy tears down the swapchain, which removes the metal view
        // from the container and releases SDL's reference to it.
        if let bridge = bridge { ftessera_destroy(bridge) }
    }
}

/// Weak forwarder so the CADisplayLink does not retain the platform view (which
/// would prevent teardown; see start()).
private final class WeakRenderProxy {
    weak var target: TesseraPlatformView?
    init(_ target: TesseraPlatformView) { self.target = target }
    @objc func tick() { target?.renderFrame() }
}
