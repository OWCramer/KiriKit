/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

// Shiori's Metal compositor kernels (MSL, compiled at startup). One compute kernel runs a
// layer operation over a rectangle, one thread per pixel, on the engine's own pixel buffers
// (32-bit pixels as KiriKiri packs them: alpha in the top byte, the colour bytes below it, in
// the order krkr2 keeps them in memory). The arithmetic of each operation is KiriKiri's
// integer blend math (krkr2's C blend functors and tvpgl, KiriKiri licence) restated per
// pixel, so a GPU result matches the CPU one bit for bit; the verifier in
// shiori_metal_render.mm checks that.
#pragma once

namespace shiori::metal {

// Operation codes; the kernel and the render manager share them.
enum Op : unsigned {
    OpCopy = 1, OpCopyOpaque, OpCopyColor, OpCopyMask,
    OpAlphaHDA = 10, OpAlphaHDAo, OpAlphaD, OpAlphaDo, OpAlphaA, OpAlphaAo,
    OpAddAlphaHDA = 20, OpAddAlphaHDAo, OpAddAlphaA, OpAddAlphaAo,
    OpConstAlpha = 30, OpConstAlphaD, OpConstAlphaA,
    OpConstAlphaSD = 40, OpConstAlphaSDa, OpConstAlphaSDd,
    OpFillARGB = 50, OpFillARGB8, OpFillColor, OpFillMask, OpConstColorAlpha, OpConstColorAlphaD,
    OpConstColorAlphaA, OpRemoveConstOpacity,
    OpUniv = 60, OpUnivSwitch, OpUnivD, OpUnivSwitchD, OpUnivA, OpUnivSwitchA,
    OpBoxBlurRows = 70, OpBoxBlurColumns, // a box blur's two passes (sums through the temp buffer)
    // Photoshop-style blends keeping the target's alpha: OpPs + kind, OpPsOpa + kind with opacity.
    OpPs = 80, OpPsOpa = 96,
};

// The Photoshop-style kinds, in krkr2's order of their names.
enum PsKind : unsigned {
    PsAlpha, PsAdd, PsSub, PsMul, PsScreen, PsOverlay, PsHardLight, PsSoftLight, PsColorDodge, PsColorDodge5,
    PsColorBurn, PsLighten, PsDarken, PsDiff, PsDiff5, PsExclusion,
};

// Parameters, laid out the same in C++ and MSL. Offsets and pitches count elements (32-bit
// pixels, or bytes for the 8-bit rule image).
struct KernelParams {
    unsigned op;
    unsigned width, height;
    unsigned tarOffset, tarPitch;
    unsigned srcOffset, srcPitch;   // the source (src1 for two-source operations)
    unsigned dstOffset, dstPitch;   // the second source (src2)
    unsigned ruleOffset, rulePitch; // the rule image (8-bit)
    unsigned opa, color;
    int src1lv, src2lv;
    // A sampled source (stretches and warps; 0 reads src as it lies): 1 warp, nearest; 2 warp,
    // bilinear; 3 stretch, nearest; 4 stretch, bilinear; 5 stretch, area (shrinking). The
    // source is the srcWidth x srcHeight
    // view at srcOffset; outside it pixels are 0.
    unsigned sample;
    int srcWidth, srcHeight;
    float matrix[9]; // warps: target pixel -> source (homogeneous); stretches: scale x, y
};

static const char *kKernelSource = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct KernelParams {
    uint op;
    uint width, height;
    uint tarOffset, tarPitch;
    uint srcOffset, srcPitch;
    uint dstOffset, dstPitch;
    uint ruleOffset, rulePitch;
    uint opa, color;
    int src1lv, src2lv;
    uint sample;
    int srcWidth, srcHeight;
    float matrix[9];
};

// Saturating per-byte add of two packed pixels.
static inline uint satAdd(uint a, uint b) {
    uint t = ((a & b) + (((a ^ b) >> 1) & 0x7f7f7f7fu)) & 0x80808080u;
    t = (t << 1) - (t >> 7);
    return (a + b - t) | t;
}

// Colour bytes of d towards s by a/256; the alpha byte comes out 0.
static inline uint mixColour(uint d, uint s, uint a) {
    uint d1 = d & 0xff00ffu;
    d1 = (d1 + (((s & 0xff00ffu) - d1) * a >> 8)) & 0xff00ffu;
    d &= 0xff00u;
    s &= 0xff00u;
    return d1 + ((d + ((s - d) * a >> 8)) & 0xff00u);
}

// Colour bytes scaled by fac/256 (alpha byte 0).
static inline uint scaleColour(uint c, uint fac) {
    return ((((c & 0x00ff00u) * fac) & 0x00ff0000u) + (((c & 0xff00ffu) * fac) & 0xff00ff00u)) >> 8;
}

static inline uint premultiply(uint s) { return scaleColour(s, s >> 24) + (s & 0xff000000u); }

// Premultiplied source over premultiplied destination; the result's alpha is the union.
static inline uint premulOver(uint d, uint s) {
    uint da = d >> 24, sa = s >> 24;
    da = da + sa - (da * sa >> 8);
    da -= da >> 8;
    sa ^= 0xffu;
    s &= 0xffffffu;
    return (da << 24) + satAdd((((d & 0xff00ffu) * sa >> 8) & 0xff00ffu) + (((d & 0xff00u) * sa >> 8) & 0xff00u), s);
}

// Premultiplied source over an opaque-treated destination (alpha byte from the add).
static inline uint premulOverColour(uint d, uint s) {
    uint inv = (~s) >> 24;
    return satAdd((((d & 0xff00ffu) * inv >> 8) & 0xff00ffu) + (((d & 0xff00u) * inv >> 8) & 0xff00u), s);
}

// Premultiplied, source opacity and its inverse given apart.
static inline uint premulOverSplit(uint d, uint sopa, uint sinv, uint s) {
    uint da = d >> 24;
    da = da + sopa - (da * sopa >> 8);
    da -= da >> 8;
    return (da << 24) + satAdd((((d & 0xff00ffu) * sinv >> 8) & 0xff00ffu) + (((d & 0xff00u) * sinv >> 8) & 0xff00u), s);
}

// a * ratio + b * (1 - ratio), every byte.
static inline uint blendARGB(uint b, uint a, uint ratio) {
    uint b2 = b & 0x00ff00ffu;
    uint t = (b2 + (((a & 0x00ff00ffu) - b2) * ratio >> 8)) & 0x00ff00ffu;
    b2 = (b & 0xff00ff00u) >> 8;
    return t + (((b2 + ((((a & 0xff00ff00u) >> 8) - b2) * ratio >> 8)) << 8) & 0xff00ff00u);
}

// Alpha-on-alpha: the source's opacity on a destination that has its own.
static inline uint alphaOnAlpha(uint d, uint s, uint addr, device const uchar *ooo, device const uchar *negmul) {
    return mixColour(d, s, ooo[addr]) + (uint(negmul[addr]) << 24);
}

// A source pixel of the sampled view (0 outside it).
static inline uint fetch(device const uint *src, constant KernelParams &p, int x, int y) {
    if (x < 0 || y < 0 || x >= p.srcWidth || y >= p.srcHeight) return 0;
    return src[p.srcOffset + uint(y) * p.srcPitch + uint(x)];
}

// Bilinear, each byte rounded half up (as the CPU's resampler does).
static inline uint bilinear(device const uint *src, constant KernelParams &p, float x, float y) {
    int x0 = int(floor(x)), y0 = int(floor(y));
    float fx = x - float(x0), fy = y - float(y0);
    uint a = fetch(src, p, x0, y0), b = fetch(src, p, x0 + 1, y0);
    uint c = fetch(src, p, x0, y0 + 1), d = fetch(src, p, x0 + 1, y0 + 1);
    uint r = 0;
    for (uint shift = 0; shift < 32; shift += 8) {
        float top = float((a >> shift) & 0xffu) * (1 - fx) + float((b >> shift) & 0xffu) * fx;
        float bottom = float((c >> shift) & 0xffu) * (1 - fx) + float((d >> shift) & 0xffu) * fx;
        float v = clamp(floor(top * (1 - fy) + bottom * fy + 0.5f), 0.0f, 255.0f);
        r |= uint(v) << shift;
    }
    return r;
}

// The sampled source pixel for target pixel (x, y) of the operation's rectangle.
static inline uint sampled(device const uint *src, constant KernelParams &p, uint2 gid) {
    float x = float(gid.x), y = float(gid.y);
    if (p.sample <= 2) {
        // Warp: inverse map, nothing beyond a pixel outside the view.
        float w = p.matrix[6] * x + p.matrix[7] * y + p.matrix[8];
        if (fabs(w) < 1e-12f) return 0;
        float sx = (p.matrix[0] * x + p.matrix[1] * y + p.matrix[2]) / w;
        float sy = (p.matrix[3] * x + p.matrix[4] * y + p.matrix[5]) / w;
        if (sx < -1 || sy < -1 || sx > float(p.srcWidth) || sy > float(p.srcHeight)) return 0;
        if (p.sample == 1) return fetch(src, p, int(floor(sx + 0.5f)), int(floor(sy + 0.5f)));
        return bilinear(src, p, sx, sy);
    }
    // Stretch: each target pixel's place in the view, kept inside it.
    if (p.sample == 3) {
        float sx = clamp(floor(x * p.matrix[0]), 0.0f, float(p.srcWidth - 1));
        float sy = clamp(floor(y * p.matrix[1]), 0.0f, float(p.srcHeight - 1));
        return fetch(src, p, int(sx), int(sy));
    }
    if (p.sample == 5) {
        // Shrinking by area: the average over the target pixel's footprint, partial pixels
        // weighted by how much of them it covers.
        float y0 = y * p.matrix[1], y1 = min((y + 1) * p.matrix[1], float(p.srcHeight));
        float x0 = x * p.matrix[0], x1 = min((x + 1) * p.matrix[0], float(p.srcWidth));
        float4 sum = 0;
        float area = 0;
        for (int py = int(y0); py < int(ceil(y1)); ++py) {
            float wy = min(float(py + 1), y1) - max(float(py), y0);
            for (int px = int(x0); px < int(ceil(x1)); ++px) {
                float w = wy * (min(float(px + 1), x1) - max(float(px), x0));
                uint c = src[p.srcOffset + uint(py) * p.srcPitch + uint(px)];
                sum += w * float4(c & 0xffu, (c >> 8) & 0xffu, (c >> 16) & 0xffu, c >> 24);
                area += w;
            }
        }
        if (area <= 0) return 0;
        uint4 v = uint4(clamp(floor(sum / area + 0.5f), 0.0f, 255.0f));
        return v.x | (v.y << 8) | (v.z << 16) | (v.w << 24);
    }
    float sx = clamp((x + 0.5f) * p.matrix[0] - 0.5f, 0.0f, float(p.srcWidth - 1));
    float sy = clamp((y + 0.5f) * p.matrix[1] - 0.5f, 0.0f, float(p.srcHeight - 1));
    return bilinear(src, p, sx, sy);
}

// The Photoshop-style blends (KiriKiri's Kenjo blender): s's colour blended into d by a/256.
static inline uint psMix(uint d, uint s, uint a) {
    uint d1 = d & 0x00ff00ffu, d2 = d & 0x0000ff00u;
    return (((((s & 0x00ff00ffu) - d1) * a) >> 8) + d1) & 0x00ff00ffu | (((((s & 0x0000ff00u) - d2) * a) >> 8) + d2) & 0x0000ff00u;
}

// Per channel through a 256 x 256 table (indexed [first][second]).
static inline uint psTable(device const uchar *t, uint first, uint second) {
    return (uint(t[((first >> 16) & 0xffu) * 256 + ((second >> 16) & 0xffu)]) << 16) |
        (uint(t[((first >> 8) & 0xffu) * 256 + ((second >> 8) & 0xffu)]) << 8) |
        uint(t[(first & 0xffu) * 256 + (second & 0xffu)]);
}

// Tables: soft light, colour dodge, colour burn, overlay (64 KiB each).
static inline uint psBlend(uint kind, uint d, uint s, uint a, device const uchar *tables) {
    device const uchar *softLight = tables, *colorDodge = tables + 65536, *colorBurn = tables + 131072,
                       *overlay = tables + 196608;
    uint n;
    switch (kind) {
    case 0: return psMix(d, s, a);
    case 1:
        n = (((d & s) << 1) + ((d ^ s) & 0x00fefefeu)) & 0x01010100u;
        n = ((n >> 8) + 0x007f7f7fu) ^ 0x007f7f7fu;
        return psMix(d, (d + s - n) | n, a);
    case 2:
        s = ~s;
        n = (((~d & s) << 1) + ((~d ^ s) & 0x00fefefeu)) & 0x01010100u;
        n = ((n >> 8) + 0x007f7f7fu) ^ 0x007f7f7fu;
        return psMix(d, (d | n) - (s | n), a);
    case 3:
        s = (((((d >> 16) & 0xffu) * (s & 0x00ff0000u)) & 0xff000000u) | ((((d >> 8) & 0xffu) * (s & 0x0000ff00u)) & 0x00ff0000u) |
             (((d >> 0) & 0xffu) * (s & 0x000000ffu))) >> 8;
        return psMix(d, s, a);
    case 4: {
        uint sd1 = (((((d >> 16) & 0xffu) * (s & 0x00ff0000u)) & 0xff000000u) | (((d >> 0) & 0xffu) * (s & 0x000000ffu))) >> 8;
        uint sd2 = (((((d >> 8) & 0xffu) * (s & 0x0000ff00u)) & 0x00ff0000u)) >> 8;
        return ((((((s & 0x00ff00ffu) - sd1) * a) >> 8) + (d & 0x00ff00ffu)) & 0x00ff00ffu) |
            ((((((s & 0x0000ff00u) - sd2) * a) >> 8) + (d & 0x0000ff00u)) & 0x0000ff00u);
    }
    case 5: return psMix(d, psTable(overlay, s, d), a);
    case 6: return psMix(d, psTable(overlay, d, s), a);
    case 7: return psMix(d, psTable(softLight, s, d), a);
    case 8: return psMix(d, psTable(colorDodge, s, d), a);
    case 9:
        s = ((((s & 0x00ff00ffu) * a) >> 8) & 0x00ff00ffu) | ((((s & 0x0000ff00u) * a) >> 8) & 0x0000ff00u);
        return psTable(colorDodge, s, d);
    case 10: return psMix(d, psTable(colorBurn, s, d), a);
    case 11:
    case 12:
    case 13:
        n = (((~d & s) << 1) + ((~d ^ s) & 0x00fefefeu)) & 0x01010100u;
        n = ((n >> 8) + 0x007f7f7fu) ^ 0x007f7f7fu;
        if (kind == 11) return psMix(d, (s & n) | (d & ~n), a);
        if (kind == 12) return psMix(d, (d & n) | (s & ~n), a);
        return psMix(d, ((s & n) - (d & n)) | ((d & ~n) - (s & ~n)), a);
    case 14:
        s = ((((s & 0x00ff00ffu) * a) >> 8) & 0x00ff00ffu) | ((((s & 0x0000ff00u) * a) >> 8) & 0x0000ff00u);
        n = (((~d & s) << 1) + ((~d ^ s) & 0x00fefefeu)) & 0x01010100u;
        n = ((n >> 8) + 0x007f7f7fu) ^ 0x007f7f7fu;
        return ((s & n) - (d & n)) | ((d & ~n) - (s & ~n));
    default: {
        uint sd1 = (((((d >> 16) & 0xffu) * ((s & 0x00ff0000u) >> 7)) & 0x01ff0000u) | ((((d >> 0) & 0xffu) * (s & 0x000000ffu)) >> 7));
        uint sd2 = (((((d >> 8) & 0xffu) * (s & 0x0000ff00u)) & 0x00ff8000u)) >> 7;
        return ((((((s & 0x00ff00ffu) - sd1) * a) >> 8) + (d & 0x00ff00ffu)) & 0x00ff00ffu) |
            ((((((s & 0x0000ff00u) - sd2) * a) >> 8) + (d & 0x0000ff00u)) & 0x0000ff00u);
    }
    }
}

// Reflect-101 edges (… 2 1 | 0 1 2 … n-2 n-1 | n-2 …).
static inline int reflect101(int i, int n) {
    if (n == 1) return 0;
    while (i < 0 || i >= n) i = i < 0 ? -i : 2 * n - 2 - i;
    return i;
}

kernel void shioriLayerOp(device uint *tar [[buffer(0)]],
                          device const uint *src [[buffer(1)]],
                          device const uint *dst [[buffer(2)]],
                          device const uchar *rule [[buffer(3)]],
                          constant KernelParams &p [[buffer(4)]],
                          device const uchar *ooo [[buffer(5)]],
                          device const uchar *negmul [[buffer(6)]],
                          constant uint *table [[buffer(7)]],
                          device uchar *tar8 [[buffer(8)]],
                          device uint4 *temp [[buffer(9)]],
                          device const uchar *psTables [[buffer(10)]],
                          uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= p.width || gid.y >= p.height) return;
    if (p.op == 70 || p.op == 71) {
        // Box blur (a box of matrix[0] x matrix[1] pixels about each, edges reflected): sums
        // along the rows of the source into temp, then down its columns into the target.
        int kw = int(p.matrix[0]), kh = int(p.matrix[1]);
        int w = int(p.width), h = int(p.height);
        int x = int(gid.x), y = int(gid.y);
        uint4 sum = 0;
        if (p.op == 70) {
            for (int k = 0; k < kw; ++k) {
                int sx = reflect101(x - kw / 2 + k, w);
                uint c = src[p.srcOffset + uint(y) * p.srcPitch + uint(sx)];
                sum += uint4(c & 0xffu, (c >> 8) & 0xffu, (c >> 16) & 0xffu, c >> 24);
            }
            temp[gid.y * p.width + gid.x] = sum;
            return;
        }
        for (int k = 0; k < kh; ++k) sum += temp[uint(reflect101(y - kh / 2 + k, h)) * p.width + gid.x];
        float4 v = clamp(floor(float4(sum) / float(kw * kh) + 0.5f), 0.0f, 255.0f);
        uint4 b = uint4(v);
        tar[p.tarOffset + gid.y * p.tarPitch + gid.x] = b.x | (b.y << 8) | (b.z << 16) | (b.w << 24);
        return;
    }
    uint ti = p.tarOffset + gid.y * p.tarPitch + gid.x;
    uint si = p.srcOffset + gid.y * p.srcPitch + gid.x;
    uint di = p.dstOffset + gid.y * p.dstPitch + gid.x;
    uint opa = p.opa;
    uint sv = (p.op < 40 || p.op >= 80) ? (p.sample ? sampled(src, p, gid) : src[si]) : 0;
    if (p.op >= 80) {
        // Photoshop-style, the target's alpha kept.
        uint d = tar[ti];
        uint kind = p.op >= 96 ? p.op - 96 : p.op - 80;
        uint a = p.op >= 96 ? ((sv >> 24) * opa) >> 8 : sv >> 24;
        tar[ti] = (psBlend(kind, d, sv, a, psTables) & 0x00ffffffu) | (d & 0xff000000u);
        return;
    }
    uint r;
    switch (p.op) {
    // ---- tar = f(tar, src)
    case 1: r = sv; break;
    case 2: r = 0xff000000u | sv; break;
    case 3: r = (tar[ti] & 0xff000000u) + (sv & 0x00ffffffu); break;
    case 4: r = (sv & 0xff000000u) + (tar[ti] & 0x00ffffffu); break;
    case 10: { uint d = tar[ti], s = sv; r = (mixColour(d, s, s >> 24) & 0x00ffffffu) | (d & 0xff000000u); break; }
    case 11: { uint d = tar[ti], s = sv; r = (mixColour(d, s, ((s >> 24) * opa) >> 8) & 0x00ffffffu) | (d & 0xff000000u); break; }
    case 12: { uint d = tar[ti], s = sv; r = alphaOnAlpha(d, s, ((s >> 16) & 0xff00u) + (d >> 24), ooo, negmul); break; }
    case 13: { uint d = tar[ti], s = sv; r = alphaOnAlpha(d, s, (((s >> 24) * opa) & 0xff00u) + (d >> 24), ooo, negmul); break; }
    case 14: r = premulOver(tar[ti], premultiply(sv)); break;
    case 15: { uint s = sv; s = (s & 0xffffffu) + ((((s >> 24) * opa) >> 8) << 24); r = premulOver(tar[ti], premultiply(s)); break; }
    case 20: { uint d = tar[ti]; r = (premulOverColour(d, sv) & 0x00ffffffu) | (d & 0xff000000u); break; }
    case 21: {
        uint d = tar[ti], s = sv;
        s = (((s & 0xff00ffu) * opa >> 8) & 0xff00ffu) + (((s >> 8) & 0xff00ffu) * opa & 0xff00ff00u);
        r = (premulOverColour(d, s) & 0x00ffffffu) + (d & 0xff000000u);
        break;
    }
    case 22: r = premulOver(tar[ti], sv); break;
    case 23: {
        uint s = sv;
        s = (((s & 0xff00ffu) * opa >> 8) & 0xff00ffu) + (((s >> 8) & 0xff00ffu) * opa & 0xff00ff00u);
        r = premulOver(tar[ti], s);
        break;
    }
    case 30: r = mixColour(tar[ti], sv, opa); break;
    case 31: { uint d = tar[ti]; r = alphaOnAlpha(d, sv, (opa << 8) + (d >> 24), ooo, negmul); break; }
    case 32: r = premulOver(tar[ti], (sv & 0xffffffu) | (opa << 24)); break;
    // ---- tar = f(src, dst)
    case 40: r = mixColour(src[si], dst[di], opa); break;
    case 41: r = blendARGB(src[si], dst[di], opa); break;
    case 42: {
        uint s1 = src[si], s2 = dst[di];
        uint o = opa > 127u ? opa + 1u : opa;
        uint io = opa > 127u ? 255u - opa : 256u - opa;
        uint a1 = s1 >> 24, a2 = s2 >> 24;
        uint alpha = ooo[(a2 * o & 0xff00u) + (a1 * io >> 8)];
        r = mixColour(s1, s2, alpha) | ((a1 + ((a2 - a1) * o >> 8)) << 24);
        break;
    }
    // ---- tar = f(tar)
    case 50: r = p.color; break;
    case 51: tar8[p.tarOffset + gid.y * p.tarPitch + gid.x] = uchar(p.color); return;
    case 52: r = (tar[ti] & 0xff000000u) + (p.color & 0xffffffu); break;
    case 53: r = (tar[ti] & 0x00ffffffu) + (opa << 24); break;
    case 54: {
        uint d = tar[ti], c = p.color, inv = 255u - opa;
        r = (d & 0xff000000u) + ((((d & 0xff00ffu) * inv + (c & 0xff00ffu) * opa) >> 8) & 0xff00ffu) +
            ((((d & 0xff00u) * inv + (c & 0xff00u) * opa) >> 8) & 0xff00u);
        break;
    }
    case 55: {
        uint d = tar[ti], c = p.color, da = d >> 24;
        uint alpha = ooo[da + (opa << 8)];
        uint d1 = d & 0xff00ffu;
        d1 = ((d1 + (((c & 0xff00ffu) - d1) * alpha >> 8)) & 0xff00ffu) | ((255u - ((255u - da) * (255u - opa) >> 8)) << 24);
        d &= 0xff00u;
        r = d1 | ((d + (((c & 0xff00u) - d) * alpha >> 8)) & 0xff00u);
        break;
    }
    case 56: r = premulOverSplit(tar[ti], opa, opa ^ 0xffu, scaleColour(p.color, opa)); break;
    case 57: { uint d = tar[ti]; r = (d & 0xffffffu) + ((((d >> 24) * (255u - opa)) << 16) & 0xff000000u); break; }
    // ---- tar = f(src, dst, rule)
    default: {
        uint s1 = src[si], s2 = dst[di];
        uint level = rule[p.ruleOffset + gid.y * p.rulePitch + gid.x];
        bool sw = (p.op & 1u) != 0;
        if (sw) {
            if (int(level) >= p.src1lv) { r = s1; break; }
            if (int(level) < p.src2lv) { r = s2; break; }
        }
        uint o = table[level];
        if (p.op <= 61) {
            r = mixColour(s1, s2, o);
        } else if (p.op <= 63) {
            uint a1 = s1 >> 24, a2 = s2 >> 24;
            uint addr = (a2 * o & 0xff00u) + (a1 * (256u - o) >> 8);
            uint alpha = ooo[addr];
            uint top = sw ? (uint(negmul[addr]) << 24) : ((a1 + ((a2 - a1) * o >> 8)) << 24);
            r = (mixColour(s1, s2, alpha) & 0x00ffffffu) | top;
        } else {
            r = blendARGB(s1, s2, o);
        }
        break;
    }
    }
    tar[ti] = r;
}
)MSL";

} // namespace shiori::metal
