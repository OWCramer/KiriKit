/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

// emoteplayer.dll's script surface for ShioriKrkr: the Motion class and the players games
// draw onto plain layers with (NEKOPARA Vol. 4's AffineSourceMotion.tjs: a resource
// manager, Motion.EmotePlayer drawing into a layer through a SeparateLayerAdaptor). Shiori's
// own classes under the plug-in's names; the models, commands and drawing go to the host's
// E-mote player (shiorikrkr_emote_host_t), as D3DEmotePlayer's do (shiorikrkr_emote.cpp).
// The constants' values are the plug-in's public interface, as the games' scripts use them.
//
// Motion.Player (the plain motion files, .mtn) is answered as an idle player for now: it
// draws nothing (NEKOPARA's characters are all E-mote).
#include "tjsCommHead.h"
#include <ncbind.hpp>
#include "tjsArray.h"
#include "tjsDictionary.h"
#include "LayerIntf.h"
#include "StorageIntf.h"
#include "ScriptMgnIntf.h"
#include "DebugIntf.h"

#include <algorithm>
#include <chrono>
extern double ShioriEmoteDrawSeconds;
extern double ShioriEmoteLoadSeconds;
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "shiorikrkr_emote.h"
#include "render/shiori_metal_render.h"
#include "RenderManager.h"
#include "LayerBitmapIntf.h"

#define NCB_MODULE_NAME TJS_W("emoteplayer.dll")

using namespace shiorikrkr::util;

namespace {

const shiorikrkr_emote_host_t *host() { return shiorikrkr::emoteHost(); }

// A host player owned here (released with the last holder).
struct HostPlayer {
    void *player = nullptr;
    explicit HostPlayer(void *p) : player(p) {}
    ~HostPlayer() {
        if (player && host()) host()->destroy(host()->context, player);
    }
};

std::string hostText(void *player, const char *verb, const char *label, double index = 0) {
    if (!player || !host()) return {};
    char out[512] = {};
    host()->query(host()->context, player, verb, label, &index, 1, out, sizeof(out));
    return out;
}

double hostNumber(void *player, const char *verb, const std::string &label = {}, const double *n = nullptr, int32_t c = 0) {
    if (!player || !host()) return 0;
    char out[4] = {};
    return host()->query(host()->context, player, verb, label.c_str(), n, c, out, sizeof(out));
}

void hostCommand(void *player, const char *verb, const std::string &label = {}, const double *n = nullptr, int32_t c = 0) {
    if (player && host()) host()->command(host()->context, player, verb, label.c_str(), n, c);
}

tTJSVariant stringArray(const std::vector<std::string> &items) {
    iTJSDispatch2 *array = TJSCreateArrayObject();
    for (size_t i = 0; i < items.size(); ++i) {
        tTJSVariant v(ttstr(items[i].c_str()));
        array->PropSetByNum(TJS_MEMBERENSURE, tjs_int(i), &v, array);
    }
    tTJSVariant result(array, array);
    array->Release();
    return result;
}

void setMember(iTJSDispatch2 *dict, const char *name, const tTJSVariant &value) {
    tTJSVariant v = value;
    dict->PropSet(TJS_MEMBERENSURE, ttstr(name).c_str(), nullptr, &v, dict);
}

tTJSVariant dictionary(iTJSDispatch2 *&out) {
    out = TJSCreateDictionaryObject();
    tTJSVariant result(out, out);
    out->Release();
    return result;
}

tTJSNI_BaseLayer *layerOf(iTJSDispatch2 *object);

} // namespace

// Motion.ResourceManager: reads each model once (the host's player is the prototype the game's
// players are copied from) and answers its metadata.
class ShioriMotionResources {
public:
    ShioriMotionResources(iTJSDispatch2 *, tjs_int) {}

    struct Resource {
        std::shared_ptr<HostPlayer> prototype;
        std::string chara, motion;
        tTJSVariant info;
        int references = 0;
    };

    tTJSVariant load(ttstr path) {
        const std::string key = utf8(path);
        auto found = resources_.find(key);
        if (found != resources_.end()) {
            found->second.references++;
            order_.erase(std::remove(order_.begin(), order_.end(), key), order_.end());
            order_.push_back(key);
            return found->second.info;
        }
        const auto loadStart = std::chrono::steady_clock::now();
        struct LoadTimer {
            std::chrono::steady_clock::time_point start;
            ~LoadTimer() { ShioriEmoteLoadSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(); }
        } loadTimer{loadStart};
        Resource r;
        std::vector<uint8_t> bytes;
        std::string error;
        // NEKO WORKs ship each character twice: the plain-layer set (emote.xp3) and the
        // Direct3D set (emotewin.xp3, "dx_" names) the PC game shows by default. Shiori's
        // renderer draws the Direct3D set; the scripts can't tell the difference.
        ttstr source = path;
        {
            const ttstr name = TVPExtractStorageName(path);
            const ttstr dx = TVPExtractStoragePath(path) + TJS_W("dx_") + name;
            if (TVPIsExistentStorage(dx)) source = dx;
        }
        if (!readStorage(source, bytes, error)) TVPThrowExceptionMessage(ttstr(error.c_str()).c_str());
        if (host()) {
            const uint8_t *data = bytes.data();
            const uint64_t size = bytes.size();
            const char *name = key.c_str();
            char message[512] = {};
            void *player = host()->create(host()->context, &data, &size, &name, 1, message, sizeof(message));
            if (!player) TVPThrowExceptionMessage((ttstr("ShioriKrkr E-mote: ") + ttstr(message)).c_str());
            r.prototype = std::make_shared<HostPlayer>(player);
            r.chara = hostText(player, "info", "chara");
            r.motion = hostText(player, "info", "motion");
            TVPAddLog(ttstr("ShioriKrkr E-mote: ") + path + ttstr(" loaded"));
        }
        // %[metadata: %[base: %[chara, motion]]], as AffineSourceMotion reads it.
        iTJSDispatch2 *info = nullptr, *metadata = nullptr, *base = nullptr;
        r.info = dictionary(info);
        tTJSVariant metadataValue = dictionary(metadata), baseValue = dictionary(base);
        setMember(base, "chara", ttstr(r.chara.c_str()));
        setMember(base, "motion", ttstr(r.motion.c_str()));
        setMember(metadata, "base", baseValue);
        setMember(info, "metadata", metadataValue);
        r.references = 1;
        resources_[key] = r;
        order_.push_back(key);
        return r.info;
    }

    void unload(ttstr path) {
        auto found = resources_.find(utf8(path));
        if (found != resources_.end() && --found->second.references <= 0) {
            order_.erase(std::remove(order_.begin(), order_.end(), found->first), order_.end());
            resources_.erase(found);
        }
    }
    void unloadAll() {
        resources_.clear();
        order_.clear();
    }
    void clearCache() {}
    void setEmotePSBDecryptSeed(tjs_int) {} // the host finds the seed from the files
    void setEmotePSBDecryptFunc(tTJSVariant) {}

    // The newest model for a character (the one loaded last when none names it).
    std::shared_ptr<HostPlayer> prototype(const std::string &chara) {
        for (auto it = order_.rbegin(); it != order_.rend(); ++it) {
            const auto &r = resources_[*it];
            if (r.prototype && (chara.empty() || r.chara == chara)) return r.prototype;
        }
        for (auto it = order_.rbegin(); it != order_.rend(); ++it)
            if (resources_[*it].prototype) return resources_[*it].prototype;
        return nullptr;
    }

private:
    std::map<std::string, Resource> resources_;
    std::vector<std::string> order_;
};

NCB_REGISTER_SUBCLASS(ShioriMotionResources) {
    NCB_CONSTRUCTOR((iTJSDispatch2 *, tjs_int));
    NCB_METHOD(load);
    NCB_METHOD(unload);
    NCB_METHOD(unloadAll);
    NCB_METHOD(clearCache);
    NCB_METHOD(setEmotePSBDecryptSeed);
    NCB_METHOD(setEmotePSBDecryptFunc);
}

// Motion.SeparateLayerAdaptor: the character drawn on a layer of its own. The game makes its
// owner a binder layer (no image of its own, ltBinder) and draws through the adaptor, so the
// character lives in a child layer the adaptor keeps over the owner's whole area.
class ShioriSeparateLayerAdaptor {
public:
    explicit ShioriSeparateLayerAdaptor(iTJSDispatch2 *owner) : owner_(owner) {
        if (owner_) owner_->AddRef();
    }
    ~ShioriSeparateLayerAdaptor() {
        if (child_) {
            if (child_->IsValid(0, nullptr, nullptr, child_) == TJS_S_TRUE)
                child_->Invalidate(0, nullptr, nullptr, child_);
            child_->Release();
        }
        if (owner_) owner_->Release();
    }
    void assign(iTJSDispatch2 *) {}
    void clear() {}
    bool getAbsolute() { return false; }
    void setAbsolute(bool) {}
    bool getIsPrimary() { return false; }
    void setIsPrimary(bool) {}
    tTJSVariant getParent() { return tTJSVariant(); }
    void setParent(tTJSVariant) {}

    // The layer to draw into, made (or resized to the owner) as needed.
    iTJSDispatch2 *layer() {
        if (!owner_) return nullptr;
        // A function of the current game's script engine, made again for each game (the last
        // game's is left alone: its engine is gone).
        static tTJSVariant *prepareFor = nullptr;
        static unsigned preparedFor = 0;
        if (!prepareFor || preparedFor != shiorikrkr::sessionGeneration()) {
            preparedFor = shiorikrkr::sessionGeneration();
            prepareFor = new tTJSVariant();
        }
        tTJSVariant &prepare = *prepareFor;
        if (prepare.Type() == tvtVoid) {
            TVPExecuteExpression(TJS_W("(function(owner, child) {"
                                       " if (child === void) {"
                                       "  child = new global.Layer(owner.window, owner);"
                                       "  child.name = 'E-mote'; child.type = ltAlpha; child.hitThreshold = 256;"
                                       "  child.visible = true; }"
                                       " var p = owner.window.primaryLayer;"
                                       " var w = owner.width > 0 ? owner.width : p.width, h = owner.height > 0 ? owner.height : p.height;"
                                       " if (child.imageWidth != w || child.imageHeight != h) {"
                                       "  child.setImageSize(w, h); child.setSizeToImageSize(); }"
                                       " return child; })"),
                                 &prepare);
        }
        tTJSVariant ownerValue(owner_, owner_), childValue, result;
        if (child_) childValue = tTJSVariant(child_, child_);
        tTJSVariant *params[] = {&ownerValue, &childValue};
        iTJSDispatch2 *fn = prepare.AsObjectNoAddRef();
        if (!fn || TJS_FAILED(fn->FuncCall(0, nullptr, nullptr, &result, 2, params, nullptr))) return nullptr;
        iTJSDispatch2 *made = result.AsObjectNoAddRef();
        if (made && made != child_) {
            made->AddRef();
            if (child_) child_->Release();
            child_ = made;
        }
        return child_;
    }

private:
    iTJSDispatch2 *owner_ = nullptr;
    iTJSDispatch2 *child_ = nullptr;
};

NCB_REGISTER_SUBCLASS(ShioriSeparateLayerAdaptor) {
    NCB_CONSTRUCTOR((iTJSDispatch2 *));
    NCB_METHOD(assign);
    NCB_METHOD(clear);
    NCB_PROPERTY(absolute, getAbsolute, setAbsolute);
    NCB_PROPERTY(isPrimary, getIsPrimary, setIsPrimary);
    NCB_PROPERTY(parent, getParent, setParent);
}

namespace {
tTJSNI_BaseLayer *layerOf(iTJSDispatch2 *object) {
    if (!object) return nullptr;
    if (auto *adaptor = ncbInstanceAdaptor<ShioriSeparateLayerAdaptor>::GetNativeInstance(object))
        object = adaptor->layer();
    tTJSNI_BaseLayer *layer = nullptr;
    if (!object || TJS_FAILED(object->NativeInstanceSupport(TJS_NIS_GETINSTANCE, tTJSNC_Layer::ClassID,
                                                             (iTJSNativeInstance **)&layer)))
        return nullptr;
    return layer;
}
} // namespace

// Motion.D3DAdaptor: the canvas the Direct3D path draws characters on (NEKOPARA Extra and
// Vol. 4 take it): players draw into it, captureCanvas copies it into a layer and starts the
// next frame clear.
namespace {
// Whether characters draw on the GPU: the host can, and the images are the Metal compositor's.
bool drawsOnGPU() {
    const shiorikrkr_emote_host_t *h = shiorikrkr::emoteHost();
    if (!h || !h->draw_gpu) return false;
    static const bool metal = std::string(TVPGetRenderManager()->GetName()) == "Metal";
    return metal;
}

// A layer's image as the compositor's texture, ready to be drawn over whole.
iTVPTexture2D *layerTexture(tTJSNI_BaseLayer *layer) {
    tTVPBaseTexture *image = layer ? layer->GetMainImage() : nullptr;
    if (!image) return nullptr;
    tTVPRect all(0, 0, tjs_int(layer->GetImageWidth()), tjs_int(layer->GetImageHeight()));
    return image->GetTextureForRender(false, &all);
}

// Fills a texture of the compositor's with an ARGB colour.
void fillTexture(iTVPTexture2D *texture, tjs_uint32 argb) {
    iTVPRenderManager *manager = TVPGetRenderManager();
    static iTVPRenderMethod *fill = manager->GetRenderMethod("FillARGB");
    static int color = fill->EnumParameterID("color");
    // As the pixel reads in memory (R, G, B, A).
    const tjs_uint32 rgba = (argb & 0xFF00FF00) | ((argb >> 16) & 0xFF) | ((argb & 0xFF) << 16);
    fill->SetParameterColor4B(color, rgba);
    manager->OperateRect(fill, texture, nullptr, tTVPRect(0, 0, texture->GetWidth(), texture->GetHeight()),
                         tRenderTexRectArray());
}
} // namespace

class ShioriD3DAdaptor {
public:
    ShioriD3DAdaptor(iTJSDispatch2 *, tjs_int width, tjs_int height, tjs_int left, tjs_int top)
        : width_(std::max<tjs_int>(width, 0)), height_(std::max<tjs_int>(height, 0)) {
        if (width_ && height_ && drawsOnGPU())
            texture_ = TVPGetRenderManager()->CreateTexture2D(nullptr, 0, width_, height_, TVPTextureFormat::RGBA);
        else
            pixels_.assign(size_t(width_) * height_ * 4, 0);
        if (getenv("SHIORIKRKR_TRACE_EMOTE"))
            fprintf(stderr, "D3DAdaptor %d,%d %dx%d\n", int(left), int(top), int(width), int(height));
    }
    void setClearColor(tjs_uint32 argb) {
        clear_ = argb;
        clearCanvas();
    }
    ~ShioriD3DAdaptor() {
        if (texture_) texture_->Release();
    }
    void captureCanvas(iTJSDispatch2 *target);
    void unloadUnusedTextures() {}

    /// The canvas as the compositor's texture (GPU drawing), or null (CPU drawing).
    iTVPTexture2D *texture() const { return texture_; }

    uint8_t *pixels() { return pixels_.data(); }
    // Only the rectangle the host reports is read back, and the host writes all of it.
    uint8_t *scratch() {
        if (scratch_.size() != pixels_.size()) scratch_.resize(pixels_.size());
        return scratch_.data();
    }
    // The scratch image's rectangle over the canvas (straight alpha, source over).
    void blendScratch(const int32_t *bounds) {
        const int x0 = std::max(0, int(bounds[0])), y0 = std::max(0, int(bounds[1]));
        const int x1 = std::min(width_, int(bounds[2])), y1 = std::min(height_, int(bounds[3]));
        for (int y = y0; y < y1; ++y) {
            const uint8_t *s = &scratch_[(size_t(y) * width_ + x0) * 4];
            uint8_t *d = &pixels_[(size_t(y) * width_ + x0) * 4];
            for (int x = x0; x < x1; ++x, s += 4, d += 4) {
                const int sa = s[3];
                if (sa == 0) continue;
                if (sa == 255 || d[3] == 0) {
                    memcpy(d, s, 4);
                    continue;
                }
                const int da = d[3] * (255 - sa) / 255, oa = sa + da;
                for (int c = 0; c < 3; ++c) d[c] = uint8_t((s[c] * sa + d[c] * da + oa / 2) / oa);
                d[3] = uint8_t(oa);
            }
        }
    }
    int width() const { return width_; }
    int height() const { return height_; }

private:
    void clearCanvas() {
        if (texture_) {
            fillTexture(texture_, clear_);
            return;
        }
        // ARGB to the R, G, B, A krkr2 keeps pixels in.
        const uint8_t px[4] = {uint8_t(clear_ >> 16), uint8_t(clear_ >> 8), uint8_t(clear_), uint8_t(clear_ >> 24)};
        for (size_t i = 0; i < pixels_.size(); i += 4) memcpy(&pixels_[i], px, 4);
    }

    int width_, height_;
    std::vector<uint8_t> pixels_, scratch_;
    iTVPTexture2D *texture_ = nullptr;
    tjs_uint32 clear_ = 0;
};

// Motion.EmotePlayer: an E-mote character drawn into a layer.
class ShioriLayerEmotePlayer {
public:
    explicit ShioriLayerEmotePlayer(ShioriMotionResources *resources) : resources_(resources) {}

    void play(ttstr motion, tjs_int flags) {
        motion_ = utf8(motion);
        if (ensure()) {
            const double f = flags;
            hostCommand(player(), "play", motion_, &f, 1);
            if (getenv("SHIORIKRKR_TRACE_EMOTE")) TVPAddLog(ttstr("E-mote play ") + motion);
        }
    }
    void initPhysics(tTJSVariant) {}

    tTJSVariant serialize() {
        // A token the next unserialize copies the state from (the game copies a layer).
        return tTJSVariant(tjs_int64(reinterpret_cast<intptr_t>(this)));
    }
    void unserialize(tTJSVariant token) {
        auto *other = reinterpret_cast<ShioriLayerEmotePlayer *>(intptr_t(token.AsInteger()));
        if (!other || !other->player_ || !host() || !host()->clone) return;
        player_ = std::make_shared<HostPlayer>(host()->clone(host()->context, other->player_->player));
        chara_ = other->chara_;
        motion_ = other->motion_;
    }

    // clear(layer, colour): the layer filled with the colour (its neutral, transparent).
    void clear(iTJSDispatch2 *target, tjs_uint32 color) {
        tTJSNI_BaseLayer *layer = layerOf(target);
        if (!layer) return;
        auto *pixels = static_cast<uint8_t *>(layer->GetMainImagePixelBufferForWrite());
        if (!pixels) return;
        const tjs_int pitch = layer->GetMainImagePixelBufferPitch();
        const tjs_uint w = layer->GetImageWidth(), h = layer->GetImageHeight();
        for (tjs_uint y = 0; y < h; ++y) {
            auto *row = reinterpret_cast<uint32_t *>(pixels + y * pitch);
            std::fill(row, row + w, color);
        }
    }

    // progress(milliseconds)
    void progress(tjs_real ms) {
        if (!ensure()) return;
        const double controls[4] = {meshDivision_, bust_, hair_, parts_};
        hostCommand(player(), "controls", "", controls, 4);
        const double frames = ms * 60.0 / 1000.0;
        hostCommand(player(), "progress", "", &frames, 1);
    }

    // draw(layer or adaptor): the character into the layer's image, through the affine the
    // game set (setDrawAffineTranslateMatrix).
    void draw(iTJSDispatch2 *target) {
        if (auto *canvas = target ? ncbInstanceAdaptor<ShioriD3DAdaptor>::GetNativeInstance(target) : nullptr) {
            if (!ensure() || !host() || !canvas->width() || !canvas->height()) return;
            if (canvas->texture()) {
                hostCommand(player(), "show");
                const auto drawStart = std::chrono::steady_clock::now();
                drawGPU(canvas->texture(), canvas->width(), canvas->height(), true);
                ShioriEmoteDrawSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - drawStart).count();
                return;
            }
            hostCommand(player(), "show");
            int32_t bounds[4] = {0, 0, 0, 0};
            const auto drawStart = std::chrono::steady_clock::now();
            // Into a scratch image, then over what the canvas holds (several characters
            // share one canvas; the host writes its whole covered rectangle).
            uint8_t *scratch = canvas->scratch();
            const int drew = host()->draw(host()->context, player(), scratch, canvas->width(), canvas->height(),
                         canvas->width() * 4, affine_, bounds);
            if (drew) canvas->blendScratch(bounds);
            static int traced = 0;
            if (getenv("SHIORIKRKR_TRACE_EMOTE") && traced++ < 20)
                fprintf(stderr, "E-mote canvas draw %p: drew %d bounds %d,%d,%d,%d affine %.2f %.2f %.2f %.2f %.1f %.1f\n", (void *)this, drew,
                        bounds[0], bounds[1], bounds[2], bounds[3], affine_[0], affine_[1], affine_[2], affine_[3], affine_[4], affine_[5]);
            ShioriEmoteDrawSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - drawStart).count();
            return;
        }
        tTJSNI_BaseLayer *layer = layerOf(target);
        if (!layer || !ensure() || !host()) return;
        if (drawsOnGPU()) {
            // On the GPU, into the layer's image whole (the character's own layer).
            if (iTVPTexture2D *texture = layerTexture(layer)) {
                hostCommand(player(), "show");
                const auto drawStart = std::chrono::steady_clock::now();
                const bool drew = drawGPU(texture, int(layer->GetImageWidth()), int(layer->GetImageHeight()), false);
                ShioriEmoteDrawSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - drawStart).count();
                if (drew) {
                    layer->Update();
                    return;
                }
            }
        }
        auto *pixels = static_cast<uint8_t *>(layer->GetMainImagePixelBufferForWrite());
        if (!pixels) return;
        const tjs_int pitch = layer->GetMainImagePixelBufferPitch();
        const int w = int(layer->GetImageWidth()), h = int(layer->GetImageHeight());
        hostCommand(player(), "show");
        int32_t bounds[4] = {0, 0, 0, 0};
        // The host draws R, G, B, A, the order krkr2 keeps layer pixels in.
        const auto drawStart = std::chrono::steady_clock::now();
        const int drew = host()->draw(host()->context, player(), pixels, w, h, pitch, affine_, bounds);
        ShioriEmoteDrawSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - drawStart).count();
        static const bool trace = getenv("SHIORIKRKR_TRACE_EMOTE") != nullptr;
        if (trace) {
            char line[256];
            snprintf(line, sizeof line, "E-mote draw %p into %dx%d: drew %d bounds %d,%d,%d,%d affine %.2f %.2f %.2f %.2f %.1f %.1f",
                     (void *)this, w, h, drew, bounds[0], bounds[1], bounds[2], bounds[3], affine_[0], affine_[1],
                     affine_[2], affine_[3], affine_[4], affine_[5]);
            TVPAddLog(ttstr(line));
        }
        layer->Update();
    }

    // Encodes the character into a texture of the compositor's (blend: over what is there).
    bool drawGPU(iTVPTexture2D *texture, int width, int height, bool blend) {
        return shiori::metal::encodeInto(texture, [&](void *buffer, size_t pitch, void *commands) {
            return host()->draw_gpu(host()->context, player(), buffer, std::min<int>(width, int(texture->GetWidth())),
                                    std::min<int>(height, int(texture->GetHeight())), int(pitch), affine_, commands,
                                    blend ? 1 : 0) != 0;
        });
    }

    // Transition times (coord, rotation, scale, colour, variables, timeline blends and fades)
    // are in frames of 1/60 s, as the games pass them (AffineSourceMotion.tjs converts its
    // milliseconds with `time * 60 / 1000` before calling); only progress() takes milliseconds.
    static tjs_error setCoord(tTJSVariant *, tjs_int n, tTJSVariant **p, ShioriLayerEmotePlayer *self) {
        const double v[4] = {number(p, n, 0), number(p, n, 1), number(p, n, 2), number(p, n, 3)};
        self->command("coord", "", v, 4);
        return TJS_S_OK;
    }
    static tjs_error setRotate(tTJSVariant *, tjs_int n, tTJSVariant **p, ShioriLayerEmotePlayer *self) {
        const double v[3] = {number(p, n, 0), number(p, n, 1), number(p, n, 2)};
        self->command("rot", "", v, 3);
        return TJS_S_OK;
    }
    static tjs_error setScale(tTJSVariant *, tjs_int n, tTJSVariant **p, ShioriLayerEmotePlayer *self) {
        const double v[3] = {number(p, n, 0, 1), number(p, n, 1), number(p, n, 2)};
        self->command("scale", "", v, 3);
        return TJS_S_OK;
    }
    static tjs_error setColor(tTJSVariant *, tjs_int n, tTJSVariant **p, ShioriLayerEmotePlayer *self) {
        // 0xAARRGGBB here; the host takes 0xRRGGBBAA.
        const tjs_uint32 argb = tjs_uint32(tjs_int64(number(p, n, 0, 0xFFFFFFFF)));
        const double v[3] = {double((argb << 8) | (argb >> 24)), number(p, n, 1), number(p, n, 2)};
        self->command("color", "", v, 3);
        return TJS_S_OK;
    }
    static tjs_error setVariable(tTJSVariant *, tjs_int n, tTJSVariant **p, ShioriLayerEmotePlayer *self) {
        const double v[3] = {number(p, n, 1), number(p, n, 2), number(p, n, 3)};
        self->command("variable", text(p, n, 0), v, 3);
        return TJS_S_OK;
    }
    static tjs_error getVariable(tTJSVariant *r, tjs_int n, tTJSVariant **p, ShioriLayerEmotePlayer *self) {
        if (r) *r = self->ensure() ? hostNumber(self->player(), "variable", text(p, n, 0)) : 0.0;
        return TJS_S_OK;
    }
    void setOuterForce(tTJSVariant, tjs_real, tjs_real) {}
    // (a, c, b, d, tx, ty): x' = a x + b y + tx, y' = c x + d y + ty.
    void setDrawAffineTranslateMatrix(tjs_real a, tjs_real c, tjs_real b, tjs_real d, tjs_real tx, tjs_real ty) {
        affine_[0] = a;
        affine_[1] = c;
        affine_[2] = b;
        affine_[3] = d;
        affine_[4] = tx;
        affine_[5] = ty;
    }
    void setCameraOffset(tjs_real, tjs_real) {}
    static tjs_error startWind(tTJSVariant *, tjs_int n, tTJSVariant **p, ShioriLayerEmotePlayer *self) {
        const double v[5] = {number(p, n, 0), number(p, n, 1), number(p, n, 2), number(p, n, 3), number(p, n, 4)};
        self->command("wind", "", v, 5);
        return TJS_S_OK;
    }
    void stopWind() { command("windstop"); }
    static tjs_error contains(tTJSVariant *r, tjs_int n, tTJSVariant **p, ShioriLayerEmotePlayer *self) {
        std::string label;
        int first = 0;
        if (n >= 3) {
            label = text(p, n, 0);
            first = 1;
        }
        const double v[2] = {number(p, n, first), number(p, n, first + 1)};
        if (r) *r = self->ensure() && hostNumber(self->player(), "contains", label, v, 2) != 0;
        return TJS_S_OK;
    }
    void skip() { command("skip"); }
    void skipToSync() { command("skip"); }
    void pass() { command("pass"); }
    void stop() { command("stop"); }
    static tjs_error playTimeline(tTJSVariant *, tjs_int n, tTJSVariant **p, ShioriLayerEmotePlayer *self) {
        const double v[1] = {number(p, n, 1)};
        self->command("play", text(p, n, 0), v, 1);
        return TJS_S_OK;
    }
    static tjs_error stopTimeline(tTJSVariant *, tjs_int n, tTJSVariant **p, ShioriLayerEmotePlayer *self) {
        self->command("stop", text(p, n, 0));
        return TJS_S_OK;
    }
    bool getTimelinePlaying(ttstr label) { return ensure() && hostNumber(player(), "playing", utf8(label)) != 0; }
    bool getLoopTimeline(ttstr label) { return ensure() && hostNumber(player(), "loop", utf8(label)) != 0; }
    tjs_real getTimelineTotalFrameCount(ttstr label) { return ensure() ? hostNumber(player(), "length", utf8(label)) : 0; }
    tTJSVariant getMainTimelineLabelList() { return stringArray(labels("main")); }
    tTJSVariant getDiffTimelineLabelList() { return stringArray(labels("diff")); }
    static tjs_error setTimelineBlendRatio(tTJSVariant *, tjs_int n, tTJSVariant **p, ShioriLayerEmotePlayer *self) {
        const double v[4] = {number(p, n, 1, 1), number(p, n, 2), number(p, n, 3), number(p, n, 4)};
        self->command("blend", text(p, n, 0), v, 4);
        return TJS_S_OK;
    }
    static tjs_error fadeInTimeline(tTJSVariant *, tjs_int n, tTJSVariant **p, ShioriLayerEmotePlayer *self) {
        const double play[1] = {number(p, n, 3)};
        self->command("play", text(p, n, 0), play, 1);
        const double zero[4] = {0, 0, 0, 0};
        self->command("blend", text(p, n, 0), zero, 4);
        const double v[4] = {1, number(p, n, 1), number(p, n, 2), 0};
        self->command("blend", text(p, n, 0), v, 4);
        return TJS_S_OK;
    }
    static tjs_error fadeOutTimeline(tTJSVariant *, tjs_int n, tTJSVariant **p, ShioriLayerEmotePlayer *self) {
        const double v[2] = {number(p, n, 1), number(p, n, 2)};
        self->command("fadeout", text(p, n, 0), v, 2);
        return TJS_S_OK;
    }
    // [%[label, flags, blendRatio], …]
    tTJSVariant getPlayingTimelineInfoList() {
        iTJSDispatch2 *array = TJSCreateArrayObject();
        const auto playing = labels("playing");
        for (size_t i = 0; i < playing.size(); ++i) {
            iTJSDispatch2 *info = nullptr;
            tTJSVariant value = dictionary(info);
            setMember(info, "label", ttstr(playing[i].c_str()));
            const double index = double(i);
            setMember(info, "flags", tjs_int(hostNumber(player(), "flags", "", &index, 1)));
            setMember(info, "blendRatio", hostNumber(player(), "blend", playing[i]));
            array->PropSetByNum(TJS_MEMBERENSURE, tjs_int(i), &value, array);
        }
        tTJSVariant result(array, array);
        array->Release();
        return result;
    }
    tTJSVariant getVariableFrameList(ttstr) { return stringArray({}); }

    bool getAnimating() { return ensure() && hostNumber(player(), "animating") != 0; }
    void setAnimating(bool) {}
    bool getPlaying() { return getAnimating(); }
    void setPlaying(bool) {}
    bool getAllplaying() { return getAnimating(); }
    void setAllplaying(bool) {}
    tjs_real getLoopTime() { return -1; }
    void setLoopTime(tjs_real) {}
    tjs_real getTickCount() { return 0; }
    void setTickCount(tjs_real) {}
    tjs_real getSpeed() { return 1; }
    void setSpeed(tjs_real) {}
    tTJSVariant getOutline() { return tTJSVariant(); }
    void setOutline(tTJSVariant) {}
    ttstr getMotion() { return ttstr(motion_.c_str()); }
    void setMotion(ttstr m) { motion_ = utf8(m); }
    ttstr getChara() { return ttstr(chara_.c_str()); }
    void setChara(ttstr c) {
        if (utf8(c) != chara_) player_.reset();
        chara_ = utf8(c);
    }
    tTJSVariant getVariableKeys() {
        std::vector<std::string> names;
        if (ensure())
            for (int i = 0, n = int(hostNumber(player(), "count", "variables")); i < n; ++i)
                names.push_back(hostText(player(), "label", "variables", i));
        return stringArray(names);
    }
    tjs_int getMaskMode() { return maskMode_; }
    void setMaskMode(tjs_int v) { maskMode_ = v; }
    tjs_int getCompletionType() { return completionType_; }
    void setCompletionType(tjs_int v) { completionType_ = v; }
    tjs_real getMeshDivisionRatio() { return meshDivision_; }
    void setMeshDivisionRatio(tjs_real v) { meshDivision_ = v; }
    tjs_real getBustScale() { return bust_; }
    void setBustScale(tjs_real v) { bust_ = v; }
    tjs_real getHairScale() { return hair_; }
    void setHairScale(tjs_real v) { hair_ = v; }
    tjs_real getPartsScale() { return parts_; }
    void setPartsScale(tjs_real v) { parts_ = v; }
    tjs_real getColorWeight() { return colorWeight_; }
    void setColorWeight(tjs_real v) { colorWeight_ = v; }

private:
    void *player() const { return player_ ? player_->player : nullptr; }

    bool ensure() {
        if (player_) return true;
        if (!resources_ || !host() || !host()->clone) return false;
        auto prototype = resources_->prototype(chara_);
        if (!prototype) return false;
        player_ = std::make_shared<HostPlayer>(host()->clone(host()->context, prototype->player));
        return player_->player != nullptr;
    }

    void command(const char *verb, const std::string &label = {}, const double *n = nullptr, int32_t c = 0) {
        if (ensure()) hostCommand(player(), verb, label, n, c);
    }

    std::vector<std::string> labels(const char *list) {
        std::vector<std::string> out;
        if (!ensure()) return out;
        for (int i = 0, n = int(hostNumber(player(), "count", list)); i < n; ++i) out.push_back(hostText(player(), "label", list, i));
        return out;
    }

    ShioriMotionResources *resources_ = nullptr;
    std::shared_ptr<HostPlayer> player_;
    std::string chara_, motion_;
    double affine_[6] = {1, 0, 0, 1, 0, 0};
    tjs_int maskMode_ = 1, completionType_ = 0;
    tjs_real meshDivision_ = 1, bust_ = 1, hair_ = 1, parts_ = 1, colorWeight_ = 1;
};

#define SHIORI_LAYER_EMOTE_METHODS \
    NCB_CONSTRUCTOR((ShioriMotionResources *)); \
    NCB_METHOD(play); NCB_METHOD(initPhysics); NCB_METHOD(serialize); NCB_METHOD(unserialize); \
    NCB_METHOD(clear); NCB_METHOD(progress); NCB_METHOD(draw); \
    RawCallback("setCoord", &Class::setCoord, 0); RawCallback("setRotate", &Class::setRotate, 0); \
    RawCallback("setScale", &Class::setScale, 0); RawCallback("setColor", &Class::setColor, 0); \
    RawCallback("setVariable", &Class::setVariable, 0); RawCallback("getVariable", &Class::getVariable, 0); \
    NCB_METHOD(setOuterForce); NCB_METHOD(setDrawAffineTranslateMatrix); NCB_METHOD(setCameraOffset); \
    RawCallback("startWind", &Class::startWind, 0); NCB_METHOD(stopWind); \
    RawCallback("contains", &Class::contains, 0); RawCallback("hitTest", &Class::contains, 0); \
    NCB_METHOD(skip); NCB_METHOD(skipToSync); NCB_METHOD(pass); NCB_METHOD(stop); \
    RawCallback("playTimeline", &Class::playTimeline, 0); RawCallback("stopTimeline", &Class::stopTimeline, 0); \
    NCB_METHOD(getTimelinePlaying); NCB_METHOD(getLoopTimeline); NCB_METHOD(getTimelineTotalFrameCount); \
    NCB_METHOD(getMainTimelineLabelList); NCB_METHOD(getDiffTimelineLabelList); \
    RawCallback("setTimelineBlendRatio", &Class::setTimelineBlendRatio, 0); \
    RawCallback("fadeInTimeline", &Class::fadeInTimeline, 0); RawCallback("fadeOutTimeline", &Class::fadeOutTimeline, 0); \
    NCB_METHOD(getPlayingTimelineInfoList); NCB_METHOD(getVariableFrameList); \
    NCB_PROPERTY(animating, getAnimating, setAnimating); NCB_PROPERTY(playing, getPlaying, setPlaying); \
    NCB_PROPERTY(allplaying, getAllplaying, setAllplaying); NCB_PROPERTY(loopTime, getLoopTime, setLoopTime); \
    NCB_PROPERTY(tickCount, getTickCount, setTickCount); NCB_PROPERTY(speed, getSpeed, setSpeed); \
    NCB_PROPERTY(outline, getOutline, setOutline); NCB_PROPERTY(motion, getMotion, setMotion); \
    NCB_PROPERTY(chara, getChara, setChara); NCB_PROPERTY_RO(variableKeys, getVariableKeys); \
    NCB_PROPERTY(maskMode, getMaskMode, setMaskMode); NCB_PROPERTY(completionType, getCompletionType, setCompletionType); \
    NCB_PROPERTY(meshDivisionRatio, getMeshDivisionRatio, setMeshDivisionRatio); \
    NCB_PROPERTY(bustScale, getBustScale, setBustScale); NCB_PROPERTY(hairScale, getHairScale, setHairScale); \
    NCB_PROPERTY(partsScale, getPartsScale, setPartsScale); NCB_PROPERTY(colorWeight, getColorWeight, setColorWeight)

NCB_REGISTER_SUBCLASS(ShioriLayerEmotePlayer) { SHIORI_LAYER_EMOTE_METHODS; }

// Motion.Player: plain motions (.mtn). Idle for now; scripts go on.
class ShioriLayerMotionPlayer : public ShioriLayerEmotePlayer {
public:
    explicit ShioriLayerMotionPlayer(ShioriMotionResources *) : ShioriLayerEmotePlayer(nullptr) {}
    tTJSVariant getOnAction() { return onAction_; }
    void setOnAction(tTJSVariant v) { onAction_ = v; }
    tTJSVariant getTags() { return stringArray({}); }
    void setTags(tTJSVariant) {}
    tTJSVariant getCommandList() { return stringArray({}); }
    void setFlip(tjs_real, tjs_real, tjs_real) {}
    void setSlant(tjs_real, tjs_real, tjs_real) {}
    void setZoom(tjs_real, tjs_real, tjs_real) {}
    static bool getUseD3D() { return false; }

private:
    tTJSVariant onAction_;
};

NCB_REGISTER_SUBCLASS(ShioriLayerMotionPlayer) {
    SHIORI_LAYER_EMOTE_METHODS;
    NCB_PROPERTY(onAction, getOnAction, setOnAction);
    NCB_PROPERTY(tags, getTags, setTags);
    NCB_METHOD(getCommandList);
    NCB_METHOD(setFlip);
    NCB_METHOD(setSlant);
    NCB_METHOD(setZoom);
    NCB_PROPERTY_RO(useD3D, getUseD3D);
}

// Motion.D3DAdaptor: a canvas players draw into, copied to a layer by captureCanvas. Shiori
// draws straight into layers, so this keeps the last layer it was given.
void ShioriD3DAdaptor::captureCanvas(iTJSDispatch2 *target) {
    tTJSNI_BaseLayer *layer = layerOf(target);
    if (!layer || !width_ || !height_) return;
    if (int(layer->GetImageWidth()) != width_ || int(layer->GetImageHeight()) != height_)
        layer->SetImageSize(width_, height_);
    if (texture_) {
        // Canvas to layer on the GPU, then the canvas cleared for the next frame.
        iTVPTexture2D *out = layerTexture(layer);
        if (!out) return;
        iTVPRenderManager *manager = TVPGetRenderManager();
        static iTVPRenderMethod *copy = manager->GetRenderMethod("Copy");
        const tTVPRect all(0, 0, width_, height_);
        tRenderTexRectArray::Element from[] = {tRenderTexRectArray::Element(texture_, all)};
        manager->OperateRect(copy, out, nullptr, all, tRenderTexRectArray(from));
        layer->Update();
        clearCanvas();
        return;
    }
    auto *out = static_cast<uint8_t *>(layer->GetMainImagePixelBufferForWrite());
    if (!out) return;
    const tjs_int pitch = layer->GetMainImagePixelBufferPitch();
    for (int y = 0; y < height_; ++y) memcpy(out + size_t(y) * pitch, &pixels_[size_t(y) * width_ * 4], size_t(width_) * 4);
    layer->Update();
    clearCanvas();
}

NCB_REGISTER_SUBCLASS(ShioriD3DAdaptor) {
    NCB_CONSTRUCTOR((iTJSDispatch2 *, tjs_int, tjs_int, tjs_int, tjs_int));
    NCB_METHOD(setClearColor);
    NCB_METHOD(captureCanvas);
    NCB_METHOD(unloadUnusedTextures);
}

struct ShioriMotion {
    static bool getD3DAvailable() { return true; }
    static bool enableD3D;
    static bool getEnableD3D() { return enableD3D; }
    static void setEnableD3D(bool v) { enableD3D = v; }
};
bool ShioriMotion::enableD3D = true;

NCB_REGISTER_CLASS_DIFFER(Motion, ShioriMotion) {
    NCB_METHOD(getD3DAvailable);
    NCB_PROPERTY(enableD3D, getEnableD3D, setEnableD3D);
    NCB_SUBCLASS(ResourceManager, ShioriMotionResources);
    NCB_SUBCLASS(Player, ShioriLayerMotionPlayer);
    NCB_SUBCLASS(EmotePlayer, ShioriLayerEmotePlayer);
    NCB_SUBCLASS(SeparateLayerAdaptor, ShioriSeparateLayerAdaptor);
    NCB_SUBCLASS(D3DAdaptor, ShioriD3DAdaptor);
    Variant("MaskModeStencil", (tjs_int)0);
    Variant("MaskModeAlpha", (tjs_int)1);
    Variant("PlayFlagForce", (tjs_int)1);
    Variant("PlayFlagChain", (tjs_int)2);
    Variant("PlayFlagAsCan", (tjs_int)4);
    Variant("PlayFlagJoin", (tjs_int)8);
    Variant("PlayFlagStealth", (tjs_int)16);
    Variant("ShapeTypePoint", (tjs_int)0);
    Variant("ShapeTypeCircle", (tjs_int)1);
    Variant("ShapeTypeRect", (tjs_int)2);
    Variant("ShapeTypeQuad", (tjs_int)3);
    Variant("TimelinePlayFlagParallel", (tjs_int)1);
    Variant("TimelinePlayFlagDifference", (tjs_int)2);
}
