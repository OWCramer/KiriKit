// swift-tools-version: 6.0
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
// If a copy of the MPL was not distributed with this file, You can obtain one at
// https://mozilla.org/MPL/2.0/.
import PackageDescription

// KiriKit, Shiori's own KiriKiri engine: krkr2's core plus Shiori's environment, renderer and
// host glue, built as Vendor/ShioriKrkr.xcframework by Scripts/krkr2/build-framework.sh from Engine/. This
// package holds its C interface, the loader the app opens it with, and the host's E-mote
// player (KiriKit, on CatSystemKit's renderer).
let package = Package(
    name: "KiriKit",
    platforms: [.iOS(.v18), .macOS(.v15), .tvOS(.v18)],
    products: [
        .library(name: "CKiriKit", targets: ["CKiriKit"]),
        .library(name: "KiriKit", targets: ["KiriKit"]),
        // For the Mac harness (Scripts/krkr2/run-harness.sh), which dlopens it.
        .library(name: "KiriKitEmoteHarness", type: .dynamic, targets: ["KiriKitEmoteHarness"]),
    ],
    dependencies: [
        .package(url: "https://github.com/OWCramer/CatSystemKit.git", from: "0.1.0"),
    ],
    targets: [
        .target(name: "CKiriKit"),
        .target(name: "KiriKit",
                dependencies: ["CKiriKit", .product(name: "CatSystemKit", package: "CatSystemKit")]),
        .target(name: "KiriKitEmoteHarness", dependencies: ["KiriKit", "CKiriKit"]),
    ]
)
