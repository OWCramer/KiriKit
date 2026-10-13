/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

#include "shiorikrkr_presenter.h"

#include <algorithm>
#include <cstring>

namespace shiorikrkr {

void PresenterBackend::BeginFrame(int, int) { drawnFirst_ = nullptr; }

void PresenterBackend::EndFrame() {
    if (drawnFirst_) presented_ = drawnFirst_;
}

void *PresenterBackend::CreateWindowTexture(int width, int height) {
    auto texture = std::make_unique<WindowTexture>();
    texture->width = std::max(width, 0);
    texture->height = std::max(height, 0);
    texture->pixels.assign(size_t(texture->width) * texture->height * 4, 0);
    WindowTexture *handle = texture.get();
    textures_.push_back(std::move(texture));
    return handle;
}

void PresenterBackend::UpdateWindowTexture(void *handle, const uint8_t *buff, int width, int height, int pitch) {
    auto *texture = static_cast<WindowTexture *>(handle);
    if (!texture || !buff) return;
    if (texture->width != width || texture->height != height) {
        texture->width = width;
        texture->height = height;
        texture->pixels.assign(size_t(width) * height * 4, 0);
    }
    const size_t row = size_t(width) * 4;
    for (int y = 0; y < height; ++y) std::memcpy(&texture->pixels[y * row], buff + size_t(y) * pitch, row);
    texture->serial = nextSerial_++;
    // A coarse look for anything not black: the host lifts its launch cover on the first such
    // frame (the game's first frames are black while its title loads).
    bool blank = true;
    for (int sy = 1; sy < 16 && blank; ++sy) {
        for (int sx = 1; sx < 16; ++sx) {
            const uint8_t *p = &texture->pixels[(size_t(sy) * height / 16) * row + (size_t(sx) * width / 16) * 4];
            if (p[0] > 8 || p[1] > 8 || p[2] > 8) {
                blank = false;
                break;
            }
        }
    }
    texture->blank = blank;
}

void PresenterBackend::DestroyWindowTexture(void *handle) {
    if (presented_ == handle) presented_ = nullptr;
    if (drawnFirst_ == handle) drawnFirst_ = nullptr;
    textures_.erase(std::remove_if(textures_.begin(), textures_.end(),
                                   [handle](const auto &texture) { return texture.get() == handle; }),
                    textures_.end());
}

void PresenterBackend::DrawWindowTexture(void *handle, float, float, float, float) {
    if (!drawnFirst_) drawnFirst_ = static_cast<WindowTexture *>(handle);
}

bool PresenterBackend::frame(PresentedFrame &out) const {
    if (!presented_ || presented_->pixels.empty()) return false;
    out.pixels = presented_->pixels.data();
    out.width = presented_->width;
    out.height = presented_->height;
    out.pitch = presented_->width * 4;
    out.serial = presented_->serial;
    out.blank = presented_->blank;
    return true;
}

} // namespace shiorikrkr
