/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

// ShioriKrkr's movies: krkrsdl3's platform video players (environ/PlatformVideo.h) written for
// Shiori on its LGPL FFmpeg (the ASF demuxer and WMV 9 / WMA 2 decoders, as for CatSystem2).
// Written fresh; nothing comes from krkrsdl3's own movie code or Kirikiroid2's (Kodi's
// VideoPlayer lineage), see PROVENANCE.md.
#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

namespace shiorikrkr {

/// A movie playing in overlay mode, drawn by the host over the game's frame.
struct MovieOverlayFrame {
    std::vector<uint8_t> rgba; // straight RGBA, top row first
    int width = 0, height = 0;
    int left = 0, top = 0, right = 0, bottom = 0; // in the game's window
    bool visible = false;
    uint64_t serial = 0;
};

/// The current overlay movie, if any (one at a time, as KAG plays them). Locked while read.
std::mutex &movieOverlayLock();
MovieOverlayFrame *movieOverlay(); // nullptr when none

/// A game ended or began: every movie still alive stops and the overlay empties.
void stopAllMovies();

} // namespace shiorikrkr
