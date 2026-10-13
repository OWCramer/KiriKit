#!/bin/bash
# This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
# If a copy of the MPL was not distributed with this file, You can obtain one at
# https://mozilla.org/MPL/2.0/.
#
# Fetches what Shiori's own KiriKiri engine (ShioriKrkr) is built from, into Vendor/
# (gitignored):
#   - krkr2 (2468785842/krkr2) at the commit AetherKrkr forked from, with Shiori's changes
#     (Scripts/krkr2/patches) applied: the engine's core (KiriKiri licence);
#   - krkrsdl3 at a pinned commit, unpatched: a few plug-ins krkr2 lacks are compiled from it
#     (KiriKiri licence plus its publish-your-changes term);
#   - vcpkg, and the dependencies it builds for each slice.
# Nothing here comes from Vendor/Aether.
#
#   Scripts/krkr2/fetch.sh            # all three slices
#   Scripts/krkr2/fetch.sh arm64-osx  # one slice
set -euo pipefail

export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$ROOT/Scripts/krkr2"
VENDOR="$ROOT/Vendor"

KRKR2_URL=https://github.com/2468785842/krkr2
KRKR2_COMMIT=3ac366c439a142f1712ed3059cb80184cb102dbf   # 2025-09-21, AetherKrkr's fork point
KRKRSDL3_URL=https://github.com/krkrsdl3/krkrsdl3
KRKRSDL3_COMMIT=358b0469bd41ffd2fc8cc08bcaa1e205e817aff9   # 2026-09-30
VCPKG_URL=https://github.com/microsoft/vcpkg
VCPKG_COMMIT=${VCPKG_COMMIT:-e456309491fd875487bf02b8a0dca76c1b1b00cc}   # 2026-10-10

mkdir -p "$VENDOR"
if [ ! -d "$VENDOR/krkr2/.git" ]; then
    git clone "$KRKR2_URL" "$VENDOR/krkr2"
fi
git -C "$VENDOR/krkr2" fetch --quiet origin || true
git -C "$VENDOR/krkr2" checkout --quiet -- .
git -C "$VENDOR/krkr2" checkout --quiet "$KRKR2_COMMIT"
for patch in "$HERE"/patches/*.patch; do
    [ -e "$patch" ] || continue
    git -C "$VENDOR/krkr2" apply --whitespace=nowarn "$patch"
done

if [ ! -d "$VENDOR/krkrsdl3/.git" ]; then
    git clone "$KRKRSDL3_URL" "$VENDOR/krkrsdl3"
fi
git -C "$VENDOR/krkrsdl3" fetch --quiet origin || true
git -C "$VENDOR/krkrsdl3" checkout --quiet -- .
git -C "$VENDOR/krkrsdl3" checkout --quiet "$KRKRSDL3_COMMIT"

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
