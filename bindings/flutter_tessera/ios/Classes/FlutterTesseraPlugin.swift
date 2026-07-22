// FlutterTesseraPlugin.swift (iOS) — registers the platform-view factory under
// the type id the Dart `TesseraView` uses ("flutter_tessera/view").
//
// NOTE: reference implementation for the iOS build (not compiled here).
import Flutter
import UIKit

public class FlutterTesseraPlugin: NSObject, FlutterPlugin {
    public static func register(with registrar: FlutterPluginRegistrar) {
        let factory = TesseraViewFactory(messenger: registrar.messenger())
        registrar.register(factory, withId: "flutter_tessera/view")
    }
}

final class TesseraViewFactory: NSObject, FlutterPlatformViewFactory {
    private let messenger: FlutterBinaryMessenger

    init(messenger: FlutterBinaryMessenger) {
        self.messenger = messenger
        super.init()
    }

    func create(withFrame frame: CGRect, viewIdentifier viewId: Int64,
                arguments args: Any?) -> FlutterPlatformView {
        return TesseraPlatformView(frame: frame, viewId: viewId, messenger: messenger)
    }

    func createArgsCodec() -> FlutterMessageCodec & NSObjectProtocol {
        FlutterStandardMessageCodec.sharedInstance()
    }
}
