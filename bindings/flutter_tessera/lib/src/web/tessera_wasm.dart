// tessera_wasm.dart — JS-interop wrapper around the Emscripten-built engine
// (assets/web/tessera_web.js + .wasm, produced by tools/web/build_web.sh).
//
// The module is compiled with ASYNCIFY: any export may suspend (WebGPU device
// creation, swapchain waits) and calling a second export while one is
// suspended corrupts the Asyncify state. Every call therefore goes through a
// single promise chain (`_chain`) so exactly one wasm call is in flight at a
// time. `call()` returns the export's result as a Future; fire-and-forget
// callers may ignore the Future — ordering is still guaranteed.
//
// Web-only: this file must only be imported behind a `dart.library.js_interop`
// conditional import.
library;

import 'dart:async';
import 'dart:js_interop';
import 'dart:js_interop_unsafe';
import 'dart:typed_data';
import 'dart:ui_web' as ui_web;

import 'package:web/web.dart' as web;

/// Raw Emscripten module object (subset we use).
extension type _EmscriptenModule(JSObject _) implements JSObject {
  external JSAny? ccall(
      JSString name, JSString? returnType, JSArray<JSAny?> argTypes,
      JSArray<JSAny?> args, JSObject opts);
  external JSNumber _malloc(JSNumber size);
  external void _free(JSNumber ptr);
  external JSString UTF8ToString(JSNumber ptr);

  // Heap views. Re-read on every use: they are detached whenever the wasm
  // memory grows (ALLOW_MEMORY_GROWTH=1).
  external JSUint8Array get HEAPU8;
}

@JS('createTesseraModule')
external JSPromise<JSObject> _createTesseraModule();

/// Wraps an `int` argument that must cross into wasm as a 64-bit integer
/// (engine ids, operation ids). Plain ints are passed as JS numbers, which
/// emscripten rejects for `uint64_t` parameters under WASM_BIGINT.
/// (A real class, not an extension type: the ccall marshaller distinguishes
/// it from ordinary ints at runtime.)
class WasmU64 {
  const WasmU64(this.value);
  final int value;
}

/// One loaded tessera wasm module (a single engine process).
///
/// All exported C functions are reached through [call] / [callSync]. Pointers
/// are Dart ints (wasm32 addresses).
class TesseraWasm {
  TesseraWasm._(this._module);

  final _EmscriptenModule _module;
  Future<void> _chain = Future.value();

  static Future<TesseraWasm>? _loading;

  /// Loads tessera_web.js (once per page) and instantiates the module.
  static Future<TesseraWasm> load() {
    if (web.window.location.search.contains('wasmtrace')) traceCalls = true;
    return _loading ??= _load();
  }

  static Future<TesseraWasm> _load() async {
    if (!web.window.hasProperty('createTesseraModule'.toJS).toDart) {
      final src = ui_web.assetManager
          .getAssetUrl('packages/flutter_tessera/assets/web/tessera_web.js');
      final script = web.HTMLScriptElement()..src = src;
      final loaded = Completer<void>();
      script.onLoad.listen((_) => loaded.complete());
      script.onError.listen((_) => loaded.completeError(
          StateError('flutter_tessera: failed to load $src')));
      web.document.head!.append(script);
      await loaded.future;
    }
    final module = _EmscriptenModule(await _createTesseraModule().toDart);
    // Debug hook: lets DevTools poke exported functions directly
    // (`__tesseraModule.ccall(...)`). Harmless in production.
    web.window.setProperty('__tesseraModule'.toJS, module);
    return TesseraWasm._(module);
  }

  /// Calls exported C function [name], serialized behind every earlier call.
  ///
  /// [returns]: 'number', 'string' or null (void). Arguments may be [int],
  /// [double], [String] or null. Returns int / double / String / null.
  /// Set true (e.g. from tests) to trace every wasm call on the console.
  static bool traceCalls = false;

  Future<Object?> call(String name, String? returns, List<Object?> args) {
    // ignore: avoid_print
    if (traceCalls) print('[wasmtrace] queue $name');
    final result = _chain.then((_) async {
      // ignore: avoid_print
      if (traceCalls) print('[wasmtrace] run $name');
      final argTypes = args
          .map<JSAny?>((a) => (a is String ? 'string' : 'number').toJS)
          .toList()
          .toJS;
      final jsArgs = args.map<JSAny?>(_toJs).toList().toJS;
      final opts = JSObject()..setProperty('async'.toJS, true.toJS);
      final r = _module.ccall(
          name.toJS, returns?.toJS, argTypes, jsArgs, opts);
      final resolved =
          r.isA<JSPromise>() ? await (r as JSPromise).toDart : r;
      // ignore: avoid_print
      if (traceCalls) print('[wasmtrace] done $name');
      return _fromJs(resolved, returns);
    });
    // Keep the chain alive even when a call fails; the error still reaches
    // the caller through `result`.
    _chain = result.then((_) {}, onError: (_) {});
    return result;
  }

  static JSAny? _toJs(Object? a) => switch (a) {
        null => null,
        final WasmU64 v => _jsBigInt(v.value),
        final int v => v.toJS,
        final double v => v.toJS,
        final String v => v.toJS,
        _ => throw ArgumentError('unsupported ccall argument: $a'),
      };

  /// `uint64_t` parameters cross the JS boundary as BigInt (WASM_BIGINT).
  static JSAny _jsBigInt(int value) => (web.window
      .callMethod('BigInt'.toJS, value.toString().toJS)) as JSAny;

  static Object? _fromJs(JSAny? v, String? returns) {
    if (v == null || returns == null) return null;
    if (returns == 'string') return (v as JSString).toDart;
    if (v.isA<JSBigInt>()) {
      return int.parse((v as JSBigInt).toString());
    }
    final n = (v as JSNumber).toDartDouble;
    return n == n.truncateToDouble() && n.abs() < 9007199254740992
        ? n.toInt()
        : n;
  }

  // ---- heap helpers (synchronous: plain memory access, no wasm calls) ----

  /// Allocates [size] bytes in the wasm heap. Free with [free].
  int malloc(int size) =>
      (_module.callMethod('_malloc'.toJS, size.toJS) as JSNumber).toDartInt;

  void free(int ptr) {
    if (ptr != 0) _module.callMethod('_free'.toJS, ptr.toJS);
  }

  /// Copies [bytes] into freshly allocated wasm memory; returns the pointer.
  int allocBytes(Uint8List bytes) {
    final ptr = malloc(bytes.isEmpty ? 1 : bytes.length);
    heapU8.toDart.setRange(ptr, ptr + bytes.length, bytes);
    return ptr;
  }

  /// Current heap view — never cache across an awaited wasm call.
  JSUint8Array get heapU8 => _module.HEAPU8;

  Uint8List readBytes(int ptr, int length) =>
      Uint8List.fromList(Uint8List.sublistView(heapU8.toDart, ptr, ptr + length));

  ByteData readData(int ptr, int length) => ByteData.sublistView(
      readBytes(ptr, length));

  void writeBytes(int ptr, Uint8List bytes) =>
      heapU8.toDart.setRange(ptr, ptr + bytes.length, bytes);

  String readCString(int ptr) =>
      ptr == 0 ? '' : _module.UTF8ToString(ptr.toJS).toDart;
}
