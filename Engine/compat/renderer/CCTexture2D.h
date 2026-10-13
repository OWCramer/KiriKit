/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

// Shiori has no cocos2d-x. krkr2's software textures still define GetAdapterTexture (their
// hand-off to a cocos2d-x sprite), which Shiori never calls: this stand-in lets those unused
// methods compile. Shiori's window layer presents textures itself (environ/).
#pragma once

namespace cocos2d {
struct Size {
    float width = 0, height = 0;
    static const Size ZERO;
};
inline const Size Size::ZERO{};
class Texture2D {
public:
    enum class PixelFormat { RGBA8888, I8, A8 };
    int getPixelsWide() const { return 0; }
    int getPixelsHigh() const { return 0; }
    void autorelease() {}
    bool initWithData(const void *, long, PixelFormat, int, int, const Size &) { return false; }
    bool updateWithData(const void *, int, int, int, int) { return false; }
};
} // namespace cocos2d
