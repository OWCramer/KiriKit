/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

// Shiori's Metal compositor: the render manager ("metal") ShioriKrkr gives krkr2's layer code.
//
// Layer images live in shared-memory Metal buffers, laid out like krkr2's own bitmaps (rows of
// 32-bit or 8-bit pixels at a pitch), so the CPU and the GPU work on the same bytes with no
// copies. Layer operations krkr2 sends through the render manager run as compute dispatches
// (shiori_metal_kernels.h), collected into one command buffer per engine tick; everything else
// - operations without a kernel, stretches, affine and perspective drawing, small operations
// on idle images, and any code that touches pixels directly - stays on krkr2's software
// renderer, which works on these buffers as on its own. Before the CPU reads or writes an
// image the GPU still has work queued on, that work is committed and waited for.
//
// Written for Shiori; the render-manager interface is krkr2's, the blend arithmetic in the
// kernels is KiriKiri's. Nothing here comes from another engine's renderer.
//
// Environment, for development:
//   SHIORIKRKR_METAL_VERIFY=1   run every GPU operation on the CPU too and report mismatches
//   SHIORIKRKR_PROFILE=1        per-operation timings in the host's profile lines

#include "tjsCommHead.h"
#include "RenderManager.h"
#include "LayerBitmapIntf.h"
#include "tvpgl.h"
#include "impl/LayerBitmapImpl.h"

#include "shiori_metal_kernels.h"
#include "shiori_metal_render.h"

#import <Metal/Metal.h>
#include <opencv2/opencv.hpp> // Shiori's stand-in (compat/): the warp matrices, as krkr2's software path makes them

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
extern unsigned char TVPOpacityOnOpacityTable[256 * 256];
extern unsigned char TVPNegativeMulTable[256 * 256];
}

namespace shiori::metal {
namespace {

// ---------------------------------------------------------------------------------------------
// The device, its queue and the open command buffer.

struct Device {
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLComputePipelineState> pipeline = nil;
    id<MTLBuffer> ooo = nil, negmul = nil;
    NSUInteger linearAlignment = 64;

    std::recursive_mutex mutex;
    id<MTLCommandBuffer> open = nil;
    id<MTLComputeCommandEncoder> encoder = nil;
    unsigned openDispatches = 0;
    uint64_t openSerial = 0, nextSerial = 1;
    id<MTLCommandBuffer> lastCommitted = nil;
    std::atomic<uint64_t> completed{0};

    // Statistics for the profile.
    unsigned syncs = 0, commits = 0, dispatches = 0;
    double syncSeconds = 0;

    bool start() {
        device = MTLCreateSystemDefaultDevice();
        if (!device) return false;
        queue = [device newCommandQueue];
        NSError *error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:@(kKernelSource) options:nil error:&error];
        if (!library) {
            fprintf(stderr, "shiorikrkr metal: kernels failed to compile: %s\n", error.localizedDescription.UTF8String);
            return false;
        }
        id<MTLFunction> function = [library newFunctionWithName:@"shioriLayerOp"];
        pipeline = [device newComputePipelineStateWithFunction:function error:&error];
        if (!pipeline) return false;
        linearAlignment = std::max<NSUInteger>(64, [device minimumLinearTextureAlignmentForPixelFormat:MTLPixelFormatRGBA8Unorm]);
        return true;
    }

    void ensureTables() {
        if (ooo) return;
        ooo = [device newBufferWithBytes:TVPOpacityOnOpacityTable length:sizeof(TVPOpacityOnOpacityTable)
                                 options:MTLResourceStorageModeShared];
        negmul = [device newBufferWithBytes:TVPNegativeMulTable length:sizeof(TVPNegativeMulTable)
                                    options:MTLResourceStorageModeShared];
    }

    // The encoder of the open command buffer, opening one if there's none.
    id<MTLComputeCommandEncoder> encoderForDispatch(uint64_t &serial) {
        openCommandBuffer();
        if (!encoder) {
            encoder = [open computeCommandEncoder];
            [encoder setComputePipelineState:pipeline];
        }
        serial = openSerial;
        ++openDispatches;
        ++dispatches;
        return encoder;
    }

    // The open command buffer, opening one if there's none.
    void openCommandBuffer() {
        if (open) return;
        open = [queue commandBuffer];
        openSerial = nextSerial++;
        openDispatches = 0;
    }

    // The open command buffer with no encoder open on it (for others' passes).
    id<MTLCommandBuffer> bareCommandBuffer(uint64_t &serial) {
        openCommandBuffer();
        if (encoder) {
            [encoder endEncoding];
            encoder = nil;
        }
        serial = openSerial;
        return open;
    }

    void commit() {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        if (!open) return;
        if (encoder) [encoder endEncoding];
        encoder = nil;
        finish();
    }

    // Copies a frame into a presentation texture behind the queued work, signals the frame
    // event, commits. Returns the event value the copy is done at.
    uint64_t commitFrame(id<MTLBuffer> buffer, size_t pitch, int w, int h, id<MTLTexture> into, uint64_t &serial) {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        bareCommandBuffer(serial);
        id<MTLBlitCommandEncoder> blit = [open blitCommandEncoder];
        [blit copyFromBuffer:buffer sourceOffset:0 sourceBytesPerRow:pitch sourceBytesPerImage:pitch * h
                  sourceSize:MTLSizeMake(w, h, 1) toTexture:into destinationSlice:0 destinationLevel:0
           destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];
        if (!frameEvent) frameEvent = [device newSharedEvent];
        uint64_t value = ++frameValue;
        [open encodeSignalEvent:frameEvent value:value];
        finish();
        return value;
    }

    id<MTLSharedEvent> frameEvent = nil;
    uint64_t frameValue = 0;

    void finish() {
        uint64_t serial = openSerial;
        std::atomic<uint64_t> *done = &completed;
        [open addCompletedHandler:^(id<MTLCommandBuffer>) {
            uint64_t seen = done->load();
            while (seen < serial && !done->compare_exchange_weak(seen, serial)) {}
        }];
        [open commit];
        lastCommitted = open;
        open = nil;
        ++commits;
    }

    // Waits for every command buffer up to `serial`.
    void wait(uint64_t serial) {
        auto started = std::chrono::steady_clock::now();
        std::lock_guard<std::recursive_mutex> lock(mutex);
        if (completed.load() >= serial) return;
        if (open && openSerial <= serial) commit();
        if (lastCommitted) [lastCommitted waitUntilCompleted];
        uint64_t seen = completed.load();
        uint64_t all = open ? openSerial - 1 : nextSerial - 1;
        while (seen < all && !completed.compare_exchange_weak(seen, all)) {}
        ++syncs;
        syncSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    }
};

Device &gpu() {
    static Device *d = new Device;
    return *d;
}

bool verifying() {
    static const bool on = getenv("SHIORIKRKR_METAL_VERIFY") != nullptr;
    return on;
}

// ---------------------------------------------------------------------------------------------
// Images.

class Texture final : public iTVPTexture2D {
public:
    Texture(unsigned w, unsigned h, TVPTextureFormat::e format) : iTVPTexture2D(w, h), Format(format) {
        Allocate(w, h);
    }
    // Queued work keeps the buffer alive (command buffers retain what they use).
    ~Texture() override { totalBytes() -= Capacity; }

    static std::atomic<uint64_t> &totalBytes() {
        static std::atomic<uint64_t> bytes{0};
        return bytes;
    }

    int BytesPerPixel() const { return Format == TVPTextureFormat::Gray ? 1 : 4; }
    id<MTLBuffer> Buffer() const { return Storage; }
    size_t Size() const { return Capacity; }

    // Waits for queued GPU work on this image (before the CPU reads or writes it).
    void Idle() {
        uint64_t use = LastUse.load(std::memory_order_acquire);
        if (use && use > gpu().completed.load(std::memory_order_acquire)) gpu().wait(use);
    }
    bool Busy() const {
        uint64_t use = LastUse.load(std::memory_order_acquire);
        return use && use > gpu().completed.load(std::memory_order_acquire);
    }
    void MarkUsed(uint64_t serial) { LastUse.store(serial, std::memory_order_release); }

    // Bytes without waiting (for the GPU path, which orders itself).
    uint8_t *Raw() const { return Bytes; }

    TVPTextureFormat::e GetFormat() const override { return Format; }
    const void *GetScanLineForRead(tjs_uint l) override {
        Idle();
        return Bytes + size_t(l) * Pitch;
    }
    void *GetScanLineForWrite(tjs_uint l) override {
        Idle();
        Opaque = false;
        return Bytes + size_t(l) * Pitch;
    }
    const void *GetPixelData() override { return GetScanLineForRead(0); }
    tjs_int GetPitch() const override { return Pitch; }

    void Update(const void *pixel, TVPTextureFormat::e format, int pitch, const tTVPRect &rc) override {
        Idle();
        // As krkr2's software image does: rows from rc.top, at most the image's height.
        const uint8_t *src = static_cast<const uint8_t *>(pixel);
        uint8_t *dst = Bytes + size_t(rc.top) * Pitch;
        int h = std::min(rc.get_height(), int(Height)) - rc.top;
        int w = rc.get_width();
        if (format == TVPTextureFormat::RGB) {
            for (int y = 0; y < h; ++y, dst += Pitch, src += pitch)
                TVPConvert24BitTo32Bit(reinterpret_cast<tjs_uint32 *>(dst), src, w);
        } else {
            size_t line = std::min<size_t>(std::min(pitch, Pitch), size_t(std::max(w, 0)) * BytesPerPixel());
            if (line == 0) line = std::min(pitch, Pitch);
            for (int y = 0; y < h; ++y, dst += Pitch, src += pitch) memcpy(dst, src, line);
        }
        Opaque = false;
    }

    uint32_t GetPoint(int x, int y) override {
        Idle();
        if (Format == TVPTextureFormat::Gray) return Bytes[size_t(y) * Pitch + x];
        return reinterpret_cast<const uint32_t *>(Bytes + size_t(y) * Pitch)[x];
    }
    void SetPoint(int x, int y, uint32_t clr) override {
        Idle();
        if (Format == TVPTextureFormat::Gray)
            Bytes[size_t(y) * Pitch + x] = uint8_t(clr);
        else
            reinterpret_cast<uint32_t *>(Bytes + size_t(y) * Pitch)[x] = clr;
        Opaque = false;
    }
    bool IsStatic() override { return false; }
    bool IsOpaque() override { return Opaque; }
    cocos2d::Texture2D *GetAdapterTexture(cocos2d::Texture2D *) override { return nullptr; }

    void SetSize(unsigned int w, unsigned int h) override {
        size_t pitch = AlignedPitch(w);
        if (pitch * std::max(h, 1u) > Capacity || pitch != size_t(Pitch)) {
            Idle();
            totalBytes() -= Capacity;
            Allocate(w, h);
        }
        Width = w;
        Height = h;
    }

    bool Opaque = false;

private:
    size_t AlignedPitch(unsigned w) const {
        size_t a = gpu().linearAlignment;
        size_t bytes = size_t(std::max(w, 1u)) * BytesPerPixel();
        return (bytes + a - 1) / a * a;
    }
    void Allocate(unsigned w, unsigned h) {
        Pitch = tjs_int(AlignedPitch(w));
        Capacity = size_t(Pitch) * std::max(h, 1u);
        Storage = [gpu().device newBufferWithLength:Capacity options:MTLResourceStorageModeShared];
        Bytes = static_cast<uint8_t *>(Storage.contents);
        totalBytes() += Capacity;
    }

    TVPTextureFormat::e Format;
    id<MTLBuffer> Storage = nil;
    uint8_t *Bytes = nullptr;
    tjs_int Pitch = 0;
    size_t Capacity = 0;
    std::atomic<uint64_t> LastUse{0};
};

Texture *ours(iTVPTexture2D *t) { return dynamic_cast<Texture *>(t); }

// ---------------------------------------------------------------------------------------------
// Operations: each of krkr2's software methods, wrapped. Parameters go on to the software
// method (which the CPU path runs) and are kept here for the kernel.

enum class Shape { None, Blt, BltAndOpa, BltWithOpa, SD, Fill, Univ };

struct Kind {
    Shape shape;
    unsigned op;     // the kernel operation (opacity 255 for BltAndOpa)
    unsigned opOpa;  // with opacity (BltAndOpa)
};

Kind kindOf(const std::string &name) {
    static const std::map<std::string, Kind> kinds = {
        {"Copy", {Shape::Blt, OpCopy, 0}},
        {"CopyOpaqueImage", {Shape::Blt, OpCopyOpaque, 0}},
        {"CopyColor", {Shape::Blt, OpCopyColor, 0}},
        {"CopyMask", {Shape::Blt, OpCopyMask, 0}},
        {"AlphaBlend", {Shape::BltAndOpa, OpAlphaHDA, OpAlphaHDAo}},
        {"AlphaBlend_d", {Shape::BltAndOpa, OpAlphaD, OpAlphaDo}},
        {"AlphaBlend_a", {Shape::BltAndOpa, OpAlphaA, OpAlphaAo}},
        {"PerspectiveAlphaBlend_a", {Shape::BltAndOpa, OpAlphaA, OpAlphaAo}},
        {"AdditiveAlphaBlend", {Shape::BltAndOpa, OpAddAlphaHDA, OpAddAlphaHDAo}},
        {"AdditiveAlphaBlend_a", {Shape::BltAndOpa, OpAddAlphaA, OpAddAlphaAo}},
        {"ConstAlphaBlend", {Shape::BltWithOpa, OpConstAlpha, 0}},
        {"ConstAlphaBlend_d", {Shape::BltWithOpa, OpConstAlphaD, 0}},
        {"ConstAlphaBlend_a", {Shape::BltWithOpa, OpConstAlphaA, 0}},
        {"AlphaBlend_SD", {Shape::BltWithOpa, OpConstAlpha, 0}},
        {"ConstAlphaBlend_SD", {Shape::SD, OpConstAlphaSD, 0}},
        {"ConstAlphaBlend_SD_a", {Shape::SD, OpConstAlphaSDa, 0}},
        {"ConstAlphaBlend_SD_d", {Shape::SD, OpConstAlphaSDd, 0}},
        {"FillARGB", {Shape::Fill, OpFillARGB, 0}},
        {"FillColor", {Shape::Fill, OpFillColor, 0}},
        {"FillMask", {Shape::Fill, OpFillMask, 0}},
        {"ConstColorAlphaBlend", {Shape::Fill, OpConstColorAlpha, 0}},
        {"ConstColorAlphaBlend_d", {Shape::Fill, OpConstColorAlphaD, 0}},
        {"ConstColorAlphaBlend_a", {Shape::Fill, OpConstColorAlphaA, 0}},
        {"RemoveConstOpacity", {Shape::Fill, OpRemoveConstOpacity, 0}},
        {"UnivTransBlend", {Shape::Univ, OpUniv, 0}},
        {"UnivTransBlend_d", {Shape::Univ, OpUnivD, 0}},
        {"UnivTransBlend_a", {Shape::Univ, OpUnivA, 0}},
    };
    auto it = kinds.find(name);
    return it == kinds.end() ? Kind{Shape::None, 0, 0} : it->second;
}

class Method final : public iTVPRenderMethod {
public:
    Method(iTVPRenderMethod *inner) : Inner(inner) {
        SetName(inner->GetName());
        K = kindOf(GetName());
        IdOpa = inner->EnumParameterID("opacity");
        IdColor = inner->EnumParameterID("color");
        IdPhase = inner->EnumParameterID("phase");
        IdVague = inner->EnumParameterID("vague");
    }

    iTVPRenderMethod *Inner;
    Kind K;
    int Opa = 255;
    uint32_t Color = 0;
    int Phase = 0, Vague = 0;
    uint32_t Table[256] = {};

    int EnumParameterID(const char *name) override { return Inner->EnumParameterID(name); }
    void SetParameterUInt(int id, unsigned int v) override { Inner->SetParameterUInt(id, v); }
    void SetParameterInt(int id, int v) override {
        Inner->SetParameterInt(id, v);
        if (id < 0) return;
        if (id == IdPhase) {
            Phase = v;
            InitTable(); // with the vague given before, as the software method does
        } else if (id == IdVague) {
            Vague = v;
        }
    }
    void SetParameterPtr(int id, const void *v) override { Inner->SetParameterPtr(id, v); }
    void SetParameterFloat(int id, float v) override { Inner->SetParameterFloat(id, v); }
    void SetParameterColor4B(int id, unsigned int v) override {
        Inner->SetParameterColor4B(id, v);
        if (id >= 0 && id == IdColor) Color = v;
    }
    void SetParameterOpa(int id, int v) override {
        Inner->SetParameterOpa(id, v);
        if (id >= 0 && id == IdOpa) Opa = v;
    }
    void SetParameterFloatArray(int id, float *v, int n) override { Inner->SetParameterFloatArray(id, v, n); }
    iTVPRenderMethod *SetBlendFuncSeparate(int func, int srcRGB, int dstRGB, int srcAlpha, int dstAlpha) override {
        Inner->SetBlendFuncSeparate(func, srcRGB, dstRGB, srcAlpha, dstAlpha);
        return this;
    }
    bool IsBlendTarget() override { return Inner->IsBlendTarget(); }

private:
    int IdOpa, IdColor, IdPhase, IdVague;

    // The universal transition's level table (KiriKiri's: full below phase - vague, none from
    // phase, a ramp between).
    void InitTable() {
        int top = Phase, bottom = Phase - Vague;
        for (int i = 0; i < 256; ++i) {
            if (i < bottom) {
                Table[i] = 255;
            } else if (i >= top) {
                Table[i] = 0;
            } else {
                int v = 255 - ((i - bottom) * 255 / Vague);
                Table[i] = uint32_t(std::clamp(v, 0, 255));
            }
        }
    }
};

// For listing the software manager's methods (its table is protected).
struct MethodTable : iTVPRenderManager {
    using iTVPRenderManager::AllMethods;
};

// ---------------------------------------------------------------------------------------------
// Profile.

struct OpStat {
    double seconds = 0;
    uint64_t pixels = 0;
    unsigned gpu = 0, cpu = 0, mismatches = 0;
};

class Manager final : public iTVPRenderManager {
public:
    Manager() {
        Software = TVPGetRenderManager(ttstr(TJS_W("software")));
        StretchTypeId = Software->EnumParameterID("StretchType");
        auto member = &MethodTable::AllMethods;
        for (auto &entry : Software->*member) {
            auto *wrapped = new Method(entry.second);
            AllMethods[entry.first] = wrapped;
        }
    }

    // ---- images

    iTVPTexture2D *CreateTexture2D(const void *pixel, int pitch, unsigned int w, unsigned int h,
                                   TVPTextureFormat::e format, int flags) override {
        auto *t = new Texture(w, h, format == TVPTextureFormat::Gray ? TVPTextureFormat::Gray : format);
        if (pixel && pitch) {
            const uint8_t *src = static_cast<const uint8_t *>(pixel);
            size_t line = std::min<size_t>(size_t(std::abs(pitch)), size_t(w) * t->BytesPerPixel());
            for (unsigned y = 0; y < h; ++y)
                memcpy(t->Raw() + size_t(y) * t->GetPitch(), src + ptrdiff_t(y) * pitch, line);
        }
        return t;
    }

    iTVPTexture2D *CreateTexture2D(tTVPBitmap *bmp) override {
        bool gray = bmp->GetBPP() == 8;
        auto *t = new Texture(bmp->GetWidth(), bmp->GetHeight(), gray ? TVPTextureFormat::Gray : TVPTextureFormat::RGBA);
        size_t line = size_t(bmp->GetWidth()) * (gray ? 1 : 4);
        const uint8_t *src = static_cast<const uint8_t *>(bmp->GetBits());
        for (unsigned y = 0; y < bmp->GetHeight(); ++y)
            memcpy(t->Raw() + size_t(y) * t->GetPitch(), src + ptrdiff_t(y) * bmp->GetPitch(), line);
        t->Opaque = bmp->IsOpaque;
        return t;
    }

    iTVPTexture2D *CreateTexture2D(TJS::tTJSBinaryStream *s) override { return Software->CreateTexture2D(s); }

    iTVPTexture2D *CreateTexture2D(unsigned int neww, unsigned int newh, iTVPTexture2D *tex) override {
        auto *t = new Texture(neww, newh, tex->GetFormat() == TVPTextureFormat::Gray ? TVPTextureFormat::Gray
                                                                                  : TVPTextureFormat::RGBA);
        unsigned w = std::min(neww, tex->GetWidth()), h = std::min(newh, tex->GetHeight());
        if (w && h) {
            Texture *from = ours(tex);
            if (from && from->Busy() && t->BytesPerPixel() == 4) {
                // Copy on the GPU behind the work already queued on the source.
                KernelParams p = {};
                p.op = OpCopy;
                p.width = w;
                p.height = h;
                p.tarPitch = unsigned(t->GetPitch() / 4);
                p.srcPitch = unsigned(from->GetPitch() / 4);
                Dispatch(p, t, from, nullptr, nullptr, nullptr);
            } else {
                for (unsigned y = 0; y < h; ++y)
                    memcpy(t->Raw() + size_t(y) * t->GetPitch(), tex->GetScanLineForRead(y), size_t(w) * t->BytesPerPixel());
            }
        }
        return t;
    }

    const char *GetName() override { return "Metal"; }
    // krkr2's layer code keeps its software paths (it touches pixels directly at times, which
    // these images allow).
    bool IsSoftware() override { return true; }

    bool GetRenderStat(unsigned int &drawCount, uint64_t &vmemsize) override {
        drawCount = DrawCount;
        DrawCount = 0;
        vmemsize = Texture::totalBytes().load();
        return true;
    }
    bool GetTextureStat(iTVPTexture2D *texture, uint64_t &vmemsize) override {
        if (Texture *t = ours(texture)) {
            vmemsize = t->Size();
            return true;
        }
        return Software->GetTextureStat(texture, vmemsize);
    }

    int EnumParameterID(const char *name) override { return Software->EnumParameterID(name); }
    void SetParameterUInt(int id, unsigned int v) override { Software->SetParameterUInt(id, v); }
    void SetParameterInt(int id, int v) override {
        Software->SetParameterInt(id, v);
        // The software manager's stretch type, as it keeps it (index into nearest, area,
        // linear, cubic; anything past the last is cubic).
        if (id >= 0 && id == StretchTypeId) StretchType = v > 4 ? 3 : v;
    }
    void SetParameterPtr(int id, const void *v) override { Software->SetParameterPtr(id, v); }
    void SetParameterFloat(int id, float v) override { Software->SetParameterFloat(id, v); }

    // ---- operations

    void OperateRect(iTVPRenderMethod *method, iTVPTexture2D *tar, iTVPTexture2D *reftar, const tTVPRect &rctar,
                     const tRenderTexRectArray &textures) override {
        auto started = std::chrono::steady_clock::now();
        ++DrawCount;
        auto *m = dynamic_cast<Method *>(method);
        bool onGPU = false;
        if (m) {
            if (verifying()) {
                onGPU = Verify(m, tar, reftar, rctar, textures);
            } else {
                onGPU = TryGPU(m, tar, rctar, textures);
                if (!onGPU) OnCPU(m, tar, reftar, rctar, textures);
            }
        } else {
            IdleAll(tar, reftar, textures);
            Software->OperateRect(method, tar, reftar, rctar, textures);
        }
        if (Profiling()) {
            OpStat &s = Stats[method->GetName()];
            s.seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            s.pixels += uint64_t(std::max(0, rctar.get_width())) * uint64_t(std::max(0, rctar.get_height()));
            (onGPU ? s.gpu : s.cpu) += 1;
        }
    }

    void OperateTriangles(iTVPRenderMethod *method, int nTriangles, iTVPTexture2D *target, iTVPTexture2D *reftar,
                          const tTVPRect &rcclip, const tTVPPointD *pttar, const tRenderTexQuadArray &textures) override {
        auto started = std::chrono::steady_clock::now();
        ++DrawCount;
        auto *m = dynamic_cast<Method *>(method);
        if (m && verifying() && ours(target) && Inside(rcclip, target)) {
            VerifyWarp(m, nTriangles, target, reftar, rcclip, pttar, textures);
            Count(method, "(triangles, verified)", started);
            return;
        }
        bool onGPU = m && TryWarp(m, nTriangles, target, rcclip, pttar, textures);
        if (!onGPU) {
            IdleAll(target, reftar, textures);
            Software->OperateTriangles(Unwrap(method), nTriangles, target, reftar, rcclip, pttar, textures);
        }
        Count(method, onGPU ? "(triangles, GPU)" : "(triangles)", started, onGPU);
    }

    void OperatePerspective(iTVPRenderMethod *method, int nQuads, iTVPTexture2D *target, iTVPTexture2D *reftar,
                            const tTVPRect &rcclip, const tTVPPointD *pttar, const tRenderTexQuadArray &textures) override {
        auto started = std::chrono::steady_clock::now();
        ++DrawCount;
        IdleAll(target, reftar, textures);
        Software->OperatePerspective(Unwrap(method), nQuads, target, reftar, rcclip, pttar, textures);
        Count(method, "(perspective)", started);
    }

    std::map<std::string, OpStat> Stats;

private:
    iTVPRenderManager *Software;
    unsigned DrawCount = 0;
    int StretchTypeId = -1;
    int StretchType = 0;

    // The kernel's sampling for the current stretch type: nearest (1/3) or bilinear (2/4);
    // 0 for what stays on the CPU (cubic; area when shrinking, for stretches).
    int SampleMode(bool warp, bool shrinking) const {
        switch (StretchType) {
        case 0: return warp ? 1 : 3;
        case 1: return warp ? 2 : (shrinking ? 0 : 4); // area: bilinear unless both shrink
        case 2: return warp ? 2 : 4;
        default: return 0;
        }
    }

    static bool Profiling() {
        static const bool on = getenv("SHIORIKRKR_PROFILE") != nullptr || verifying();
        return on;
    }

    static iTVPRenderMethod *Unwrap(iTVPRenderMethod *method) {
        auto *m = dynamic_cast<Method *>(method);
        return m ? m->Inner : method;
    }

    void Count(iTVPRenderMethod *method, const char *suffix, std::chrono::steady_clock::time_point started,
               bool onGPU = false) {
        if (!Profiling()) return;
        OpStat &s = Stats[method->GetName() + suffix];
        s.seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        (onGPU ? s.gpu : s.cpu) += 1;
    }

    static bool IsBlt(const Kind &k) {
        return k.shape == Shape::Blt || k.shape == Shape::BltAndOpa || k.shape == Shape::BltWithOpa;
    }

    // A blend of a sampled source (a stretch or a warp of `view` in `src`) into `rect` of `tar`.
    void DispatchSampled(Method *m, Texture *tar, const tTVPRect &rect, Texture *src, const tTVPRect &view,
                         unsigned sample, const float *matrix) {
        KernelParams p = {};
        p.width = unsigned(rect.get_width());
        p.height = unsigned(rect.get_height());
        p.tarPitch = unsigned(tar->GetPitch() / 4);
        p.tarOffset = unsigned(rect.top) * p.tarPitch + unsigned(rect.left);
        p.srcPitch = unsigned(src->GetPitch() / 4);
        p.srcOffset = unsigned(view.top) * p.srcPitch + unsigned(view.left);
        p.srcWidth = view.get_width();
        p.srcHeight = view.get_height();
        p.sample = sample;
        for (int i = 0; i < 9; ++i) p.matrix[i] = matrix[i];
        p.opa = unsigned(m->Opa);
        p.color = m->Color;
        p.op = m->K.op;
        if (m->K.shape == Shape::BltAndOpa && m->Opa != 255) p.op = m->K.opOpa;
        Dispatch(p, tar, src, nullptr, nullptr, nullptr);
    }

    // A stretched blend (krkr2's software path: clip to the target, move the source edges in
    // proportion, resize, blend), on the GPU where the stretch type allows.
    bool TryStretch(Method *m, Texture *tar, tTVPRect rctar, Texture *src, tTVPRect rcsrc) {
        int sw = rcsrc.get_width(), sh = rcsrc.get_height(), dw = rctar.get_width(), dh = rctar.get_height();
        if (dw == 0 || dh == 0 || sw == 0 || sh == 0) return true;
        if (sw < 0 || sh < 0 || dw < 0 || dh < 0) return false; // flips
        tTVPRect cr(0, 0, tar->GetWidth(), tar->GetHeight());
        if (cr.left > rctar.left) {
            rcsrc.left += (float)sw / dw * (cr.left - rctar.left);
            rctar.left = cr.left;
        }
        if (cr.right < rctar.right) {
            rcsrc.right -= (float)rcsrc.get_width() / rctar.get_width() * (rctar.right - cr.right);
            rctar.right = cr.right;
        }
        if (cr.top > rctar.top) {
            rcsrc.top += (float)sh / dh * (cr.top - rctar.top);
            rctar.top = cr.top;
        }
        if (cr.bottom < rctar.bottom) {
            rcsrc.bottom -= (float)rcsrc.get_height() / rctar.get_height() * (rctar.bottom - cr.bottom);
            rctar.bottom = cr.bottom;
        }
        sw = rcsrc.get_width(), sh = rcsrc.get_height(), dw = rctar.get_width(), dh = rctar.get_height();
        if (sh == 0 || sw == 0 || dh == 0 || dw == 0) return true;
        if (sw < 0 || sh < 0 || dw < 0 || dh < 0 || !Inside(rcsrc, src) || !Inside(rctar, tar)) return false;
        if (src == tar && Overlap(rcsrc, rctar)) return false;
        const double sx = double(sw) / dw, sy = double(sh) / dh;
        int mode = SampleMode(false, sx >= 1 && sy >= 1);
        if (!mode) return false;
        const float matrix[9] = {float(sx), float(sy), 0, 0, 0, 0, 0, 0, 0};
        DispatchSampled(m, tar, rctar, src, rcsrc, unsigned(mode), matrix);
        return true;
    }

    // krkr2's software path for two triangles making a quad: a rectangle-to-rectangle stretch,
    // or a warp (affine when the target is a parallelogram, else perspective) of the source
    // over the whole clip rectangle, blended in. On the GPU when the images and the stretch
    // type allow it.
    bool TryWarp(Method *m, int nTriangles, iTVPTexture2D *targetTex, const tTVPRect &rcclip, const tTVPPointD *dstpt,
                 const tRenderTexQuadArray &textures) {
        if (getenv("SHIORIKRKR_TRACE_WARP")) {
            static int traced = 0;
            if (traced++ < 5)
                fprintf(stderr, "warp: %s n=%d tex=%zu blt=%d tar=%d src=%d same=%d bpp=%d/%d clip=%d,%d,%d,%d\n", m->GetName().c_str(),
                        nTriangles, textures.size(), IsBlt(m->K), ours(targetTex) != nullptr,
                        textures.size() ? ours(textures[0].first) != nullptr : -1, textures.size() ? targetTex == textures[0].first : -1,
                        ours(targetTex) ? ours(targetTex)->BytesPerPixel() : -1,
                        textures.size() && ours(textures[0].first) ? ours(textures[0].first)->BytesPerPixel() : -1,
                        rcclip.left, rcclip.top, rcclip.right, rcclip.bottom);
        }
        if (!IsBlt(m->K) || textures.size() != 1 || nTriangles != 2) return false;
        Texture *tar = ours(targetTex), *src = ours(textures[0].first);
        if (!tar || !src || tar == src || tar->BytesPerPixel() != 4 || src->BytesPerPixel() != 4) return false;
        const tTVPPointD *srcpt = textures[0].second;
        auto quad = [](const tTVPPointD *pt) {
            return pt[1].x == pt[3].x && pt[1].y == pt[3].y && pt[2].x == pt[4].x && pt[2].y == pt[4].y;
        };
        if (!quad(dstpt) || !quad(srcpt)) { if (getenv("SHIORIKRKR_TRACE_WARP")) fprintf(stderr, "warp: not quads\n"); return false; }
        auto eq = [](double a, double b) { return std::fabs(a - b) < 0.001; };
        const bool isSrcRect = eq(srcpt[0].y, srcpt[1].y) && eq(srcpt[1].x, srcpt[5].x) && eq(srcpt[0].x, srcpt[2].x) &&
            eq(srcpt[2].y, srcpt[5].y);
        const bool isDstRect = eq(dstpt[0].y, dstpt[1].y) && eq(dstpt[1].x, dstpt[5].x) && eq(dstpt[0].x, dstpt[2].x) &&
            eq(dstpt[2].y, dstpt[5].y);
        if (isSrcRect && isDstRect) {
            tTVPRect dstrect(dstpt[0].x, dstpt[0].y, dstpt[5].x, dstpt[5].y);
            tTVPRect refrect(srcpt[0].x, srcpt[0].y, srcpt[5].x, srcpt[5].y);
            if (!(dstrect.left > dstrect.right || dstrect.top > dstrect.bottom || refrect.left > refrect.right ||
                  refrect.top > refrect.bottom)) {
                tTVPRect rcdest;
                if (!TVPIntersectRect(&rcdest, rcclip, dstrect)) return true;
                tjs_int dw = dstrect.get_width(), dh = dstrect.get_height();
                tjs_int rw = refrect.get_width(), rh = refrect.get_height();
                if (rcdest.left > dstrect.left) refrect.left += (float)rw / dw * (rcdest.left - dstrect.left);
                if (rcdest.right < dstrect.right) refrect.right -= (float)rw / dw * (dstrect.right - rcdest.right);
                if (rcdest.top > dstrect.top) refrect.top += (float)rh / dh * (rcdest.top - dstrect.top);
                if (rcdest.bottom < dstrect.bottom) refrect.bottom -= (float)rh / dh * (dstrect.bottom - rcdest.bottom);
                if (SameSize(refrect, rcdest)) {
                    tRenderTexRectArray::Element one[] = {tRenderTexRectArray::Element(src, refrect)};
                    return TryGPU(m, tar, rcdest, tRenderTexRectArray(one));
                }
                return TryStretch(m, tar, rcdest, src, refrect);
            }
        }
        if (!Inside(rcclip, tar) || rcclip.get_width() <= 0 || rcclip.get_height() <= 0) return false;
        // upper-left, upper-right, bottom-right, bottom-left
        cv::Point2f ps[] = {cv::Point2f(srcpt[0].x, srcpt[0].y), cv::Point2f(srcpt[1].x + 1, srcpt[1].y),
                            cv::Point2f(srcpt[5].x + 1, srcpt[5].y + 1), cv::Point2f(srcpt[2].x, srcpt[2].y + 1)};
        cv::Point2f pd[] = {cv::Point2f(dstpt[0].x - rcclip.left, dstpt[0].y - rcclip.top),
                            cv::Point2f(dstpt[1].x - rcclip.left, dstpt[1].y - rcclip.top),
                            cv::Point2f(dstpt[5].x - rcclip.left, dstpt[5].y - rcclip.top),
                            cv::Point2f(dstpt[2].x - rcclip.left, dstpt[2].y - rcclip.top)};
        tTVPRect view(0, 0, src->GetWidth(), src->GetHeight());
        if (isSrcRect) {
            tTVPRect r(0x7FFFFFFF, 0x7FFFFFFF, -1, -1);
            for (auto &pt : ps) {
                tjs_int x = pt.x, y = pt.y;
                if (x < r.left) r.left = x;
                if (++x > r.right) r.right = x;
                if (y < r.top) r.top = y;
                if (++y > r.bottom) r.bottom = y;
            }
            for (auto &pt : ps) pt.x -= r.left, pt.y -= r.top;
            if (r.get_width() > tjs_int(src->GetWidth())) r.set_width(src->GetWidth());
            if (r.get_height() > tjs_int(src->GetHeight())) r.set_height(src->GetHeight());
            view = r;
        }
        if (!Inside(view, src)) { if (getenv("SHIORIKRKR_TRACE_WARP")) fprintf(stderr, "warp: view %d,%d,%d,%d outside %u x %u\n", view.left, view.top, view.right, view.bottom, src->GetWidth(), src->GetHeight()); return false; }
        const bool affine = isSrcRect && [&] {
            auto d = [](const tTVPPointD &a, const tTVPPointD &b) { return (a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y); };
            return eq(d(dstpt[0], dstpt[1]), d(dstpt[2], dstpt[3])) && eq(d(dstpt[1], dstpt[2]), d(dstpt[0], dstpt[3]));
        }();
        const cv::Mat forward = affine ? cv::getAffineTransform(ps, pd) : cv::getPerspectiveTransform(ps, pd);
        double inv[9];
        if (!cv::detail::invert3(forward.m, inv)) return true; // the software path draws nothing either
        int mode = SampleMode(true, false);
        if (!mode) { if (getenv("SHIORIKRKR_TRACE_WARP")) fprintf(stderr, "warp: stretch type %d\n", StretchType); return false; }
        float matrix[9];
        for (int i = 0; i < 9; ++i) matrix[i] = float(inv[i]);
        DispatchSampled(m, tar, rcclip, src, view, unsigned(mode), matrix);
        return true;
    }

    template <typename Array>
    static void IdleAll(iTVPTexture2D *tar, iTVPTexture2D *reftar, const Array &textures) {
        if (Texture *t = ours(tar)) t->Idle();
        if (Texture *t = ours(reftar)) t->Idle();
        for (size_t i = 0; i < textures.size(); ++i)
            if (Texture *t = ours(textures[i].first)) t->Idle();
    }

    void OnCPU(Method *m, iTVPTexture2D *tar, iTVPTexture2D *reftar, const tTVPRect &rctar,
               const tRenderTexRectArray &textures) {
        IdleAll(tar, reftar, textures);
        Software->OperateRect(m->Inner, tar, reftar, rctar, textures);
    }

    static bool Inside(const tTVPRect &r, iTVPTexture2D *t) {
        return r.left >= 0 && r.top >= 0 && r.right <= tjs_int(t->GetWidth()) && r.bottom <= tjs_int(t->GetHeight()) &&
            r.left <= r.right && r.top <= r.bottom;
    }
    static bool SameSize(const tTVPRect &a, const tTVPRect &b) {
        return a.get_width() == b.get_width() && a.get_height() == b.get_height();
    }
    static bool Overlap(const tTVPRect &a, const tTVPRect &b) {
        return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
    }

    // Queues the operation on the GPU when it has a kernel and its images allow it; false
    // leaves it to the CPU.
    bool TryGPU(Method *m, iTVPTexture2D *tarTex, const tTVPRect &rctar, const tRenderTexRectArray &textures) {
        const Kind &k = m->K;
        if (k.shape == Shape::None) return false;
        Texture *tar = ours(tarTex);
        if (!tar || !Inside(rctar, tar)) return false;
        int w = rctar.get_width(), h = rctar.get_height();

        Texture *src = nullptr, *dst = nullptr, *rule = nullptr;
        tTVPRect rs, rd, rr;
        size_t n = textures.size();
        switch (k.shape) {
        case Shape::Fill:
            if (n != 0) return false;
            break;
        case Shape::Blt:
        case Shape::BltAndOpa:
        case Shape::BltWithOpa:
            // tar x src -> tar (a second texture must be the target itself)
            if (n == 2) {
                if (textures[1].first != tarTex || !(textures[1].second == rctar)) return false;
            } else if (n != 1) {
                return false;
            }
            src = ours(textures[0].first);
            rs = textures[0].second;
            break;
        case Shape::SD:
            if (n == 1) {
                dst = tar;
                rd = rctar;
            } else if (n == 2) {
                dst = ours(textures[1].first);
                rd = textures[1].second;
            } else {
                return false;
            }
            src = ours(textures[0].first);
            rs = textures[0].second;
            break;
        case Shape::Univ:
            if (n != 3) return false;
            src = ours(textures[0].first);
            rs = textures[0].second;
            dst = ours(textures[1].first);
            rd = textures[1].second;
            rule = ours(textures[2].first);
            rr = textures[2].second;
            break;
        case Shape::None:
            return false;
        }
        if (IsBlt(k) && n == 1 && src && src->BytesPerPixel() == 4 && tar->BytesPerPixel() == 4 &&
            !SameSize(rs, rctar))
            return TryStretch(m, tar, rctar, src, rs);
        if (k.shape != Shape::Fill) {
            if (!src || !SameSize(rs, rctar) || !Inside(rs, src)) return false; // flips: CPU
            if (src->BytesPerPixel() != 4) return false;
            if (src == tar && !(rs == rctar) && Overlap(rs, rctar)) return false;
        }
        if (dst) {
            if (!SameSize(rd, rctar) || !Inside(rd, dst) || dst->BytesPerPixel() != 4) return false;
            if (dst == tar && !(rd == rctar) && Overlap(rd, rctar)) return false;
        }
        if (k.shape == Shape::Univ) {
            if (!rule || rule->BytesPerPixel() != 1 || !SameSize(rr, rctar) || !Inside(rr, rule)) return false;
        }
        bool gray = tar->BytesPerPixel() == 1;
        if (gray && k.op != OpFillARGB) return false;
        if (w <= 0 || h <= 0) return true;

        // Small work on idle images is quicker on the CPU than a dispatch.
        bool busy = tar->Busy() || (src && src->Busy()) || (dst && dst->Busy()) || (rule && rule->Busy());
        if (!busy && int64_t(w) * h < 4096) return false;

        KernelParams p = {};
        p.width = unsigned(w);
        p.height = unsigned(h);
        unsigned bpp = unsigned(tar->BytesPerPixel());
        p.tarPitch = unsigned(tar->GetPitch()) / bpp;
        p.tarOffset = unsigned(rctar.top) * p.tarPitch + unsigned(rctar.left);
        if (src) {
            p.srcPitch = unsigned(src->GetPitch() / 4);
            p.srcOffset = unsigned(rs.top) * p.srcPitch + unsigned(rs.left);
        }
        if (dst) {
            p.dstPitch = unsigned(dst->GetPitch() / 4);
            p.dstOffset = unsigned(rd.top) * p.dstPitch + unsigned(rd.left);
        }
        if (rule) {
            p.rulePitch = unsigned(rule->GetPitch());
            p.ruleOffset = unsigned(rr.top) * p.rulePitch + unsigned(rr.left);
        }
        p.opa = unsigned(m->Opa);
        p.color = m->Color;
        p.op = k.op;
        if (k.shape == Shape::BltAndOpa && m->Opa != 255) p.op = k.opOpa;
        if (k.op == OpFillARGB && gray) p.op = OpFillARGB8;
        if (k.shape == Shape::Univ) {
            if (m->Vague < 512) {
                p.op += 1; // the switch variant
                p.src1lv = m->Phase;
                p.src2lv = m->Phase - m->Vague;
            }
        }
        Dispatch(p, tar, src, dst, rule, k.shape == Shape::Univ ? m->Table : nullptr);
        return true;
    }

    void Dispatch(const KernelParams &p, Texture *tar, Texture *src, Texture *dst, Texture *rule, const uint32_t *table) {
        Device &g = gpu();
        std::lock_guard<std::recursive_mutex> lock(g.mutex);
        g.ensureTables();
        uint64_t serial = 0;
        id<MTLComputeCommandEncoder> e = g.encoderForDispatch(serial);
        id<MTLBuffer> t = tar->Buffer();
        [e setBuffer:t offset:0 atIndex:0];
        [e setBuffer:src ? src->Buffer() : t offset:0 atIndex:1];
        [e setBuffer:dst ? dst->Buffer() : t offset:0 atIndex:2];
        [e setBuffer:rule ? rule->Buffer() : t offset:0 atIndex:3];
        [e setBytes:&p length:sizeof(p) atIndex:4];
        [e setBuffer:g.ooo offset:0 atIndex:5];
        [e setBuffer:g.negmul offset:0 atIndex:6];
        static const uint32_t none[256] = {};
        [e setBytes:table ? table : none length:sizeof(none) atIndex:7];
        [e setBuffer:t offset:0 atIndex:8];
        [e dispatchThreads:MTLSizeMake(p.width, p.height, 1) threadsPerThreadgroup:MTLSizeMake(32, 8, 1)];
        tar->MarkUsed(serial);
        if (src) src->MarkUsed(serial);
        if (dst) dst->MarkUsed(serial);
        if (rule) rule->MarkUsed(serial);
        // Keep a long frame's work moving.
        if (g.openDispatches >= 256) g.commit();
    }

    // Warps and stretches sample in float on the GPU and in double on the CPU: a byte may differ
    // by one, a pixel at an edge may come from its neighbour. Reports how many pixels differ by
    // more than one in a byte; keeps the CPU's.
    void VerifyWarp(Method *m, int nTriangles, iTVPTexture2D *targetTex, iTVPTexture2D *reftar, const tTVPRect &rc,
                    const tTVPPointD *pt, const tRenderTexQuadArray &textures) {
        Texture *tar = ours(targetTex);
        IdleAll(targetTex, reftar, textures);
        const int w = rc.get_width(), h = rc.get_height();
        auto save = [&](std::vector<uint32_t> &out) {
            out.resize(size_t(w) * h);
            for (int y = 0; y < h; ++y)
                memcpy(&out[size_t(y) * w], tar->Raw() + size_t(rc.top + y) * tar->GetPitch() + size_t(rc.left) * 4, size_t(w) * 4);
        };
        std::vector<uint32_t> before, gpuResult, cpuResult;
        save(before);
        bool onGPU = TryWarp(m, nTriangles, targetTex, rc, pt, textures);
        if (onGPU) {
            gpu().commit();
            tar->Idle();
            save(gpuResult);
            for (int y = 0; y < h; ++y)
                memcpy(tar->Raw() + size_t(rc.top + y) * tar->GetPitch() + size_t(rc.left) * 4, &before[size_t(y) * w], size_t(w) * 4);
        }
        Software->OperateTriangles(m->Inner, nTriangles, targetTex, reftar, rc, pt, textures);
        if (!onGPU) return;
        save(cpuResult);
        size_t off = 0;
        for (size_t i = 0; i < cpuResult.size(); ++i)
            for (int b = 0; b < 32; b += 8) {
                int d = int((cpuResult[i] >> b) & 0xff) - int((gpuResult[i] >> b) & 0xff);
                if (d > 1 || d < -1) {
                    ++off;
                    break;
                }
            }
        OpStat &st = Stats[m->GetName() + "(warp check)"];
        st.gpu += 1;
        st.pixels += cpuResult.size();
        if (off * 1000 > cpuResult.size()) { // more than 0.1% of the pixels
            if (st.mismatches++ < 3)
                fprintf(stderr, "shiorikrkr metal verify: %s warp: %zu of %zu pixels differ (stretch type %d)\n",
                        m->GetName().c_str(), off, cpuResult.size(), StretchType);
        }
    }

    // Runs the operation on the GPU and on the CPU from the same starting pixels, compares
    // the results and keeps the CPU's.
    bool Verify(Method *m, iTVPTexture2D *tarTex, iTVPTexture2D *reftar, const tTVPRect &rctar,
                const tRenderTexRectArray &textures) {
        Texture *tar = ours(tarTex);
        if (!tar) {
            OnCPU(m, tarTex, reftar, rctar, textures);
            return false;
        }
        IdleAll(tarTex, reftar, textures);
        int bpp = tar->BytesPerPixel();
        int w = std::max(0, rctar.get_width()), h = std::max(0, rctar.get_height());
        bool inside = Inside(rctar, tar);
        if (!inside || w == 0 || h == 0) {
            OnCPU(m, tarTex, reftar, rctar, textures);
            return false;
        }
        std::vector<uint8_t> before, gpuResult;
        auto save = [&](std::vector<uint8_t> &out) {
            out.resize(size_t(w) * h * bpp);
            for (int y = 0; y < h; ++y)
                memcpy(out.data() + size_t(y) * w * bpp, tar->Raw() + size_t(rctar.top + y) * tar->GetPitch() + size_t(rctar.left) * bpp,
                       size_t(w) * bpp);
        };
        auto restore = [&](const std::vector<uint8_t> &in) {
            for (int y = 0; y < h; ++y)
                memcpy(tar->Raw() + size_t(rctar.top + y) * tar->GetPitch() + size_t(rctar.left) * bpp, in.data() + size_t(y) * w * bpp,
                       size_t(w) * bpp);
        };
        save(before);
        // Force the GPU path regardless of size.
        tar->MarkUsed(gpu().nextSerial); // looks busy, so TryGPU doesn't hand small work back
        bool onGPU = TryGPU(m, tarTex, rctar, textures);
        if (!onGPU) {
            tar->MarkUsed(0);
            OnCPU(m, tarTex, reftar, rctar, textures);
            return false;
        }
        gpu().commit();
        tar->Idle();
        save(gpuResult);
        restore(before);
        OnCPU(m, tarTex, reftar, rctar, textures);
        std::vector<uint8_t> cpuResult;
        save(cpuResult);
        if (cpuResult != gpuResult) {
            OpStat &s = Stats[m->GetName()];
            if (s.mismatches++ < 3) {
                size_t i = 0;
                while (i < cpuResult.size() && cpuResult[i] == gpuResult[i]) ++i;
                size_t px = i / bpp;
                uint32_t c = 0, g = 0, b = 0;
                memcpy(&c, cpuResult.data() + px * bpp, std::min(bpp, 4));
                memcpy(&g, gpuResult.data() + px * bpp, std::min(bpp, 4));
                memcpy(&b, before.data() + px * bpp, std::min(bpp, 4));
                fprintf(stderr, "shiorikrkr metal verify: %s differs at (%zu,%zu) of %dx%d: cpu %08x gpu %08x before %08x opa %d color %08x\n",
                        m->GetName().c_str(), px % w, px / w, w, h, c, g, b, m->Opa, m->Color);
            }
        }
        return true;
    }
};

Manager *instance = nullptr;

iTVPRenderManager *makeManager() {
    if (!gpu().device && !gpu().start()) {
        fprintf(stderr, "shiorikrkr metal: no Metal device; using the software renderer\n");
        return TVPGetRenderManager(ttstr(TJS_W("software")));
    }
    instance = new Manager;
    return instance;
}

struct Registration {
    Registration() { TVPRegisterRenderManager("metal", makeManager); }
} registration;

} // namespace

void flush() {
    if (gpu().device) gpu().commit();
}

std::string profileReport(double ticks) {
    if (!instance) return {};
    std::vector<std::pair<std::string, OpStat>> rows(instance->Stats.begin(), instance->Stats.end());
    std::sort(rows.begin(), rows.end(), [](auto &a, auto &b) { return a.second.seconds > b.second.seconds; });
    std::string out;
    char line[256];
    Device &g = gpu();
    snprintf(line, sizeof line, "metal: %.2f syncs/tick (%.2f ms), %.1f dispatches, %.2f commits;", g.syncs / ticks,
             g.syncSeconds * 1000 / ticks, g.dispatches / ticks, g.commits / ticks);
    out += line;
    int shown = 0;
    for (auto &[name, s] : rows) {
        if (shown++ >= 8) break;
        snprintf(line, sizeof line, " %s %.2fms %ugpu/%ucpu %.0fkpx%s", name.c_str(), s.seconds * 1000 / ticks, s.gpu, s.cpu,
                 s.pixels / 1000.0 / ticks, s.mismatches ? " MISMATCH" : "");
        out += line;
    }
    for (auto &[name, s] : rows)
        if (s.mismatches) {
            snprintf(line, sizeof line, " [%s: %u mismatches]", name.c_str(), s.mismatches);
            out += line;
        }
    instance->Stats.clear();
    g.syncs = g.commits = g.dispatches = 0;
    g.syncSeconds = 0;
    return out;
}

bool encodeInto(iTVPTexture2D *texture, const std::function<bool(void *, size_t, void *)> &encode) {
    Texture *t = ours(texture);
    if (!t || t->BytesPerPixel() != 4) return false;
    Device &g = gpu();
    std::lock_guard<std::recursive_mutex> lock(g.mutex);
    uint64_t serial = 0;
    id<MTLCommandBuffer> commands = g.bareCommandBuffer(serial);
    const bool encoded = encode((__bridge void *)t->Buffer(), size_t(t->GetPitch()), (__bridge void *)commands);
    t->MarkUsed(serial);
    return encoded;
}

bool presentFrame(iTVPTexture2D *texture, int width, int height, bool changed, PresentedFrame &out) {
    Texture *t = ours(texture);
    if (!t || width <= 0 || height <= 0) return false;
    static id<MTLTexture> ring[3];
    static int next = 0;
    static PresentedFrame last;
    Device &g = gpu();
    if (!ring[0] || int(ring[0].width) != width || int(ring[0].height) != height) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                     width:width height:height mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead;
        d.storageMode = MTLStorageModeShared;
        for (auto &r : ring) r = [g.device newTextureWithDescriptor:d];
        changed = true;
    }
    if (changed || !last.texture) {
        // Three in turn: the host is done with one by the time it comes round again.
        id<MTLTexture> into = ring[next];
        next = (next + 1) % 3;
        uint64_t serial = 0;
        uint64_t value = g.commitFrame(t->Buffer(), size_t(t->GetPitch()), width, height, into, serial);
        t->MarkUsed(serial);
        last.texture = (__bridge void *)into;
        last.event = (__bridge void *)g.frameEvent;
        last.value = value;
    }
    out = last;
    return true;
}

} // namespace shiori::metal
