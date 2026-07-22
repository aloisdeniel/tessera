// tessera_view.dart — the embedded renderer widget (a native platform view).
//
// On macOS this is an `AppKitView` whose NSView hosts a CAMetalLayer; the native
// plugin creates the Tessera engine bound to it and drives the render loop on
// the main thread. When the view is created, [onCreated] fires with a
// [TesseraController] ready for setup.

import 'dart:ffi';

import 'package:flutter/foundation.dart';
import 'package:flutter/gestures.dart';
import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart';

import 'tessera_controller.dart';

/// The platform-view type id registered by the native plugin.
const String kTesseraViewType = 'flutter_tessera/view';

/// Signature for [TesseraView.onCreated].
typedef TesseraCreatedCallback = void Function(TesseraController controller);

/// Embeds a live Tessera scene in the widget tree.
///
/// ```dart
/// TesseraView(
///   onCreated: (c) async {
///     final tile = c.registerTileType(const TesseraTileType(...));
///     // ... register entities, set light/quality/timing ...
///     c.setScene(buildScene());
///     await c.start();
///   },
/// )
/// ```
class TesseraView extends StatelessWidget {
  const TesseraView({
    super.key,
    required this.onCreated,
    this.gestureRecognizers = const <Factory<OneSequenceGestureRecognizer>>{},
    this.library,
  });

  /// Fires once the native view + engine exist, with a controller to drive them.
  final TesseraCreatedCallback onCreated;

  /// Gesture recognizers the platform view should claim (pass-through hit-test).
  final Set<Factory<OneSequenceGestureRecognizer>> gestureRecognizers;

  /// Optional override for native-library symbol resolution (see
  /// [TesseraController.attach]).
  final DynamicLibrary? library;

  Future<void> _onPlatformViewCreated(int id) async {
    final controller = await TesseraController.attach(id, library: library);
    onCreated(controller);
  }

  @override
  Widget build(BuildContext context) {
    const params = <String, Object?>{};
    const codec = StandardMessageCodec();
    switch (defaultTargetPlatform) {
      case TargetPlatform.macOS:
        return AppKitView(
          viewType: kTesseraViewType,
          onPlatformViewCreated: _onPlatformViewCreated,
          gestureRecognizers: gestureRecognizers,
          creationParams: params,
          creationParamsCodec: codec,
        );
      case TargetPlatform.iOS:
        return UiKitView(
          viewType: kTesseraViewType,
          onPlatformViewCreated: _onPlatformViewCreated,
          gestureRecognizers: gestureRecognizers,
          creationParams: params,
          creationParamsCodec: codec,
        );
      case TargetPlatform.android:
        return AndroidView(
          viewType: kTesseraViewType,
          onPlatformViewCreated: _onPlatformViewCreated,
          gestureRecognizers: gestureRecognizers,
          creationParams: params,
          creationParamsCodec: codec,
        );
      default:
        return const _Unsupported();
    }
  }
}

class _Unsupported extends StatelessWidget {
  const _Unsupported();

  @override
  Widget build(BuildContext context) {
    return const ColoredBox(
      color: Color(0xFF1A1D24),
      child: Center(
        child: Text(
          'flutter_tessera: this platform is not supported yet.\n'
          'A macOS (Metal) reference implementation is provided; iOS and '
          'Android are stubbed.',
          textAlign: TextAlign.center,
          style: TextStyle(color: Color(0xFFB0B4BC), fontSize: 13),
        ),
      ),
    );
  }
}
