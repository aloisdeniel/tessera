// swift-tools-version: 5.9
//
// Two targets:
//   • CTessera         — the C bridge (tessera_bridge.c) + vendored tessera.h.
//   • flutter_tessera  — the Swift plugin (platform view; reparents the engine's
//                        swapchain metal view for zero-copy presentation).
//
// The C target must find the SDL3 headers and link the prebuilt `libtessera`
// and `SDL3` libraries. Those paths are environment-specific and CANNOT be
// resolved at package-parse time in a portable way, so they are read from
// environment variables with sensible Homebrew / repo defaults:
//
//   TESSERA_INCLUDE_DIR  headers for SDL3 (SDL3/SDL.h)  (default: /opt/homebrew/include)
//   TESSERA_LIB_DIR      dir with libtessera.dylib      (default: <repo>/build)
//   SDL3_LIB_DIR         dir with libSDL3.dylib         (default: /opt/homebrew/lib)
//
// Build libtessera first (see the repo README) and ensure SDL3 is installed.
// This is a reference configuration; on-device linking/signing is the
// integration step that must be completed in an Xcode/Flutter build.
import PackageDescription
import Foundation

let env = ProcessInfo.processInfo.environment
let sdlInclude = env["TESSERA_INCLUDE_DIR"] ?? "/opt/homebrew/include"
let tesseraLibDir = env["TESSERA_LIB_DIR"] ?? "../../../../build"
let sdlLibDir = env["SDL3_LIB_DIR"] ?? "/opt/homebrew/lib"

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
