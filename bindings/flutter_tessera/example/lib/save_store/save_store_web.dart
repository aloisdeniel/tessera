// save_store_web.dart — web save store: base64 bytes in localStorage.
import 'dart:convert';
import 'dart:typed_data';

import 'package:web/web.dart' as web;

class SaveStore {
  static String _key(String key) => 'tessera.save.$key';

  static bool exists(String key) =>
      web.window.localStorage.getItem(_key(key)) != null;

  /// Returns the stored bytes, or null when absent/unreadable.
  static Uint8List? read(String key) {
    try {
      final s = web.window.localStorage.getItem(_key(key));
      return s == null ? null : base64Decode(s);
    } catch (_) {
      return null;
    }
  }

  /// Stores [bytes]; returns false on failure (e.g. quota exceeded).
  static bool write(String key, Uint8List bytes) {
    try {
      web.window.localStorage.setItem(_key(key), base64Encode(bytes));
      return true;
    } catch (_) {
      return false;
    }
  }
}
