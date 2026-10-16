#!/bin/bash
# This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
# If a copy of the MPL was not distributed with this file, You can obtain one at
# https://mozilla.org/MPL/2.0/.
#
# Builds Shiori's own KiriKiri engine (krkr2's core + Engine/) per slice
# and packs the iOS slices as Vendor/ShioriKrkr.xcframework (one dynamic framework each,
# exporting only shiorikrkr.h). The macOS slice is the harness's. Run fetch.sh first.
#
#   Scripts/krkr2/build-framework.sh            # macOS harness, simulator, device, xcframework
#   Scripts/krkr2/build-framework.sh macos      # just the harness
set -euo pipefail

export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ENGINE="$ROOT/Engine"
# FFmpeg: github.com/OWCramer/shiori-ffmpeg, checked out beside this repository.
FFMPEG="${SHIORI_FFMPEG:-$ROOT/../shiori-ffmpeg}"
VENDOR="$ROOT/Vendor"
WORK="$ROOT/build/shiorikrkr"
NAME=ShioriKrkr
MIN_IOS=17.0

SLICES=("$@")
[ ${#SLICES[@]} -gt 0 ] || SLICES=(macos simulator device)

configure_and_build() { # slice triplet extra-args...
    local slice=$1 triplet=$2
    shift 2
    echo "== $slice ($triplet)"
    cmake -S "$ENGINE" -B "$WORK/$slice" -G Ninja \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_TOOLCHAIN_FILE="$VENDOR/vcpkg/scripts/buildsystems/vcpkg.cmake" \
        -DVCPKG_TARGET_TRIPLET="$triplet" \
        -DVCPKG_INSTALLED_DIR="$VENDOR/krkr2-deps/$triplet" \
        -DVCPKG_MANIFEST_MODE=OFF \
        -DVENDOR="$VENDOR" \
        "$@" > "$WORK/$slice.configure.log"
    cmake --build "$WORK/$slice" --parallel ${SHIORIKRKR_TARGET:+--target $SHIORIKRKR_TARGET}
}

make_framework() { # slice platform
    local slice=$1 platform=$2
    local framework="$WORK/frameworks/$slice/$NAME.framework"
    rm -rf "$framework"
    mkdir -p "$framework"
    cp "$WORK/$slice/lib$NAME.dylib" "$framework/$NAME"
    install_name_tool -id "@rpath/$NAME.framework/$NAME" "$framework/$NAME"
    # The debug symbols go to Vendor/ShioriKrkr.dSYM/<slice> (crash logs from the shipped,
    # stripped binary symbolicate against it; the archive script copies it in).
    mkdir -p "$VENDOR/ShioriKrkr.dSYM/$slice"
    rm -rf "$VENDOR/ShioriKrkr.dSYM/$slice/$NAME.framework.dSYM"
    dsymutil "$framework/$NAME" -o "$VENDOR/ShioriKrkr.dSYM/$slice/$NAME.framework.dSYM"
    strip -x "$framework/$NAME" 2>/dev/null || true
    cat > "$framework/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleExecutable</key><string>$NAME</string>
  <key>CFBundleIdentifier</key><string>com.owencramer.vnhub.shiorikrkr</string>
  <key>CFBundleName</key><string>$NAME</string>
  <key>CFBundlePackageType</key><string>FMWK</string>
  <key>CFBundleShortVersionString</key><string>0.1</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>CFBundleSupportedPlatforms</key><array><string>$platform</string></array>
  <key>MinimumOSVersion</key><string>$MIN_IOS</string>
</dict>
</plist>
PLIST
}

mkdir -p "$WORK"
built=()
for slice in "${SLICES[@]}"; do
    case $slice in
    macos)
        [ -f "$FFMPEG/build/ffmpeg-macos/install/lib/libffmpeg.a" ] || "$FFMPEG/Scripts/ffmpeg/build-ffmpeg-macos.sh"
        configure_and_build macos arm64-osx -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 \
            -DFFMPEG_INCLUDE="$FFMPEG/build/ffmpeg-macos/install/include" \
            -DFFMPEG_LIBRARY="$FFMPEG/build/ffmpeg-macos/install/lib/libffmpeg.a"
        echo "== harness: $WORK/macos/shiorikrkr-harness"
        ;;
    simulator)
        configure_and_build simulator arm64-ios-simulator -DCMAKE_SYSTEM_NAME=iOS \
            -DCMAKE_OSX_SYSROOT=iphonesimulator -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=$MIN_IOS \
            -DFFMPEG_INCLUDE="$FFMPEG/Vendor/ShioriFFmpeg.xcframework/ios-arm64-simulator/Headers" \
            -DFFMPEG_LIBRARY="$FFMPEG/Vendor/ShioriFFmpeg.xcframework/ios-arm64-simulator/libffmpeg.a"
        make_framework simulator iPhoneSimulator
        built+=(-framework "$WORK/frameworks/simulator/$NAME.framework")
        ;;
    device)
        configure_and_build device arm64-ios -DCMAKE_SYSTEM_NAME=iOS \
            -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=$MIN_IOS \
            -DFFMPEG_INCLUDE="$FFMPEG/Vendor/ShioriFFmpeg.xcframework/ios-arm64/Headers" \
            -DFFMPEG_LIBRARY="$FFMPEG/Vendor/ShioriFFmpeg.xcframework/ios-arm64/libffmpeg.a"
        make_framework device iPhoneOS
        built+=(-framework "$WORK/frameworks/device/$NAME.framework")
        ;;
    *) echo "unknown slice $slice" >&2; exit 2 ;;
    esac
done

if [ ${#built[@]} -eq 4 ]; then
    rm -rf "$VENDOR/$NAME.xcframework"
    xcodebuild -create-xcframework "${built[@]}" -output "$VENDOR/$NAME.xcframework"
    for framework in "$VENDOR/$NAME.xcframework"/*/"$NAME.framework"; do
        cp "$ROOT/Scripts/privacy/Framework.xcprivacy" "$framework/PrivacyInfo.xcprivacy"
    done
    echo "== Vendor/$NAME.xcframework"
    du -sh "$VENDOR/$NAME.xcframework"/*/"$NAME.framework/$NAME"
fi
