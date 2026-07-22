// Tests the native-library discovery logic. These need no native library — they
// exercise name resolution and the not-found failure path.

import 'package:tessera/tessera.dart';
import 'package:test/test.dart';

void main() {
  test('tesseraLibraryName is a platform-appropriate name', () {
    final name = tesseraLibraryName;
    expect(name, contains('tessera'));
    expect(
      name,
      anyOf(endsWith('.dylib'), endsWith('.so'), endsWith('.dll')),
    );
  });

  test('TesseraLibraryNotFound renders an actionable message', () {
    final e = TesseraLibraryNotFound(
      'libtessera.dylib',
      const ['/tmp/a/libtessera.dylib', '/tmp/b/libtessera.dylib'],
    );
    final msg = e.toString();
    expect(msg, contains('Could not load'));
    expect(msg, contains('TESSERA_LIBRARY_PATH'));
    expect(msg, contains('Locations searched'));
    expect(msg, contains('/tmp/a/libtessera.dylib'));
  });
}
