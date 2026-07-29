// flutter_tessera_web.dart — web plugin entry point.
//
// Registered through pubspec's `flutter.plugin.platforms.web`; the Flutter
// tool calls [FlutterTesseraWebPlugin.registerWith] at startup. All the real
// work lives in src/web/ (the conditional exports of flutter_tessera.dart);
// this just registers the `<canvas>` platform-view factory.

import 'package:flutter_web_plugins/flutter_web_plugins.dart';

import 'src/web/tessera_view_web.dart';

/// Web plugin registrant for flutter_tessera.
class FlutterTesseraWebPlugin {
  /// Called by the generated plugin registrant on web startup.
  static void registerWith(Registrar registrar) {
    registerTesseraViewFactory();
  }
}
