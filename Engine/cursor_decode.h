/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

// Windows cursor files (.cur, and an animated .ani's first frame) to RGBA, for the host to
// draw at the pointer. Written from the formats' public descriptions (ICONDIR/ICONDIRENTRY,
// BITMAPINFOHEADER with its XOR and AND masks, PNG entries, RIFF "ACON").
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace shiorikrkr {

struct DecodedCursor {
    std::vector<uint8_t> rgba; // straight alpha, top row first
    int width = 0, height = 0;
    int hotX = 0, hotY = 0;
};

bool decodeCursor(const uint8_t *data, size_t size, DecodedCursor &out);

} // namespace shiorikrkr
