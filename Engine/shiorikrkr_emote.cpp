/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

// E-mote characters for ShioriKrkr. The games' scripts (NEKOPARA's D3DAffineSourceEmote.tjs)
// drive a D3DEmotePlayer per character layer: load the character's PSB files, set its
// variables and timelines, and call progress() each frame. This is Shiori's own class under
// that name: it keeps the script-facing surface and hands the models, the commands and the
// drawing to the host's E-mote player (shiorikrkr_emote_host_t; in the app, Shiori's Swift
// renderer from CatSystemKit). krkrsdl3's own emoteplayer plug-in isn't built (its lineage
// is unaudited, PROVENANCE.md). The drawn character goes into the D3D layer's E-mote target,
// which krkrsdl3's DrawDeviceD3D composes into the frame.
#include "tjsCommHead.h"
#include "ncbind/ncbind.hpp"
#include "TVPStorage.h"
#include "TVPDebug.h"
#include "DrawDeviceD3D/DrawDeviceD3D.h"

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "shiorikrkr_emote.h"

#define NCB_MODULE_NAME TJS_N("drawdeviceD3D.dll")

namespace shiorikrkr {

namespace {
shiorikrkr_emote_host_t g_host{};
bool g_hasHost = false;
} // namespace

void setEmoteHost(const shiorikrkr_emote_host_t *host) {
    g_hasHost = host && host->struct_size >= sizeof(shiorikrkr_emote_host_t) && host->create && host->command &&
                host->query && host->draw && host->destroy;
    if (g_hasHost) g_host = *host;
    else g_host = {};
}

const shiorikrkr_emote_host_t *emoteHost() { return g_hasHost ? &g_host : nullptr; }

} // namespace shiorikrkr

namespace {

std::string utf8(const ttstr &s) { return s.AsStdString(); }

// The storage's bytes: by its name as the game gives it (found through the auto paths).
bool readStorage(const ttstr &name, std::vector<uint8_t> &out, std::string &error) {
    ttstr placed = TVPGetPlacedPath(name);
    if (placed.IsEmpty()) {
        error = "not found: " + utf8(name);
        return false;
    }
    tTJSBinaryStream *stream = nullptr;
    try {
        stream = TVPCreateStream(placed, TJS_BS_READ);
    } catch (...) {
        stream = nullptr;
    }
    if (!stream) {
        error = "can't open: " + utf8(name);
        return false;
    }
    const tjs_uint64 size = stream->GetSize();
    out.resize(size_t(size));
    size_t done = 0;
    while (done < out.size()) {
        const tjs_uint chunk = tjs_uint(std::min<size_t>(out.size() - done, 1 << 24));
        const tjs_uint read = stream->Read(out.data() + done, chunk);
        if (read == 0) break;
        done += read;
    }
    delete stream;
    if (done != out.size()) {
        error = "short read: " + utf8(name);
        return false;
    }
    return true;
}

double number(tTJSVariant **param, tjs_int count, tjs_int index, double fallback = 0) {
    if (index >= count || !param[index] || param[index]->Type() == tvtVoid) return fallback;
    return param[index]->AsReal();
}

std::string text(tTJSVariant **param, tjs_int count, tjs_int index) {
    if (index >= count || !param[index] || param[index]->Type() == tvtVoid) return {};
    return utf8(ttstr(*param[index]));
}

} // namespace

// One character on a D3D layer.
class ShioriEmotePlayer {
public:
    explicit ShioriEmotePlayer(iTJSDispatch2 *d3dlayer) {
        if (d3dlayer) layer_ = ncbInstanceAdaptor<drawdevice::D3DLayer>::GetNativeInstance(d3dlayer);
    }

    ~ShioriEmotePlayer() { release(); }

    // load(storage, …): the character's PSB files (one character may be split over several).
    static tjs_error load(tTJSVariant *result, tjs_int count, tTJSVariant **param, ShioriEmotePlayer *self) {
        if (!self) return TJS_E_NATIVECLASSCRASH;
        self->release();
        const auto *host = shiorikrkr::emoteHost();
        std::vector<std::vector<uint8_t>> files;
        std::vector<std::string> names;
        for (tjs_int i = 0; i < count; ++i) {
            if (!param[i] || param[i]->Type() == tvtVoid) continue;
            ttstr name(*param[i]);
            std::vector<uint8_t> bytes;
            std::string error;
            if (!readStorage(name, bytes, error)) {
                TVPAddLog(ttstr("ShioriKrkr E-mote: ") + ttstr(error.c_str()));
                continue;
            }
            files.push_back(std::move(bytes));
            names.push_back(utf8(name));
        }
        if (!host) {
            TVPAddLog(ttstr("ShioriKrkr E-mote: no host player; the character is not drawn"));
            return TJS_S_OK;
        }
        if (files.empty()) return TJS_S_OK;
        std::vector<const uint8_t *> data;
        std::vector<uint64_t> sizes;
        std::vector<const char *> cnames;
        for (size_t i = 0; i < files.size(); ++i) {
            data.push_back(files[i].data());
            sizes.push_back(files[i].size());
            cnames.push_back(names[i].c_str());
        }
        char error[512] = {};
        self->player_ = host->create(host->context, data.data(), sizes.data(), cnames.data(), int32_t(files.size()),
                                     error, sizeof(error));
        if (!self->player_) {
            TVPAddLog(ttstr("ShioriKrkr E-mote: ") + ttstr(names.front().c_str()) + ttstr(": ") + ttstr(error));
            return TJS_S_OK;
        }
        self->animating_ = true;
        TVPAddLog(ttstr("ShioriKrkr E-mote: ") + ttstr(names.front().c_str()) + ttstr(" loaded"));
        return TJS_S_OK;
    }

    void show() {
        shown_ = true;
        command("show");
    }
    void hide() {
        shown_ = false;
        command("hide");
    }

    tTJSVariant clone(iTJSDispatch2 *newlayer) {
        auto *copy = new ShioriEmotePlayer(newlayer);
        const auto *host = shiorikrkr::emoteHost();
        if (host && player_ && host->clone) copy->player_ = host->clone(host->context, player_);
        copy->shown_ = shown_;
        copy->smoothing_ = smoothing_;
        copy->meshDivision_ = meshDivision_;
        copy->bust_ = bust_;
        copy->hair_ = hair_;
        copy->parts_ = parts_;
        copy->animating_ = animating_;
        iTJSDispatch2 *object = ncbInstanceAdaptor<ShioriEmotePlayer>::CreateAdaptor(copy, false, true);
        if (!object) {
            delete copy;
            return tTJSVariant();
        }
        tTJSVariant value(object, object);
        object->Release();
        return value;
    }

    void skip() { command("skip"); }
    void pass() { command("pass"); }

    // progress(frames at 60 a second): the player moves on, then draws into the layer.
    void progress(tjs_real frames) {
        if (!player_) return;
        const double controls[4] = {meshDivision_, bust_, hair_, parts_};
        command("controls", "", controls, 4);
        const double f = frames;
        command("progress", "", &f, 1);
        animating_ = query("animating") != 0;
        draw();
    }

    static tjs_error setCoord(tTJSVariant *, tjs_int n, tTJSVariant **p, ShioriEmotePlayer *self) {
        const double v[4] = {number(p, n, 0), number(p, n, 1), number(p, n, 2), number(p, n, 3)};
        self->command("coord", "", v, 4);
        return TJS_S_OK;
    }
    static tjs_error setRot(tTJSVariant *, tjs_int n, tTJSVariant **p, ShioriEmotePlayer *self) {
        const double v[3] = {number(p, n, 0), number(p, n, 1), number(p, n, 2)};
        self->command("rot", "", v, 3);
        return TJS_S_OK;
    }
    static tjs_error setScale(tTJSVariant *, tjs_int n, tTJSVariant **p, ShioriEmotePlayer *self) {
        const double v[3] = {number(p, n, 0, 1), number(p, n, 1), number(p, n, 2)};
        self->command("scale", "", v, 3);
        return TJS_S_OK;
    }
    static tjs_error setColor(tTJSVariant *, tjs_int n, tTJSVariant **p, ShioriEmotePlayer *self) {
        // 0xRRGGBBAA, as the scripts pack it.
        const double v[3] = {double(tjs_uint32(tjs_int64(number(p, n, 0, 0xFFFFFFFF)))), number(p, n, 1), number(p, n, 2)};
        self->command("color", "", v, 3);
        return TJS_S_OK;
    }
    static tjs_error setVariable(tTJSVariant *, tjs_int n, tTJSVariant **p, ShioriEmotePlayer *self) {
        const double v[3] = {number(p, n, 1), number(p, n, 2), number(p, n, 3)};
        self->command("variable", text(p, n, 0), v, 3);
        return TJS_S_OK;
    }
    static tjs_error getVariable(tTJSVariant *r, tjs_int n, tTJSVariant **p, ShioriEmotePlayer *self) {
        if (r) *r = self->query("variable", text(p, n, 0));
        return TJS_S_OK;
    }
    static tjs_error startWind(tTJSVariant *, tjs_int n, tTJSVariant **p, ShioriEmotePlayer *self) {
        const double v[5] = {number(p, n, 0), number(p, n, 1), number(p, n, 2), number(p, n, 3), number(p, n, 4)};
        self->command("wind", "", v, 5);
        return TJS_S_OK;
    }
    void stopWind() { command("windstop"); }

    // contains(label, x, y) / contains(x, y) with x, y in the layer's own pixels; hitTest
    // takes canvas pixels.
    static tjs_error contains(tTJSVariant *r, tjs_int n, tTJSVariant **p, ShioriEmotePlayer *self) {
        std::string label;
        int first = 0;
        if (n >= 3) {
            label = text(p, n, 0);
            first = 1;
        }
        const double v[2] = {number(p, n, first), number(p, n, first + 1)};
        if (r) *r = self->query("contains", label, v, 2) != 0;
        return TJS_S_OK;
    }
    static tjs_error hitTest(tTJSVariant *r, tjs_int n, tTJSVariant **p, ShioriEmotePlayer *self) {
        return contains(r, n, p, self);
    }

    static tjs_error playTimeline(tTJSVariant *, tjs_int n, tTJSVariant **p, ShioriEmotePlayer *self) {
        const double v[1] = {number(p, n, 1)};
        self->command("play", text(p, n, 0), v, 1);
        self->animating_ = true;
        return TJS_S_OK;
    }
    static tjs_error stopTimeline(tTJSVariant *, tjs_int n, tTJSVariant **p, ShioriEmotePlayer *self) {
        self->command("stop", text(p, n, 0));
        return TJS_S_OK;
    }
    static tjs_error fadeOutTimeline(tTJSVariant *, tjs_int n, tTJSVariant **p, ShioriEmotePlayer *self) {
        const double v[2] = {number(p, n, 1), number(p, n, 2)};
        self->command("fadeout", text(p, n, 0), v, 2);
        return TJS_S_OK;
    }
    static tjs_error setTimelineBlendRatio(tTJSVariant *, tjs_int n, tTJSVariant **p, ShioriEmotePlayer *self) {
        const double v[4] = {number(p, n, 1, 1), number(p, n, 2), number(p, n, 3), number(p, n, 4)};
        self->command("blend", text(p, n, 0), v, 4);
        return TJS_S_OK;
    }
    static tjs_error isTimelinePlaying(tTJSVariant *r, tjs_int n, tTJSVariant **p, ShioriEmotePlayer *self) {
        if (r) *r = self->query("playing", text(p, n, 0)) != 0;
        return TJS_S_OK;
    }
    static tjs_error getTimelineBlendRatio(tTJSVariant *r, tjs_int n, tTJSVariant **p, ShioriEmotePlayer *self) {
        if (r) *r = self->query("blend", text(p, n, 0));
        return TJS_S_OK;
    }
    tjs_int countMainTimelines() { return tjs_int(query("count", "main")); }
    tTJSString getMainTimelineLabelAt(tjs_int i) { return labelAt("main", i); }
    tjs_int countDiffTimelines() { return tjs_int(query("count", "diff")); }
    tTJSString getDiffTimelineLabelAt(tjs_int i) { return labelAt("diff", i); }
    tjs_int countPlayingTimelines() { return tjs_int(query("count", "playing")); }
    tTJSString getPlayingTimelineLabelAt(tjs_int i) { return labelAt("playing", i); }
    tjs_int getPlayingTimelineFlagsAt(tjs_int i) {
        const double v = i;
        return tjs_int(query("flags", "", &v, 1));
    }
    void finalize() {}

    bool getAnimating() { return animating_; }
    void setAnimating(bool v) { animating_ = v; }
    bool getSmoothing() { return smoothing_; }
    void setSmoothing(bool v) { smoothing_ = v; }
    tjs_real getMeshDivisionRatio() { return meshDivision_; }
    void setMeshDivisionRatio(tjs_real v) { meshDivision_ = v; }
    tjs_real getBustScale() { return bust_; }
    void setBustScale(tjs_real v) { bust_ = v; }
    tjs_real getHairScale() { return hair_; }
    void setHairScale(tjs_real v) { hair_ = v; }
    tjs_real getPartsScale() { return parts_; }
    void setPartsScale(tjs_real v) { parts_ = v; }
    bool getVisible() { return shown_; }
    void setVisible(bool v) { v ? show() : hide(); }

private:
    void release() {
        const auto *host = shiorikrkr::emoteHost();
        if (player_ && host) host->destroy(host->context, player_);
        player_ = nullptr;
    }

    void command(const char *verb, const std::string &label = {}, const double *numbers = nullptr, int32_t count = 0) {
        const auto *host = shiorikrkr::emoteHost();
        if (!player_ || !host) return;
        host->command(host->context, player_, verb, label.c_str(), numbers, count);
    }

    double query(const char *verb, const std::string &label = {}, const double *numbers = nullptr, int32_t count = 0) {
        const auto *host = shiorikrkr::emoteHost();
        if (!player_ || !host) return 0;
        char out[4] = {};
        return host->query(host->context, player_, verb, label.c_str(), numbers, count, out, sizeof(out));
    }

    tTJSString labelAt(const char *list, tjs_int index) {
        const auto *host = shiorikrkr::emoteHost();
        if (!player_ || !host) return tTJSString();
        char out[512] = {};
        const double v = index;
        host->query(host->context, player_, "label", list, &v, 1, out, sizeof(out));
        return tTJSString(out);
    }

    // Into the layer's E-mote target (the canvas's size), through the layer's matrix about
    // the canvas's centre: the character's own coordinates are centred on the canvas.
    void draw() {
        const auto *host = shiorikrkr::emoteHost();
        if (!layer_ || !layer_->Device || !layer_->Device->GetBackend() || !host) return;
        auto *backend = layer_->Device->GetBackend();
        const int width = layer_->Device->GetWidth(), height = layer_->Device->GetHeight();
        if (width <= 0 || height <= 0) return;
        if (!layer_->EmoteTarget) layer_->EmoteTarget = backend->CreateTarget(width, height);
        if (!layer_->EmoteTarget) return;
        int pitch = 0;
        uint8_t *pixels = backend->LockTarget(layer_->EmoteTarget, pitch);
        if (!pixels) return;
        // Clear what the last frame drew (all of it when that isn't known).
        int *rect = layer_->EmoteRect;
        if (rect[0] < 0) {
            std::memset(pixels, 0, size_t(pitch) * height);
        } else if (rect[2] > rect[0] && rect[3] > rect[1]) {
            for (int y = std::max(0, rect[1]); y < std::min(height, rect[3]); ++y)
                std::memset(pixels + size_t(y) * pitch + size_t(std::max(0, rect[0])) * 4, 0,
                            size_t(std::min(width, rect[2]) - std::max(0, rect[0])) * 4);
        }
        int32_t bounds[4] = {0, 0, 0, 0};
        int32_t drew = 0;
        // The layer's 4x4 (row vectors, as Direct3D): x' = m11 x + m21 y + m41.
        const float *m = layer_->Matrix;
        const double transform[6] = {m[0], m[1], m[4], m[5], m[12] + width * 0.5, m[13] + height * 0.5};
        if (shown_ && player_) drew = host->draw(host->context, player_, pixels, width, height, pitch, transform, bounds);
        if (drew) {
            rect[0] = std::clamp<int>(bounds[0], 0, width);
            rect[1] = std::clamp<int>(bounds[1], 0, height);
            rect[2] = std::clamp<int>(bounds[2], 0, width);
            rect[3] = std::clamp<int>(bounds[3], 0, height);
        } else {
            rect[0] = rect[1] = rect[2] = rect[3] = 0;
        }
        static const bool trace = getenv("SHIORIKRKR_TRACE_EMOTE") != nullptr;
        if (trace && (++traceCount_ % 120) == 1)
            fprintf(stderr, "emote %p shown=%d drew=%d rect=%d,%d,%d,%d plane=%d front=%d matrix=%.2f,%.2f,%.2f,%.2f,%.1f,%.1f\n",
                    (void *)this, shown_, drew, rect[0], rect[1], rect[2], rect[3], layer_->drawPlane, layer_->frontIndex,
                    transform[0], transform[1], transform[2], transform[3], transform[4], transform[5]);
        backend->UnlockTarget(layer_->EmoteTarget);
    }


    drawdevice::D3DLayer *layer_ = nullptr;
    void *player_ = nullptr;
    bool shown_ = false;
    bool smoothing_ = true;
    bool animating_ = false;
    tjs_real meshDivision_ = 1, bust_ = 1, hair_ = 1, parts_ = 1;
    unsigned traceCount_ = 0;
};

NCB_REGISTER_CLASS_DIFFER(D3DEmotePlayer, ShioriEmotePlayer) {
    NCB_CONSTRUCTOR((iTJSDispatch2 *));
    RawCallback("load", &Class::load, 0);
    NCB_METHOD(show);
    NCB_METHOD(hide);
    NCB_METHOD(clone);
    NCB_METHOD(skip);
    NCB_METHOD(pass);
    NCB_METHOD(progress);
    RawCallback("setCoord", &Class::setCoord, 0);
    RawCallback("setRot", &Class::setRot, 0);
    RawCallback("setScale", &Class::setScale, 0);
    RawCallback("setColor", &Class::setColor, 0);
    RawCallback("setVariable", &Class::setVariable, 0);
    RawCallback("getVariable", &Class::getVariable, 0);
    RawCallback("startWind", &Class::startWind, 0);
    NCB_METHOD(stopWind);
    RawCallback("contains", &Class::contains, 0);
    RawCallback("hitTest", &Class::hitTest, 0);
    RawCallback("playTimeline", &Class::playTimeline, 0);
    RawCallback("stopTimeline", &Class::stopTimeline, 0);
    RawCallback("fadeOutTimeline", &Class::fadeOutTimeline, 0);
    RawCallback("setTimelineBlendRatio", &Class::setTimelineBlendRatio, 0);
    RawCallback("isTimelinePlaying", &Class::isTimelinePlaying, 0);
    RawCallback("getTimelineBlendRatio", &Class::getTimelineBlendRatio, 0);
    NCB_METHOD(countMainTimelines);
    NCB_METHOD(getMainTimelineLabelAt);
    NCB_METHOD(countDiffTimelines);
    NCB_METHOD(getDiffTimelineLabelAt);
    NCB_METHOD(countPlayingTimelines);
    NCB_METHOD(getPlayingTimelineLabelAt);
    NCB_METHOD(getPlayingTimelineFlagsAt);
    NCB_METHOD(finalize);
    NCB_PROPERTY(animating, getAnimating, setAnimating);
    NCB_PROPERTY(smoothing, getSmoothing, setSmoothing);
    NCB_PROPERTY(meshDivisionRatio, getMeshDivisionRatio, setMeshDivisionRatio);
    NCB_PROPERTY(bustScale, getBustScale, setBustScale);
    NCB_PROPERTY(hairScale, getHairScale, setHairScale);
    NCB_PROPERTY(partsScale, getPartsScale, setPartsScale);
    NCB_PROPERTY(visible, getVisible, setVisible);
}
