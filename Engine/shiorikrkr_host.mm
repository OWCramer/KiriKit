/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

// ShioriKrkr's host side: the C interface (shiorikrkr.h) over krkr2's core and Shiori's own
// environment (environ/). The host app keeps its run loop and ticks the engine from its display
// link; each tick runs krkr2's application step (events, timers, scripts, layer composition).
#include "shiorikrkr.h"

#include "tjsCommHead.h"
#include "tjsError.h"
#include "Application.h"
#include "DebugIntf.h"
#include "EventIntf.h"
#include "ScriptMgnIntf.h"
#include "StorageIntf.h"
#include "SysInitIntf.h"
#include "RenderManager.h"
#include "XP3Archive.h"
#include "BinaryStream.h"
#include "WindowImpl.h"

#include "environ/shiori_environ.h"
#include "cx_decrypt.h"
#include "nekoworks.h"
#include "cursor_decode.h"
#include "shiorikrkr_emote.h"
#include "render/shiori_metal_render.h"
#include "shiorikrkr_movie.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/base_sink.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unistd.h>
#include <vector>

#import <Foundation/Foundation.h>

#define SHIORIKRKR_EXPORT extern "C" __attribute__((visibility("default")))

extern int _argc;
extern char **_argv;
extern bool TVPTerminated;
void TVPShioriPrepareRestart();
static unsigned sessionCount = 0;
unsigned shiorikrkr::sessionGeneration() { return sessionCount; }

void TVPClearScnearioCache();            // krkr2's KAGParser (sic)
void TVPShioriKAGExClearScenarioCache(); // KAGParserEx's, under Shiori's name
static double profileFrameWait = 0; // seconds frame() waited for the GPU since the last profile line
extern double TVPShioriRenderSeconds;
extern double TVPShioriImageLoadSeconds;
double ShioriEmoteLoadSeconds = 0;
extern unsigned TVPShioriRenderCalls;
double ShioriEmoteDrawSeconds = 0;

namespace {

struct Host {
    bool started = false;
    bool finished = false;
    int tickDepth = 0;
    shiorikrkr_log_fn log = nullptr;
    void *logContext = nullptr;
    shiorikrkr_message_fn message = nullptr;
    void *messageContext = nullptr;
    std::vector<std::string> argumentStorage;
    std::vector<char *> argv;
    std::unique_ptr<shiorikrkr::CxDecryptor> cx;
    shiorikrkr::NekoWorksKey neko;
    // The game's cursor: the last one set, decoded files by cursor number.
    int cursor = 0;
    uint32_t cursorSerial = 0;
    std::map<int, std::shared_ptr<shiorikrkr::DecodedCursor>> cursorFiles;
    // Pointer input waiting for the engine. A button goes down a tick after the pointer
    // arrives and comes up at least a tick later: KAG's buttons light up on the move and
    // only then take a click, which a tap delivered all at once never was.
    struct PendingInput {
        bool button;
        std::function<void()> deliver;
    };
    std::deque<PendingInput> input;
    // The frame handed out last.
    std::vector<uint8_t> frameCopy;
    // The host presents frames (as Metal textures, when the engine composes on the GPU).
    bool presents = false;
    // Some frame had something on it (no more looking for blank ones).
    bool sawContent = false;
    uint64_t presentedSerial = 0;
};

Host &host() {
    static Host instance;
    return instance;
}

void hostLog(const std::string &line) {
    Host &h = host();
    if (h.log) h.log(h.logContext, line.c_str());
    else fprintf(stderr, "%s\n", line.c_str());
}

// spdlog's loggers ("core", "tjs2", "plugin") write to the host's log.
template <typename Mutex>
class HostSink : public spdlog::sinks::base_sink<Mutex> {
protected:
    void sink_it_(const spdlog::details::log_msg &msg) override {
        hostLog(std::string(msg.payload.data(), msg.payload.size()));
    }
    void flush_() override {}
};

void installLoggers() {
    static bool done = false;
    if (done) return;
    done = true;
    auto sink = std::make_shared<HostSink<std::mutex>>();
    for (const char *name : {"core", "tjs2", "plugin"}) {
        auto logger = std::make_shared<spdlog::logger>(name, sink);
        logger->set_level(spdlog::level::info);
        spdlog::register_logger(logger);
    }
    spdlog::set_default_logger(std::make_shared<spdlog::logger>("shiori", sink));
}

// XP3 extraction filter: called for every block read from an archive member, with the
// member's adlr hash and the block's offset in the member.
void xp3Filter(tTVPXP3ExtractionFilterInfo *info, tTJSVariant *) {
    if (!info || !info->Buffer || info->BufferSize == 0) return;
    Host &h = host();
    if (h.neko.valid) {
        shiorikrkr::decodeNekoWorks(h.neko, info->FileHash, info->Offset, static_cast<uint8_t *>(info->Buffer),
                                    info->BufferSize);
        return;
    }
    if (h.cx) h.cx->decrypt(info->FileHash, info->Offset, static_cast<uint8_t *>(info->Buffer), info->BufferSize);
}

void cursorChanged(int cursor, const std::string &storage) {
    Host &h = host();
    if (cursor == h.cursor) return;
    h.cursor = cursor;
    h.cursorSerial++;
    if (!storage.empty() && !h.cursorFiles.count(cursor)) {
        auto decoded = std::make_shared<shiorikrkr::DecodedCursor>();
        try {
            std::unique_ptr<tTJSBinaryStream> stream(TVPCreateBinaryStreamForRead(ttstr(storage.c_str()), TJS_W("")));
            std::vector<uint8_t> bytes(size_t(stream->GetSize()));
            if (!bytes.empty()) stream->ReadBuffer(bytes.data(), tjs_uint(bytes.size()));
            if (!shiorikrkr::decodeCursor(bytes.data(), bytes.size(), *decoded)) decoded.reset();
        } catch (...) {
            decoded.reset();
        }
        hostLog("ShioriKrkr: cursor " + storage + (decoded ? "" : " (couldn't decode)"));
        h.cursorFiles[cursor] = decoded;
    }
}

int evaluate(const std::string &expression, std::string &text) {
    if (!TVPGetScriptEngine()) {
        text = "no script engine";
        return 2;
    }
    try {
        // Development aid: "#strings:<storage>" lists the identifiers in a compiled (TJS2
        // bytecode) script's string table, to see which members a script uses.
        // "#seq:<storage>" lists them in the order they're stored.
        const bool sequence = expression.rfind("#seq:", 0) == 0;
        if (sequence || expression.rfind("#strings:", 0) == 0) {
            std::unique_ptr<tTJSBinaryStream> stream(
                TVPCreateBinaryStreamForRead(ttstr(expression.substr(sequence ? 5 : 9).c_str()), TJS_W("")));
            std::vector<uint8_t> bytes(size_t(stream->GetSize()));
            if (!bytes.empty()) stream->ReadBuffer(bytes.data(), tjs_uint(bytes.size()));
            std::set<std::string> found;
            std::vector<std::string> ordered;
            std::u16string run;
            auto flush = [&] {
                if (run.size() >= 2) {
                    const std::string s = ttstr(reinterpret_cast<const tjs_char *>(run.c_str())).AsStdString();
                    if (found.insert(s).second || sequence) ordered.push_back(s);
                }
                run.clear();
            };
            for (int phase = 0; phase < (sequence ? 1 : 2); ++phase) {
                for (size_t i = size_t(phase); i + 1 < bytes.size(); i += 2) {
                    const char16_t c = char16_t(bytes[i] | bytes[i + 1] << 8);
                    if ((c >= 0x20 && c < 0x7f) || (c >= 0x3000 && c < 0xa000)) run += c;
                    else flush();
                }
                flush();
            }
            if (!sequence) std::sort(ordered.begin(), ordered.end());
            for (const auto &s : ordered) text += s + (sequence ? " | " : " ");
            return 0;
        }
        tTJSVariant result;
        TVPExecuteExpression(ttstr(expression.c_str()), &result);
        text = result.Type() == tvtVoid ? std::string() : ttstr(result).AsStdString();
        return 0;
    } catch (const TJS::eTJS &e) {
        text = e.GetMessage().AsStdString();
    } catch (const std::exception &e) {
        text = e.what();
    } catch (...) {
        text = "unknown error";
    }
    return 1;
}

void canvasPoint(float x, float y, int &px, int &py) {
    int w = 0, h = 0;
    shiori::windowCanvasSize(shiori::activeWindowLayer(), w, h);
    px = int(x * w);
    py = int(y * h);
}

// Windows key codes the host sends that KiriKiri knows by the same number.
bool knownKey(int32_t vk) { return vk > 0 && vk < 0x200; }

} // namespace

SHIORIKRKR_EXPORT int shiorikrkr_start(const shiorikrkr_config_t *config) {
    Host &h = host();
    if (h.started) return SHIORIKRKR_ERROR_STATE;
    if (!config || config->struct_size < offsetof(shiorikrkr_config_t, emote) || !config->game_root)
        return SHIORIKRKR_ERROR_ARGUMENTS;
    h.log = config->log;
    h.presents = config->host_presents != 0;
    h.sawContent = false;
    h.presentedSerial = 0;
    h.logContext = config->log_context;
    h.message = config->message;
    h.messageContext = config->message_context;
    h.finished = false;
    installLoggers();
    shiorikrkr::setEmoteHost(config->struct_size >= sizeof(shiorikrkr_config_t) ? config->emote : nullptr);
    shiori::resetExit();

    std::string root = config->game_root;
    if (!root.empty() && root.back() != '/') root += '/';
    if (access(root.c_str(), R_OK) != 0) return SHIORIKRKR_ERROR_ARGUMENTS;

    auto &services = shiori::host();
    services.log = hostLog;
    services.messageBox = [](const std::string &text, const std::string &caption,
                             const std::vector<std::string> &buttons) -> int {
        Host &h = host();
        if (!h.message) return 0;
        std::vector<const char *> labels;
        for (const auto &b : buttons) labels.push_back(b.c_str());
        return h.message(h.messageContext, text.c_str(), caption.c_str(), labels.data(), int32_t(labels.size()));
    };
    services.gameRoot = root;
    services.exeName = config->exe_name ? config->exe_name : "game.exe";
    NSString *temp = [NSTemporaryDirectory() stringByAppendingPathComponent:@"ShioriKrkr"];
    [NSFileManager.defaultManager createDirectoryAtPath:temp withIntermediateDirectories:YES attributes:nil error:nil];
    services.dataPath = std::string(temp.UTF8String) + "/";
    shiori::cursorChanged = cursorChanged;

    // Options: "-key=value" become engine settings (renderer, default_font ...).
    shiori::clearConfigValues();
    // Shiori's Metal compositor; SHIORIKRKR_RENDERER=software for krkr2's own (comparisons).
    const char *renderer = getenv("SHIORIKRKR_RENDERER");
    shiori::setConfigValue("renderer", renderer && *renderer ? renderer : "metal");
    for (uint32_t i = 0; i < config->argument_count; ++i) {
        if (!config->arguments || !config->arguments[i]) continue;
        std::string option = config->arguments[i];
        if (option.empty() || option[0] != '-') continue;
        const size_t eq = option.find('=');
        if (eq == std::string::npos) shiori::setConfigValue(option.substr(1), "1");
        else shiori::setConfigValue(option.substr(1, eq - 1), option.substr(eq + 1));
    }

    TVPXP3FilterOnlyFlaggedMembers = false;
    TVPSetXP3ArchiveExtractionFilter(nullptr);
    if (config->cx_scheme_path && *config->cx_scheme_path) {
        shiorikrkr::CxScheme scheme;
        std::string error;
        if (!shiorikrkr::CxScheme::load(config->cx_scheme_path, scheme, error)) {
            hostLog("ShioriKrkr: Cx scheme: " + error);
            return SHIORIKRKR_ERROR_SCHEME;
        }
        h.cx = std::make_unique<shiorikrkr::CxDecryptor>(scheme);
        TVPSetXP3ArchiveExtractionFilter(xp3Filter);
        // Members the archive stores plainly (info flags 0) are read as they are, as GARbro does.
        TVPXP3FilterOnlyFlaggedMembers = true;
        hostLog("ShioriKrkr: CxDec scheme loaded");
    }
    // NEKO WORKs archives (NEKOPARA): their key comes from their own PNG and Ogg entries.
    h.neko = {};
    if (!h.cx) {
        std::string line;
        if (shiorikrkr::detectNekoWorksKey(root, h.neko, line)) {
            TVPSetXP3ArchiveExtractionFilter(xp3Filter);
            TVPXP3FilterOnlyFlaggedMembers = true;
        }
        if (!line.empty()) hostLog("ShioriKrkr: " + line);
    }

    // The game opens by its startup archive: data.xp3 when the folder has one, else the folder.
    std::string project = root;
    for (const char *name : {"data.xp3", "Data.xp3", "DATA.XP3"}) {
        if (access((root + name).c_str(), R_OK) == 0) {
            project = root + name;
            break;
        }
    }
    h.argumentStorage = {root + services.exeName, project};
    h.argv.clear();
    for (auto &argument : h.argumentStorage) h.argv.push_back(argument.data());
    h.argv.push_back(nullptr);
    _argc = 2;
    _argv = h.argv.data();

    TVPShioriPrepareRestart(); // a game played before in this process
    ++sessionCount;
    // Scenarios parsed for the last game (cached by name) are not this one's.
    TVPClearScnearioCache();
    TVPShioriKAGExClearScenarioCache();
    TVPTerminated = false;
    if (!::Application) ::Application = new tTVPApplication();
    bool ok = false;
    try {
        ok = ::Application->StartApplication(ttstr(project.c_str()));
    } catch (const TJS::eTJS &e) {
        hostLog("ShioriKrkr: startup threw: " + e.GetMessage().AsStdString());
    } catch (const std::exception &e) {
        hostLog(std::string("ShioriKrkr: startup threw: ") + e.what());
    } catch (...) {
        hostLog("ShioriKrkr: startup threw");
    }
    // StartApplication returns false both when the startup scripts are under way and when they
    // failed; a failure shows its message box and asks to quit.
    (void)ok;
    if (shiori::exitRequested() && !shiori::mainWindowLayer()) {
        hostLog("ShioriKrkr: startup failed");
        return SHIORIKRKR_ERROR_START;
    }
    h.started = true;
    return SHIORIKRKR_OK;
}

SHIORIKRKR_EXPORT int shiorikrkr_tick(void) {
    Host &h = host();
    if (!h.started || h.finished) return 0;
    if (h.tickDepth > 0) {
        // Ticked again from inside a modal window's wait (the host's display link keeps
        // running in the nested run loop): only the engine's step.
    }
    ++h.tickDepth;
    // Moves up to and including one button change.
    while (!h.input.empty()) {
        Host::PendingInput next = std::move(h.input.front());
        h.input.pop_front();
        next.deliver();
        if (next.button) break;
    }
    // Profiled unless SHIORIKRKR_PROFILE=0: the lines go to the engine's log (the app keeps it
    // in the game's logs folder), so a device run tells where frames went.
    static const bool profile = [] {
        const char *e = getenv("SHIORIKRKR_PROFILE");
        return !(e && std::string(e) == "0");
    }();
    static double lastSlowLine = 0;
    static int profileTicks = 0;
    static double profileTick = 0, profileWorst = 0;
    const auto tickStart = std::chrono::steady_clock::now();
    try {
        ::Application->Run();
        iTVPTexture2D::RecycleProcess();
        shiori::metal::flush();
    } catch (const TJS::eTJS &e) {
        hostLog("ShioriKrkr: frame threw: " + e.GetMessage().AsStdString());
    } catch (const std::exception &e) {
        hostLog(std::string("ShioriKrkr: frame threw: ") + e.what());
    } catch (...) {
        hostLog("ShioriKrkr: frame threw");
    }
    --h.tickDepth;
    if (profile) {
        const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - tickStart).count();
        profileTick += took;
        profileWorst = std::max(profileWorst, took);
        // A tick long enough to drop a frame: what it spent its time on.
        static double lastEmote = 0, lastRender = 0, lastImages = 0, lastModels = 0;
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (took > 0.020 && now - lastSlowLine >= 1.0) { // at most a line a second
            lastSlowLine = now;
            char line[240];
            snprintf(line, sizeof line,
                     "ShioriKrkr slow tick: %.1f ms (images loaded %.1f ms, E-mote models loaded %.1f ms, E-mote drawn %.1f ms, "
                     "software layer ops %.1f ms)",
                     took * 1000, (TVPShioriImageLoadSeconds - lastImages) * 1000, (ShioriEmoteLoadSeconds - lastModels) * 1000,
                     (ShioriEmoteDrawSeconds - lastEmote) * 1000, (TVPShioriRenderSeconds - lastRender) * 1000);
            hostLog(line);
        }
        lastEmote = ShioriEmoteDrawSeconds;
        lastRender = TVPShioriRenderSeconds;
        lastImages = TVPShioriImageLoadSeconds;
        lastModels = ShioriEmoteLoadSeconds;
        if (++profileTicks == 600) {
            char line[256];
            snprintf(line, sizeof line,
                     "ShioriKrkr profile (600 ticks): tick %.2f ms avg, %.1f ms worst; render ops %.2f ms (%u calls); E-mote %.2f ms",
                     profileTick * 1000 / 600, profileWorst * 1000, TVPShioriRenderSeconds * 1000 / 600, TVPShioriRenderCalls / 600,
                     ShioriEmoteDrawSeconds * 1000 / 600);
            hostLog(line);
            const std::string metal = shiori::metal::profileReport(600);
            if (!metal.empty()) hostLog(std::string("ShioriKrkr profile: frame wait ") +
                                        std::to_string(profileFrameWait * 1000 / 600).substr(0, 5) + " ms; " + metal);
            profileFrameWait = 0;
            profileTicks = 0;
            profileTick = profileWorst = 0;
            TVPShioriRenderSeconds = 0;
            TVPShioriRenderCalls = 0;
            ShioriEmoteDrawSeconds = 0;
            lastEmote = lastRender = 0;
            lastImages = TVPShioriImageLoadSeconds;
            lastModels = ShioriEmoteLoadSeconds;
        }
    }
    if (shiori::exitRequested() || ::Application->IsTarminate()) h.finished = true;
    return h.finished ? 0 : 1;
}

SHIORIKRKR_EXPORT void *shiorikrkr_take_view(void) { return nullptr; }

// The game's frame with an overlay-mode movie drawn over it at its rectangle (scaled to fit,
// nearest pixel), recomposed only when either changes. False when no movie shows.
static bool composeMovie(shiorikrkr_frame_t *frame, iTVPTexture2D *texture) {
    static std::vector<uint8_t> composed;
    static uint64_t composedGame = 0, composedMovie = 0, serial = 0;
    std::lock_guard<std::mutex> lock(shiorikrkr::movieOverlayLock());
    const shiorikrkr::MovieOverlayFrame *movie = shiorikrkr::movieOverlay();
    if (!movie || !movie->visible || movie->rgba.empty()) return false;
    const int w = frame->width, h = frame->height, pitch = w * 4;
    if (frame->serial != composedGame || movie->serial != composedMovie || composed.size() != size_t(pitch) * h) {
        composedGame = frame->serial;
        composedMovie = movie->serial;
        composed.resize(size_t(pitch) * h);
        for (int y = 0; y < h; ++y)
            memcpy(&composed[size_t(y) * pitch], texture->GetScanLineForRead(y), size_t(pitch));
        int l = movie->left, t = movie->top, r = movie->right, b = movie->bottom;
        if (r <= l || b <= t) l = 0, t = 0, r = w, b = h;
        for (int y = std::max(t, 0); y < std::min(b, h); ++y) {
            const int sy = (y - t) * movie->height / (b - t);
            uint8_t *row = &composed[size_t(y) * pitch];
            const uint8_t *source = &movie->rgba[size_t(sy) * movie->width * 4];
            for (int x = std::max(l, 0); x < std::min(r, w); ++x) {
                const int sx = (x - l) * movie->width / (r - l);
                memcpy(row + x * 4, source + sx * 4, 4);
            }
        }
        ++serial;
    }
    frame->pixels = composed.data();
    frame->pitch = pitch;
    frame->serial = (uint64_t(1) << 63) | serial;
    frame->blank = 0;
    frame->texture = nullptr;
    return true;
}

SHIORIKRKR_EXPORT int shiorikrkr_frame(shiorikrkr_frame_t *frame) {
    Host &h = host();
    if (!frame || !h.started) return 0;
    shiori::WindowLayer *window = shiori::activeWindowLayer();
    iTVPTexture2D *texture = shiori::windowTexture(window);
    if (!texture) return 0;
    int w = 0, hgt = 0;
    shiori::windowCanvasSize(window, w, hgt);
    if (w <= 0 || hgt <= 0) {
        w = int(texture->GetWidth());
        hgt = int(texture->GetHeight());
    }
    w = std::min<int>(w, int(texture->GetWidth()));
    hgt = std::min<int>(hgt, int(texture->GetHeight()));
    *frame = shiorikrkr_frame_t{};
    frame->width = w;
    frame->height = hgt;
    frame->serial = shiori::windowFrameSerial(window);
    frame->format = SHIORIKRKR_FRAME_RGBA; // krkr2 keeps pixels as R, G, B, A in memory
    frame->blank = 0;

    // A coarse look for anything not black, until there is some: the host lifts its launch
    // cover on the first such frame (the game's first frames are black while its title loads).
    if (!h.sawContent) {
        const auto *pixels = static_cast<const uint8_t *>(texture->GetScanLineForRead(0));
        const int pitch = texture->GetPitch();
        bool blank = true;
        for (int sy = 1; sy < 16 && blank; ++sy)
            for (int sx = 1; sx < 16; ++sx) {
                const uint8_t *p = pixels + size_t(sy * hgt / 16) * pitch + size_t(sx * w / 16) * 4;
                if (p[0] > 8 || p[1] > 8 || p[2] > 8) {
                    blank = false;
                    break;
                }
            }
        frame->blank = blank ? 1 : 0;
        h.sawContent = !blank;
    }

    if (composeMovie(frame, texture)) return 1;

    // Composed on the GPU: a texture the host's GPU work waits for, no wait here.
    shiori::metal::PresentedFrame presented;
    if (h.presents &&
        shiori::metal::presentFrame(texture, w, hgt, frame->serial != h.presentedSerial, presented)) {
        h.presentedSerial = frame->serial;
        frame->texture = presented.texture;
        frame->event = presented.event;
        frame->event_value = presented.value;
        return 1;
    }

    const auto waitStart = std::chrono::steady_clock::now();
    frame->pixels = static_cast<const uint8_t *>(texture->GetScanLineForRead(0));
    profileFrameWait += std::chrono::duration<double>(std::chrono::steady_clock::now() - waitStart).count();
    frame->pitch = texture->GetPitch();
    return frame->pixels ? 1 : 0;
}

SHIORIKRKR_EXPORT uint32_t shiorikrkr_cursor(const uint8_t **rgba, int32_t *width, int32_t *height,
                                             int32_t *hotX, int32_t *hotY, int32_t *kind) {
    Host &h = host();
    int32_t k = h.cursor == -1 ? 2 : 0;
    const shiorikrkr::DecodedCursor *decoded = nullptr;
    auto found = h.cursorFiles.find(h.cursor);
    if (found != h.cursorFiles.end() && found->second) {
        decoded = found->second.get();
        k = 1;
    }
    if (rgba) *rgba = decoded ? decoded->rgba.data() : nullptr;
    if (width) *width = decoded ? decoded->width : 0;
    if (height) *height = decoded ? decoded->height : 0;
    if (hotX) *hotX = decoded ? decoded->hotX : 0;
    if (hotY) *hotY = decoded ? decoded->hotY : 0;
    if (kind) *kind = k;
    return h.cursorSerial;
}

SHIORIKRKR_EXPORT void shiorikrkr_canvas_size(int32_t *width, int32_t *height) {
    int w = 0, h = 0;
    shiori::windowCanvasSize(shiori::activeWindowLayer(), w, h);
    if (width) *width = w;
    if (height) *height = h;
}

SHIORIKRKR_EXPORT void shiorikrkr_pointer(int32_t phase, float x, float y, int32_t button) {
    Host &h = host();
    if (!h.started) return;
    int px = 0, py = 0;
    canvasPoint(x, y, px, py);
    auto &queue = h.input;
    auto move = [px, py] { shiori::pointerMove(px, py); };
    switch (phase) {
    case SHIORIKRKR_POINTER_DOWN:
        // The move this tick; the press the next.
        queue.push_back({true, move});
        queue.push_back({true, [button, px, py] { shiori::pointerDown(button, px, py); }});
        break;
    case SHIORIKRKR_POINTER_MOVED:
        if (queue.empty()) move();
        else queue.push_back({false, move});
        break;
    case SHIORIKRKR_POINTER_CANCEL: {
        // A press still waiting is dropped; one already down comes up off the window.
        bool dropped = false;
        for (auto it = queue.rbegin(); it != queue.rend(); ++it) {
            if (it->button) {
                queue.erase(std::next(it).base());
                dropped = true;
                break;
            }
        }
        if (!dropped)
            queue.push_back({true, [button] {
                shiori::pointerMove(-10000, -10000);
                shiori::pointerUp(button, -10000, -10000);
            }});
        break;
    }
    case SHIORIKRKR_POINTER_UP:
        queue.push_back({true, [button, px, py] {
            shiori::pointerMove(px, py);
            shiori::pointerUp(button, px, py);
        }});
        break;
    }
}

SHIORIKRKR_EXPORT void shiorikrkr_wheel(float dy, float x, float y) {
    if (!host().started) return;
    int px = 0, py = 0;
    canvasPoint(x, y, px, py);
    // KiriKiri's wheel delta: 120 a notch, negative towards the user (scrolling up is positive).
    shiori::wheel(int(dy * 120), px, py);
}

SHIORIKRKR_EXPORT void shiorikrkr_key(int32_t virtualKey, int32_t down) {
    if (!host().started || !knownKey(virtualKey)) return;
    if (down) shiori::keyDown(virtualKey);
    else shiori::keyUp(virtualKey);
}

SHIORIKRKR_EXPORT int shiorikrkr_eval(const char *expression, char *out, uint32_t capacity) {
    std::string text;
    const int status = expression ? evaluate(expression, text) : 2;
    if (out && capacity > 0) {
        const size_t n = std::min<size_t>(text.size(), capacity - 1);
        std::memcpy(out, text.data(), n);
        out[n] = 0;
    }
    return status;
}

SHIORIKRKR_EXPORT int shiorikrkr_capture(const char *path) {
    if (!path) return 1;
    // The primary layer piled with everything over it, copied into a scratch layer and saved.
    std::string storage = TVPNormalizeStorageName(ttstr(path)).AsStdString();
    std::string escaped;
    for (char c : storage) {
        if (c == '"' || c == '\\') escaped += '\\';
        escaped += c;
    }
    const std::string script =
        "(function(path){ var w = Window.mainWindow; if (w === void) return 'no window';"
        " var p = w.primaryLayer; var l = new Layer(w, p);"
        " l.setImageSize(p.width, p.height); l.setSizeToImageSize();"
        " l.piledCopy(0, 0, p, 0, 0, p.width, p.height);"
        " l.saveLayerImage(path, 'bmp'); invalidate l; return 'ok'; })(\"" +
        escaped + "\")";
    std::string text;
    const int status = evaluate(script, text);
    if (status != 0 || text != "ok") {
        hostLog("ShioriKrkr: capture: " + text);
        return 1;
    }
    return 0;
}

SHIORIKRKR_EXPORT void shiorikrkr_stop(void) {
    Host &h = host();
    if (!h.started) return;
    try {
        if (!TVPSystemUninitCalled) TVPSystemUninit();
    } catch (...) {
        hostLog("ShioriKrkr: shutdown threw");
    }
    // Messages the game's objects posted and never saw delivered hold pointers to them.
    if (::Application) {
        ::Application->FilterUserMessage(
            [](std::vector<std::tuple<void *, int, tTVPApplication::tMsg>> &messages) { messages.clear(); });
        // The next game gets its own application object (its terminate flag, title and
        // image loading thread are this game's).
        delete ::Application;
        ::Application = nullptr;
    }
    if (const int left = shiori::forgetWindowLayers())
        hostLog("ShioriKrkr: " + std::to_string(left) + " window(s) left after shutdown");
    shiori::audioShutdown();
    shiorikrkr::setEmoteHost(nullptr);
    shiori::cursorChanged = nullptr;
    h.cursorFiles.clear();
    h.input.clear();
    h.cursor = 0;
    TVPXP3FilterOnlyFlaggedMembers = false;
    TVPSetXP3ArchiveExtractionFilter(nullptr);
    h.cx.reset();
    h.neko = {};
    h.started = false;
    h.finished = true;
}
