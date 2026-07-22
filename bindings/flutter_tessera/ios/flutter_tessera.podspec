#
# flutter_tessera iOS plugin.
#
# iOS uses the same Metal backend and MSL shaders as macOS. It links a prebuilt
# iOS build of `libtessera` and `SDL3`, and bundles the engine's MSL shaders as
# `tessera_assets.bundle` (loaded at runtime via TesseraPlatformView.assetDir()).
#
# Prerequisites (device + simulator, arm64):
#   - Build libtessera for iOS (cross-compile the C engine; MSL shaders work).
#   - Provide SDL3 for iOS (SDL ships an Xcode project / xcframework).
# Point the linker at them with the TESSERA_IOS_LIB_DIR / SDL3_IOS_LIB_DIR env
# vars (defaults assume a `build-ios/` at the repo root). This config is a
# reference; on-device linking/signing must be completed in an Xcode build and
# is NOT verified here.
#
Pod::Spec.new do |s|
  s.name             = 'flutter_tessera'
  s.version          = '0.1.0'
  s.summary          = 'Flutter platform-view embedding of the Tessera 3D board-game renderer.'
  s.description      = <<-DESC
Flutter platform-view embedding of the Tessera 3D board-game renderer (iOS/Metal).
                       DESC
  s.homepage         = 'https://github.com/aloisdeniel/tessera'
  s.license          = { :file => '../LICENSE' }
  s.author           = { 'Aloïs Deniel' => 'alois.deniel@gmail.com' }

  s.source           = { :path => '.' }
  s.source_files = 'Classes/**/*.swift', 'CTessera/**/*.{c,h}'
  s.public_header_files = 'CTessera/include/tessera_bridge.h'

  # Bundle the engine's Metal shaders. Reference the `shaders` directory (not a
  # file glob) so CocoaPods preserves it as tessera_assets.bundle/shaders/, which
  # is where the engine looks (<asset_dir>/shaders/<name>.<stage>.msl). The files
  # are copied into the pod (Assets/shaders) because CocoaPods can't reach assets
  # outside the pod through Flutter's symlink farm.
  s.resource_bundles = { 'tessera_assets' => ['Assets/shaders'] }

  s.dependency 'Flutter'
  s.platform = :ios, '13.0'
  s.swift_version = '5.0'

  tessera_root = ENV['TESSERA_ROOT'] ||
    File.expand_path('../../../..', File.realpath(__FILE__))
  # Simulator static slices built by:
  #   cmake -S third_party/SDL -B build-sdl-iossim -GXcode -DCMAKE_SYSTEM_NAME=iOS \
  #     -DCMAKE_OSX_SYSROOT=iphonesimulator -DCMAKE_OSX_ARCHITECTURES=arm64 \
  #     -DSDL_SHARED=OFF -DSDL_STATIC=ON && cmake --build build-sdl-iossim --config Release
  #   cmake -S . -B build-ios -GXcode -DCMAKE_SYSTEM_NAME=iOS \
  #     -DCMAKE_OSX_SYSROOT=iphonesimulator -DCMAKE_OSX_ARCHITECTURES=arm64 \
  #     -DTESSERA_BUILD_SHARED=OFF -DTESSERA_BUILD_EXAMPLES=OFF -DTESSERA_BUILD_TESTS=OFF \
  #     -DSDL3_DIR=$PWD/build-sdl-iossim && cmake --build build-ios --config Release
  # For a device build, produce device slices and point these env vars at them
  # (or build xcframeworks); the defaults below are the iOS *simulator* slices.
  tessera_lib = ENV['TESSERA_IOS_LIB_DIR'] || "#{tessera_root}/build-ios/Release-iphonesimulator"
  sdl_lib     = ENV['SDL3_IOS_LIB_DIR']    || "#{tessera_root}/build-sdl-iossim/Release-iphonesimulator"
  # Use the SDL source we built against so headers match the linked lib exactly.
  sdl_include = ENV['SDL3_INCLUDE_DIR'] || "#{tessera_root}/third_party/SDL/include"

  # Frameworks required by SDL3's static iOS build (from its CMake link interface).
  sdl_frameworks = '-framework CoreMedia -framework CoreVideo -framework CoreAudio ' \
    '-framework AudioToolbox -framework AVFoundation -framework CoreBluetooth ' \
    '-framework CoreGraphics -framework CoreMotion -framework Foundation ' \
    '-framework GameController -framework Metal -framework OpenGLES ' \
    '-framework QuartzCore -framework UIKit -weak_framework CoreHaptics'

  # Flutter uses dynamic frameworks (`use_frameworks!`), so the pod's
  # flutter_tessera.framework is linked at its OWN build step and must resolve
  # the bridge's references to tessera_*/SDL_* there — the prebuilt static libs
  # get baked into the framework. -force_load (absolute paths) loads every
  # archive member regardless of link order (the referencing objects are in this
  # same pod), which -ltessera/-lSDL3 would miss.
  s.pod_target_xcconfig = {
    'DEFINES_MODULE' => 'YES',
    'HEADER_SEARCH_PATHS' => "\"#{tessera_root}/include\" \"#{sdl_include}\"",
    'LIBRARY_SEARCH_PATHS' => "\"#{tessera_lib}\" \"#{sdl_lib}\"",
    'OTHER_LDFLAGS' =>
      "-Wl,-force_load,\"#{tessera_lib}/libtessera.a\" " \
      "-Wl,-force_load,\"#{tessera_lib}/libtessera_thirdparty.a\" " \
      "-Wl,-force_load,\"#{sdl_lib}/libSDL3.a\" #{sdl_frameworks}",
  }
end
