# KiriKit

Shiori's KiriKiri engine: krkr2's core (KiriKiri 2 / KiriKiri Z) with Shiori's own environment, Metal compositor, audio, movies and host glue, for iOS and iPadOS, plus a Mac harness. Part of [Shiori](https://github.com/OWCramer/shiori).

It plays games you own. No game content, and no game-specific keys, are included: encrypted archives are opened with schemes built at run time from GARbro's public database.

## Layout

- `Engine/`: the engine, built with CMake into `ShioriKrkr.framework` (exporting only `shiorikrkr.h`).
- `Sources/CKiriKit`: the C interface and the loader the app opens the framework with.
- `Sources/KiriKit`: the E-mote character player, on [CatSystemKit](https://github.com/OWCramer/CatSystemKit)'s renderer.
- `Scripts/krkr2`: fetch, patches, build and the Mac harness.

## Building

Needs Xcode 26 or later, CMake and Ninja, and FFmpeg built by [Shiori](https://github.com/OWCramer/shiori)'s `Scripts/ffmpeg` in a checkout beside this one (or `SHIORI_FFMPEG=<path to that checkout>`).

```sh
Scripts/krkr2/fetch.sh             # krkr2, krkrsdl3 and wamsoft plug-ins at pinned commits, Shiori's patches, vcpkg dependencies
Scripts/krkr2/build-framework.sh   # Mac harness, simulator and device; Vendor/ShioriKrkr.xcframework
Scripts/krkr2/run-harness.sh <game folder> <work folder>
```

The Swift package (`CKiriKit`, `KiriKit`) is what the app depends on; the framework is copied into the app's `Vendor/`.

## Licences

KiriKit's own code is under the [Mozilla Public License 2.0](LICENSE). It contains no code from AetherKrkr. Upstream code is fetched by `Scripts/krkr2/fetch.sh` and built from `Vendor/` with its own licence headers kept; KiriKit's changes to it are the patches in `Scripts/krkr2/patches`.

| Component | Licence |
|---|---|
| [krkr2](https://github.com/2468785842/krkr2) at `3ac366c` (KiriKiri 2 / Z core and plug-ins) | KiriKiri licence (BSD-style) |
| [wamsoft plug-ins](https://github.com/wamsoft) (AlphaMovie, KAGParserEx, extrans, layerEx*, shrinkCopy) | KiriKiri licence; layerExImage includes CxImage (zlib-style) |
| [krkrsdl3](https://github.com/krkrsdl3/krkrsdl3) plug-ins (PackinOne, lzfs, gfxEffect, LayerExDraw) | KiriKiri licence + krkrsdl3's publish-your-changes term (met by `Scripts/krkr2/patches/krkrsdl3`) |
| [GARbro](https://github.com/crskycode/GARbro) Cx and Hx algorithms (`Engine/cx_decrypt.*`, `Engine/shiori_hx.*`) | MIT, Copyright (C) 2014-2018 morkt |
| vcpkg dependencies: oniguruma, zlib, libpng, libwebp, libjpeg-turbo, libvorbis, opusfile, FreeType, plutovg, fmt, spdlog, lz4, xxHash, uchardet | BSD / IJG / FTL / MIT / zlib; uchardet used under MPL-1.1 |
| FFmpeg n8.0.3 (built by Shiori's `Scripts/ffmpeg`) | LGPL-2.1+, linked statically; rebuild and relink with those scripts |
