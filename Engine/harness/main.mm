/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

// The Mac harness for ShioriKrkr: runs a KiriKiri game through the same C interface the
// app uses, ticking a fixed number of frames, with scripted clicks, TJS and frame captures.
// The game's base/ (and patch/) are linked into a scratch folder, as the app does, so the
// game's own folder is never written to.
//
//   shiorikrkr-harness --base <game>/base [--patch <game>/patch] --work <scratch>
//       [--present]  (frames go to the host, as the app's Metal presenter takes them)
//       [--exe KARAKARA2.exe] [--scheme scheme.shcx] [--font default.ttf] [--frames 600]
//       [--click FRAME:X,Y]... [--eval FRAME:TJS]... [--capture FRAME:out.bmp]...
//       [-- engine settings, e.g. -renderer=metal]
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <ImageIO/ImageIO.h>
#include <chrono>

#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <execinfo.h>
#include <csignal>
#include <exception>
#include <unistd.h>
#include <map>
#include <array>
#include <string>
#include <thread>
#include <vector>

#include "shiorikrkr.h"

// A backtrace on a crash or an uncaught exception (lldb isn't always at hand).
static void printBacktrace() {
    void *frames[64];
    const int count = backtrace(frames, 64);
    std::fflush(stdout);
    backtrace_symbols_fd(frames, count, 2);
}
static void crashed(int signal) {
    std::fprintf(stderr, "== signal %d\n", signal);
    printBacktrace();
    _exit(128 + signal);
}
static void installCrashHandlers() {
    std::set_terminate([] {
        std::fprintf(stderr, "== terminate\n");
        printBacktrace();
        abort();
    });
    for (int s : {SIGSEGV, SIGBUS, SIGILL, SIGFPE}) signal(s, crashed);
}

static void logLine(void *, const char *line) {
    std::fprintf(stdout, "%s\n", line);
    std::fflush(stdout);
}

/// Links every entry of `from` into `into`, patch entries replacing base ones.
static void link(NSString *from, NSString *into, NSFileManager *fm) {
    for (NSString *name in [fm contentsOfDirectoryAtPath:from error:nil]) {
        if ([name.lowercaseString isEqualToString:@"savedata"]) continue;
        NSString *target = [into stringByAppendingPathComponent:name];
        [fm removeItemAtPath:target error:nil];
        [fm createSymbolicLinkAtPath:target withDestinationPath:[from stringByAppendingPathComponent:name] error:nil];
    }
}

// Saves a presented frame (RGBA or BGRA bytes, or the engine's Metal texture) as a PNG.
static void writePNG(const std::string &path, const shiorikrkr_frame_t &frame) {
    std::vector<uint8_t> rgba(size_t(frame.width) * frame.height * 4);
    if (frame.texture) {
        id<MTLTexture> texture = (__bridge id<MTLTexture>)frame.texture;
        id<MTLSharedEvent> event = (__bridge id<MTLSharedEvent>)frame.event;
        if (event) [event waitUntilSignaledValue:frame.event_value timeoutMS:5000];
        [texture getBytes:rgba.data() bytesPerRow:size_t(frame.width) * 4
               fromRegion:MTLRegionMake2D(0, 0, frame.width, frame.height) mipmapLevel:0];
        for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
    } else {
        for (int y = 0; y < frame.height; ++y)
            for (int x = 0; x < frame.width; ++x) {
                const uint8_t *p = frame.pixels + size_t(y) * frame.pitch + size_t(x) * 4;
                uint8_t *q = &rgba[(size_t(y) * frame.width + x) * 4];
                const bool bgra = frame.format == SHIORIKRKR_FRAME_BGRA;
                q[0] = bgra ? p[2] : p[0];
                q[1] = p[1];
                q[2] = bgra ? p[0] : p[2];
                q[3] = 255;
            }
    }
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGContextRef context = CGBitmapContextCreate(rgba.data(), frame.width, frame.height, 8, size_t(frame.width) * 4,
                                                 space, kCGImageAlphaNoneSkipLast);
    CGImageRef image = CGBitmapContextCreateImage(context);
    NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
    CGImageDestinationRef dest = CGImageDestinationCreateWithURL((__bridge CFURLRef)url, CFSTR("public.png"), 1, nullptr);
    CGImageDestinationAddImage(dest, image, nullptr);
    CGImageDestinationFinalize(dest);
    CFRelease(dest);
    CGImageRelease(image);
    CGContextRelease(context);
    CGColorSpaceRelease(space);
}

int main(int argc, char **argv) {
    installCrashHandlers();
    setvbuf(stdout, nullptr, _IOLBF, 0);
    @autoreleasepool {
        std::string base, patch, work, exe = "game.exe", scheme, font;
        // --next-base/--next-patch/--next-exe/--next-scheme: a different game as the second
        // session (leaving the first and starting another in the same process, as the app does).
        std::string nextBase, nextPatch, nextExe = "game.exe", nextScheme;
        int frames = 600;
        bool present = false;
        int sessions = 1; // --again: leave the game and start it again in the same process
        std::multimap<int, std::pair<float, float>> clicks;
        std::multimap<int, int> keys; // --key FRAME:VK (hex or decimal): pressed, then let go after a few ticks
        // --keydown FRAME:VK / --keyup FRAME:VK: a key held from one tick to another (Ctrl held is
        // KAG's skip, as the app's two-finger swipe latches it)
        std::multimap<int, std::pair<int, int>> keyChanges;
        // --swipe FRAME:X0,Y0,X1,Y1: a finger dragged across over 10 ticks (touch and pointer, as the app sends)
        std::multimap<int, std::array<float, 4>> swipes;
        std::multimap<int, float> wheels; // --wheel FRAME:DY (a notch up is 1, at the centre)
        std::multimap<int, std::string> evals, captures;
        std::vector<std::string> options;
        auto split = [](const std::string &s) {
            const size_t colon = s.find(':');
            return std::make_pair(std::atoi(s.substr(0, colon).c_str()), s.substr(colon + 1));
        };
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
            if (a == "--base") base = next();
            else if (a == "--patch") patch = next();
            else if (a == "--work") work = next();
            else if (a == "--exe") exe = next();
            else if (a == "--scheme") scheme = next();
            else if (a == "--font") font = next();
            else if (a == "--present") present = true;
            else if (a == "--again") sessions = 2;
            else if (a == "--next-base") nextBase = next(), sessions = 2;
            else if (a == "--next-patch") nextPatch = next();
            else if (a == "--next-exe") nextExe = next();
            else if (a == "--next-scheme") nextScheme = next();
            else if (a == "--frames") frames = std::atoi(next().c_str());
            else if (a == "--click") {
                auto [frame, rest] = split(next());
                float x = 0, y = 0;
                std::sscanf(rest.c_str(), "%f,%f", &x, &y);
                clicks.emplace(frame, std::make_pair(x, y));
            } else if (a == "--key") {
                auto [frame, rest] = split(next());
                keys.emplace(frame, int(std::strtol(rest.c_str(), nullptr, 0)));
            } else if (a == "--keydown" || a == "--keyup") {
                auto [frame, rest] = split(next());
                keyChanges.emplace(frame, std::make_pair(int(std::strtol(rest.c_str(), nullptr, 0)), a == "--keydown" ? 1 : 0));
            } else if (a == "--wheel") {
                auto [frame, rest] = split(next());
                wheels.emplace(frame, float(std::atof(rest.c_str())));
            } else if (a == "--swipe") {
                auto [frame, rest] = split(next());
                std::array<float, 4> v{};
                std::sscanf(rest.c_str(), "%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3]);
                swipes.emplace(frame, v);
            } else if (a == "--eval") {
                auto [frame, rest] = split(next());
                evals.emplace(frame, rest);
            } else if (a == "--capture") {
                auto [frame, rest] = split(next());
                captures.emplace(frame, rest);
            } else if (a == "--") {
                for (++i; i < argc; ++i) options.emplace_back(argv[i]);
            }
        }
        if (base.empty() || work.empty()) {
            std::fprintf(stderr, "usage: shiorikrkr-harness --base DIR --work DIR [options]\n");
            return 2;
        }

        NSFileManager *fm = NSFileManager.defaultManager;
        NSString *root = [NSString stringWithUTF8String:(work + "/game").c_str()];
        auto prepareRoot = [&]() {
        [fm removeItemAtPath:root error:nil];
        [fm createDirectoryAtPath:root withIntermediateDirectories:YES attributes:nil error:nil];
        link([NSString stringWithUTF8String:base.c_str()], root, fm);
        if (!patch.empty()) link([NSString stringWithUTF8String:patch.c_str()], root, fm);
        // A fallback font for text the game draws without its own (the engine looks for
        // default.ttf in the game folder first); the app links Shiori's bundled one.
        if (!font.empty())
            [fm createSymbolicLinkAtPath:[root stringByAppendingPathComponent:@"default.ttf"]
                     withDestinationPath:[NSString stringWithUTF8String:font.c_str()] error:nil];
        NSString *saves = [NSString stringWithUTF8String:(work + "/saves").c_str()];
        [fm createDirectoryAtPath:saves withIntermediateDirectories:YES attributes:nil error:nil];
        [fm createSymbolicLinkAtPath:[root stringByAppendingPathComponent:@"savedata"] withDestinationPath:saves error:nil];
        };
        prepareRoot();

        std::vector<const char *> optionPointers;
        for (auto &o : options) optionPointers.push_back(o.c_str());
        shiorikrkr_config_t config = {};
        config.struct_size = sizeof config;
        const std::string rootPath = root.UTF8String;
        config.game_root = rootPath.c_str();
        config.exe_name = exe.c_str();
        config.cx_scheme_path = scheme.empty() ? nullptr : scheme.c_str();
        config.arguments = optionPointers.data();
        config.argument_count = (uint32_t)optionPointers.size();
        config.log = logLine;
        // Message boxes: logged and answered with the first button.
        config.message = [](void *, const char *text, const char *caption, const char *const *, int32_t count) -> int32_t {
            std::printf("== message box [%s] %s (%d buttons) -> 0\n", caption ? caption : "", text ? text : "", count);
            return 0;
        };
        config.host_presents = present ? 1 : 0;
        // Shiori's E-mote player (ShioriKrkrEmoteHarness, a Swift library run-harness.sh
        // builds), when SHIORIKRKR_EMOTE_LIB names it.
        if (const char *lib = getenv("SHIORIKRKR_EMOTE_LIB"); lib && *lib) {
            if (void *handle = dlopen(lib, RTLD_NOW)) {
                using TableFn = const shiorikrkr_emote_host_t *(*)();
                if (auto table = reinterpret_cast<TableFn>(dlsym(handle, "shiorikrkr_emote_harness_table")))
                    config.emote = table();
            } else {
                std::printf("== E-mote library: %s\n", dlerror());
            }
        }

        for (int session = 0; session < sessions; ++session) {
        if (session == 1 && !nextBase.empty()) {
            base = nextBase;
            patch = nextPatch;
            exe = nextExe;
            scheme = nextScheme;
            work += "/next";
            root = [NSString stringWithUTF8String:(work + "/game").c_str()];
            [fm createDirectoryAtPath:root withIntermediateDirectories:YES attributes:nil error:nil];
            prepareRoot();
            config.exe_name = exe.c_str();
            config.cx_scheme_path = scheme.empty() ? nullptr : scheme.c_str();
            static std::string nextRoot;
            nextRoot = root.UTF8String;
            config.game_root = nextRoot.c_str();
            std::printf("== next game: %s (%s)\n", base.c_str(), exe.c_str());
        }
        const auto started = std::chrono::steady_clock::now();
        const int status = shiorikrkr_start(&config);
        std::printf("== start: %d (%.2f s)\n", status,
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
        if (status != SHIORIKRKR_OK) return 1;

        char buffer[4096];
        int frame = 0;
        double tickSeconds = 0, slowest = 0;
        for (; frame < frames; ++frame) {
            const auto tickStart = std::chrono::steady_clock::now();
            const int running = shiorikrkr_tick();
            if (present) {
                // As the app does after each tick: take the frame (waits for the GPU's work).
                shiorikrkr_frame_t taken = {};
                shiorikrkr_frame(&taken);
            }
            const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - tickStart).count();
            tickSeconds += took;
            slowest = std::max(slowest, took);
            if (frame % 300 == 299) {
                std::printf("== frames %d-%d: tick %.1f ms average, %.1f ms slowest\n", frame - 299, frame,
                            tickSeconds / 300 * 1000, slowest * 1000);
                tickSeconds = 0;
                slowest = 0;
            }
            if (!running) {
                std::printf("== game quit at frame %d\n", frame);
                break;
            }
            for (auto it = clicks.lower_bound(frame); it != clicks.upper_bound(frame); ++it) {
                std::printf("== frame %d: click %.3f,%.3f\n", frame, it->second.first, it->second.second);
                shiorikrkr_pointer(SHIORIKRKR_POINTER_DOWN, it->second.first, it->second.second, SHIORIKRKR_BUTTON_LEFT);
                shiorikrkr_tick();
                shiorikrkr_pointer(SHIORIKRKR_POINTER_UP, it->second.first, it->second.second, SHIORIKRKR_BUTTON_LEFT);
            }
            for (auto it = wheels.lower_bound(frame); it != wheels.upper_bound(frame); ++it) {
                std::printf("== frame %d: wheel %.1f\n", frame, it->second);
                shiorikrkr_wheel(it->second, 0.5f, 0.5f);
            }
            for (auto it = swipes.lower_bound(frame); it != swipes.upper_bound(frame); ++it) {
                const auto &v = it->second;
                std::printf("== frame %d: swipe %.3f,%.3f -> %.3f,%.3f\n", frame, v[0], v[1], v[2], v[3]);
                shiorikrkr_touch(0, v[0], v[1], 1);
                shiorikrkr_pointer(SHIORIKRKR_POINTER_DOWN, v[0], v[1], SHIORIKRKR_BUTTON_LEFT);
                for (int step = 1; step <= 10; ++step) {
                    shiorikrkr_tick();
                    const float x = v[0] + (v[2] - v[0]) * step / 10, y = v[1] + (v[3] - v[1]) * step / 10;
                    shiorikrkr_touch(1, x, y, 1);
                    shiorikrkr_pointer(SHIORIKRKR_POINTER_MOVED, x, y, SHIORIKRKR_BUTTON_LEFT);
                }
                shiorikrkr_touch(2, v[2], v[3], 1);
                shiorikrkr_pointer(SHIORIKRKR_POINTER_UP, v[2], v[3], SHIORIKRKR_BUTTON_LEFT);
            }
            for (auto it = keyChanges.lower_bound(frame); it != keyChanges.upper_bound(frame); ++it) {
                std::printf("== frame %d: key 0x%02X %s\n", frame, it->second.first, it->second.second ? "down" : "up");
                shiorikrkr_key(it->second.first, it->second.second);
            }
            for (auto it = keys.lower_bound(frame); it != keys.upper_bound(frame); ++it) {
                std::printf("== frame %d: key 0x%02X\n", frame, it->second);
                shiorikrkr_key(it->second, 1);
                for (int i = 0; i < 6; ++i) shiorikrkr_tick(); // as a held key (the app holds keys at least 0.1 s)
                shiorikrkr_key(it->second, 0);
            }
            for (auto it = evals.lower_bound(frame); it != evals.upper_bound(frame); ++it) {
                const int r = shiorikrkr_eval(it->second.c_str(), buffer, sizeof buffer);
                std::printf("== frame %d: eval [%d] %s\n", frame, r, buffer);
            }
            for (auto it = captures.lower_bound(frame); it != captures.upper_bound(frame); ++it) {
                const int r = shiorikrkr_capture(it->second.c_str());
                int32_t w = 0, h = 0;
                shiorikrkr_canvas_size(&w, &h);
                std::printf("== frame %d: capture %s -> %d (canvas %dx%d)\n", frame, it->second.c_str(), r, w, h);
                shiorikrkr_frame_t presented = {};
                if (shiorikrkr_frame(&presented)) {
                    // The host presenter's frame too, as a PNG beside the capture.
                    writePNG(it->second + ".presented.png", presented);
                    std::printf("== frame %d: presented %dx%d serial %llu blank %d\n", frame, presented.width,
                                presented.height, (unsigned long long)presented.serial, presented.blank);
                }
            }
            // On a 60 Hz schedule from the start (as a display link): frame N comes N/60 s in,
            // however long the ticks took (so captures line up with another engine's).
            {
                static std::chrono::steady_clock::time_point origin;
                if (frame == 0) origin = std::chrono::steady_clock::now();
                const auto due = origin + std::chrono::microseconds(int64_t(frame + 1) * 1000000 / 60);
                if (std::chrono::steady_clock::now() < due) std::this_thread::sleep_until(due);
            }
        }
        std::printf("== ran %d frames\n", frame);
        shiorikrkr_stop();
        std::printf("== stopped (session %d)\n", session + 1);
        }
        // The engine's threads outlive the game; leave without running static destructors.
        std::fflush(stdout);
        _exit(0);
    }
}
