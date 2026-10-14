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
// current scenario's names, and the lookahead starts after it. Scenarios may name a picture
// by part of its file name (Yuzusoft's "繁華街c" for "その他_繁華街c.png"): the pictures in the
// auto path folders are indexed by their names and by each part after an underscore. Nothing here changes what the
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
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

bool TVPShioriResolveGraphic(const ttstr &name, ttstr &placed, ttstr &province);
iTVPTexture2D *TVPShioriDecodeGraphic(const ttstr &placed, std::vector<tTVPGraphicMetaInfoPair> **meta);
bool TVPShioriGraphicCached(const ttstr &nname);
extern tjs_uint TVPShioriAutoPathGeneration;
void TVPShioriAutoPathNames(std::vector<ttstr> &out);

namespace {

constexpr size_t kAhead = 3;          // pictures decoded ahead of the last one loaded
constexpr size_t kAheadFast = 8;      // the same while pictures come quickly (skipping)
constexpr size_t kWindow = 80;        // names looked through after it
constexpr size_t kWindowFast = 240;
constexpr size_t kThreads = 2;        // decoding threads
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
        static Prefetcher *p = new Prefetcher(); // lives for the process (its threads too)
        return *p;
    }

    void noteScenario(const std::string &name, const uint8_t *bytes, size_t size) {
        ensureIndex();
        Scenario s;
        s.name = name;
        std::string run;
        bool wide = false;
        auto flush = [&] {
            // A picture's name has a digit or an underscore (bg003_04, ev01a, bg_c_white); plain
            // words of the text don't. A name in another script counts when a picture has it.
            const bool named = wide ? (run.size() <= 64 && index_.count(keyOf(run)))
                                    : (run.size() >= 3 && run.find_first_of("0123456789_") != std::string::npos);
            wide = false;
            if (named) {
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
            if (c >= 0x80) {
                run += char(c);
                wide = true;
            } else if (std::isalnum(c) || c == '_' || c == '-') run += char(c);
            else flush();
        }
        flush();
        if (s.keys.empty()) return;
        scenarios_.erase(std::remove_if(scenarios_.begin(), scenarios_.end(), [&](const Scenario &o) { return o.name == name; }),
                         scenarios_.end());
        scenarios_.push_front(std::move(s));
        if (scenarios_.size() > kScenarios) scenarios_.pop_back();
        trace("scenario " + name + ": " + std::to_string(scenarios_.front().keys.size()) + " names");
        // Its first pictures start now (the game usually shows one as the scenario begins).
        schedule(scenarios_.front(), 0, std::string());
    }

    // The game loaded `nname` (engine thread): start the next pictures after it.
    void used(const std::string &nname) {
        const std::string key = keyOf(nname);
        if (key.empty()) return;
        // Skipping (four or more pictures in the last second): further ahead, more at once.
        const auto now = std::chrono::steady_clock::now();
        recent_.push_back(now);
        while (!recent_.empty() && now - recent_.front() > std::chrono::seconds(1)) recent_.pop_front();
        fast_ = recent_.size() >= 4;
        // The scenario may name it by its whole name or by a part after an underscore.
        std::vector<std::string> names{key};
        for (size_t u = key.find('_'); u != std::string::npos && u + 1 < key.size(); u = key.find('_', u + 1))
            names.push_back(key.substr(u + 1));
        for (const std::string &n : names) {
            for (size_t i = 0; i < scenarios_.size(); ++i) {
                auto it = scenarios_[i].first.find(n);
                if (it == scenarios_[i].first.end()) continue;
                if (i) std::rotate(scenarios_.begin(), scenarios_.begin() + long(i), scenarios_.begin() + long(i) + 1);
                schedule(scenarios_.front(), it->second + 1, key);
                return;
            }
        }
    }

    // The decoded texture for `nname`, waiting for a decode under way; nullptr when there's none.
    iTVPTexture2D *take(const std::string &nname, std::vector<tTVPGraphicMetaInfoPair> **meta, ttstr *province) {
        const std::string key = keyOf(nname);
        std::unique_lock<std::mutex> lock(mutex_);
        // Not started yet: the engine thread decodes it now, as it always did.
        queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [&](const Job &j) { return j.key == key; }), queue_.end());
        if (decoding_.count(key)) {
            const auto started = std::chrono::steady_clock::now();
            done_.wait_for(lock, std::chrono::seconds(2), [&] { return !decoding_.count(key); });
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
        index_.clear();
        indexGeneration_ = ~0u;
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
    Prefetcher() {
        for (size_t i = 0; i < kThreads; ++i) std::thread([this] { run(); }).detach();
    }

    void schedule(const Scenario &s, size_t from, const std::string &current) {
        std::vector<Job> wanted;
        size_t resolves = 0;
        const size_t ahead = fast_ ? kAheadFast : kAhead, window = fast_ ? kWindowFast : kWindow;
        for (size_t i = from; i < s.keys.size() && i < from + window && wanted.size() < ahead; ++i) {
            const std::string &name = s.keys[i];
            if (name == current) continue;
            auto r = resolved_.find(name);
            if (r == resolved_.end()) {
                if (resolves++ >= kResolvesPerCall) break;
                Resolved fresh;
                auto indexed = index_.find(name);
                const ttstr storage = indexed != index_.end() ? indexed->second : ttstr(name.c_str());
                fresh.image = TVPShioriResolveGraphic(storage, fresh.placed, fresh.province);
                r = resolved_.emplace(name, fresh).first;
            }
            if (!r->second.image) continue;
            // Kept under the file's own name (what the game loads it by).
            const std::string key = keyOf(r->second.placed.AsStdString());
            if (key == current) continue;
            if (std::any_of(wanted.begin(), wanted.end(), [&](const Job &j) { return j.key == key; })) continue;
            if (TVPShioriGraphicCached(r->second.placed)) continue;
            wanted.push_back({key, r->second.placed, r->second.province});
        }
        std::lock_guard<std::mutex> lock(mutex_);
        maxReady_ = ahead + 2;
        // What's queued and no longer wanted (the story moved on) is dropped.
        queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
                                    [&](const Job &j) {
                                        return std::none_of(wanted.begin(), wanted.end(), [&](const Job &w) { return w.key == j.key; });
                                    }),
                     queue_.end());
        for (Job &j : wanted) {
            if (ready_.count(j.key) || decoding_.count(j.key)) continue;
            if (std::any_of(queue_.begin(), queue_.end(), [&](const Job &q) { return q.key == j.key; })) continue;
            trace("queued " + j.key);
            queue_.push_back(std::move(j));
        }
        wake_.notify_all();
    }

    void run() {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [&] { return !queue_.empty(); });
                job = std::move(queue_.front());
                queue_.pop_front();
                decoding_.insert(job.key);
            }
            const auto started = std::chrono::steady_clock::now();
            Decoded d;
            d.texture = TVPShioriDecodeGraphic(job.placed, &d.meta);
            d.province = job.province;
            d.ext = extOf(job.placed.AsStdString());
            const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                decoding_.erase(job.key);
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
                    while (readyOrder_.size() > maxReady_) {
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

    // The pictures in the auto path folders by name, and by each part of the name after an
    // underscore (the first picture with that part); rebuilt when the folders change.
    void ensureIndex() {
        if (indexGeneration_ == TVPShioriAutoPathGeneration && !index_.empty()) return;
        std::vector<ttstr> names;
        try {
            TVPShioriAutoPathNames(names);
        } catch (...) {
            return;
        }
        indexGeneration_ = TVPShioriAutoPathGeneration;
        index_.clear();
        static const char *const kImages[] = {".png", ".tlg", ".jpg", ".jpeg", ".bmp", ".webp", ".jxr", ".tga"};
        for (const ttstr &n : names) {
            const std::string name = n.AsStdString();
            const std::string ext = extOf(name);
            if (std::none_of(std::begin(kImages), std::end(kImages), [&](const char *e) { return ext == e; })) continue;
            const std::string key = keyOf(name);
            if (key.empty()) continue;
            index_[key] = n;
            for (size_t u = key.find('_'); u != std::string::npos && u + 1 < key.size(); u = key.find('_', u + 1))
                index_.emplace(key.substr(u + 1), n);
        }
        resolved_.clear();
        trace("index: " + std::to_string(index_.size()) + " names");
    }

    // Engine thread only.
    std::unordered_map<std::string, ttstr> index_;
    tjs_uint indexGeneration_ = ~0u;
    std::deque<Scenario> scenarios_;
    std::unordered_map<std::string, Resolved> resolved_;
    // Shared with the decoding thread.
    std::mutex mutex_;
    std::condition_variable wake_, done_;
    std::deque<Job> queue_;
    std::set<std::string> decoding_;
    size_t maxReady_ = kAhead + 2;
    // Engine thread: when the last pictures were loaded, and whether that's quick (skipping).
    std::deque<std::chrono::steady_clock::time_point> recent_;
    bool fast_ = false;
    std::map<std::string, Decoded> ready_;
    std::vector<std::string> readyOrder_;
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
