/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

// Shiori's image prefetcher: a scene change in these games loads a background or event picture
// on the frame thread, and a 3360x2240 PNG takes over 100 ms to decode there. The scenario
// files (PSB .scn) name their pictures in the order the scenes use them, so each time the game
// loads a picture the next couple named after it start decoding on a thread of their own, into
// renderer textures; when the game asks for one, krkr2's loader takes it from here (or waits
// for the rest of a decode already under way) instead of decoding it again.
//
// The position comes from the pictures themselves: the last one loaded is found among the
// current scenario's names, and the lookahead starts after it. Nothing here changes what the
// game sees: the same storage, the same pixels, the same province image and meta information.
// SHIORIKRKR_PREFETCH=0 turns it off; SHIORIKRKR_TRACE_PREFETCH=1 logs what it does.
#include "tjsCommHead.h"
#include "GraphicsLoaderIntf.h"
#include "RenderManager.h"
#include "DebugIntf.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

bool TVPShioriResolveGraphic(const ttstr &name, ttstr &placed, ttstr &province);
iTVPTexture2D *TVPShioriDecodeGraphic(const ttstr &placed, std::vector<tTVPGraphicMetaInfoPair> **meta);
bool TVPShioriGraphicCached(const ttstr &nname);

namespace {

constexpr size_t kAhead = 2;          // pictures decoded ahead of the last one loaded
constexpr size_t kWindow = 80;        // names looked through after it
constexpr size_t kResolvesPerCall = 24; // names checked against storage per load (the engine thread)
constexpr size_t kScenarios = 6;      // scenarios remembered (the newest is searched first)

bool enabled() {
    static const bool on = [] {
        const char *e = getenv("SHIORIKRKR_PREFETCH");
        return !(e && std::string(e) == "0");
    }();
    return on;
}
bool tracing() {
    static const bool on = getenv("SHIORIKRKR_TRACE_PREFETCH") != nullptr;
    return on;
}
void trace(const std::string &line) {
    if (tracing()) TVPAddLog(ttstr(("Shiori prefetch: " + line).c_str()));
}

// A picture's key: its base name in lower case, without folders, archive or extension.
std::string keyOf(const std::string &name) {
    size_t start = name.find_last_of("/\\>");
    start = start == std::string::npos ? 0 : start + 1;
    size_t end = name.find('.', start);
    std::string key = name.substr(start, end == std::string::npos ? std::string::npos : end - start);
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return key;
}
std::string extOf(const std::string &name) {
    size_t start = name.find_last_of("/\\>");
    size_t dot = name.rfind('.');
    if (dot == std::string::npos || (start != std::string::npos && dot < start)) return {};
    std::string ext = name.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return ext;
}

struct Decoded {
    iTVPTexture2D *texture = nullptr;
    std::vector<tTVPGraphicMetaInfoPair> *meta = nullptr;
    ttstr province;
    std::string ext;
};
struct Job {
    std::string key;
    ttstr placed, province;
};
struct Resolved {
    bool image = false;
    ttstr placed, province;
};
struct Scenario {
    std::string name;
    std::vector<std::string> keys; // the scenario's names, in order (duplicates in a row folded)
    std::unordered_map<std::string, size_t> first; // where each name first appears
};

class Prefetcher {
public:
    static Prefetcher &shared() {
        static Prefetcher *p = new Prefetcher(); // lives for the process (its thread too)
        return *p;
    }

    void noteScenario(const std::string &name, const uint8_t *bytes, size_t size) {
        Scenario s;
        s.name = name;
        std::string run;
        auto flush = [&] {
            // A picture's name has a digit or an underscore (bg003_04, ev01a, bg_c_white); plain
            // words of the text don't.
            if (run.size() >= 3 && run.find_first_of("0123456789_") != std::string::npos) {
                std::string key = keyOf(run);
                if (!key.empty() && (s.keys.empty() || s.keys.back() != key)) {
                    s.first.emplace(key, s.keys.size());
                    s.keys.push_back(std::move(key));
                }
            }
            run.clear();
        };
        for (size_t i = 0; i < size; ++i) {
            const unsigned char c = bytes[i];
            if (std::isalnum(c) || c == '_' || c == '-') run += char(c);
            else flush();
        }
        flush();
        if (s.keys.empty()) return;
        scenarios_.erase(std::remove_if(scenarios_.begin(), scenarios_.end(), [&](const Scenario &o) { return o.name == name; }),
                         scenarios_.end());
        scenarios_.push_front(std::move(s));
        if (scenarios_.size() > kScenarios) scenarios_.pop_back();
        trace("scenario " + name + ": " + std::to_string(scenarios_.front().keys.size()) + " names");
    }

    // The game loaded `nname` (engine thread): start the next pictures after it.
    void used(const std::string &nname) {
        const std::string key = keyOf(nname);
        if (key.empty()) return;
        for (size_t i = 0; i < scenarios_.size(); ++i) {
            auto it = scenarios_[i].first.find(key);
            if (it == scenarios_[i].first.end()) continue;
            if (i) std::rotate(scenarios_.begin(), scenarios_.begin() + long(i), scenarios_.begin() + long(i) + 1);
            schedule(scenarios_.front(), it->second + 1, key);
            return;
        }
    }

    // The decoded texture for `nname`, waiting for a decode under way; nullptr when there's none.
    iTVPTexture2D *take(const std::string &nname, std::vector<tTVPGraphicMetaInfoPair> **meta, ttstr *province) {
        const std::string key = keyOf(nname);
        std::unique_lock<std::mutex> lock(mutex_);
        // Not started yet: the engine thread decodes it now, as it always did.
        queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [&](const Job &j) { return j.key == key; }), queue_.end());
        if (decoding_ == key) {
            const auto started = std::chrono::steady_clock::now();
            done_.wait_for(lock, std::chrono::seconds(2), [&] { return decoding_ != key; });
            trace("waited " +
                  std::to_string(int(std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() * 1000)) +
                  " ms for " + key);
        }
        auto it = ready_.find(key);
        if (it == ready_.end()) return nullptr;
        Decoded d = it->second;
        ready_.erase(it);
        readyOrder_.erase(std::remove(readyOrder_.begin(), readyOrder_.end(), key), readyOrder_.end());
        lock.unlock();
        const std::string ext = extOf(nname);
        if (!ext.empty() && ext != d.ext) { // another file of the same name
            d.texture->Release();
            delete d.meta;
            return nullptr;
        }
        *meta = d.meta;
        if (province) *province = d.province;
        trace("took " + key);
        return d.texture;
    }

    // A new game in the process: nothing of the last one's stays (its names, storages, textures).
    void reset() {
        scenarios_.clear();
        resolved_.clear();
        std::unique_lock<std::mutex> lock(mutex_);
        queue_.clear();
        done_.wait_for(lock, std::chrono::seconds(5), [&] { return decoding_.empty(); });
        for (auto &[key, d] : ready_) {
            d.texture->Release();
            delete d.meta;
        }
        ready_.clear();
        readyOrder_.clear();
    }

private:
    Prefetcher() : thread_([this] { run(); }) { thread_.detach(); }

    void schedule(const Scenario &s, size_t from, const std::string &current) {
        std::vector<Job> wanted;
        size_t resolves = 0;
        for (size_t i = from; i < s.keys.size() && i < from + kWindow && wanted.size() < kAhead; ++i) {
            const std::string &key = s.keys[i];
            if (key == current) continue;
            auto r = resolved_.find(key);
            if (r == resolved_.end()) {
                if (resolves++ >= kResolvesPerCall) break;
                Resolved fresh;
                fresh.image = TVPShioriResolveGraphic(ttstr(key.c_str()), fresh.placed, fresh.province);
                r = resolved_.emplace(key, fresh).first;
            }
            if (!r->second.image) continue;
            if (std::any_of(wanted.begin(), wanted.end(), [&](const Job &j) { return j.key == key; })) continue;
            if (TVPShioriGraphicCached(r->second.placed)) continue;
            wanted.push_back({key, r->second.placed, r->second.province});
        }
        std::lock_guard<std::mutex> lock(mutex_);
        // What's queued and no longer wanted (the story moved on) is dropped.
        queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
                                    [&](const Job &j) {
                                        return std::none_of(wanted.begin(), wanted.end(), [&](const Job &w) { return w.key == j.key; });
                                    }),
                     queue_.end());
        for (Job &j : wanted) {
            if (ready_.count(j.key) || decoding_ == j.key) continue;
            if (std::any_of(queue_.begin(), queue_.end(), [&](const Job &q) { return q.key == j.key; })) continue;
            trace("queued " + j.key);
            queue_.push_back(std::move(j));
        }
        wake_.notify_one();
    }

    void run() {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [&] { return !queue_.empty(); });
                job = std::move(queue_.front());
                queue_.pop_front();
                decoding_ = job.key;
            }
            const auto started = std::chrono::steady_clock::now();
            Decoded d;
            d.texture = TVPShioriDecodeGraphic(job.placed, &d.meta);
            d.province = job.province;
            d.ext = extOf(job.placed.AsStdString());
            const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                decoding_.clear();
                if (d.texture) {
                    auto old = ready_.find(job.key);
                    if (old != ready_.end()) {
                        old->second.texture->Release();
                        delete old->second.meta;
                        ready_.erase(old);
                        readyOrder_.erase(std::remove(readyOrder_.begin(), readyOrder_.end(), job.key), readyOrder_.end());
                    }
                    ready_[job.key] = d;
                    readyOrder_.push_back(job.key);
                    // At most a few decoded pictures wait here (the oldest go).
                    while (readyOrder_.size() > kAhead + 2) {
                        auto drop = ready_.find(readyOrder_.front());
                        if (drop != ready_.end()) {
                            drop->second.texture->Release();
                            delete drop->second.meta;
                            ready_.erase(drop);
                        }
                        readyOrder_.erase(readyOrder_.begin());
                    }
                }
            }
            done_.notify_all();
            if (tracing())
                fprintf(stderr, "Shiori prefetch: decoded %s in %.1f ms%s\n", job.key.c_str(), took * 1000,
                        d.texture ? "" : " (failed)");
        }
    }

    // Engine thread only.
    std::deque<Scenario> scenarios_;
    std::unordered_map<std::string, Resolved> resolved_;
    // Shared with the decoding thread.
    std::mutex mutex_;
    std::condition_variable wake_, done_;
    std::deque<Job> queue_;
    std::string decoding_;
    std::map<std::string, Decoded> ready_;
    std::vector<std::string> readyOrder_;
    std::thread thread_;
};

} // namespace

void ShioriPrefetchNoteScenario(const ttstr &name, const uint8_t *bytes, size_t size) {
    if (enabled()) Prefetcher::shared().noteScenario(name.AsStdString(), bytes, size);
}
void ShioriPrefetchUsed(const ttstr &nname) {
    if (enabled()) Prefetcher::shared().used(nname.AsStdString());
}
iTVPTexture2D *ShioriPrefetchTake(const ttstr &nname, std::vector<tTVPGraphicMetaInfoPair> **meta, ttstr *province) {
    if (!enabled()) return nullptr;
    return Prefetcher::shared().take(nname.AsStdString(), meta, province);
}
// A new game in the process: what the last one decoded or named goes.
void ShioriPrefetchReset() { Prefetcher::shared().reset(); }
