#!/bin/bash
# This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
# If a copy of the MPL was not distributed with this file, You can obtain one at
# https://mozilla.org/MPL/2.0/.
#
# Fetches what Shiori's own KiriKiri engine (ShioriKrkr) is built from, into Vendor/
# (gitignored), and applies Shiori's changes (Scripts/krkr2/patches, written back by
# export-patches.sh):
#   - krkr2 (2468785842/krkr2) at commit 3ac366c: the engine's core
#     (KiriKiri licence);
#   - krkrsdl3 at a pinned commit: the few plug-ins with no other source (PackinOne, lzfs,
#     gfxEffect, LayerExDraw on plutovg; KiriKiri licence plus its publish-your-changes term);
#   - wamsoft's plug-in repositories (KiriKiri licence), pinned;
#   - vcpkg, and the dependencies it builds for each slice.
#
#   Scripts/krkr2/fetch.sh            # all three slices
#   Scripts/krkr2/fetch.sh arm64-osx  # one slice
set -euo pipefail

export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$ROOT/Scripts/krkr2"
VENDOR="$ROOT/Vendor"

KRKR2_URL=https://github.com/2468785842/krkr2
KRKR2_COMMIT=3ac366c439a142f1712ed3059cb80184cb102dbf   # 2025-09-21
KRKRSDL3_URL=https://github.com/krkrsdl3/krkrsdl3
KRKRSDL3_COMMIT=358b0469bd41ffd2fc8cc08bcaa1e205e817aff9   # 2026-09-30
VCPKG_URL=https://github.com/microsoft/vcpkg
VCPKG_COMMIT=${VCPKG_COMMIT:-e456309491fd875487bf02b8a0dca76c1b1b00cc}   # 2026-10-10

checkout() { # dir url commit patch
    local dir=$1 url=$2 commit=$3 patch=$4
    if [ ! -d "$dir/.git" ]; then
        mkdir -p "$dir"
        git -C "$dir" init -q
        git -C "$dir" remote add origin "$url"
    fi
    git -C "$dir" fetch -q --depth 1 origin "$commit"
    git -C "$dir" checkout -q -f FETCH_HEAD
    git -C "$dir" clean -fdq
    if [ -e "$patch" ]; then git -C "$dir" apply --whitespace=nowarn "$patch"; fi
}

mkdir -p "$VENDOR"
checkout "$VENDOR/krkr2" "$KRKR2_URL" "$KRKR2_COMMIT" "$HERE/patches/krkr2/shiori.patch"
checkout "$VENDOR/krkrsdl3" "$KRKRSDL3_URL" "$KRKRSDL3_COMMIT" "$HERE/patches/krkrsdl3/shiori.patch"
# krkrsdl3's plug-ins include "ncbind/ncbind.hpp", which must be krkr2's (its copy declares a
# tTJSNativeClassForPlugin that clashes with krkr2's TJS one).
rm -rf "$VENDOR/krkrsdl3/plugins/ncbind"
while read -r repo commit; do
    [ -n "$repo" ] || continue
    checkout "$VENDOR/upstream-plugins/$repo" "https://github.com/$repo" "$commit" \
        "$HERE/patches/wamsoft/$(basename "$repo").patch"
done <<'PINS'
wamsoft/AlphaMovie 774002179df78ea447696cf47cde3465690a36bb
wamsoft/KAGParserEx 61d5f1e65f500b5aef5a621333f86f3457939b31
wamsoft/extrans bd0611eced7e2731a8c8403148e06abea827467e
wamsoft/layerExAreaAverage b04fbd29548e43d76293625e1d06a3141f0853d8
wamsoft/layerExBTOA a57e7a25a9e079f95b93cf9768949e0f8789f685
wamsoft/layerExImage c8e9e71f5ce475340732888ddda7270f77e19122
wamsoft/layerExRaster a4c9be0c7510cca552e27c0c3e931b2eaee399af
wamsoft/shrinkCopy 007fbba3b3531b476d6b97e7ee80553868f31b2d
PINS

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
        --x-install-root="$VENDOR/krkr2-deps/$triplet" \
        --disable-metrics
done
