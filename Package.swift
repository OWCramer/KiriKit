// swift-tools-version: 6.0
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
// If a copy of the MPL was not distributed with this file, You can obtain one at
// https://mozilla.org/MPL/2.0/.
import PackageDescription

// Shiori's own KiriKiri engine (Phase 23): krkrsdl3 plus Shiori's host glue, built as
// Vendor/ShioriKrkr.xcframework by Scripts/krkrsdl3/build-framework.sh from Engine/. This
// package holds its C interface and the loader the app opens it with. Clean-room: nothing
// here comes from AetherKrkr (PROVENANCE.md).
let package = Package(
    name: "ShioriKrkrKit",
    platforms: [.iOS(.v18), .macOS(.v15)],
    products: [
        .library(name: "CShioriKrkr", targets: ["CShioriKrkr"]),
    ],
    targets: [
        .target(name: "CShioriKrkr"),
    ]
)
