// library.dart — locate and open the native Tessera shared library.
//
// The Dart `tessera` package is a thin FFI binding: it does NOT bundle the
// compiled engine. You build `libtessera` from the C sources once (see the
// package README) and point Dart at it. Resolution order, first hit wins:
//
//   1. An explicit path passed to `Tessera(libraryPath: ...)`.
//   2. $TESSERA_LIBRARY_PATH   — full path to the shared library file.
//   3. $TESSERA_LIBRARY_DIR    — a directory containing it (name inferred).
//   4. A handful of conventional locations relative to the CWD and the running
//      script (./, ./build/, ../build/, ../../build/, next to the executable).
//   5. The bare platform name, letting the OS loader search its own paths
//      (DYLD_LIBRARY_PATH / LD_LIBRARY_PATH / PATH, rpaths, /usr/local/lib …).
//
// On a miss we throw with the full list of places we looked, so the failure is
// actionable rather than a bare "image not found".

import 'dart:ffi';
import 'dart:io';

/// Platform-specific default file name for the shared library.
String get tesseraLibraryName {
  if (Platform.isMacOS) return 'libtessera.dylib';
  if (Platform.isWindows) return 'tessera.dll';
  return 'libtessera.so'; // Linux, Android, and other ELF platforms.
}

/// Open the Tessera native library, searching the conventional locations.
///
/// Pass [path] to bypass discovery entirely (either a full file path or a
/// directory containing the library). Throws [TesseraLibraryNotFound] with the
/// searched candidates if nothing loads.
DynamicLibrary openTesseraLibrary({String? path}) {
  final name = tesseraLibraryName;
  final tried = <String>[];

  DynamicLibrary? tryOpen(String candidate) {
    tried.add(candidate);
    try {
      return DynamicLibrary.open(candidate);
    } on ArgumentError {
      return null;
    } catch (_) {
      return null;
    }
  }

  // Normalize a path that may be a directory into a file path.
  String asFile(String p) {
    if (FileSystemEntity.isDirectorySync(p)) {
      return '$p${Platform.pathSeparator}$name';
    }
    return p;
  }

  // 1. Explicit override.
  if (path != null && path.isNotEmpty) {
    final lib = tryOpen(asFile(path));
    if (lib != null) return lib;
  }

  // 2 & 3. Environment overrides.
  final env = Platform.environment;
  final envPath = env['TESSERA_LIBRARY_PATH'];
  if (envPath != null && envPath.isNotEmpty) {
    final lib = tryOpen(asFile(envPath));
    if (lib != null) return lib;
  }
  final envDir = env['TESSERA_LIBRARY_DIR'];
  if (envDir != null && envDir.isNotEmpty) {
    final lib = tryOpen('$envDir${Platform.pathSeparator}$name');
    if (lib != null) return lib;
  }

  // 4. Conventional locations relative to the CWD and the running script.
  final roots = <String>{
    Directory.current.path,
    _dir(Platform.resolvedExecutable),
  };
  final scriptPath = Platform.script.toFilePath();
  if (scriptPath.isNotEmpty) roots.add(_dir(scriptPath));

  const subdirs = ['', 'build', '../build', '../../build', '../../../build'];
  for (final root in roots) {
    for (final sub in subdirs) {
      final dir = sub.isEmpty ? root : '$root${Platform.pathSeparator}$sub';
      final lib = tryOpen('$dir${Platform.pathSeparator}$name');
      if (lib != null) return lib;
    }
  }

  // 5. Let the OS loader search its own configured paths.
  final lib = tryOpen(name);
  if (lib != null) return lib;

  throw TesseraLibraryNotFound(name, tried);
}

String _dir(String filePath) {
  final i = filePath.lastIndexOf(Platform.pathSeparator);
  return i <= 0 ? '.' : filePath.substring(0, i);
}

/// Thrown when the native library cannot be located or loaded.
class TesseraLibraryNotFound implements Exception {
  TesseraLibraryNotFound(this.name, this.tried);

  final String name;
  final List<String> tried;

  @override
  String toString() {
    final buf = StringBuffer()
      ..writeln('Could not load the Tessera native library ($name).')
      ..writeln('Build it from the C sources, then make it discoverable via:')
      ..writeln('  • Tessera(libraryPath: "/path/to/$name"), or')
      ..writeln('  • the TESSERA_LIBRARY_PATH / TESSERA_LIBRARY_DIR env vars, or')
      ..writeln('  • placing it in ./ or ./build/ next to where you run Dart.')
      ..writeln('Locations searched:');
    for (final t in tried) {
      buf.writeln('  - $t');
    }
    return buf.toString();
  }
}
