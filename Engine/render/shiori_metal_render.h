/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

// Shiori's Metal compositor (render manager "metal"); see shiori_metal_render.mm.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

class iTVPTexture2D;

namespace shiori::metal {

/// Commits the GPU work queued this tick (without waiting for it).
void flush();

/// The per-operation profile since the last report, averaged over `ticks`; clears it.
std::string profileReport(double ticks);

/// Lets `encode` put GPU work on one of this renderer's images: it gets the image's Metal buffer
/// (an `id<MTLBuffer>`, unretained; R, G, B, A rows of `pitch` bytes) and the renderer's open
/// command buffer (an `id<MTLCommandBuffer>`, no encoder open), so the work runs in order with
/// the renderer's own and nothing waits. False when the image isn't this renderer's.
bool encodeInto(iTVPTexture2D *texture, const std::function<bool(void *buffer, size_t pitch, void *commandBuffer)> &encode);

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
