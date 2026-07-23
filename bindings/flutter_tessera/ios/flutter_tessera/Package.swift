// swift-tools-version: 5.9
//
// Swift Package Manager manifest for the flutter_tessera iOS plugin. This is the
// primary, documented build path (the sibling ios/flutter_tessera.podspec mirrors
// it for CocoaPods-based apps). Structure follows Flutter's SPM plugin template:
//
//   • FlutterFramework  — Flutter's generated framework package, symlinked next to
//                         this one at build time (`../FlutterFramework`).
//   • CTessera          — the C bridge (tessera_bridge.c) + vendored tessera.h.
//   • flutter_tessera   — the Swift plugin (platform view; reparents the engine's
//                         swapchain metal view for zero-copy presentation), plus
//                         the bundled MSL shaders as a resource.
//
// iOS uses the same Metal backend / MSL shaders as macOS but links STATIC slices
// of `libtessera` and `SDL3` (force-loaded so every archive member survives),
// together with the system frameworks SDL3's static build requires.
//
// The archive/header locations are environment-specific and CANNOT be resolved
// portably at package-parse time, so they come from environment variables with
// the same defaults as the podspec (iOS *simulator* arm64 slices under the repo):
//
//   TESSERA_INCLUDE_DIR   engine headers (default: <repo>/include)
//   SDL3_INCLUDE_DIR      SDL3 headers   (default: <repo>/third_party/SDL/include)
//   TESSERA_IOS_LIB_DIR   dir with libtessera.a / libtessera_thirdparty.a
//                         (default: <repo>/build-ios/Release-iphonesimulator)
//   SDL3_IOS_LIB_DIR      dir with libSDL3.a
//                         (default: <repo>/build-sdl-iossim/Release-iphonesimulator)
//
// Build those static slices first (see ios/flutter_tessera.podspec for the exact
// cmake invocations, or the repo README). For a device build, produce device
// slices and point the *_IOS_LIB_DIR vars at them. This is a reference
// configuration; on-device linking/signing is the integration step that must be
// completed in an Xcode/Flutter build and is NOT verified here.
import PackageDescription
import Foundation

let env = ProcessInfo.processInfo.environment
// ios/flutter_tessera -> ios -> flutter_tessera -> bindings -> <repo>
let repoRoot = "../../../.."
let tesseraInclude = env["TESSERA_INCLUDE_DIR"] ?? "\(repoRoot)/include"
let sdlInclude = env["SDL3_INCLUDE_DIR"] ?? "\(repoRoot)/third_party/SDL/include"
let tesseraLibDir = env["TESSERA_IOS_LIB_DIR"] ?? "\(repoRoot)/build-ios/Release-iphonesimulator"
let sdlLibDir = env["SDL3_IOS_LIB_DIR"] ?? "\(repoRoot)/build-sdl-iossim/Release-iphonesimulator"

let package = Package(
    name: "flutter_tessera",
    platforms: [
        .iOS("13.0")
    ],
    products: [
        .library(name: "flutter-tessera", targets: ["flutter_tessera"])
    ],
    dependencies: [
        .package(name: "FlutterFramework", path: "../FlutterFramework")
    ],
    targets: [
        .target(
            name: "CTessera",
            cSettings: [
                .unsafeFlags(["-I", tesseraInclude, "-I", sdlInclude])
            ],
            linkerSettings: [
                // Force-load the static archives so every member survives — the
                // objects that reference tessera_*/SDL_* live in this same target,
                // which -ltessera/-lSDL3 alone would let the linker drop.
                .unsafeFlags([
                    "-L", tesseraLibDir,
                    "-L", sdlLibDir,
                    "-Wl,-force_load,\(tesseraLibDir)/libtessera.a",
                    "-Wl,-force_load,\(tesseraLibDir)/libtessera_thirdparty.a",
                    "-Wl,-force_load,\(sdlLibDir)/libSDL3.a",
                    "-weak_framework", "CoreHaptics",
                ]),
                // Frameworks required by SDL3's static iOS build (from its CMake
                // link interface). CoreHaptics is weak-linked above.
                .linkedFramework("CoreMedia"),
                .linkedFramework("CoreVideo"),
                .linkedFramework("CoreAudio"),
                .linkedFramework("AudioToolbox"),
                .linkedFramework("AVFoundation"),
                .linkedFramework("CoreBluetooth"),
                .linkedFramework("CoreGraphics"),
                .linkedFramework("CoreMotion"),
                .linkedFramework("Foundation"),
                .linkedFramework("GameController"),
                .linkedFramework("Metal"),
                .linkedFramework("OpenGLES"),
                .linkedFramework("QuartzCore"),
                .linkedFramework("UIKit"),
            ]
        ),
        .target(
            name: "flutter_tessera",
            dependencies: [
                "CTessera",
                .product(name: "FlutterFramework", package: "FlutterFramework")
            ],
            resources: [
                // Copy the shaders/ folder verbatim into the target's resource
                // bundle (Bundle.module); the engine reads <asset_dir>/shaders/
                // <name>.<stage>.msl. See assetDir() in TesseraPlatformView.swift.
                .copy("Resources/shaders"),
                .process("PrivacyInfo.xcprivacy"),
            ]
        )
    ]
)
