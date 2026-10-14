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

/// layerExRaster's copyRaster on the GPU: rows of `source` shifted by offsets[row] into
/// `target` within the clip; false when either image isn't this renderer's (the CPU does it).
bool rasterCopy(iTVPTexture2D *target, iTVPTexture2D *source, int left, int top, int width, int height, const int *offsets);

/// One of layerExImage's per-pixel operations on a rectangle of one of this renderer's images
/// (pixels R, G, B, A): 1 a lookup table on R, G and B (`lut`), 2 colorize (i0 hue, i1
/// saturation, i2/i3 the blend's weights out of 256, f0 1 for a full blend), 3 modulate (f0 hue
/// turns, f1 saturation, f2 luminance, -1..1), 4 noise (i0 level), 5 white noise. False when the
/// image isn't this renderer's or the rectangle isn't inside it (the CPU does it).
struct ImageOp {
    unsigned op = 0;
    int left = 0, top = 0, width = 0, height = 0;
    int i[4] = {};
    float f[3] = {};
    unsigned seed = 0;
    unsigned char lut[256] = {};
};
bool imageOp(iTVPTexture2D *texture, const ImageOp &op);

/// layerExImage's gaussianBlur on a rectangle: rows, then columns, with the plug-in's weights.
bool gaussianBlur(iTVPTexture2D *texture, int left, int top, int width, int height, const float *matrix, int length);

/// Seconds the CPU has waited for the GPU in all (for slow-tick lines).
double gpuWaitSeconds();

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
