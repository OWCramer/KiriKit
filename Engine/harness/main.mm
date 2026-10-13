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
//       [-- krkrsdl3 options, e.g. -render=software]
#import <Foundation/Foundation.h>

#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "shiorikrkr.h"

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

int main(int argc, char **argv) {
    @autoreleasepool {
        std::string base, patch, work, exe = "game.exe", scheme, font;
        int frames = 600;
        bool present = false;
        int sessions = 1; // --again: leave the game and start it again in the same process
        std::multimap<int, std::pair<float, float>> clicks;
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
            else if (a == "--frames") frames = std::atoi(next().c_str());
            else if (a == "--click") {
                auto [frame, rest] = split(next());
                float x = 0, y = 0;
                std::sscanf(rest.c_str(), "%f,%f", &x, &y);
                clicks.emplace(frame, std::make_pair(x, y));
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
        [fm removeItemAtPath:root error:nil];
        [fm createDirectoryAtPath:root withIntermediateDirectories:YES attributes:nil error:nil];
        link([NSString stringWithUTF8String:base.c_str()], root, fm);
        if (!patch.empty()) link([NSString stringWithUTF8String:patch.c_str()], root, fm);
        // A fallback font for text the game draws without its own (krkrsdl3 looks for
        // default.ttf in the game folder first); the app links Shiori's bundled one.
        if (!font.empty())
            [fm createSymbolicLinkAtPath:[root stringByAppendingPathComponent:@"default.ttf"]
                     withDestinationPath:[NSString stringWithUTF8String:font.c_str()] error:nil];
        NSString *saves = [NSString stringWithUTF8String:(work + "/saves").c_str()];
        [fm createDirectoryAtPath:saves withIntermediateDirectories:YES attributes:nil error:nil];
        [fm createSymbolicLinkAtPath:[root stringByAppendingPathComponent:@"savedata"] withDestinationPath:saves error:nil];

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
        const auto started = std::chrono::steady_clock::now();
        const int status = shiorikrkr_start(&config);
        std::printf("== start: %d (%.2f s)\n", status,
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
        if (status != SHIORIKRKR_OK) return 1;

        char buffer[4096];
        int frame = 0;
        for (; frame < frames; ++frame) {
            if (!shiorikrkr_tick()) {
                std::printf("== game quit at frame %d\n", frame);
                break;
            }
            for (auto it = clicks.lower_bound(frame); it != clicks.upper_bound(frame); ++it) {
                std::printf("== frame %d: click %.3f,%.3f\n", frame, it->second.first, it->second.second);
                shiorikrkr_pointer(SHIORIKRKR_POINTER_DOWN, it->second.first, it->second.second, SHIORIKRKR_BUTTON_LEFT);
                shiorikrkr_tick();
                shiorikrkr_pointer(SHIORIKRKR_POINTER_UP, it->second.first, it->second.second, SHIORIKRKR_BUTTON_LEFT);
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
                    // The host presenter's frame too, as a PPM beside the capture.
                    const std::string ppm = it->second + ".presented.ppm";
                    if (FILE *out = std::fopen(ppm.c_str(), "wb")) {
                        std::fprintf(out, "P6\n%d %d\n255\n", presented.width, presented.height);
                        for (int y = 0; y < presented.height; ++y)
                            for (int x = 0; x < presented.width; ++x)
                                std::fwrite(presented.pixels + y * presented.pitch + x * 4, 1, 3, out);
                        std::fclose(out);
                    }
                    // And its alpha channel (a PGM), which the presenter ignores but MetalFX sees.
                    if (FILE *out = std::fopen((it->second + ".alpha.pgm").c_str(), "wb")) {
                        std::fprintf(out, "P5\n%d %d\n255\n", presented.width, presented.height);
                        for (int y = 0; y < presented.height; ++y)
                            for (int x = 0; x < presented.width; ++x)
                                std::fputc(presented.pixels[y * presented.pitch + x * 4 + 3], out);
                        std::fclose(out);
                    }
                    std::printf("== frame %d: presented %dx%d serial %llu blank %d\n", frame, presented.width,
                                presented.height, (unsigned long long)presented.serial, presented.blank);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        std::printf("== ran %d frames\n", frame);
        shiorikrkr_stop();
        std::printf("== stopped (session %d)\n", session + 1);
        }
        return 0;
    }
}
