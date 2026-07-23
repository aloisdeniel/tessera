// swift-tools-version: 5.9
//
// Two targets:
//   • CTessera         — the C bridge (tessera_bridge.c) + vendored tessera.h.
//   • flutter_tessera  — the Swift plugin (platform view; reparents the engine's
//                        swapchain metal view for zero-copy presentation).
//
// The C target must find the SDL3 headers and link the prebuilt `libtessera`
// and `SDL3` libraries. These come from publish.sh, which installs the universal
// macOS dylibs (libtessera.dylib + libSDL3.dylib, side by side) into the plugin's
// native/macos dir and builds SDL from the vendored source — so the headers under
// third_party/SDL/include match the linked binary exactly (no Homebrew SDL). Run
// `./publish.sh --targets macos` to (re)build them. Paths can still be overridden
// (e.g. to stage the libs elsewhere) via environment variables:
//
//   TESSERA_INCLUDE_DIR  headers for SDL3 (SDL3/SDL.h)  (default: <repo>/third_party/SDL/include)
//   TESSERA_MACOS_LIB_DIR dir with both dylibs          (default: <plugin>/native/macos)
//
// This is a reference configuration; on-device linking/signing is the
// integration step that must be completed in an Xcode/Flutter build.
import PackageDescription
import Foundation

let env = ProcessInfo.processInfo.environment
// Resolve paths against THIS manifest so the runtime rpath is ABSOLUTE: a
// relative -rpath would be interpreted against the launch CWD and fail to load
// libtessera/libSDL3 at runtime. #filePath is macos/flutter_tessera/Package.swift,
// so its dir is the SwiftPM package root (one below the plugin root).
let manifestDir = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
func resolve(_ path: String) -> String {
    URL(fileURLWithPath: path, relativeTo: manifestDir).standardizedFileURL.path
}
// macos/flutter_tessera -> macos -> flutter_tessera (plugin root) -> native/macos.
let sdlInclude = resolve(env["TESSERA_INCLUDE_DIR"] ?? "../../../../third_party/SDL/include")
let nativeMacos = resolve(env["TESSERA_MACOS_LIB_DIR"] ?? "../../native/macos")
let tesseraLibDir = nativeMacos
let sdlLibDir = nativeMacos

let package = Package(
    name: "flutter_tessera",
    platforms: [
        .macOS("10.15")
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
                .unsafeFlags(["-I", sdlInclude])
            ],
            linkerSettings: [
                .unsafeFlags([
                    "-L", tesseraLibDir, "-ltessera",
                    "-L", sdlLibDir, "-lSDL3",
                    "-Wl,-rpath,\(sdlLibDir)",
                ])
            ]
        ),
        .target(
            name: "flutter_tessera",
            dependencies: [
                "CTessera",
                .product(name: "FlutterFramework", package: "FlutterFramework")
            ]
        )
    ]
)
