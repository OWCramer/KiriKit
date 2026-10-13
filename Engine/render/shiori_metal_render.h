/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

// Shiori's Metal compositor (render manager "metal"); see shiori_metal_render.mm.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

class iTVPTexture2D;

namespace shiori::metal {

/// Commits the GPU work queued this tick (without waiting for it).
void flush();

/// The per-operation profile since the last report, averaged over `ticks`; clears it.
std::string profileReport(double ticks);

struct PresentedFrame {
    void *texture = nullptr; // id<MTLTexture>, RGBA8 (unretained; the renderer keeps it)
    void *event = nullptr;   // id<MTLSharedEvent> (unretained)
    uint64_t value = 0;      // the texture is ready once the event reaches this
};

/// The window's image as a texture for the host to present: copied on the GPU behind the work
/// queued for it (when `changed`, or the first time), with no wait on the CPU. False when the
/// image isn't one of this renderer's.
bool presentFrame(iTVPTexture2D *texture, int width, int height, bool changed, PresentedFrame &out);

} // namespace shiori::metal
