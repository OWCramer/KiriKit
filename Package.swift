// swift-tools-version: 6.0
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
// If a copy of the MPL was not distributed with this file, You can obtain one at
// https://mozilla.org/MPL/2.0/.
import PackageDescription

// Shiori's own KiriKiri engine (Phase 23): krkr2's core plus Shiori's environment, renderer and
// host glue, built as Vendor/ShioriKrkr.xcframework by Scripts/krkr2/build-framework.sh from Engine/. This
// package holds its C interface, the loader the app opens it with, and the host's E-mote
// player (ShioriKrkrEmote, on CatSystemKit's renderer). Clean-room: nothing here comes from
// AetherKrkr (PROVENANCE.md).
let package = Package(
    name: "ShioriKrkrKit",
    platforms: [.iOS(.v18), .macOS(.v15), .tvOS(.v18)],
    products: [
        .library(name: "CShioriKrkr", targets: ["CShioriKrkr"]),
        .library(name: "ShioriKrkrEmote", targets: ["ShioriKrkrEmote"]),
        // For the Mac harness (Scripts/krkr2/run-harness.sh), which dlopens it.
        .library(name: "ShioriKrkrEmoteHarness", type: .dynamic, targets: ["ShioriKrkrEmoteHarness"]),
    ],
    dependencies: [
        .package(path: "../CatSystemKit"),
    ],
    targets: [
        .target(name: "CShioriKrkr"),
        // Drawn every frame for every character: optimised even in Debug.
        .target(name: "ShioriKrkrEmote",
                dependencies: ["CShioriKrkr", .product(name: "CatSystemKit", package: "CatSystemKit")],
                swiftSettings: [.unsafeFlags(["-O"])]),
        .target(name: "ShioriKrkrEmoteHarness", dependencies: ["ShioriKrkrEmote", "CShioriKrkr"]),
    ]
)
