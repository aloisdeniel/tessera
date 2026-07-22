// FlutterTesseraPlugin.swift — plugin entry point: registers the platform-view
// factory under the type id the Dart `TesseraView` uses ("flutter_tessera/view").
//
// NOTE: reference implementation for the macOS build (not compiled in this
// environment). See TesseraPlatformView.swift.
import Cocoa
import FlutterMacOS

public class FlutterTesseraPlugin: NSObject, FlutterPlugin {
    public static func register(with registrar: FlutterPluginRegistrar) {
        let factory = TesseraViewFactory(messenger: registrar.messenger)
        registrar.register(factory, withId: "flutter_tessera/view")
    }
}

final class TesseraViewFactory: NSObject, FlutterPlatformViewFactory {
    private let messenger: FlutterBinaryMessenger

    init(messenger: FlutterBinaryMessenger) {
        self.messenger = messenger
        super.init()
    }

    func create(withViewIdentifier viewId: Int64, arguments args: Any?) -> NSView {
        // Flutter retains the returned NSView, which owns the engine + loop.
        return TesseraPlatformView(viewId: viewId, messenger: messenger, frame: .zero)
    }

    func createArgsCodec() -> (FlutterMessageCodec & NSObjectProtocol)? {
        FlutterStandardMessageCodec.sharedInstance()
    }
}
