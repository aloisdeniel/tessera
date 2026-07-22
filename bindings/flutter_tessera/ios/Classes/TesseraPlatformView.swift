// TesseraPlatformView.swift (iOS) — one embedded Tessera engine in a UIView.
//
// The UIKit counterpart of the macOS platform view: a UIView backed by a
// CAMetalLayer, owning the engine (via the C bridge) and a CADisplayLink render
// loop on the main thread. Same per-view method channel (handle / start / pick).
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
    private let container: TesseraMetalView
    private let channel: FlutterMethodChannel
    private var bridge: OpaquePointer?      // FTessera*
    private var presenter: MetalPresenter?
    private var buffer = [UInt8]()
    private var bufW = 0
    private var bufH = 0
    private var lastTime: CFTimeInterval = 0
    private var link: CADisplayLink?

    init(frame: CGRect, viewId: Int64, messenger: FlutterBinaryMessenger) {
        container = TesseraMetalView(frame: frame)
        channel = FlutterMethodChannel(
            name: "flutter_tessera/view_\(viewId)", binaryMessenger: messenger)
        super.init()

        presenter = MetalPresenter()
        container.metalLayer.device = presenter?.device
        container.metalLayer.pixelFormat = .bgra8Unorm

        let (w, h) = drawablePixelSize()
        // Bundled shaders live next to the plugin resources; hand the engine the
        // directory that contains `shaders/`.
        let dir = TesseraPlatformView.assetDir()
        bridge = dir.withCString { ftessera_create(Int32(w), Int32(h), Float(scale()), $0) }
        if bridge == nil {
            let err = String(cString: ftessera_create_error())
            NSLog("flutter_tessera: engine create FAILED — \(err) (assetDir=\(dir))")
        }

        channel.setMethodCallHandler { [weak self] call, result in
            self?.handle(call, result)
        }
    }

    func view() -> UIView { container }

    private func handle(_ call: FlutterMethodCall, _ result: @escaping FlutterResult) {
        switch call.method {
        case "handle":
            result(bridge != nil ? Int(ftessera_engine_handle(bridge)) : 0)
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
        let l = CADisplayLink(target: self, selector: #selector(renderFrame))
        l.add(to: .main, forMode: .common)
        link = l
    }

    @objc private func renderFrame() {
        guard let bridge = bridge, let presenter = presenter else { return }
        let (w, h) = drawablePixelSize()
        if w == 0 || h == 0 { return }

        if w != bufW || h != bufH {
            bufW = w; bufH = h
            buffer = [UInt8](repeating: 0, count: w * h * 4)
            ftessera_resize(bridge, Int32(w), Int32(h), Float(scale()))
            container.metalLayer.drawableSize = CGSize(width: w, height: h)
        }

        let now = CACurrentMediaTime()
        let dt = min(0.1, now - lastTime)
        lastTime = now

        let ok = buffer.withUnsafeMutableBytes { raw -> Bool in
            ftessera_render_rgba(bridge, dt, Int32(w), Int32(h), raw.baseAddress, raw.count)
        }
        if !ok { return }
        buffer.withUnsafeBytes { raw in
            if let base = raw.baseAddress {
                presenter.present(rgba: base, width: w, height: h, layer: container.metalLayer)
            }
        }
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

    /// The directory that contains the bundled `shaders/` folder. The podspec
    /// bundles the engine assets as `tessera_assets.bundle`.
    private static func assetDir() -> String {
        let base = Bundle(for: TesseraPlatformView.self)
        if let url = base.url(forResource: "tessera_assets", withExtension: "bundle") {
            return url.path
        }
        return base.bundlePath
    }

    deinit {
        link?.invalidate()
        channel.setMethodCallHandler(nil)
        if let bridge = bridge { ftessera_destroy(bridge) }
    }
}

/// A UIView whose backing layer is a CAMetalLayer.
final class TesseraMetalView: UIView {
    override class var layerClass: AnyClass { CAMetalLayer.self }
    var metalLayer: CAMetalLayer { layer as! CAMetalLayer }
}
