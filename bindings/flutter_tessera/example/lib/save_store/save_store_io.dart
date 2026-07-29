// save_store_io.dart — desktop/mobile save store: a file in the temp dir.
import 'dart:io';
import 'dart:typed_data';

class SaveStore {
  static String _path(String key) => '${Directory.systemTemp.path}/$key.bin';

  static bool exists(String key) => File(_path(key)).existsSync();

  /// Returns the stored bytes, or null when absent/unreadable.
  static Uint8List? read(String key) {
    try {
      return File(_path(key)).readAsBytesSync();
    } catch (_) {
      return null;
    }
  }

  /// Stores [bytes]; returns false on failure (disk trouble is the caller's
  /// status note, never a crash).
  static bool write(String key, Uint8List bytes) {
    try {
      File(_path(key)).writeAsBytesSync(bytes);
      return true;
    } catch (_) {
      return false;
    }
  }
}
