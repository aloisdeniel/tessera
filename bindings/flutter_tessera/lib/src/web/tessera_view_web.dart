// tessera_view_web.dart — the web implementation of TesseraView.
//
// On the web the "platform view" is a plain `<canvas>` element registered with
// the web platform-view registry; the WebGPU engine (an Emscripten wasm
// module, see tessera_controller_web.dart) binds SDL to that canvas and the
// Dart-side controller drives the render loop.
//
// LIMITATION: at most ONE TesseraView may be live at a time on the web — SDL
// binds a single canvas per wasm module. Creating a second concurrent view
// throws a [StateError]; dispose the previous view (and its controller) first.

import 'dart:ui_web' as ui_web;

import 'package:flutter/foundation.dart';
import 'package:flutter/gestures.dart';
import 'package:flutter/widgets.dart';
import 'package:web/web.dart' as web;

import 'tessera_controller_web.dart';

/// The platform-view type id (same id as the native platforms).
const String kTesseraViewType = 'flutter_tessera/view';

/// Signature for [TesseraView.onCreated].
typedef TesseraCreatedCallback = void Function(TesseraController controller);

bool _factoryRegistered = false;

/// Register the `<canvas>` platform-view factory (idempotent). Called from the
/// web plugin entry point and defensively from [TesseraView].
void registerTesseraViewFactory() {
  if (_factoryRegistered) return;
  _factoryRegistered = true;
  ui_web.platformViewRegistry.registerViewFactory(kTesseraViewType,
      (int viewId) {
    final canvas = web.HTMLCanvasElement()..id = 'tessera-canvas-$viewId';
    canvas.style
      ..width = '100%'
      ..height = '100%'
      ..display = 'block'
      // Let Flutter's gesture system see pointer events (the host app wraps
      // the view in GestureDetectors; picking goes through controller.pick,
      // never through DOM events on the canvas).
      ..pointerEvents = 'none';
    return canvas;
  });
}

/// Embeds a live Tessera scene in the widget tree — web implementation,
/// mirroring the io TesseraView's API.
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
///
/// Only one TesseraView may be live at a time on the web (SDL binds one
/// canvas); a second concurrent view throws a [StateError].
class TesseraView extends StatefulWidget {
  const TesseraView({
    super.key,
    required this.onCreated,
    this.gestureRecognizers = const <Factory<OneSequenceGestureRecognizer>>{},
    this.library,
  });

  /// Fires once the canvas + engine exist, with a controller to drive them.
  final TesseraCreatedCallback onCreated;

  /// Accepted for API compatibility with the native platforms; pointer events
  /// pass through the canvas to Flutter's own gesture system on the web.
  final Set<Factory<OneSequenceGestureRecognizer>> gestureRecognizers;

  /// Accepted for API compatibility with the io platforms (there it overrides
  /// native-library symbol resolution); ignored on the web.
  final Object? library;

  @override
  State<TesseraView> createState() => _TesseraViewState();
}

class _TesseraViewState extends State<TesseraView> {
  /// Guards the one-live-view-at-a-time constraint at the widget level (the
  /// controller enforces it again at attach time).
  static bool _mounted = false;

  @override
  void initState() {
    super.initState();
    if (_mounted) {
      throw StateError(
          'flutter_tessera: only one TesseraView may be live at a time on the '
          'web (the SDL/WebGPU engine binds a single canvas). Dispose the '
          'previous view first.');
    }
    _mounted = true;
    registerTesseraViewFactory();
  }

  @override
  void dispose() {
    _mounted = false;
    super.dispose();
  }

  Future<void> _onPlatformViewCreated(int id) async {
    final TesseraController controller;
    try {
      controller = await TesseraController.attach(id, library: widget.library);
    } catch (error, stack) {
      // Surface the failure — an unhandled rejection here would leave the
      // view silently blank with no diagnostic.
      FlutterError.reportError(FlutterErrorDetails(
        exception: error,
        stack: stack,
        library: 'flutter_tessera',
        context: ErrorDescription('while attaching the web Tessera engine'),
      ));
      return;
    }
    if (!mounted) {
      await controller.dispose();
      return;
    }
    widget.onCreated(controller);
  }

  @override
  Widget build(BuildContext context) {
    // Two halves of making the embedded canvas behave like the native
    // platform views for input:
    //  - the DOM side falls through (the canvas + its flt-platform-view
    //    wrapper are pointer-events:none, see TesseraController.attach), so
    //    browser events reach Flutter's glass pane; and
    //  - an opaque, transparent hit-test surface stacked above the
    //    HtmlElementView makes this region hit-testable inside Flutter, so
    //    ancestor GestureDetectors receive taps/drags. HtmlElementView itself
    //    never participates in framework hit testing on web.
    return Stack(
      fit: StackFit.expand,
      children: [
        HtmlElementView(
          viewType: kTesseraViewType,
          onPlatformViewCreated: _onPlatformViewCreated,
        ),
        const Positioned.fill(
          child: Listener(
            behavior: HitTestBehavior.opaque,
            child: SizedBox.expand(),
          ),
        ),
      ],
    );
  }
}
