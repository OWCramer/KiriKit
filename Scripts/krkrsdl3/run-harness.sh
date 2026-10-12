#!/bin/bash
# This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
# If a copy of the MPL was not distributed with this file, You can obtain one at
# https://mozilla.org/MPL/2.0/.
#
# Runs a KiriKiri game folder (one holding base/, optionally patch/) on the ShioriKrkr Mac
# harness. The CxDec scheme, if the game has one, is exported from GARbro's database
# (Vendor/garbro, MIT) into build/, never into the repo; the game's own folder is only linked,
# never written (saves go to the work folder).
#
#   Scripts/krkrsdl3/run-harness.sh <game folder> <work folder> [--garbro "KARAKARA 2"]
#       [--exe KARAKARA2.exe] [harness options: --frames N --click F:X,Y --capture F:file.bmp ...]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
GAME=$1
WORK=$2
shift 2
TITLE=""
ARGS=()
while [ $# -gt 0 ]; do
    case $1 in
    --garbro) TITLE=$2; shift 2 ;;
    *) ARGS+=("$1"); shift ;;
    esac
done
mkdir -p "$WORK"
SCHEME=()
if [ -n "$TITLE" ]; then
    DB="$ROOT/Vendor/garbro/ArcFormats/Resources/Formats.dat"
    if [ ! -f "$DB" ]; then
        [ -d "$ROOT/Vendor/garbro/.git" ] || git clone --depth 1 --filter=blob:none --sparse https://github.com/crskycode/GARbro "$ROOT/Vendor/garbro"
        git -C "$ROOT/Vendor/garbro" sparse-checkout set ArcFormats/KiriKiri ArcFormats/Resources
    fi
    python3 -I "$ROOT/Scripts/kirikiri/garbro-cx.py" "$DB" "$TITLE" "$WORK/scheme.shcx" >/dev/null
    SCHEME=(--scheme "$WORK/scheme.shcx")
fi
PATCH=()
[ -d "$GAME/patch" ] && PATCH=(--patch "$GAME/patch")
exec "$ROOT/build/shiorikrkr/macos/shiorikrkr-harness" --base "$GAME/base" "${PATCH[@]}" --work "$WORK" \
    "${SCHEME[@]}" --font "$ROOT/App/Resources/SiglusFonts/BIZUDGothic-Regular.ttf" "${ARGS[@]}"
