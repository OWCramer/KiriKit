/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

// The render backend ShioriKrkr gives krkrsdl3 (krkrsdl3 patch 0002's host factory): krkrsdl3's
// software compositor as it is for layers and meshes, but window textures stay CPU buffers
// that the host app presents with its own Metal code, instead of going to an SDL renderer.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "backend/SWRenderBackend.h"

namespace shiorikrkr {

struct PresentedFrame {
    const uint8_t *pixels = nullptr; // RGBA, top row first
    int width = 0, height = 0, pitch = 0;
    uint64_t serial = 0;             // changes when the pixels do
    bool blank = true;               // every sampled pixel black
};

class PresenterBackend : public krkrsdl3::SWRenderBackend {
public:
    // "software": krkrsdl3 code (DrawDeviceD3D) picks its CPU paths by this name.
    const char *GetName() const override { return "software"; }

    void BeginFrame(int winWidth, int winHeight) override;
    void EndFrame() override;
    void *CreateWindowTexture(int width, int height) override;
    void UpdateWindowTexture(void *handle, const uint8_t *buff, int width, int height, int pitch) override;
    void DestroyWindowTexture(void *handle) override;
    void DrawWindowTexture(void *handle, float posX, float posY, float width, float height) override;

    /// The main window's latest composed frame (the first window texture drawn last pass).
    bool frame(PresentedFrame &out) const;

private:
    struct WindowTexture {
        std::vector<uint8_t> pixels;
        int width = 0, height = 0;
        uint64_t serial = 0;
        bool blank = true;
    };
    std::vector<std::unique_ptr<WindowTexture>> textures_;
    WindowTexture *drawnFirst_ = nullptr;
    WindowTexture *presented_ = nullptr;
    uint64_t nextSerial_ = 1;
};

} // namespace shiorikrkr
