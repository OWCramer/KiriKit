/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

// wfTypicalDSP.dll and wfBasicEffect.dll: the wave filters KiriKiri games put on a
// WaveSoundBuffer's `filters` (Yuzusoft's voiceeffect.tjs: a voice heard over a phone, a radio,
// a PA system or through a door; an echo for a hall). Shiori's own filters on krkr2's filter
// chain (iTVPBasicWaveFilter, as the core's PhaseVocoder), processing float samples:
//
//   WaveSoundBuffer.WaveDSPFilter(type, design)       setParams(...), getParamInfo()
//     types LowPass HighPass BandPass(1, 2) BandStop LowShelf HighShelf BandShelf AllPass
//     (OnePole/TwoPole pass through). RBJ designs are one biquad (RBJ's cookbook); every other
//     design (Butterworth, Chebyshev I/II, Elliptic, Bessel, Legendre) is built as Butterworth
//     of the order asked for: the same pass band, a slightly different knee.
//     setParams takes the design's parameters after the sample rate, in the order of the
//     DSP filter library the plug-in names its types after: RBJ (frequency, Q or bandwidth
//     in octaves, gain, slope); the others (order, frequency, bandwidth in Hz, gain).
//   WaveSoundBuffer.DelayEffect(delayMs, feedback, wet, tailMs)   an echo
//   WaveSoundBuffer.StkFreeVerb()   a Freeverb reverb (effectMix, roomSize, damping, width, mode)
//   WaveSoundBuffer.GraphicEqualizer()   ten bands an octave apart; setGain(band, dB), getGain
//
// Written from the class and member names the plug-ins register and how the games' scripts
// call them; filter maths from public formulas (RBJ's Audio EQ Cookbook; Butterworth pole
// angles; Jezar's public-domain Freeverb tunings). No plug-in code read.
#include "tjsCommHead.h"
#include "ScriptMgnIntf.h"
#include "WaveIntf.h"
#include "WaveLoopManager.h"
#include "WaveSegmentQueue.h"
#include <ncbind.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

// One biquad (direct form I coefficients, a0 = 1).
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
};

Biquad normalize(double b0, double b1, double b2, double a0, double a1, double a2) {
    return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

// RBJ's Audio EQ Cookbook.
Biquad rbjLowPass(double fs, double f, double q) {
    const double w = 2 * kPi * f / fs, c = std::cos(w), alpha = std::sin(w) / (2 * q);
    return normalize((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + alpha, -2 * c, 1 - alpha);
}
Biquad rbjHighPass(double fs, double f, double q) {
    const double w = 2 * kPi * f / fs, c = std::cos(w), alpha = std::sin(w) / (2 * q);
    return normalize((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + alpha, -2 * c, 1 - alpha);
}
double octaveAlpha(double w, double octaves) {
    return std::sin(w) * std::sinh(std::log(2.0) / 2 * octaves * w / std::sin(w));
}
Biquad rbjBandPass(double fs, double f, double octaves, bool peakGain) {
    const double w = 2 * kPi * f / fs, c = std::cos(w), alpha = octaveAlpha(w, octaves), s = std::sin(w);
    if (peakGain) return normalize(s / 2, 0, -s / 2, 1 + alpha, -2 * c, 1 - alpha);
    return normalize(alpha, 0, -alpha, 1 + alpha, -2 * c, 1 - alpha);
}
Biquad rbjBandStop(double fs, double f, double octaves) {
    const double w = 2 * kPi * f / fs, c = std::cos(w), alpha = octaveAlpha(w, octaves);
    return normalize(1, -2 * c, 1, 1 + alpha, -2 * c, 1 - alpha);
}
Biquad rbjAllPass(double fs, double f, double q) {
    const double w = 2 * kPi * f / fs, c = std::cos(w), alpha = std::sin(w) / (2 * q);
    return normalize(1 - alpha, -2 * c, 1 + alpha, 1 + alpha, -2 * c, 1 - alpha);
}
Biquad rbjPeak(double fs, double f, double gainDb, double q) {
    const double A = std::pow(10, gainDb / 40), w = 2 * kPi * f / fs, c = std::cos(w), alpha = std::sin(w) / (2 * q);
    return normalize(1 + alpha * A, -2 * c, 1 - alpha * A, 1 + alpha / A, -2 * c, 1 - alpha / A);
}
Biquad rbjShelf(double fs, double f, double gainDb, double slope, bool high) {
    const double A = std::pow(10, gainDb / 40), w = 2 * kPi * f / fs, c = std::cos(w);
    slope = slope > 0 ? slope : 1;
    const double alpha = std::sin(w) / 2 * std::sqrt((A + 1 / A) * (1 / slope - 1) + 2), sa = 2 * std::sqrt(A) * alpha;
    if (!high)
        return normalize(A * ((A + 1) - (A - 1) * c + sa), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - sa),
                         (A + 1) + (A - 1) * c + sa, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - sa);
    return normalize(A * ((A + 1) + (A - 1) * c + sa), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - sa),
                     (A + 1) - (A - 1) * c + sa, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - sa);
}
// A first-order section by the bilinear transform.
Biquad onePole(double fs, double f, bool high) {
    const double k = std::tan(kPi * f / fs);
    if (!high) return normalize(k, k, 0, k + 1, k - 1, 0);
    return normalize(1, -1, 0, k + 1, k - 1, 0);
}

// Butterworth of order n: biquads with the analog prototype's pole angles (and a first-order
// section for an odd order).
void butterworth(std::vector<Biquad> &out, double fs, double f, int order, bool high) {
    order = std::max(1, std::min(order, 50));
    for (int k = 0; k < order / 2; ++k) {
        const double theta = kPi * (2 * k + 1) / (2.0 * order);
        const double q = 1 / (2 * std::cos(kPi / 2 - theta));
        out.push_back(high ? rbjHighPass(fs, f, q) : rbjLowPass(fs, f, q));
    }
    if (order % 2) out.push_back(onePole(fs, f, high));
}

double clampFrequency(double f, double fs) { return std::max(10.0, std::min(f, fs * 0.49)); }

// The float filter chain stage the filters share: the source decoded (converted to float),
// processed in place a block at a time, an optional tail after the source runs dry.
class FloatStage : public iTVPBasicWaveFilter, public tTVPSampleAndLabelSource {
public:
    virtual ~FloatStage() = default;
    tjs_int64 interfacePointer() { return tjs_int64(reinterpret_cast<tjs_intptr_t>(static_cast<iTVPBasicWaveFilter *>(this))); }

    tTVPSampleAndLabelSource *Recreate(tTVPSampleAndLabelSource *source) override {
        Source = source;
        InputFormat = source->GetFormat();
        OutputFormat = InputFormat;
        OutputFormat.IsFloat = true;
        OutputFormat.BitsPerSample = 32;
        OutputFormat.BytesPerSample = 4;
        prepare(int(InputFormat.SamplesPerSec), int(InputFormat.Channels));
        TailLeft = -1;
        return this;
    }
    void Clear() override { Source = nullptr; }
    void Reset() override {
        prepare(int(InputFormat.SamplesPerSec), int(InputFormat.Channels));
        TailLeft = -1;
    }
    void Update() override { Dirty = true; }

    void Decode(void *dest, tjs_uint samples, tjs_uint &written, tTVPWaveSegmentQueue &segments) override {
        auto *out = static_cast<float *>(dest);
        const int ch = int(InputFormat.Channels);
        written = 0;
        if (!Source) return;
        if (TailLeft < 0) {
            if (InputFormat.IsFloat && InputFormat.BitsPerSample == 32) {
                Source->Decode(out, samples, written, segments);
            } else {
                Convert.resize(size_t(samples) * InputFormat.BytesPerSample * ch);
                Source->Decode(Convert.data(), samples, written, segments);
                TVPConvertPCMToFloat(out, Convert.data(), InputFormat, written);
            }
            if (written < samples) TailLeft = tailSamples(); // the source ran dry
        }
        tjs_uint tail = 0;
        if (TailLeft > 0 && written < samples) {
            tail = tjs_uint(std::min<tjs_int64>(TailLeft, samples - written));
            std::memset(out + size_t(written) * ch, 0, size_t(tail) * ch * sizeof(float));
            TailLeft -= tail;
            // The tail is output with no source behind it.
            segments.Enqueue(tTVPWaveSegment(LastPosition, 0, tail));
        }
        const tjs_uint total = written + tail;
        if (!segments.GetSegments().empty()) {
            const auto &last = segments.GetSegments().back();
            LastPosition = last.Start + last.Length;
        }
        if (total) {
            if (Dirty) {
                Dirty = false;
                refresh();
            }
            std::lock_guard<std::mutex> lock(Mutex);
            process(out, int(total), ch);
        }
        written = total;
    }
    const tTVPWaveFormat &GetFormat() const override { return OutputFormat; }

protected:
    virtual void prepare(int rate, int channels) = 0;
    virtual void process(float *samples, int count, int channels) = 0;
    virtual tjs_int64 tailSamples() { return 0; }
    virtual void refresh() {}

    std::mutex Mutex;
    bool Dirty = false;
    tTVPWaveFormat InputFormat{}, OutputFormat{};

private:
    tTVPSampleAndLabelSource *Source = nullptr;
    std::vector<char> Convert;
    tjs_int64 TailLeft = -1;
    tjs_int64 LastPosition = 0;
};

// A cascade of biquads, each channel with its own history.
struct Cascade {
    std::vector<Biquad> sections;
    std::vector<double> state; // per channel, per section: x1 x2 y1 y2
    double gain = 1;
    void reset(int channels) { state.assign(size_t(channels) * sections.size() * 4, 0.0); }
    void run(float *s, int count, int channels) {
        if (sections.empty() && gain == 1) return;
        if (state.size() != size_t(channels) * sections.size() * 4) reset(channels);
        for (int i = 0; i < count; ++i) {
            for (int c = 0; c < channels; ++c) {
                double x = s[size_t(i) * channels + c];
                for (size_t k = 0; k < sections.size(); ++k) {
                    double *h = &state[(size_t(c) * sections.size() + k) * 4];
                    const Biquad &q = sections[k];
                    const double y = q.b0 * x + q.b1 * h[0] + q.b2 * h[1] - q.a1 * h[2] - q.a2 * h[3];
                    h[1] = h[0]; h[0] = x; h[3] = h[2]; h[2] = y;
                    x = y;
                }
                s[size_t(i) * channels + c] = float(x * gain);
            }
        }
    }
};

//---------------------------------------------------------------------------------------------
// WaveDSPFilter
class ShioriWaveDSPFilter : public FloatStage {
public:
    ShioriWaveDSPFilter(const std::string &type, const std::string &design) : Type(type), Design(design) {}

    static tjs_error factory(ShioriWaveDSPFilter **result, tjs_int n, tTJSVariant **p, iTJSDispatch2 *) {
        const std::string type = n > 0 ? ttstr(*p[0]).AsStdString() : "LowPass";
        const std::string design = n > 1 ? ttstr(*p[1]).AsStdString() : "RBJ";
        *result = new ShioriWaveDSPFilter(type, design);
        return TJS_S_OK;
    }
    static tjs_error setParams(tTJSVariant *, tjs_int n, tTJSVariant **p, ShioriWaveDSPFilter *self) {
        std::lock_guard<std::mutex> lock(self->Mutex);
        self->Params.clear();
        for (tjs_int i = 0; i < n; ++i) self->Params.push_back(p[i]->Type() == tvtVoid ? 0.0 : double(*p[i]));
        self->Dirty = true;
        return TJS_S_OK;
    }
    // [%[label, name, defaultValue, currentValue], ...] in setParams' order.
    tTJSVariant getParamInfo() {
        iTJSDispatch2 *array = TJSCreateArrayObject();
        const auto names = paramNames();
        for (size_t i = 0; i < names.size(); ++i) {
            iTJSDispatch2 *dict = TJSCreateDictionaryObject();
            tTJSVariant v(ttstr(names[i].c_str()));
            dict->PropSet(TJS_MEMBERENSURE, TJS_W("label"), nullptr, &v, dict);
            dict->PropSet(TJS_MEMBERENSURE, TJS_W("name"), nullptr, &v, dict);
            tTJSVariant d(defaultParam(i));
            dict->PropSet(TJS_MEMBERENSURE, TJS_W("defaultValue"), nullptr, &d, dict);
            tTJSVariant c(i < Params.size() ? Params[i] : defaultParam(i));
            dict->PropSet(TJS_MEMBERENSURE, TJS_W("currentValue"), nullptr, &c, dict);
            tTJSVariant item(dict, dict);
            dict->Release();
            array->PropSetByNum(TJS_MEMBERENSURE, tjs_int(i), &item, array);
        }
        tTJSVariant result(array, array);
        array->Release();
        return result;
    }
    tjs_int64 getInterface() { return interfacePointer(); }

private:
    bool rbj() const { return Design == "RBJ"; }
    std::vector<std::string> paramNames() const {
        if (rbj()) {
            if (Type == "LowShelf" || Type == "HighShelf") return {"Frequency", "Gain", "Slope"};
            if (Type == "BandShelf") return {"Frequency", "Gain", "BandWidth"};
            if (Type.rfind("BandPass", 0) == 0 || Type == "BandStop") return {"Frequency", "BandWidth"};
            return {"Frequency", "Q"};
        }
        if (Type == "LowShelf" || Type == "HighShelf") return {"Order", "Frequency", "Gain"};
        if (Type == "BandShelf") return {"Order", "Frequency", "BandWidthHz", "Gain"};
        if (Type.rfind("BandPass", 0) == 0 || Type == "BandStop") return {"Order", "Frequency", "BandWidthHz"};
        return {"Order", "Frequency"};
    }
    double defaultParam(size_t i) const {
        const std::string n = i < paramNames().size() ? paramNames()[i] : "";
        if (n == "Order") return 2;
        if (n == "Frequency") return 1000;
        if (n == "BandWidthHz") return 1000;
        if (n == "BandWidth" || n == "Slope") return 1;
        if (n == "Q") return 0.7071;
        return 0;
    }
    double param(size_t i) const { return i < Params.size() ? Params[i] : defaultParam(i); }

    void prepare(int rate, int channels) override {
        Rate = rate > 0 ? rate : 44100;
        Channels = channels;
        design();
    }
    void refresh() override {
        std::lock_guard<std::mutex> lock(Mutex);
        design();
    }
    void design() {
        Filter.sections.clear();
        const double fs = Rate;
        if (rbj()) {
            const double f = clampFrequency(param(0), fs);
            if (Type == "LowPass") Filter.sections.push_back(rbjLowPass(fs, f, std::max(0.01, param(1))));
            else if (Type == "HighPass") Filter.sections.push_back(rbjHighPass(fs, f, std::max(0.01, param(1))));
            else if (Type == "BandPass1") Filter.sections.push_back(rbjBandPass(fs, f, std::max(0.01, param(1)), true));
            else if (Type.rfind("BandPass", 0) == 0) Filter.sections.push_back(rbjBandPass(fs, f, std::max(0.01, param(1)), false));
            else if (Type == "BandStop") Filter.sections.push_back(rbjBandStop(fs, f, std::max(0.01, param(1))));
            else if (Type == "LowShelf") Filter.sections.push_back(rbjShelf(fs, f, param(1), param(2), false));
            else if (Type == "HighShelf") Filter.sections.push_back(rbjShelf(fs, f, param(1), param(2), true));
            else if (Type == "BandShelf") Filter.sections.push_back(rbjPeak(fs, f, param(1), 1 / std::max(0.01, param(2))));
            else if (Type == "AllPass") Filter.sections.push_back(rbjAllPass(fs, f, std::max(0.01, param(1))));
        } else {
            const int order = int(std::lround(param(0)));
            const double f = clampFrequency(param(1), fs);
            if (Type == "LowPass") butterworth(Filter.sections, fs, f, order, false);
            else if (Type == "HighPass") butterworth(Filter.sections, fs, f, order, true);
            else if (Type.rfind("BandPass", 0) == 0 || Type == "BandStop" || Type == "BandShelf") {
                // The band's edges about its centre (geometric where it fits).
                const double bw = std::max(1.0, param(2));
                const double low = clampFrequency(std::sqrt(f * f + bw * bw / 4) - bw / 2, fs);
                const double high = clampFrequency(low + bw, fs);
                if (Type == "BandShelf") {
                    Filter.sections.push_back(rbjPeak(fs, f, param(3), f / bw));
                } else if (Type == "BandStop") {
                    // Notch of the band's width, as deep as the order asks.
                    for (int k = 0; k < std::max(1, order / 2); ++k) Filter.sections.push_back(rbjBandStop(fs, f, std::log2(high / low)));
                } else {
                    butterworth(Filter.sections, fs, low, order, true);
                    butterworth(Filter.sections, fs, high, order, false);
                }
            } else if (Type == "LowShelf") Filter.sections.push_back(rbjShelf(fs, f, param(2), 1, false));
            else if (Type == "HighShelf") Filter.sections.push_back(rbjShelf(fs, f, param(2), 1, true));
        }
        Filter.reset(Channels);
    }
    void process(float *s, int count, int channels) override { Filter.run(s, count, channels); }

    std::string Type, Design;
    std::vector<double> Params;
    int Rate = 44100, Channels = 1;
    Cascade Filter;
};

//---------------------------------------------------------------------------------------------
// DelayEffect(delayMs, feedback, wet, tailMs): the signal plus echoes every delayMs, each
// feedback times the last, mixed in at wet; the echoes ring on for tailMs after the voice ends.
class ShioriDelayEffect : public FloatStage {
public:
    static tjs_error factory(ShioriDelayEffect **result, tjs_int n, tTJSVariant **p, iTJSDispatch2 *) {
        auto *self = new ShioriDelayEffect();
        self->init(n, p);
        *result = self;
        return TJS_S_OK;
    }
    static tjs_error initRaw(tTJSVariant *, tjs_int n, tTJSVariant **p, ShioriDelayEffect *self) {
        std::lock_guard<std::mutex> lock(self->Mutex);
        self->init(n, p);
        self->Dirty = true;
        return TJS_S_OK;
    }
    tjs_int64 getInterface() { return interfacePointer(); }

private:
    void init(tjs_int n, tTJSVariant **p) {
        auto arg = [&](int i, double fallback) { return i < n && p[i]->Type() != tvtVoid ? double(*p[i]) : fallback; };
        DelayMs = std::max(1.0, arg(0, 300));
        Feedback = std::max(0.0, std::min(arg(1, 0.3), 0.95));
        Wet = std::max(0.0, std::min(arg(2, 0.3), 1.0));
        TailMs = std::max(0.0, arg(3, DelayMs));
    }
    void prepare(int rate, int channels) override {
        Rate = rate > 0 ? rate : 44100;
        Channels = channels;
        Line.assign(size_t(std::max(1.0, DelayMs * Rate / 1000)) * Channels, 0.f);
        Pos = 0;
    }
    void refresh() override { prepare(Rate, Channels); }
    tjs_int64 tailSamples() override { return tjs_int64(TailMs * Rate / 1000); }
    void process(float *s, int count, int channels) override {
        const size_t frames = Line.size() / size_t(std::max(1, channels));
        if (!frames) return;
        for (int i = 0; i < count; ++i) {
            for (int c = 0; c < channels; ++c) {
                float &slot = Line[Pos * channels + c];
                const float dry = s[size_t(i) * channels + c];
                const float echo = slot;
                slot = dry + echo * float(Feedback);
                s[size_t(i) * channels + c] = dry + echo * float(Wet);
            }
            Pos = (Pos + 1) % frames;
        }
    }

    double DelayMs = 300, Feedback = 0.3, Wet = 0.3, TailMs = 300;
    int Rate = 44100, Channels = 1;
    std::vector<float> Line;
    size_t Pos = 0;
};

//---------------------------------------------------------------------------------------------
// StkFreeVerb: Freeverb (Jezar, public domain): eight combs and four allpasses a channel.
class ShioriFreeVerb : public FloatStage {
public:
    static tjs_error factory(ShioriFreeVerb **result, tjs_int, tTJSVariant **, iTJSDispatch2 *) {
        *result = new ShioriFreeVerb();
        return TJS_S_OK;
    }
    tjs_int64 getInterface() { return interfacePointer(); }
    tjs_real getEffectMix() const { return Mix; }
    void setEffectMix(tjs_real v) { Mix = std::max(0.0, std::min(v, 1.0)); }
    tjs_real getRoomSize() const { return Room; }
    void setRoomSize(tjs_real v) { Room = std::max(0.0, std::min(v, 1.0)); Dirty = true; }
    tjs_real getDamping() const { return Damp; }
    void setDamping(tjs_real v) { Damp = std::max(0.0, std::min(v, 1.0)); Dirty = true; }
    tjs_real getWidth() const { return Width; }
    void setWidth(tjs_real v) { Width = std::max(0.0, std::min(v, 1.0)); }
    tjs_real getMode() const { return Freeze; }
    void setMode(tjs_real v) { Freeze = v; Dirty = true; }

private:
    struct Comb {
        std::vector<float> buf; size_t i = 0; float store = 0;
        float run(float x, float feedback, float damp) {
            const float out = buf[i];
            store = out * (1 - damp) + store * damp;
            buf[i] = x + store * feedback;
            i = (i + 1) % buf.size();
            return out;
        }
    };
    struct AllPass {
        std::vector<float> buf; size_t i = 0;
        float run(float x) {
            const float b = buf[i];
            buf[i] = x + b * 0.5f;
            i = (i + 1) % buf.size();
            return b - x;
        }
    };
    void prepare(int rate, int channels) override {
        Rate = rate > 0 ? rate : 44100;
        Channels = std::max(1, std::min(channels, 2));
        static const int combs[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
        static const int allpasses[4] = {556, 441, 341, 225};
        const double scale = Rate / 44100.0;
        for (int c = 0; c < 2; ++c) {
            const int spread = c ? 23 : 0;
            for (int k = 0; k < 8; ++k) {
                Combs[c][k].buf.assign(size_t(std::max(1.0, (combs[k] + spread) * scale)), 0.f);
                Combs[c][k].i = 0; Combs[c][k].store = 0;
            }
            for (int k = 0; k < 4; ++k) {
                AllPasses[c][k].buf.assign(size_t(std::max(1.0, (allpasses[k] + spread) * scale)), 0.f);
                AllPasses[c][k].i = 0;
            }
        }
    }
    tjs_int64 tailSamples() override { return tjs_int64(Rate * (0.5 + 2.5 * Room)); }
    void process(float *s, int count, int channels) override {
        const float feedback = Freeze >= 0.5 ? 1.f : float(0.7 + 0.28 * Room);
        const float damp = Freeze >= 0.5 ? 0.f : float(0.4 * Damp);
        const float wet1 = float(Width / 2 + 0.5), wet2 = float((1 - Width) / 2);
        for (int i = 0; i < count; ++i) {
            float *frame = s + size_t(i) * channels;
            const float input = (channels > 1 ? (frame[0] + frame[1]) / 2 : frame[0]) * 0.015f;
            float out[2] = {0, 0};
            for (int c = 0; c < 2; ++c) {
                float acc = 0;
                for (auto &comb : Combs[c]) acc += comb.run(input, feedback, damp);
                for (auto &ap : AllPasses[c]) acc = ap.run(acc);
                out[c] = acc;
            }
            const float l = out[0] * wet1 + out[1] * wet2, r = out[1] * wet1 + out[0] * wet2;
            const float mix = float(Mix);
            if (channels > 1) {
                frame[0] = frame[0] * (1 - mix) + l * 3 * mix;
                frame[1] = frame[1] * (1 - mix) + r * 3 * mix;
            } else {
                frame[0] = frame[0] * (1 - mix) + (l + r) * 1.5f * mix;
            }
        }
    }

    double Mix = 0.3, Room = 0.5, Damp = 0.5, Width = 1, Freeze = 0;
    int Rate = 44100, Channels = 2;
    Comb Combs[2][8];
    AllPass AllPasses[2][4];
};

//---------------------------------------------------------------------------------------------
// GraphicEqualizer: ten peaking bands an octave apart from 31.25 Hz; gains in dB.
class ShioriGraphicEqualizer : public FloatStage {
public:
    static tjs_error factory(ShioriGraphicEqualizer **result, tjs_int, tTJSVariant **, iTJSDispatch2 *) {
        *result = new ShioriGraphicEqualizer();
        return TJS_S_OK;
    }
    tjs_int64 getInterface() { return interfacePointer(); }
    tjs_real getGain(tjs_int band) const { return band >= 0 && band < 10 ? Gains[band] : 0; }
    void setGain(tjs_int band, tjs_real db) {
        if (band < 0 || band >= 10) return;
        std::lock_guard<std::mutex> lock(Mutex);
        Gains[band] = db;
        Dirty = true;
    }

private:
    void prepare(int rate, int channels) override {
        Rate = rate > 0 ? rate : 44100;
        Channels = channels;
        design();
    }
    void refresh() override {
        std::lock_guard<std::mutex> lock(Mutex);
        design();
    }
    void design() {
        Filter.sections.clear();
        for (int b = 0; b < 10; ++b) {
            const double f = 31.25 * std::pow(2.0, b);
            if (Gains[b] != 0 && f < Rate * 0.45) Filter.sections.push_back(rbjPeak(Rate, f, Gains[b], 1.41));
        }
        Filter.reset(Channels);
    }
    void process(float *s, int count, int channels) override { Filter.run(s, count, channels); }

    double Gains[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    int Rate = 44100, Channels = 1;
    Cascade Filter;
};

// The classes go under WaveSoundBuffer, where the scripts look for them
// (WaveSoundBuffer.WaveDSPFilter, as the core's WaveSoundBuffer.PhaseVocoder).
void attachToWaveSoundBuffer(const tjs_char *name) {
    iTJSDispatch2 *global = TVPGetScriptDispatch();
    if (!global) return;
    tTJSVariant cls, wave;
    if (TJS_SUCCEEDED(global->PropGet(0, name, nullptr, &cls, global)) &&
        TJS_SUCCEEDED(global->PropGet(0, TJS_W("WaveSoundBuffer"), nullptr, &wave, global)) && wave.Type() == tvtObject) {
        iTJSDispatch2 *w = wave.AsObjectNoAddRef();
        w->PropSet(TJS_MEMBERENSURE | TJS_IGNOREPROP | TJS_STATICMEMBER, name, nullptr, &cls, w);
    }
    global->Release();
}
void linkTypicalDSP() { attachToWaveSoundBuffer(TJS_W("WaveDSPFilter")); }
void linkBasicEffect() {
    attachToWaveSoundBuffer(TJS_W("DelayEffect"));
    attachToWaveSoundBuffer(TJS_W("StkFreeVerb"));
    attachToWaveSoundBuffer(TJS_W("GraphicEqualizer"));
}

} // namespace

#define NCB_MODULE_NAME TJS_W("wfTypicalDSP.dll")
NCB_REGISTER_CLASS_DIFFER(WaveDSPFilter, ShioriWaveDSPFilter) {
    Factory(&ClassT::factory);
    RawCallback(TJS_W("setParams"), &ClassT::setParams, 0);
    NCB_METHOD(getParamInfo);
    Property(TJS_W("interface"), &ClassT::getInterface, (int)0);
}
NCB_POST_REGIST_CALLBACK(linkTypicalDSP);

#undef NCB_MODULE_NAME
#define NCB_MODULE_NAME TJS_W("wfBasicEffect.dll")
NCB_REGISTER_CLASS_DIFFER(DelayEffect, ShioriDelayEffect) {
    Factory(&ClassT::factory);
    RawCallback(TJS_W("init"), &ClassT::initRaw, 0);
    Property(TJS_W("interface"), &ClassT::getInterface, (int)0);
}
NCB_REGISTER_CLASS_DIFFER(StkFreeVerb, ShioriFreeVerb) {
    Factory(&ClassT::factory);
    Property(TJS_W("interface"), &ClassT::getInterface, (int)0);
    NCB_PROPERTY(effectMix, getEffectMix, setEffectMix);
    NCB_PROPERTY(roomSize, getRoomSize, setRoomSize);
    NCB_PROPERTY(damping, getDamping, setDamping);
    NCB_PROPERTY(width, getWidth, setWidth);
    NCB_PROPERTY(mode, getMode, setMode);
}
NCB_REGISTER_CLASS_DIFFER(GraphicEqualizer, ShioriGraphicEqualizer) {
    Factory(&ClassT::factory);
    Property(TJS_W("interface"), &ClassT::getInterface, (int)0);
    NCB_METHOD(getGain);
    NCB_METHOD(setGain);
}
NCB_POST_REGIST_CALLBACK(linkBasicEffect);
