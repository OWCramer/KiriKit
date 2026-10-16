# KiriKit

Shiori's KiriKiri engine: krkr2's core (KiriKiri 2 / KiriKiri Z) with Shiori's own environment, Metal compositor, audio, movies and host glue, for iOS and iPadOS, plus a Mac harness. Part of [Shiori](https://github.com/OWCramer/shiori).

It plays games you own. No game content, and no game-specific keys, are included: encrypted archives are opened with schemes built at run time from GARbro's public database.

## Layout

- `Engine/`: the engine, built with CMake into `ShioriKrkr.framework` (exporting only `shiorikrkr.h`). What comes from where: [PROVENANCE.md](PROVENANCE.md).
- `Sources/CKiriKit`: the C interface and the loader the app opens the framework with.
- `Sources/KiriKit`: the E-mote character player, on [CatSystemKit](https://github.com/OWCramer/CatSystemKit)'s renderer.
- `Scripts/krkr2`: fetch, patches, build, the Mac harness and frame comparison tools.

## Building

Needs Xcode 26 or later, CMake and Ninja, and FFmpeg built by [shiori-ffmpeg](https://github.com/OWCramer/shiori-ffmpeg) in a checkout beside this one (or `SHIORI_FFMPEG=<path>`).

```sh
Scripts/krkr2/fetch.sh             # krkr2, krkrsdl3 and wamsoft plug-ins at pinned commits, Shiori's patches, vcpkg dependencies
Scripts/krkr2/build-framework.sh   # Mac harness, simulator and device; Vendor/ShioriKrkr.xcframework
Scripts/krkr2/run-harness.sh <game folder> <work folder>
```

The Swift package (`CKiriKit`, `KiriKit`) is what the app depends on; the framework is copied into the app's `Vendor/`.

## Licence

KiriKit's own code is under the [Mozilla Public License 2.0](LICENSE). krkr2 and the plug-ins it builds keep their licences (the KiriKiri licence, BSD-style; krkrsdl3's plug-ins add a publish-your-changes term, met by `Scripts/krkr2/patches`); vcpkg dependencies are listed with their licences in Shiori's [docs/licences.md](https://github.com/OWCramer/shiori/blob/main/docs/licences.md). FFmpeg is LGPL-2.1+ and linked statically; rebuild it with shiori-ffmpeg to relink.
