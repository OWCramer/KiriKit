/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

// Audio output for krkr2's core (its iTVPSoundBuffer, environ side), written for Shiori in
// place of krkr2's OpenAL/SDL2 mixer. Every WaveSoundBuffer gets a stream with a small queue of
// buffers, as the core expects (it fills up to the queue's size, asks how many remain and where
// playback is, in its own samples); one Core Audio output unit mixes the streams, converting
// format and sample rate on the way.
#include "shiori_environ.h"

#include "tjsCommHead.h"
#include "WaveMixer.h"
#include "DebugIntf.h"

#import <AudioToolbox/AudioToolbox.h>
#import <Foundation/Foundation.h>
#if TARGET_OS_IPHONE
#import <AVFoundation/AVFoundation.h>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <mutex>
#include <vector>

namespace {

class Mixer;
Mixer *theMixer = nullptr;

// SHIORIKRKR_TRACE_AUDIO=1: each stream's play, pause, stop and reset, with what it had queued.
bool tracing() {
    static const bool on = getenv("SHIORIKRKR_TRACE_AUDIO") != nullptr;
    return on;
}

// One queued buffer, already converted to float stereo at the stream's own sample rate.
struct Chunk {
    std::vector<float> frames; // interleaved L R
    size_t count() const { return frames.size() / 2; }
};

class Stream final : public iTVPSoundBuffer {
public:
    explicit Stream(const tTVPWaveFormat &format, int bufferCount)
        : Format(format), BufferCount(std::max(bufferCount, 1)) {
        static int next = 0;
        Id = ++next;
        trace("new " + std::to_string(Format.SamplesPerSec) + " Hz x" + std::to_string(Format.Channels));
    }
    ~Stream() override;

    void Release() override { delete this; }

    void Play() override {
        trace("play");
        Playing = true;
    }
    void Pause() override {
        trace("pause");
        Playing = false;
    }
    void Stop() override {
        trace("stop");
        Playing = false;
        Reset();
    }
    void Reset() override {
        trace("reset");
        std::lock_guard<std::mutex> lock(Lock);
        Chunks.clear();
        Cursor = 0;
        PlayedSamples = 0;
    }
    bool IsPlaying() override { return Playing; }

    void SetVolume(float v) override {
        if (tracing() && std::abs(v - Volume.load()) > 0.001f) trace("volume " + std::to_string(v));
        Volume = v;
    }
    float GetVolume() override { return Volume; }
    void SetPan(float v) override { Pan = std::clamp(v, -1.0f, 1.0f); }
    float GetPan() override { return Pan; }

    void AppendBuffer(const void *data, unsigned int bytes) override {
        const int channels = std::max<int>(Format.Channels, 1);
        const int sampleBytes = Format.BytesPerSample;
        const size_t frames = sampleBytes > 0 ? bytes / (size_t(sampleBytes) * channels) : 0;
        if (frames == 0) return;
        Chunk chunk;
        chunk.frames.resize(frames * 2);
        const uint8_t *p = static_cast<const uint8_t *>(data);
        for (size_t i = 0; i < frames; ++i) {
            float l = 0, r = 0;
            for (int c = 0; c < channels; ++c) {
                const float v = sample(p + (i * channels + c) * sampleBytes);
                if (channels == 1) l = r = v;
                else if (c == 0) l = v;
                else if (c == 1) r = v;
            }
            chunk.frames[i * 2] = l;
            chunk.frames[i * 2 + 1] = r;
        }
        std::lock_guard<std::mutex> lock(Lock);
        Chunks.push_back(std::move(chunk));
    }

    bool IsBufferValid() override {
        std::lock_guard<std::mutex> lock(Lock);
        return int(Chunks.size()) < BufferCount;
    }

    tjs_uint GetCurrentPlaySamples() override {
        std::lock_guard<std::mutex> lock(Lock);
        return tjs_uint(PlayedSamples + tjs_uint64(Cursor));
    }

    tjs_uint GetLatencySamples() override {
        std::lock_guard<std::mutex> lock(Lock);
        double queued = -Cursor;
        for (const Chunk &c : Chunks) queued += c.count();
        return tjs_uint(std::max(queued, 0.0));
    }

    float GetLatencySeconds() override {
        return Format.SamplesPerSec ? float(GetLatencySamples()) / Format.SamplesPerSec : 0.0f;
    }

    int GetRemainBuffers() override {
        std::lock_guard<std::mutex> lock(Lock);
        return int(Chunks.size());
    }

    // Mixes into out (interleaved stereo float, outRate) on the audio thread.
    void mix(float *out, int frames, double outRate) {
        if (!Playing) return;
        std::unique_lock<std::mutex> lock(Lock, std::try_to_lock);
        if (!lock.owns_lock()) return;
        const double step = Format.SamplesPerSec ? double(Format.SamplesPerSec) / outRate : 1.0;
        const float volume = Volume;
        const float leftGain = volume * (Pan > 0 ? 1.0f - Pan : 1.0f);
        const float rightGain = volume * (Pan < 0 ? 1.0f + Pan : 1.0f);
        for (int i = 0; i < frames && !Chunks.empty(); ++i) {
            Chunk &chunk = Chunks.front();
            const size_t index = size_t(Cursor);
            const double frac = Cursor - double(index);
            float l = chunk.frames[index * 2], r = chunk.frames[index * 2 + 1];
            // The next frame for interpolation, from this chunk or the next.
            float nl = l, nr = r;
            if (index + 1 < chunk.count()) {
                nl = chunk.frames[(index + 1) * 2];
                nr = chunk.frames[(index + 1) * 2 + 1];
            } else if (Chunks.size() > 1) {
                nl = Chunks[1].frames[0];
                nr = Chunks[1].frames[1];
            }
            out[i * 2] += (l + (nl - l) * float(frac)) * leftGain;
            out[i * 2 + 1] += (r + (nr - r) * float(frac)) * rightGain;
            Cursor += step;
            while (!Chunks.empty() && Cursor >= double(Chunks.front().count())) {
                Cursor -= double(Chunks.front().count());
                PlayedSamples += Chunks.front().count();
                Chunks.pop_front();
            }
        }
        if (Chunks.empty()) Cursor = 0;
    }

private:
    void trace(const std::string &what) {
        if (!tracing()) return;
        size_t queued = 0;
        {
            std::lock_guard<std::mutex> lock(Lock);
            for (const Chunk &c : Chunks) queued += c.count();
        }
        shiori::log("audio " + std::to_string(Id) + ": " + what + " (playing " + std::to_string(int(Playing.load())) +
                    ", played " + std::to_string(PlayedSamples) + ", queued " + std::to_string(queued) + ")");
    }

    int Id = 0;

    float sample(const uint8_t *p) const {
        if (Format.IsFloat) {
            float f;
            memcpy(&f, p, 4);
            return f;
        }
        switch (Format.BytesPerSample) {
        case 1: return (int(p[0]) - 128) / 128.0f;
        case 2: {
            int16_t v;
            memcpy(&v, p, 2);
            return v / 32768.0f;
        }
        case 3: {
            int32_t v = (int32_t(p[0]) << 8) | (int32_t(p[1]) << 16) | (int32_t(p[2]) << 24);
            return v / 2147483648.0f;
        }
        case 4: {
            int32_t v;
            memcpy(&v, p, 4);
            return v / 2147483648.0f;
        }
        default: return 0;
        }
    }

    tTVPWaveFormat Format;
    int BufferCount;
    std::mutex Lock;
    std::deque<Chunk> Chunks;
    double Cursor = 0;            // position in the front chunk, in source frames
    tjs_uint64 PlayedSamples = 0; // frames of chunks already played
    std::atomic<bool> Playing{false};
    std::atomic<float> Volume{1.0f};
    std::atomic<float> Pan{0.0f};
};

class Mixer {
public:
    bool start() {
        std::lock_guard<std::recursive_mutex> unit(UnitLock);
        Wanted = true;
        observe();
        return open();
    }

    void suspend(bool suspended) {
        std::lock_guard<std::recursive_mutex> unit(UnitLock);
        Suspended = suspended;
        if (!Unit) return;
        if (suspended && Running) {
            AudioOutputUnitStop(Unit);
            Running = false;
        } else if (!suspended && !Running) {
            resume("the app came back");
        }
    }

    void shutdown() {
        std::lock_guard<std::recursive_mutex> unit(UnitLock);
        Wanted = false;
        close();
        // The last game's streams are its own (any it never freed stay silent).
        std::lock_guard<std::mutex> lock(Lock);
        Streams.clear();
    }

    // The output moved (another device, headphones, a controller's speaker), the system
    // reset its media services, or an interruption (a call, Siri) ended: the output unit is
    // made again at the device's format, if it wanted to run; the streams keep playing
    // where they were.
    void restart(const char *why) {
        std::lock_guard<std::recursive_mutex> unit(UnitLock);
        if (!Wanted) return;
        close();
        if (open()) shiori::log(std::string("ShioriKrkr: audio output restarted (") + why + ")");
        else shiori::log(std::string("ShioriKrkr: audio output couldn't restart (") + why + ")");
    }

    // An interruption ended or the app came back: the same unit starts again (made again
    // when that fails).
    void resume(const char *why) {
        std::lock_guard<std::recursive_mutex> unit(UnitLock);
        if (!Wanted || Suspended) return;
#if TARGET_OS_IPHONE
        [AVAudioSession.sharedInstance setActive:YES error:nil];
#endif
        if (Unit && !Running) Running = AudioOutputUnitStart(Unit) == noErr;
        if (!Unit || !Running) restart(why);
    }

    void interrupted() {
        std::lock_guard<std::recursive_mutex> unit(UnitLock);
        if (Unit && Running) AudioOutputUnitStop(Unit);
        Running = false;
        shiori::log("ShioriKrkr: audio interrupted");
    }

    // The device's sample rate now (the output unit's output scope).
    double deviceRate() {
        std::lock_guard<std::recursive_mutex> unit(UnitLock);
        if (!Unit) return 0;
        AudioStreamBasicDescription device = {};
        UInt32 size = sizeof(device);
        AudioUnitGetProperty(Unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &device, &size);
        return device.mSampleRate;
    }
    double rate() const { return Rate; }
    bool running() const { return Running; }

private:
    bool open() {
        if (Unit) return true;
#if TARGET_OS_IPHONE
        AVAudioSession *session = AVAudioSession.sharedInstance;
        if (![session.category isEqualToString:AVAudioSessionCategoryPlayback])
            [session setCategory:AVAudioSessionCategoryPlayback error:nil];
        [session setActive:YES error:nil];
#endif
        AudioComponentDescription desc = {};
        desc.componentType = kAudioUnitType_Output;
#if TARGET_OS_IPHONE
        desc.componentSubType = kAudioUnitSubType_RemoteIO;
#else
        desc.componentSubType = kAudioUnitSubType_DefaultOutput;
#endif
        desc.componentManufacturer = kAudioUnitManufacturer_Apple;
        AudioComponent component = AudioComponentFindNext(nullptr, &desc);
        if (!component || AudioComponentInstanceNew(component, &Unit) != noErr) {
            Unit = nullptr;
            return false;
        }

        AudioStreamBasicDescription device = {};
        UInt32 size = sizeof(device);
        AudioUnitGetProperty(Unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &device, &size);
        Rate = device.mSampleRate > 0 ? device.mSampleRate : 48000.0;

        AudioStreamBasicDescription format = {};
        format.mSampleRate = Rate;
        format.mFormatID = kAudioFormatLinearPCM;
        format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        format.mChannelsPerFrame = 2;
        format.mBitsPerChannel = 32;
        format.mFramesPerPacket = 1;
        format.mBytesPerFrame = 8;
        format.mBytesPerPacket = 8;
        AudioUnitSetProperty(Unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &format, sizeof(format));

        AURenderCallbackStruct callback = {render, this};
        AudioUnitSetProperty(Unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &callback,
                             sizeof(callback));
        if (AudioUnitInitialize(Unit) != noErr) {
            AudioComponentInstanceDispose(Unit);
            Unit = nullptr;
            return false;
        }
        Running = !Suspended && AudioOutputUnitStart(Unit) == noErr;
        shiori::log("ShioriKrkr: audio out at " + std::to_string(int(Rate)) + " Hz");
        return Running || Suspended;
    }

    void close() {
        if (!Unit) return;
        AudioOutputUnitStop(Unit);
        AudioUnitUninitialize(Unit);
        AudioComponentInstanceDispose(Unit);
        Unit = nullptr;
        Running = false;
    }

    // The session's notifications, once per process (the mixer lives as long).
    void observe() {
        if (Observing) return;
        Observing = true;
#if TARGET_OS_IPHONE
        NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
        Mixer *self = this;
        [center addObserverForName:AVAudioSessionRouteChangeNotification object:nil queue:nil
                        usingBlock:^(NSNotification *note) {
                            const auto reason = AVAudioSessionRouteChangeReason(
                                [note.userInfo[AVAudioSessionRouteChangeReasonKey] unsignedIntegerValue]);
                            // A new device, the old one gone, or the route reconfigured: the unit
                            // may have stopped or run at the old rate.
                            if (reason == AVAudioSessionRouteChangeReasonNewDeviceAvailable ||
                                reason == AVAudioSessionRouteChangeReasonOldDeviceUnavailable ||
                                reason == AVAudioSessionRouteChangeReasonOverride ||
                                reason == AVAudioSessionRouteChangeReasonRouteConfigurationChange ||
                                reason == AVAudioSessionRouteChangeReasonCategoryChange ||
                                reason == AVAudioSessionRouteChangeReasonWakeFromSleep ||
                                reason == AVAudioSessionRouteChangeReasonNoSuitableRouteForCategory)
                                self->restart("the route changed");
                        }];
        [center addObserverForName:AVAudioSessionInterruptionNotification object:nil queue:nil
                        usingBlock:^(NSNotification *note) {
                            const auto type = AVAudioSessionInterruptionType(
                                [note.userInfo[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue]);
                            if (type == AVAudioSessionInterruptionTypeBegan) self->interrupted();
                            else self->resume("an interruption ended");
                        }];
        [center addObserverForName:AVAudioSessionMediaServicesWereResetNotification object:nil queue:nil
                        usingBlock:^(NSNotification *) {
                            // Every audio object of the old services is gone: made again (the old
                            // unit is still stopped and disposed of first, which fails harmlessly
                            // when it's already dead and keeps two units from rendering at once
                            // when it isn't).
                            self->restart("media services were reset");
                        }];
#endif
    }

public:
    bool started() const { return Unit != nullptr; }

    void add(Stream *s) {
        std::lock_guard<std::mutex> lock(Lock);
        Streams.push_back(s);
    }
    void remove(Stream *s) {
        std::lock_guard<std::mutex> lock(Lock);
        Streams.erase(std::remove(Streams.begin(), Streams.end(), s), Streams.end());
    }

private:
    static OSStatus render(void *context, AudioUnitRenderActionFlags *, const AudioTimeStamp *, UInt32, UInt32 frames,
                           AudioBufferList *buffers) {
        auto *self = static_cast<Mixer *>(context);
        self->Renders.fetch_add(1, std::memory_order_relaxed);
        float *out = static_cast<float *>(buffers->mBuffers[0].mData);
        memset(out, 0, size_t(frames) * 8);
        std::lock_guard<std::mutex> lock(self->Lock);
        for (Stream *s : self->Streams) s->mix(out, int(frames), self->Rate.load());
        // Soft limit so stacked voices never wrap.
        for (UInt32 i = 0; i < frames * 2; ++i) {
            const float v = out[i];
            if (v > 1.0f || v < -1.0f) out[i] = std::tanh(v);
        }
        // SHIORIKRKR_AUDIO_DUMP=<file>: what goes out, as raw float stereo at the output's rate
        // (for checking loops and gaps on the harness).
        static FILE *dump = [] {
            const char *path = getenv("SHIORIKRKR_AUDIO_DUMP");
            return path ? fopen(path, "wb") : nullptr;
        }();
        if (dump) fwrite(out, sizeof(float), size_t(frames) * 2, dump);
        return noErr;
    }

    AudioComponentInstance Unit = nullptr;
    std::atomic<double> Rate{48000.0};
    std::atomic<bool> Running{false};
    bool Wanted = false, Suspended = false, Observing = false;
public:
    std::atomic<uint64_t> Renders{0}; // render callbacks (the profile: is sound going out?)
private:
    std::recursive_mutex UnitLock; // the unit's life (notifications come on other threads)
    std::mutex Lock;
    std::vector<Stream *> Streams;
};

Stream::~Stream() {
    if (theMixer) theMixer->remove(this);
}

} // namespace

void TVPInitDirectSound(int) {
    // The mixer once per process; its output again for each game (it's shut down when a game
    // is left, and the next game played in silence when this only ran once).
    if (!theMixer) theMixer = new Mixer();
    if (theMixer->started()) return;
    if (!theMixer->start()) shiori::log("ShioriKrkr: audio output couldn't start");
}

void TVPUninitDirectSound() {}

iTVPSoundBuffer *TVPCreateSoundBuffer(tTVPWaveFormat &fmt, int bufcount) {
    TVPInitDirectSound(48000);
    auto *stream = new Stream(fmt, bufcount);
    theMixer->add(stream);
    return stream;
}

void shiori::audioSuspend(bool suspended) {
    if (theMixer) theMixer->suspend(suspended);
}

uint64_t shiori::audioRenders() { return theMixer ? theMixer->Renders.load() : 0; }

void shiori::audioShutdown() {
    if (!theMixer) return;
    theMixer->shutdown();
}
