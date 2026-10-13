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

kernel void shioriLayerOp(device uint *tar [[buffer(0)]],
                          device const uint *src [[buffer(1)]],
                          device const uint *dst [[buffer(2)]],
                          device const uchar *rule [[buffer(3)]],
                          constant KernelParams &p [[buffer(4)]],
                          device const uchar *ooo [[buffer(5)]],
                          device const uchar *negmul [[buffer(6)]],
                          constant uint *table [[buffer(7)]],
                          device uchar *tar8 [[buffer(8)]],
                          uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= p.width || gid.y >= p.height) return;
    uint ti = p.tarOffset + gid.y * p.tarPitch + gid.x;
    uint si = p.srcOffset + gid.y * p.srcPitch + gid.x;
    uint di = p.dstOffset + gid.y * p.dstPitch + gid.x;
    uint opa = p.opa;
    uint r;
    switch (p.op) {
    // ---- tar = f(tar, src)
    case 1: r = src[si]; break;
    case 2: r = 0xff000000u | src[si]; break;
    case 3: r = (tar[ti] & 0xff000000u) + (src[si] & 0x00ffffffu); break;
    case 4: r = (src[si] & 0xff000000u) + (tar[ti] & 0x00ffffffu); break;
    case 10: { uint d = tar[ti], s = src[si]; r = (mixColour(d, s, s >> 24) & 0x00ffffffu) | (d & 0xff000000u); break; }
    case 11: { uint d = tar[ti], s = src[si]; r = (mixColour(d, s, ((s >> 24) * opa) >> 8) & 0x00ffffffu) | (d & 0xff000000u); break; }
    case 12: { uint d = tar[ti], s = src[si]; r = alphaOnAlpha(d, s, ((s >> 16) & 0xff00u) + (d >> 24), ooo, negmul); break; }
    case 13: { uint d = tar[ti], s = src[si]; r = alphaOnAlpha(d, s, (((s >> 24) * opa) & 0xff00u) + (d >> 24), ooo, negmul); break; }
    case 14: r = premulOver(tar[ti], premultiply(src[si])); break;
    case 15: { uint s = src[si]; s = (s & 0xffffffu) + ((((s >> 24) * opa) >> 8) << 24); r = premulOver(tar[ti], premultiply(s)); break; }
    case 20: { uint d = tar[ti]; r = (premulOverColour(d, src[si]) & 0x00ffffffu) | (d & 0xff000000u); break; }
    case 21: {
        uint d = tar[ti], s = src[si];
        s = (((s & 0xff00ffu) * opa >> 8) & 0xff00ffu) + (((s >> 8) & 0xff00ffu) * opa & 0xff00ff00u);
        r = (premulOverColour(d, s) & 0x00ffffffu) + (d & 0xff000000u);
        break;
    }
    case 22: r = premulOver(tar[ti], src[si]); break;
    case 23: {
        uint s = src[si];
        s = (((s & 0xff00ffu) * opa >> 8) & 0xff00ffu) + (((s >> 8) & 0xff00ffu) * opa & 0xff00ff00u);
        r = premulOver(tar[ti], s);
        break;
    }
    case 30: r = mixColour(tar[ti], src[si], opa); break;
    case 31: { uint d = tar[ti]; r = alphaOnAlpha(d, src[si], (opa << 8) + (d >> 24), ooo, negmul); break; }
    case 32: r = premulOver(tar[ti], (src[si] & 0xffffffu) | (opa << 24)); break;
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
