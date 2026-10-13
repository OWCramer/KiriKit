/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

#include "cursor_decode.h"

#include <algorithm>
#include <cstring>

#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>

namespace shiorikrkr {

namespace {

uint16_t u16(const uint8_t *p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t u32(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

bool decodePNG(const uint8_t *data, size_t size, DecodedCursor &out) {
    CFDataRef cfData = CFDataCreate(nullptr, data, CFIndex(size));
    CGImageSourceRef source = cfData ? CGImageSourceCreateWithData(cfData, nullptr) : nullptr;
    CGImageRef image = source ? CGImageSourceCreateImageAtIndex(source, 0, nullptr) : nullptr;
    bool ok = false;
    if (image) {
        const size_t w = CGImageGetWidth(image), h = CGImageGetHeight(image);
        out.rgba.assign(w * h * 4, 0);
        CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
        CGContextRef context = CGBitmapContextCreate(out.rgba.data(), w, h, 8, w * 4, space,
                                                     kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
        if (context) {
            CGContextDrawImage(context, CGRectMake(0, 0, w, h), image);
            CGContextRelease(context);
            // Back to straight alpha.
            for (size_t i = 0; i < w * h; ++i) {
                uint8_t *p = &out.rgba[i * 4];
                if (p[3] && p[3] < 255)
                    for (int c = 0; c < 3; ++c) p[c] = uint8_t(std::min(255, p[c] * 255 / p[3]));
            }
            out.width = int(w);
            out.height = int(h);
            ok = true;
        }
        CGColorSpaceRelease(space);
        CGImageRelease(image);
    }
    if (source) CFRelease(source);
    if (cfData) CFRelease(cfData);
    return ok;
}

bool decodeDIB(const uint8_t *data, size_t size, DecodedCursor &out) {
    if (size < 40 || u32(data) < 40) return false;
    const uint32_t headerSize = u32(data);
    const int width = int(int32_t(u32(data + 4)));
    const int height = int(int32_t(u32(data + 8))) / 2; // XOR and AND masks stacked
    const int bpp = u16(data + 14);
    const uint32_t compression = u32(data + 16);
    uint32_t paletteCount = u32(data + 32);
    if (width <= 0 || height <= 0 || width > 1024 || height > 1024 || compression != 0) return false;
    if (bpp <= 8 && paletteCount == 0) paletteCount = 1u << bpp;
    if (bpp > 8) paletteCount = 0;
    const uint8_t *palette = data + headerSize;
    const uint8_t *pixels = palette + paletteCount * 4;
    const size_t xorStride = size_t((width * bpp + 31) / 32) * 4;
    const size_t andStride = size_t((width + 31) / 32) * 4;
    const uint8_t *mask = pixels + xorStride * height;
    if (size_t(mask + andStride * height - data) > size) return false;

    out.width = width;
    out.height = height;
    out.rgba.assign(size_t(width) * height * 4, 0);
    bool anyAlpha = false;
    for (int y = 0; y < height; ++y) {
        const uint8_t *row = pixels + xorStride * (height - 1 - y); // bottom-up
        for (int x = 0; x < width; ++x) {
            uint8_t *o = &out.rgba[(size_t(y) * width + x) * 4];
            uint8_t b = 0, g = 0, r = 0, a = 255;
            if (bpp == 32) {
                b = row[x * 4]; g = row[x * 4 + 1]; r = row[x * 4 + 2]; a = row[x * 4 + 3];
                if (a) anyAlpha = true;
            } else if (bpp == 24) {
                b = row[x * 3]; g = row[x * 3 + 1]; r = row[x * 3 + 2];
            } else {
                const int bit = x * bpp;
                const int index = (row[bit / 8] >> (8 - bpp - bit % 8)) & ((1 << bpp) - 1);
                if (uint32_t(index) < paletteCount) {
                    b = palette[index * 4]; g = palette[index * 4 + 1]; r = palette[index * 4 + 2];
                }
            }
            o[0] = r; o[1] = g; o[2] = b; o[3] = a;
        }
    }
    // The AND mask gives transparency where the colour data has no alpha of its own.
    if (bpp != 32 || !anyAlpha) {
        for (int y = 0; y < height; ++y) {
            const uint8_t *row = mask + andStride * (height - 1 - y);
            for (int x = 0; x < width; ++x)
                if ((row[x / 8] >> (7 - x % 8)) & 1) out.rgba[(size_t(y) * width + x) * 4 + 3] = 0;
        }
    }
    return true;
}

bool decodeCUR(const uint8_t *data, size_t size, DecodedCursor &out) {
    if (size < 6 + 16 || u16(data) != 0 || (u16(data + 2) != 1 && u16(data + 2) != 2) || u16(data + 4) == 0) return false;
    // The largest entry.
    const int count = u16(data + 4);
    int best = -1, bestArea = -1;
    for (int i = 0; i < count && size_t(6 + (i + 1) * 16) <= size; ++i) {
        const uint8_t *e = data + 6 + i * 16;
        const int w = e[0] ? e[0] : 256, h = e[1] ? e[1] : 256;
        if (w * h > bestArea) { bestArea = w * h; best = i; }
    }
    if (best < 0) return false;
    const uint8_t *entry = data + 6 + best * 16;
    const uint32_t length = u32(entry + 8), offset = u32(entry + 12);
    if (offset >= size || length > size - offset) return false;
    const uint8_t *image = data + offset;
    const bool ok = (length >= 8 && std::memcmp(image, "\x89PNG", 4) == 0) ? decodePNG(image, length, out)
                                                                           : decodeDIB(image, length, out);
    if (!ok) return false;
    const bool cursor = u16(data + 2) == 2;
    out.hotX = cursor ? u16(entry + 4) : 0;
    out.hotY = cursor ? u16(entry + 6) : 0;
    return true;
}

} // namespace

bool decodeCursor(const uint8_t *data, size_t size, DecodedCursor &out) {
    if (!data || size < 12) return false;
    if (std::memcmp(data, "RIFF", 4) == 0 && std::memcmp(data + 8, "ACON", 4) == 0) {
        // Animated: the first "icon" chunk of the "fram" list.
        size_t pos = 12;
        while (pos + 8 <= size) {
            const uint32_t length = u32(data + pos + 4);
            if (std::memcmp(data + pos, "LIST", 4) == 0 && pos + 12 <= size && std::memcmp(data + pos + 8, "fram", 4) == 0) {
                size_t inner = pos + 12;
                const size_t end = std::min(size, pos + 8 + size_t(length));
                while (inner + 8 <= end) {
                    const uint32_t chunk = u32(data + inner + 4);
                    if (std::memcmp(data + inner, "icon", 4) == 0 && inner + 8 + chunk <= end)
                        return decodeCUR(data + inner + 8, chunk, out);
                    inner += 8 + chunk + (chunk & 1);
                }
                return false;
            }
            pos += 8 + length + (length & 1);
        }
        return false;
    }
    return decodeCUR(data, size, out);
}

} // namespace shiorikrkr
