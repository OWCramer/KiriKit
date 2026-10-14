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
    // A colour through an 8-bit coverage map (text): over the target as is, and keeping its
    // alpha; each with an opacity. The map is read through the rule buffer.
    OpColorMap = 112, OpColorMapO, OpColorMapD, OpColorMapDO,
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
    if (p.op >= 112 && p.op <= 115) {
        // KiriKiri's ApplyColorMap (and _d): the colour blended in by the map's level.
        uint s = rule[p.ruleOffset + gid.y * p.rulePitch + gid.x];
        uint d = tar[ti];
        uint c1 = p.color & 0xff00ffu, c2 = p.color & 0x00ff00u;
        if (p.op <= 113) {
            uint sopa = p.op == 113 ? (s * p.opa) >> 8 : s;
            uint d1 = d & 0xff00ffu;
            d1 = ((d1 + ((c1 - d1) * sopa >> 8)) & 0xff00ffu) + (d & 0xff000000u);
            uint dg = d & 0x00ff00u;
            tar[ti] = d1 | ((dg + ((c2 - dg) * sopa >> 8)) & 0x00ff00u);
        } else {
            uint addr = p.op == 115 ? (((s * p.opa) & 0xff00u) + (d >> 24)) : ((s << 8) + (d >> 24));
            uint destalpha = uint(negmul[addr]) << 24;
            uint sopa = ooo[addr];
            uint d1 = d & 0xff00ffu;
            d1 = (d1 + ((c1 - d1) * sopa >> 8)) & 0xff00ffu;
            uint dg = d & 0x00ff00u;
            tar[ti] = d1 + ((dg + ((c2 - dg) * sopa >> 8)) & 0x00ff00u) + destalpha;
        }
        return;
    }
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

// layerExRaster's copyRaster: each row of the source, shifted sideways by that row's offset
// (positive: right, the row's start left as it was; negative: left), into the target. Pitches
// in pixels; offsets[row] for each row of the clip.
struct RasterParams {
    uint tarPitch, srcPitch;
    uint left, top, width, height;
};
kernel void shioriRasterCopy(device uint *tar [[buffer(0)]],
                             device const uint *src [[buffer(1)]],
                             device const int *offsets [[buffer(2)]],
                             constant RasterParams &p [[buffer(3)]],
                             uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= p.width || gid.y >= p.height) return;
    int d = offsets[gid.y];
    int x = int(gid.x);
    uint row = p.top + gid.y;
    if (d >= 0) {
        if (x < int(p.width) - d) tar[row * p.tarPitch + p.left + uint(x + d)] = src[row * p.srcPitch + p.left + uint(x)];
    } else {
        if (x < int(p.width) + d) tar[row * p.tarPitch + p.left + uint(x)] = src[row * p.srcPitch + p.left + uint(x - d)];
    }
}

// layerExImage's operations on a layer's clip, one thread per pixel (pixels R, G, B, A from the
// low byte, as Shiori's krkr2 keeps them). The arithmetic is the plug-in's (CxImage's DSP
// functions), restated per pixel; the plug-in computes the same on the CPU when the GPU can't.
struct ImageOpParams {
    uint pitch;                     // pixels
    uint left, top, width, height;  // the clip
    uint op;                        // 1 lut, 2 colorize, 3 modulate, 4 noise, 5 white noise
    int i0, i1, i2, i3;
    float f0, f1, f2;
    uint seed;
};

static uint shioriHash(uint x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}
// 0..1 for a pixel, a channel and a call.
static float shioriRandom(uint seed, uint x, uint y, uint c) {
    return float(shioriHash(seed ^ shioriHash(x * 0x9E3779B1u ^ shioriHash(y * 0x85EBCA77u + c))) >> 8) / 16777215.0f;
}

// CxImage's RGBtoHSL (bytes, HSLMAX 255): returns H, S, L.
static uint3 shioriRGBtoHSL(uint R, uint G, uint B) {
    uint cMax = max(max(R, G), B), cMin = min(min(R, G), B);
    uint L = ((cMax + cMin) * 255u + 255u) / 510u;
    uint H, S;
    if (cMax == cMin) {
        S = 0; H = 170;
    } else {
        if (L <= 127u) S = (((cMax - cMin) * 255u) + ((cMax + cMin) / 2u)) / (cMax + cMin);
        else S = (((cMax - cMin) * 255u) + ((510u - cMax - cMin) / 2u)) / (510u - cMax - cMin);
        S &= 0xffu;
        uint d = cMax - cMin;
        uint Rd = ((((cMax - R) * 42u) + (d / 2u)) / d) & 0xffffu;
        uint Gd = ((((cMax - G) * 42u) + (d / 2u)) / d) & 0xffffu;
        uint Bd = ((((cMax - B) * 42u) + (d / 2u)) / d) & 0xffffu;
        if (R == cMax) H = (Bd - Gd) & 0xffu;
        else if (G == cMax) H = (85u + Rd - Bd) & 0xffu;
        else H = (170u + Gd - Rd) & 0xffu;
    }
    return uint3(H, S, L);
}
static float shioriHueToRGB(float n1, float n2, float hue) {
    if (hue > 360) hue -= 360;
    else if (hue < 0) hue += 360;
    if (hue < 60) return n1 + (n2 - n1) * hue / 60.0f;
    if (hue < 180) return n2;
    if (hue < 240) return n1 + (n2 - n1) * (240 - hue) / 60;
    return n1;
}
// CxImage's HSLtoRGB: R, G, B.
static uint3 shioriHSLtoRGB(uint Hb, uint Sb, uint Lb) {
    float h = float(Hb) * 360.0f / 255.0f, s = float(Sb) / 255.0f, l = float(Lb) / 255.0f;
    float m2 = l <= 0.5f ? l * (1 + s) : l + s - l * s;
    float m1 = 2 * l - m2;
    if (s == 0) {
        uint v = uint(l * 255.0f) & 0xffu;
        return uint3(v, v, v);
    }
    return uint3(uint(shioriHueToRGB(m1, m2, h + 120) * 255.0f) & 0xffu, uint(shioriHueToRGB(m1, m2, h) * 255.0f) & 0xffu,
                 uint(shioriHueToRGB(m1, m2, h - 120) * 255.0f) & 0xffu);
}
static int shioriHue2rgb(float n1, float n2, float hue) {
    if (hue < 0) hue += 1.0f;
    else if (hue > 1.0f) hue -= 1.0f;
    float c;
    if (hue < 1.0f / 6.0f) c = n1 + (n2 - n1) * hue * 6.0f;
    else if (hue < 0.5f) c = n2;
    else if (hue < 2.0f / 3.0f) c = n1 + (n2 - n1) * (2.0f / 3.0f - hue) * 6.0f;
    else c = n1;
    return int(c * 255.0f);
}

kernel void shioriImageOp(device uint *img [[buffer(0)]],
                          device const uchar *lut [[buffer(1)]],
                          constant ImageOpParams &p [[buffer(2)]],
                          uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= p.width || gid.y >= p.height) return;
    const uint x = p.left + gid.x, y = p.top + gid.y;
    const uint i = y * p.pitch + x;
    const uint px = img[i];
    uint R = px & 0xffu, G = (px >> 8) & 0xffu, B = (px >> 16) & 0xffu;
    const uint A = px & 0xff000000u;
    switch (p.op) {
    case 1: R = lut[R]; G = lut[G]; B = lut[B]; break;
    case 2: {
        uint3 hsl = shioriRGBtoHSL(R, G, B);
        uint3 c = shioriHSLtoRGB(uint(p.i0) & 0xffu, uint(p.i1) & 0xffu, hsl.z);
        if (p.f0 > 0.5f) { R = c.x; G = c.y; B = c.z; }
        else {
            uint a0 = uint(p.i2), a1 = uint(p.i3);
            R = ((c.x * a0 + R * a1) >> 8) & 0xffu;
            G = ((c.y * a0 + G * a1) >> 8) & 0xffu;
            B = ((c.z * a0 + B * a1) >> 8) & 0xffu;
        }
        break;
    }
    case 3: {
        float red = R / 255.0f, green = G / 255.0f, blue = B / 255.0f;
        float cMax = max(max(red, green), blue), cMin = min(min(red, green), blue);
        float delta = cMax - cMin, add = cMax + cMin;
        float lum = add / 2.0f, hue, sat;
        if (delta == 0) { sat = 0; hue = 0; }
        else {
            sat = lum < 0.5f ? delta / add : delta / (2.0f - add);
            if (red == cMax) hue = (green - blue) / delta;
            else if (green == cMax) hue = 2.0f + (blue - red) / delta;
            else hue = 4.0f + (red - green) / delta;
            hue /= 6.0f;
        }
        hue += p.f0;
        while (hue < 0) hue += 1.0f;
        while (hue > 1.0f) hue -= 1.0f;
        sat += p.f1 > 0 ? (1.0f - sat) * p.f1 : sat * p.f1;
        lum += p.f2 > 0 ? (1.0f - lum) * p.f2 : lum * p.f2;
        if (sat == 0.0f) { R = G = B = uint(int(lum * 255.0f)) & 0xffu; }
        else {
            float m2 = lum <= 0.5f ? lum * (1 + sat) : lum + sat - lum * sat;
            float m1 = 2.0f * lum - m2;
            R = uint(shioriHue2rgb(m1, m2, hue + 1.0f / 3.0f)) & 0xffu;
            G = uint(shioriHue2rgb(m1, m2, hue)) & 0xffu;
            B = uint(shioriHue2rgb(m1, m2, hue - 1.0f / 3.0f)) & 0xffu;
        }
        break;
    }
    case 4: {
        float level = float(p.i0);
        R = uint(clamp(int(R) + int((shioriRandom(p.seed, x, y, 0) - 0.5f) * level), 0, 255));
        G = uint(clamp(int(G) + int((shioriRandom(p.seed, x, y, 1) - 0.5f) * level), 0, 255));
        B = uint(clamp(int(B) + int((shioriRandom(p.seed, x, y, 2) - 0.5f) * level), 0, 255));
        break;
    }
    case 5: R = G = B = min(uint(shioriRandom(p.seed, x, y, 0) * 256.0f), 255u); break;
    }
    img[i] = A | (B << 16) | (G << 8) | R;
}

// layerExImage's gaussianBlur, one pass: along rows (vertical 0: image clip -> temp) or columns
// (1: temp -> image clip). The weights are the plug-in's matrix; at the edges only the weights
// inside the clip count, scaled to one, as the plug-in's blur_line does.
struct BlurParams {
    uint pitch;                    // the image's, in pixels
    uint left, top, width, height; // the clip; the temp buffer is width x height
    uint length;                   // matrix length (odd)
    uint vertical;
};
kernel void shioriGaussianBlur(device uint *img [[buffer(0)]],
                               device uint *tmp [[buffer(1)]],
                               device const float *matrix [[buffer(2)]],
                               constant BlurParams &p [[buffer(3)]],
                               uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= p.width || gid.y >= p.height) return;
    const int mid = int(p.length / 2);
    const int n = p.vertical ? int(p.height) : int(p.width);
    const int at = p.vertical ? int(gid.y) : int(gid.x);
    float4 sum = 0;
    float scale = 0;
    for (int j = 0; j < int(p.length); ++j) {
        const int k = at + j - mid;
        if (k < 0 || k >= n) continue;
        uint v = p.vertical ? tmp[uint(k) * p.width + gid.x] : img[(p.top + gid.y) * p.pitch + p.left + uint(k)];
        sum += matrix[j] * float4(v & 0xffu, (v >> 8) & 0xffu, (v >> 16) & 0xffu, v >> 24);
        scale += matrix[j];
    }
    uint4 c = uint4(clamp(0.5f + sum / scale, 0.0f, 255.0f));
    const uint out = (c.w << 24) | (c.z << 16) | (c.y << 8) | c.x;
    if (p.vertical) img[(p.top + gid.y) * p.pitch + p.left + gid.x] = out;
    else tmp[gid.y * p.width + gid.x] = out;
}
)MSL";

} // namespace shiori::metal
