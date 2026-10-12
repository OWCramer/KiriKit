#!/bin/bash
# This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
# If a copy of the MPL was not distributed with this file, You can obtain one at
# https://mozilla.org/MPL/2.0/.
#
# Fetches what Shiori's own KiriKiri engine (ShioriKrkr) is built from, into Vendor/
# (gitignored): krkrsdl3 at a pinned commit, vcpkg at a pinned commit, and the
# dependencies vcpkg builds for each slice (macOS for the harness, the iOS simulator and
# iOS devices). Nothing here comes from Vendor/Aether.
#
#   Scripts/krkrsdl3/fetch.sh            # all three slices
#   Scripts/krkrsdl3/fetch.sh arm64-osx  # one slice
set -euo pipefail

export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$ROOT/Scripts/krkrsdl3"
VENDOR="$ROOT/Vendor"

# krkrsdl3 (KiriKiri licence + its supplemental publish-your-changes term; see
# Packages/ShioriKrkrKit/PROVENANCE.md). Shiori's changes to it live in Scripts/krkrsdl3/patches.
KRKRSDL3_URL=https://github.com/krkrsdl3/krkrsdl3
KRKRSDL3_COMMIT=358b0469bd41ffd2fc8cc08bcaa1e205e817aff9   # 2026-09-30
KRKRSDL3_BUILD_URL=https://github.com/krkrsdl3/krkrsdl3_build
KRKRSDL3_BUILD_COMMIT=66fb7d9533478d33317208cd8ec8696ab9340d6f   # 2026-08-29
VCPKG_URL=https://github.com/microsoft/vcpkg
VCPKG_COMMIT=${VCPKG_COMMIT:-e456309491fd875487bf02b8a0dca76c1b1b00cc}   # 2026-10-10

mkdir -p "$VENDOR"
if [ ! -d "$VENDOR/krkrsdl3/.git" ]; then
    git clone "$KRKRSDL3_URL" "$VENDOR/krkrsdl3"
fi
git -C "$VENDOR/krkrsdl3" fetch --quiet origin || true
git -C "$VENDOR/krkrsdl3" checkout --quiet "$KRKRSDL3_COMMIT"
# Shiori's changes to krkrsdl3, published with each build as its licence asks.
git -C "$VENDOR/krkrsdl3" checkout --quiet -- .
for patch in "$HERE"/patches/*.patch; do
    [ -e "$patch" ] || continue
    git -C "$VENDOR/krkrsdl3" apply --whitespace=nowarn "$patch"
done

# krkrsdl3's build repository (same licence): only its CMake source lists are read
# (Packages/ShioriKrkrKit/Engine/CMakeLists.txt), so the file set follows upstream.
if [ ! -d "$VENDOR/krkrsdl3_build/.git" ]; then
    git clone "$KRKRSDL3_BUILD_URL" "$VENDOR/krkrsdl3_build"
fi
git -C "$VENDOR/krkrsdl3_build" fetch --quiet origin || true
git -C "$VENDOR/krkrsdl3_build" checkout --quiet "$KRKRSDL3_BUILD_COMMIT"

if [ ! -x "$VENDOR/vcpkg/vcpkg" ]; then
    [ -d "$VENDOR/vcpkg/.git" ] || git clone "$VCPKG_URL" "$VENDOR/vcpkg"
    [ -z "$VCPKG_COMMIT" ] || git -C "$VENDOR/vcpkg" checkout --quiet "$VCPKG_COMMIT"
    "$VENDOR/vcpkg/bootstrap-vcpkg.sh" -disableMetrics
fi

TRIPLETS=("$@")
[ ${#TRIPLETS[@]} -gt 0 ] || TRIPLETS=(arm64-osx arm64-ios-simulator arm64-ios)
for triplet in "${TRIPLETS[@]}"; do
    echo "== vcpkg $triplet"
    "$VENDOR/vcpkg/vcpkg" install \
        --x-manifest-root="$HERE" \
        --overlay-triplets="$HERE/triplets" \
        --triplet="$triplet" \
        --x-install-root="$VENDOR/krkrsdl3-deps/$triplet" \
        --disable-metrics
done
