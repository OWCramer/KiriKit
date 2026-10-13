/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

// Movie playback for ShioriKrkr: KiriKiri's video overlay objects (krmovie.h's
// iTVPVideoOverlay, which krkr2's VideoOverlay class drives), on FFmpeg (LGPL). A decoder
// thread reads the movie from the game's archive through a custom AVIOContext (so a 280 MB
// opening is never loaded at once), turns WMV 9 frames into RGBA and feeds WMA audio to one of
// the engine's own sound streams; the engine's continuous event shows each frame when its time
// comes, and the VideoOverlay hears of new frames and the end through its event queue.
//
// Overlay mode (KAG's default [video]): the frame goes to the host, which draws it over the
// game (shiorikrkr_frame). Layer mode: the frame is copied into the engine's two video
// bitmaps and shown through the layers KiriKiri assigns.
#include "shiorikrkr_movie.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <string>
#include <thread>

#include "tjsCommHead.h"
#include "DebugIntf.h"
#include "MsgIntf.h"
#include "EventIntf.h"
#include "LayerBitmapIntf.h"
#include "StorageIntf.h"
#include "WaveIntf.h"
#include "WaveMixer.h"
#include "WindowImpl.h"
#include "impl/VideoOvlImpl.h"
#include "krmovie.h"
#include "NativeEventQueue.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

namespace shiorikrkr {

std::mutex &movieOverlayLock() {
    static std::mutex lock;
    return lock;
}

static std::unique_ptr<MovieOverlayFrame> &overlaySlot() {
    static std::unique_ptr<MovieOverlayFrame> slot;
    return slot;
}

MovieOverlayFrame *movieOverlay() { return overlaySlot().get(); }

namespace {

using Clock = std::chrono::steady_clock;

uint8_t clip(int v) { return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v); }

/// YUV 4:2:0 (BT.601, limited range, as WMV 9 is) to 8-bit RGBA or BGRA.
void convertYUV420(const AVFrame *frame, uint8_t *out, bool bgra) {
    const int w = frame->width, h = frame->height;
    for (int y = 0; y < h; ++y) {
        const uint8_t *py = frame->data[0] + size_t(y) * frame->linesize[0];
        const uint8_t *pu = frame->data[1] + size_t(y / 2) * frame->linesize[1];
        const uint8_t *pv = frame->data[2] + size_t(y / 2) * frame->linesize[2];
        uint8_t *o = out + size_t(y) * w * 4;
        for (int x = 0; x < w; ++x) {
            const int c = 298 * (py[x] - 16), d = pu[x / 2] - 128, e = pv[x / 2] - 128;
            const uint8_t r = clip((c + 409 * e + 128) >> 8);
            const uint8_t g = clip((c - 100 * d - 208 * e + 128) >> 8);
            const uint8_t b = clip((c + 516 * d + 128) >> 8);
            o[0] = bgra ? b : r;
            o[1] = g;
            o[2] = bgra ? r : b;
            o[3] = 255;
            o += 4;
        }
    }
}

struct DecodedFrame {
    int64_t ms = 0;
    std::vector<uint8_t> pixels;
};

class MovieCore {
public:
    MovieCore(tTJSNI_VideoOverlay *owner, bool layer) : owner_(owner), layer_(layer) {}

    ~MovieCore() {
        quit_ = true;
        wake_.notify_all();
        if (thread_.joinable()) thread_.join();
        hideOverlay();
        if (audio_) audio_->Release();
        if (video_) avcodec_free_context(&video_);
        if (sound_) avcodec_free_context(&sound_);
        if (format_) avformat_close_input(&format_);
        if (io_) {
            av_freep(&io_->buffer);
            avio_context_free(&io_);
        }
        delete stream_;
    }

    bool open(tTJSBinaryStream *stream, const ttstr &name) {
        stream_ = stream;
        name_ = name.AsStdString();
        unsigned char *buffer = static_cast<unsigned char *>(av_malloc(1 << 16));
        io_ = avio_alloc_context(buffer, 1 << 16, 0, this, &MovieCore::readPacket, nullptr, &MovieCore::seekStream);
        format_ = avformat_alloc_context();
        format_->pb = io_;
        format_->flags |= AVFMT_FLAG_CUSTOM_IO;
        if (avformat_open_input(&format_, name_.c_str(), nullptr, nullptr) < 0) {
            format_ = nullptr;
            return fail("couldn't read the movie's container");
        }
        if (avformat_find_stream_info(format_, nullptr) < 0) return fail("no stream information");
        videoIndex_ = av_find_best_stream(format_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        audioIndex_ = av_find_best_stream(format_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (videoIndex_ < 0 || !(video_ = openCodec(videoIndex_))) return fail("no video it can decode");
        if (audioIndex_ >= 0 && !(sound_ = openCodec(audioIndex_))) audioIndex_ = -1;
        width_ = video_->width;
        height_ = video_->height;
        const AVRational rate = format_->streams[videoIndex_]->avg_frame_rate;
        fps_ = rate.num > 0 && rate.den > 0 ? av_q2d(rate) : 30.0;
        durationMs_ = format_->duration > 0 ? format_->duration / 1000 : 0;
        if (sound_) {
            tTVPWaveFormat format = {};
            format.SamplesPerSec = tjs_uint(sound_->sample_rate);
            format.Channels = tjs_uint(std::max(1, sound_->ch_layout.nb_channels));
            format.BitsPerSample = 32;
            format.BytesPerSample = 4;
            format.IsFloat = true;
            audioChannels_ = int(format.Channels);
            audio_ = TVPCreateSoundBuffer(format, 1 << 20);
            setVolume(volume_);
        }
        TVPAddLog(ttstr(TJS_W("(info) ShioriKrkr movie: ")) + ttstr(name_) + ttstr(" ") + ttstr((tjs_int)width_) + ttstr("x") +
                  ttstr((tjs_int)height_) + (sound_ ? ttstr(" with sound") : ttstr("")));
        thread_ = std::thread([this] { decodeLoop(); });
        return true;
    }

    // MARK: Control (engine thread)

    void play() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ended_) requestSeek(0);
        ended_ = false;
        completeSent_ = false;
        if (!playing_ || paused_) {
            startedAt_ = Clock::now() - std::chrono::milliseconds(positionMs_);
            playing_ = true;
            paused_ = false;
            if (audio_) audio_->Play();
        }
        wake_.notify_all();
    }

    void stop() {
        std::lock_guard<std::mutex> lock(mutex_);
        playing_ = false;
        paused_ = false;
        if (audio_) audio_->Pause();
        requestSeek(0);
        hideOverlay();
    }

    void pause() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!playing_ || paused_) return;
        positionMs_ = elapsedLocked();
        paused_ = true;
        if (audio_) audio_->Pause();
    }

    void seek(int64_t ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        ended_ = false;
        completeSent_ = false;
        requestSeek(std::max<int64_t>(0, ms));
    }

    int64_t position() {
        std::lock_guard<std::mutex> lock(mutex_);
        return elapsedLocked();
    }

    tTVPVideoStatus status() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ended_) return vsEnded;
        if (paused_) return vsPaused;
        return playing_ ? vsPlaying : vsStopped;
    }

    void setVolume(long volume) {
        // KiriKiri's 0...100000 (or a percentage).
        const float gain = volume > 100 ? volume / 100000.0f : volume / 100.0f;
        if (audio_) audio_->SetVolume(std::clamp(gain, 0.0f, 1.0f));
        volume_ = volume;
    }

    void setRect(int l, int t, int r, int b) {
        // KiriKiri zooms the rectangle to the window (Window.setZoom, which KAG sets to fit the
        // screen); the host draws in the game's own pixels, so it's zoomed back.
        if (TVPMainWindow && TVPMainWindow->GetZoomNumer() > 0) {
            const int64_t numer = TVPMainWindow->GetZoomNumer(), denom = TVPMainWindow->GetZoomDenom();
            l = int(l * denom / numer), t = int(t * denom / numer), r = int(r * denom / numer), b = int(b * denom / numer);
        }
        left_ = l, top_ = t, right_ = r, bottom_ = b;
        std::lock_guard<std::mutex> lock(movieOverlayLock());
        if (auto *overlay = movieOverlay(); overlay && drawn_ == this) {
            overlay->left = l, overlay->top = t, overlay->right = r, overlay->bottom = b;
            overlay->serial++;
        }
    }

    void setVisible(bool visible) {
        visible_ = visible;
        std::lock_guard<std::mutex> lock(movieOverlayLock());
        if (auto *overlay = movieOverlay(); overlay && drawn_ == this) {
            overlay->visible = visible;
            overlay->serial++;
        }
    }

    /// Each engine frame: shows the frame whose time has come; posts completion at the end.
    void tick() {
        int64_t now;
        bool done;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!playing_ || paused_) return;
            now = elapsedLocked();
            positionMs_ = now;
            done = decodeDone_ && queue_.empty();
        }
        std::unique_ptr<DecodedFrame> show;
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            while (!queue_.empty() && queue_.front()->ms <= now) {
                show = std::move(queue_.front());
                queue_.pop_front();
            }
        }
        if (show) {
            wake_.notify_all();
            frameIndex_ = int(show->ms * fps_ / 1000.0);
            if (layer_) {
                std::lock_guard<std::mutex> lock(frameMutex_);
                current_ = std::move(show);
            } else {
                publishOverlay(*show);
            }
            if (layer_) post(EC_UPDATE, frameIndex_);
        }
        if (done && !completeSent_) {
            completeSent_ = true;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ended_ = true;
                playing_ = false;
            }
            if (audio_) audio_->Pause();
            post(EC_COMPLETE, 0);
        }
    }

    /// Layer mode: the newest frame into one of the engine's two bitmaps, in turn.
    tTVPBaseTexture *frontBuffer() {
        std::lock_guard<std::mutex> lock(frameMutex_);
        if (!current_ || !buffers_[0]) return buffers_[front_];
        front_ = buffers_[1] ? 1 - front_ : 0;
        tTVPBaseTexture *target = buffers_[front_];
        const int w = std::min<int>(width_, int(target->GetWidth())), h = std::min<int>(height_, int(target->GetHeight()));
        for (int y = 0; y < h; ++y)
            memcpy(target->GetScanLineForWrite(y), current_->pixels.data() + size_t(y) * width_ * 4, size_t(w) * 4);
        return target;
    }

    void setBuffers(tTVPBaseTexture *a, tTVPBaseTexture *b) {
        std::lock_guard<std::mutex> lock(frameMutex_);
        buffers_[0] = a;
        buffers_[1] = b;
    }

    int width_ = 0, height_ = 0;
    double fps_ = 30;
    int64_t durationMs_ = 0;
    int frameIndex_ = 0;
    long volume_ = 100000;

private:
    void post(long code, int frame) {
        NativeEvent ev(WM_GRAPHNOTIFY);
        ev.WParam = code;
        ev.LParam = frame;
        if (owner_) owner_->PostEvent(ev);
    }

    bool fail(const char *why) {
        TVPAddLog(ttstr("(warn) ShioriKrkr movie: ") + ttstr(name_) + ttstr(": ") + ttstr(why));
        return false;
    }

    AVCodecContext *openCodec(int index) {
        const AVCodecParameters *parameters = format_->streams[index]->codecpar;
        const AVCodec *codec = avcodec_find_decoder(parameters->codec_id);
        if (!codec) return nullptr;
        AVCodecContext *context = avcodec_alloc_context3(codec);
        if (avcodec_parameters_to_context(context, parameters) < 0 || avcodec_open2(context, codec, nullptr) < 0) {
            avcodec_free_context(&context);
            return nullptr;
        }
        return context;
    }

    static int readPacket(void *opaque, uint8_t *buffer, int size) {
        auto *self = static_cast<MovieCore *>(opaque);
        const tjs_uint read = self->stream_->Read(buffer, tjs_uint(size));
        return read == 0 ? AVERROR_EOF : int(read);
    }

    static int64_t seekStream(void *opaque, int64_t offset, int whence) {
        auto *self = static_cast<MovieCore *>(opaque);
        if (whence & AVSEEK_SIZE) return int64_t(self->stream_->GetSize());
        whence &= ~AVSEEK_FORCE;
        int origin = TJS_BS_SEEK_SET;
        if (whence == SEEK_CUR) origin = TJS_BS_SEEK_CUR;
        else if (whence == SEEK_END) origin = TJS_BS_SEEK_END;
        return int64_t(self->stream_->Seek(offset, origin));
    }

    int64_t elapsedLocked() const {
        if (!playing_ || paused_) return positionMs_;
        return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - startedAt_).count();
    }

    void requestSeek(int64_t ms) {
        seekMs_ = ms;
        positionMs_ = ms;
        startedAt_ = Clock::now() - std::chrono::milliseconds(ms);
        decodeDone_ = false;
        wake_.notify_all();
    }

    void hideOverlay() {
        std::lock_guard<std::mutex> lock(movieOverlayLock());
        if (drawn_ == this) {
            overlaySlot().reset();
            drawn_ = nullptr;
        }
    }

    void publishOverlay(const DecodedFrame &frame) {
        std::lock_guard<std::mutex> lock(movieOverlayLock());
        auto &slot = overlaySlot();
        if (!slot) slot = std::make_unique<MovieOverlayFrame>();
        drawn_ = this;
        slot->rgba = frame.pixels;
        slot->width = width_;
        slot->height = height_;
        slot->left = left_, slot->top = top_, slot->right = right_, slot->bottom = bottom_;
        slot->visible = visible_;
        slot->serial++;
    }

    // MARK: Decoder thread

    void decodeLoop() {
        AVPacket *packet = av_packet_alloc();
        AVFrame *frame = av_frame_alloc();
        std::vector<float> interleaved;
        while (!quit_) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                if (seekMs_ >= 0) {
                    const int64_t target = seekMs_;
                    seekMs_ = -1;
                    lock.unlock();
                    av_seek_frame(format_, -1, target * 1000, AVSEEK_FLAG_BACKWARD);
                    avcodec_flush_buffers(video_);
                    if (sound_) avcodec_flush_buffers(sound_);
                    {
                        std::lock_guard<std::mutex> q(queueMutex_);
                        queue_.clear();
                    }
                    if (audio_) audio_->Reset();
                    skipBeforeMs_ = target;
                    continue;
                }
                // Enough decoded ahead (frames waiting, a second of sound queued): wait.
                const bool full = queueSize() >= 6 || (audio_ && audio_->GetLatencySamples() > tjs_uint(sound_->sample_rate));
                if (decodeDone_ || full) {
                    wake_.wait_for(lock, std::chrono::milliseconds(full ? 4 : 50));
                    continue;
                }
            }
            const int read = av_read_frame(format_, packet);
            if (read < 0) {
                // End of the movie: drain both decoders.
                avcodec_send_packet(video_, nullptr);
                receiveVideo(frame);
                if (sound_) {
                    avcodec_send_packet(sound_, nullptr);
                    receiveAudio(frame, interleaved);
                }
                std::lock_guard<std::mutex> lock(mutex_);
                decodeDone_ = true;
                continue;
            }
            if (packet->stream_index == videoIndex_) {
                if (avcodec_send_packet(video_, packet) >= 0) receiveVideo(frame);
            } else if (packet->stream_index == audioIndex_ && sound_) {
                if (avcodec_send_packet(sound_, packet) >= 0) receiveAudio(frame, interleaved);
            }
            av_packet_unref(packet);
        }
        av_frame_free(&frame);
        av_packet_free(&packet);
    }

    size_t queueSize() {
        std::lock_guard<std::mutex> lock(queueMutex_);
        return queue_.size();
    }

    int64_t frameMs(const AVFrame *frame, int index) const {
        const int64_t pts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
        if (pts == AV_NOPTS_VALUE) return 0;
        return av_rescale_q(pts, format_->streams[index]->time_base, AVRational{1, 1000});
    }

    void receiveVideo(AVFrame *frame) {
        while (avcodec_receive_frame(video_, frame) >= 0) {
            const int64_t ms = frameMs(frame, videoIndex_);
            if (ms + 1 < skipBeforeMs_ || frame->format != AV_PIX_FMT_YUV420P) {
                av_frame_unref(frame);
                continue;
            }
            auto decoded = std::make_unique<DecodedFrame>();
            decoded->ms = ms;
            decoded->pixels.resize(size_t(frame->width) * frame->height * 4);
            convertYUV420(frame, decoded->pixels.data(), false); // the engine's bitmaps are RGBA too
            av_frame_unref(frame);
            std::lock_guard<std::mutex> lock(queueMutex_);
            queue_.push_back(std::move(decoded));
        }
    }

    void receiveAudio(AVFrame *frame, std::vector<float> &interleaved) {
        while (avcodec_receive_frame(sound_, frame) >= 0) {
            if (audio_ && frameMs(frame, audioIndex_) + 1 >= skipBeforeMs_) {
                const int channels = frame->ch_layout.nb_channels, count = frame->nb_samples;
                interleaved.resize(size_t(channels) * count);
                for (int i = 0; i < count; ++i)
                    for (int c = 0; c < channels; ++c) {
                        float v = 0;
                        switch (frame->format) {
                        case AV_SAMPLE_FMT_FLTP: v = reinterpret_cast<const float *>(frame->extended_data[c])[i]; break;
                        case AV_SAMPLE_FMT_FLT: v = reinterpret_cast<const float *>(frame->extended_data[0])[i * channels + c]; break;
                        case AV_SAMPLE_FMT_S16P: v = reinterpret_cast<const int16_t *>(frame->extended_data[c])[i] / 32768.0f; break;
                        case AV_SAMPLE_FMT_S16: v = reinterpret_cast<const int16_t *>(frame->extended_data[0])[i * channels + c] / 32768.0f; break;
                        default: break;
                        }
                        interleaved[size_t(i) * channels + c] = v;
                    }
                audio_->AppendBuffer(interleaved.data(), unsigned(interleaved.size() * sizeof(float)));
            }
            av_frame_unref(frame);
        }
    }

    tTJSNI_VideoOverlay *owner_;
    bool layer_;
    tTJSBinaryStream *stream_ = nullptr;
    std::string name_;
    AVIOContext *io_ = nullptr;
    AVFormatContext *format_ = nullptr;
    AVCodecContext *video_ = nullptr, *sound_ = nullptr;
    int videoIndex_ = -1, audioIndex_ = -1, audioChannels_ = 2;
    iTVPSoundBuffer *audio_ = nullptr;
    std::thread thread_;
    std::atomic<bool> quit_{false};

    std::mutex mutex_; // playing state
    std::condition_variable_any wake_;
    bool playing_ = false, paused_ = false, ended_ = false, decodeDone_ = false, completeSent_ = false;
    int64_t positionMs_ = 0, seekMs_ = -1;
    std::atomic<int64_t> skipBeforeMs_{0};
    Clock::time_point startedAt_ = Clock::now();

    std::mutex queueMutex_;
    std::deque<std::unique_ptr<DecodedFrame>> queue_;

    std::mutex frameMutex_; // layer mode
    std::unique_ptr<DecodedFrame> current_;
    tTVPBaseTexture *buffers_[2] = {nullptr, nullptr};
    int front_ = 0;

    int left_ = 0, top_ = 0, right_ = 0, bottom_ = 0;
    bool visible_ = true;
    static inline MovieCore *drawn_ = nullptr; // the movie drawn in the overlay slot
};

/// KiriKiri's video overlay object over MovieCore (overlay and layer modes differ in how a
/// frame is shown).
class Player : public iTVPVideoOverlay, public tTVPContinuousEventCallbackIntf {
public:
    Player(tTJSNI_VideoOverlay *owner, bool layer) : core_(owner, layer) {}
    ~Player() { TVPRemoveContinuousEventHook(this); }

    bool open(const ttstr &name) {
        tTJSBinaryStream *stream = TVPCreateStream(name, TJS_BS_READ);
        return core_.open(stream, name);
    }
    void OnContinuousCallback(tjs_uint64) override { core_.tick(); }

    void AddRef() override { ++references_; }
    void Release() override {
        if (--references_ == 0) delete this;
    }
    void SetWindow(class tTJSNI_Window *) override {}
    void SetMessageDrainWindow(void *) override {}
    void SetRect(int l, int t, int r, int b) override { core_.setRect(l, t, r, b); }
    void SetVisible(bool b) override { core_.setVisible(b); }
    void Play() override {
        core_.play();
        TVPAddContinuousEventHook(this);
    }
    void Stop() override {
        core_.stop();
        TVPRemoveContinuousEventHook(this);
    }
    void Pause() override { core_.pause(); }
    void SetPosition(uint64_t tick) override { core_.seek(int64_t(tick)); }
    void GetPosition(uint64_t *tick) override { if (tick) *tick = uint64_t(core_.position()); }
    void GetStatus(tTVPVideoStatus *status) override { if (status) *status = core_.status(); }
    void Rewind() override { core_.seek(0); }
    void SetFrame(int f) override { core_.seek(int64_t(f * 1000.0 / core_.fps_)); }
    void GetFrame(int *f) override { if (f) *f = int(core_.position() * core_.fps_ / 1000.0); }
    void GetFPS(double *f) override { if (f) *f = core_.fps_; }
    void GetNumberOfFrame(int *f) override { if (f) *f = int(core_.durationMs_ * core_.fps_ / 1000.0); }
    void GetTotalTime(int64_t *t) override { if (t) *t = core_.durationMs_; }
    void GetVideoSize(long *w, long *h) override {
        if (w) *w = core_.width_;
        if (h) *h = core_.height_;
    }
    tTVPBaseTexture *GetFrontBuffer() override { return core_.frontBuffer(); }
    void SetVideoBuffer(tTVPBaseTexture *a, tTVPBaseTexture *b, long) override { core_.setBuffers(a, b); }
    void SetStopFrame(int) override {}
    void GetStopFrame(int *f) override { if (f) *f = 0; }
    void SetDefaultStopFrame() override {}
    void SetPlayRate(double) override {}
    void GetPlayRate(double *r) override { if (r) *r = 1.0; }
    void SetAudioBalance(long) override {}
    void GetAudioBalance(long *b) override { if (b) *b = 0; }
    void SetAudioVolume(long v) override { core_.setVolume(v); }
    void GetAudioVolume(long *v) override { if (v) *v = core_.volume_; }
    void GetNumberOfAudioStream(unsigned long *c) override { if (c) *c = 1; }
    void SelectAudioStream(unsigned long) override {}
    void GetEnableAudioStreamNum(long *n) override { if (n) *n = 0; }
    void DisableAudioStream() override {}
    void GetNumberOfVideoStream(unsigned long *c) override { if (c) *c = 1; }
    void SelectVideoStream(unsigned long) override {}
    void GetEnableVideoStreamNum(long *n) override { if (n) *n = 0; }
    void SetMixingBitmap(class tTVPBaseTexture *, float) override {}
    void ResetMixingBitmap() override {}
    void SetMixingMovieAlpha(float) override {}
    void GetMixingMovieAlpha(float *a) override { if (a) *a = 1.0f; }
    void SetMixingMovieBGColor(unsigned long) override {}
    void GetMixingMovieBGColor(unsigned long *c) override { if (c) *c = 0xFF000000; }
    void PresentVideoImage() override {}
    void GetContrastRangeMin(float *) override {}
    void GetContrastRangeMax(float *) override {}
    void GetContrastDefaultValue(float *) override {}
    void GetContrastStepSize(float *) override {}
    void GetContrast(float *) override {}
    void SetContrast(float) override {}
    void GetBrightnessRangeMin(float *) override {}
    void GetBrightnessRangeMax(float *) override {}
    void GetBrightnessDefaultValue(float *) override {}
    void GetBrightnessStepSize(float *) override {}
    void GetBrightness(float *) override {}
    void SetBrightness(float) override {}
    void GetHueRangeMin(float *) override {}
    void GetHueRangeMax(float *) override {}
    void GetHueDefaultValue(float *) override {}
    void GetHueStepSize(float *) override {}
    void GetHue(float *) override {}
    void SetHue(float) override {}
    void GetSaturationRangeMin(float *) override {}
    void GetSaturationRangeMax(float *) override {}
    void GetSaturationDefaultValue(float *) override {}
    void GetSaturationStepSize(float *) override {}
    void GetSaturation(float *) override {}
    void SetSaturation(float) override {}
    void SetLoopSegement(int, int) override {}

private:
    MovieCore core_;
    int references_ = 1;
};

iTVPVideoOverlay *makePlayer(tTJSNI_VideoOverlay *owner, const tjs_char *name, bool layer) {
    auto *player = new Player(owner, layer);
    if (!player->open(ttstr(name))) {
        player->Release();
        TVPThrowExceptionMessage((ttstr(TJS_W("ShioriKrkr can't play this movie: ")) + name).c_str());
    }
    return player;
}

} // namespace
} // namespace shiorikrkr

// The environ's video overlay factories krkr2's VideoOverlay class calls (the stream it opened
// is left alone: the player opens the storage itself, to read it as it plays).
struct IStream;
void GetVideoOverlayObject(tTJSNI_VideoOverlay *owner, IStream *, const tjs_char *name, const tjs_char *, uint64_t,
                           iTVPVideoOverlay **out) {
    *out = shiorikrkr::makePlayer(owner, name, false);
}
void GetVideoLayerObject(tTJSNI_VideoOverlay *owner, IStream *, const tjs_char *name, const tjs_char *, uint64_t,
                         iTVPVideoOverlay **out) {
    *out = shiorikrkr::makePlayer(owner, name, true);
}
void GetMixingVideoOverlayObject(tTJSNI_VideoOverlay *owner, IStream *, const tjs_char *name, const tjs_char *, uint64_t,
                                 iTVPVideoOverlay **out) {
    *out = shiorikrkr::makePlayer(owner, name, false);
}
void GetMFVideoOverlayObject(tTJSNI_VideoOverlay *owner, IStream *, const tjs_char *name, const tjs_char *, uint64_t,
                             iTVPVideoOverlay **out) {
    *out = shiorikrkr::makePlayer(owner, name, false);
}
