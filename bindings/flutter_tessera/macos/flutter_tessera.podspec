#
# flutter_tessera macOS plugin.
#
# Reference build configuration. The plugin links the prebuilt `libtessera` and
# `SDL3`; build libtessera from the repo first (see the repo README) and install
# SDL3 (`brew install sdl3`). Paths below assume Homebrew + the repo's `build/`
# dir and can be overridden with the TESSERA_LIB_DIR / SDL headers as needed.
#
# NOTE: the Swift Package Manager manifest (macos/flutter_tessera/Package.swift)
# is the primary, documented build path. This podspec mirrors it for CocoaPods
# projects and may need per-project path adjustments; it is not verified here.
#
Pod::Spec.new do |s|
  s.name             = 'flutter_tessera'
  s.version          = '0.1.0'
  s.summary          = 'Flutter platform-view embedding of the Tessera 3D board-game renderer.'
  s.description      = <<-DESC
Flutter platform-view embedding of the Tessera 3D board-game renderer.
                       DESC
  s.homepage         = 'https://github.com/aloisdeniel/tessera'
  s.license          = { :file => '../LICENSE' }
  s.author           = { 'Aloïs Deniel' => 'alois.deniel@gmail.com' }

  s.source           = { :path => '.' }
  s.source_files = 'flutter_tessera/Sources/flutter_tessera/**/*.swift',
                   'flutter_tessera/Sources/CTessera/**/*.{c,h}'
  s.public_header_files = 'flutter_tessera/Sources/CTessera/include/tessera_bridge.h'

  s.dependency 'FlutterMacOS'

  s.platform = :osx, '10.15'
  s.swift_version = '5.0'

  # Resolve the repo root relative to this podspec so -ltessera/-lSDL3 link and
  # @rpath/libtessera.dylib loads. CocoaPods evaluates the podspec through
  # Flutter's symlink farm (.../ephemeral/.symlinks/plugins/flutter_tessera/...),
  # so follow the symlink with realpath before walking up to the repo root
  # (macos/ -> flutter_tessera/ -> bindings/ -> <repo>). TESSERA_ROOT overrides.
  tessera_root = ENV['TESSERA_ROOT'] ||
    File.expand_path('../../../..', File.realpath(__FILE__))
  # The prebuilt native libs come from publish.sh, which installs the universal
  # macOS dylibs (libtessera.dylib + libSDL3.dylib, side by side) into the
  # plugin's native/macos dir. Run `./publish.sh --targets macos` to (re)build
  # them. SDL3 headers come from the vendored source publish.sh built against, so
  # they match the linked binary exactly (no Homebrew SDL required). Override the
  # lib dir with TESSERA_MACOS_LIB_DIR if you stage the libs elsewhere.
  native_macos = ENV['TESSERA_MACOS_LIB_DIR'] ||
    "#{tessera_root}/bindings/flutter_tessera/native/macos"
  s.pod_target_xcconfig = {
    'DEFINES_MODULE' => 'YES',
    'HEADER_SEARCH_PATHS' =>
      "\"#{tessera_root}/include\" \"#{tessera_root}/third_party/SDL/include\"",
    'LIBRARY_SEARCH_PATHS' => "\"#{native_macos}\"",
    'OTHER_LDFLAGS' => '-ltessera -lSDL3',
    # libtessera's install name is @rpath/libtessera.dylib and it records an
    # @rpath/libSDL3.dylib reference (both resolved via native/macos, where the
    # two dylibs sit together), so an rpath to that dir loads both.
    'LD_RUNPATH_SEARCH_PATHS' =>
      "\"#{native_macos}\" @executable_path/../Frameworks",
  }
end
