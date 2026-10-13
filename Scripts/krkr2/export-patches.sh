#!/bin/bash
# This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
# If a copy of the MPL was not distributed with this file, You can obtain one at
# https://mozilla.org/MPL/2.0/.
#
# Writes Shiori's changes to the upstream checkouts in Vendor/ (edited in place while
# working) back into Scripts/krkr2/patches, where fetch.sh applies them from:
#   patches/krkr2/shiori.patch         krkr2's core (KiriKiri licence)
#   patches/krkrsdl3/shiori.patch      the krkrsdl3 plug-ins Shiori builds (its term asks
#                                      that changes be published: this file is that)
#   patches/wamsoft/<repo>.patch       wamsoft plug-ins
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/Scripts/krkr2/patches"
VENDOR="$ROOT/Vendor"
mkdir -p "$OUT/krkr2" "$OUT/krkrsdl3" "$OUT/wamsoft"
git -C "$VENDOR/krkr2" diff > "$OUT/krkr2/shiori.patch"
git -C "$VENDOR/krkrsdl3" add -A -N . >/dev/null 2>&1 || true
git -C "$VENDOR/krkrsdl3" diff > "$OUT/krkrsdl3/shiori.patch"
for repo in "$VENDOR"/upstream-plugins/wamsoft/*; do
    name=$(basename "$repo")
    if [ -n "$(git -C "$repo" diff --stat)" ]; then
        git -C "$repo" diff > "$OUT/wamsoft/$name.patch"
    else
        rm -f "$OUT/wamsoft/$name.patch"
    fi
done
for f in "$OUT"/*/*.patch; do [ -s "$f" ] || rm -f "$f"; done
ls -la "$OUT"/*/
