// save_store.dart — tiny cross-platform byte store for game saves.
//
// The io implementation writes a file under the system temp directory; the
// web implementation keeps the bytes base64-encoded in localStorage (save
// blobs are a few KB, far under the storage quota). Selected at compile time
// so no dart:io reaches the web build.
export 'save_store_io.dart' if (dart.library.js_interop) 'save_store_web.dart';
