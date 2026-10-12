/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

// ShioriKrkr's host side: the C interface (shiorikrkr.h) over krkrsdl3. krkrsdl3's own
// SDL3 application callbacks (environ/sdl3/sdl3_app.cpp: SDL_AppInit, SDL_AppEvent,
// SDL_AppIterate, SDL_AppQuit) are driven from here instead of SDL's main, so the host app
// keeps its UIApplication and run loop and ticks the engine from its own display link.
#include "shiorikrkr.h"

#define SDL_MAIN_USE_CALLBACKS
#define SDL_MAIN_NOIMPL
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <atomic>
#include <unistd.h>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "tjsCommHead.h"
#include "tjsError.h"
#include "PlatformView.h"
#include "TVPDebug.h"
#include "TVPScript.h"
#include "TVPStorage.h"
#include "WindowManager.h"
#include "XP3Archive.h"

#include "cx_decrypt.h"

#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#else
#import <AppKit/AppKit.h>
#endif

#define SHIORIKRKR_EXPORT extern "C" __attribute__((visibility("default")))


namespace {

struct Host {
    bool started = false;
    bool finished = false;
    shiorikrkr_log_fn log = nullptr;
    void *logContext = nullptr;
    std::vector<std::string> argumentStorage;
    std::vector<char *> argv;
    std::unique_ptr<shiorikrkr::CxDecryptor> cx;
    bool viewTaken = false;
};

Host &host() {
    static Host instance;
    return instance;
}

void deliverLog(const ttstr &line) {
    Host &h = host();
    if (h.log) h.log(h.logContext, line.AsStdString().c_str());
}

void hostLog(const std::string &line) {
    Host &h = host();
    if (h.log) h.log(h.logContext, line.c_str());
}

// krkrsdl3's XP3 extraction filter: called for every block read from an archive member,
// with the member's adlr hash and the block's offset in the member.
void cxFilter(tTVPXP3ExtractionFilterInfo *info, tTJSVariant *) {
    const auto &cx = host().cx;
    if (!cx || !info || !info->Buffer || info->BufferSize == 0) return;
    static const bool trace = getenv("SHIORIKRKR_TRACE_CX") != nullptr;
    if (trace) {
        const uint8_t *b = static_cast<const uint8_t *>(info->Buffer);
        fprintf(stderr, "cx %s hash=%08x off=%llu size=%u first=%02x%02x%02x%02x\n", info->FileName.AsStdString().c_str(),
                info->FileHash, (unsigned long long)info->Offset, info->BufferSize, b[0], info->BufferSize > 1 ? b[1] : 0,
                info->BufferSize > 2 ? b[2] : 0, info->BufferSize > 3 ? b[3] : 0);
    }
    cx->decrypt(info->FileHash, info->Offset, static_cast<uint8_t *>(info->Buffer), info->BufferSize);
}

SDL_Window *engineWindow() {
    int count = 0;
    SDL_Window **windows = SDL_GetWindows(&count);
    SDL_Window *window = (windows && count > 0) ? windows[0] : nullptr;
    SDL_free(windows);
    return window;
}

void windowPoint(float x, float y, int &px, int &py) {
    int width = 0, height = 0;
    TVPGetWindowSize(&width, &height);
    px = static_cast<int>(x * width);
    py = static_cast<int>(y * height);
}

tTVPMouseButton mouseButton(int32_t button) {
    switch (button) {
    case SHIORIKRKR_BUTTON_RIGHT: return mbRight;
    case SHIORIKRKR_BUTTON_MIDDLE: return mbMiddle;
    default: return mbLeft;
    }
}

// krkrsdl3 takes SDL scancodes and turns them into VK codes itself; the host speaks VK.
SDL_Scancode scancodeForVirtualKey(int32_t vk) {
    if (vk >= 0x41 && vk <= 0x5A) return SDL_Scancode(SDL_SCANCODE_A + (vk - 0x41));
    if (vk == 0x30) return SDL_SCANCODE_0;
    if (vk >= 0x31 && vk <= 0x39) return SDL_Scancode(SDL_SCANCODE_1 + (vk - 0x31));
    if (vk >= 0x70 && vk <= 0x7B) return SDL_Scancode(SDL_SCANCODE_F1 + (vk - 0x70));
    switch (vk) {
    case 0x08: return SDL_SCANCODE_BACKSPACE;
    case 0x09: return SDL_SCANCODE_TAB;
    case 0x0D: return SDL_SCANCODE_RETURN;
    case 0x10: return SDL_SCANCODE_LSHIFT;
    case 0x11: return SDL_SCANCODE_LCTRL;
    case 0x12: return SDL_SCANCODE_LALT;
    case 0x1B: return SDL_SCANCODE_ESCAPE;
    case 0x20: return SDL_SCANCODE_SPACE;
    case 0x21: return SDL_SCANCODE_PAGEUP;
    case 0x22: return SDL_SCANCODE_PAGEDOWN;
    case 0x23: return SDL_SCANCODE_END;
    case 0x24: return SDL_SCANCODE_HOME;
    case 0x25: return SDL_SCANCODE_LEFT;
    case 0x26: return SDL_SCANCODE_UP;
    case 0x27: return SDL_SCANCODE_RIGHT;
    case 0x28: return SDL_SCANCODE_DOWN;
    case 0x2E: return SDL_SCANCODE_DELETE;
    default: return SDL_SCANCODE_UNKNOWN;
    }
}

int evaluate(const std::string &expression, std::string &text) {
    if (!TVPGetScriptEngine()) {
        text = "no script engine";
        return 2;
    }
    try {
        tTJSVariant result;
        TVPExecuteExpression(ttstr(expression), &result);
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

} // namespace

SHIORIKRKR_EXPORT int shiorikrkr_start(const shiorikrkr_config_t *config) {
    Host &h = host();
    if (h.started) return SHIORIKRKR_ERROR_STATE;
    if (!config || config->struct_size < sizeof(shiorikrkr_config_t) || !config->game_root)
        return SHIORIKRKR_ERROR_ARGUMENTS;
    h.log = config->log;
    h.logContext = config->log_context;
    TVPSetOnLog(deliverLog);

    if (config->cx_scheme_path && *config->cx_scheme_path) {
        shiorikrkr::CxScheme scheme;
        std::string error;
        if (!shiorikrkr::CxScheme::load(config->cx_scheme_path, scheme, error)) {
            hostLog("ShioriKrkr: Cx scheme: " + error);
            return SHIORIKRKR_ERROR_SCHEME;
        }
        h.cx = std::make_unique<shiorikrkr::CxDecryptor>(scheme);
        TVPSetXP3ArchiveExtractionFilter(cxFilter);
        hostLog("ShioriKrkr: CxDec scheme loaded");
    }

    // argv as krkrsdl3 expects it: the executable (its folder is System.exePath), the game
    // folder, then options.
    std::string root = config->game_root;
    if (!root.empty() && root.back() != '/') root += '/';
    h.argumentStorage.clear();
    h.argumentStorage.push_back(root + (config->exe_name ? config->exe_name : "game.exe"));
    // krkrsdl3 (like Kirikiroid2) opens the game by its startup archive: data.xp3 when the
    // folder has one (its folder is then the project folder), else the folder itself.
    std::string projectData = root;
    for (const char *name : {"data.xp3", "Data.xp3", "DATA.XP3"}) {
        if (access((root + name).c_str(), R_OK) == 0) {
            projectData = root + name;
            break;
        }
    }
    h.argumentStorage.push_back(projectData);
    for (uint32_t i = 0; i < config->argument_count; ++i)
        if (config->arguments && config->arguments[i]) h.argumentStorage.emplace_back(config->arguments[i]);
    h.argv.clear();
    for (auto &argument : h.argumentStorage) h.argv.push_back(argument.data());
    h.argv.push_back(nullptr);

#if TARGET_OS_IPHONE
    // The host's display link ticks the engine: SDL mustn't run the run loop itself.
    SDL_SetiOSEventPump(false);
#endif
    SDL_SetMainReady();
    void *state = nullptr;
    SDL_AppResult result = SDL_APP_FAILURE;
    try {
        result = SDL_AppInit(&state, static_cast<int>(h.argumentStorage.size()), h.argv.data());
    } catch (const TJS::eTJS &e) {
        hostLog("ShioriKrkr: startup threw: " + e.GetMessage().AsStdString());
    } catch (...) {
        hostLog("ShioriKrkr: startup threw");
    }
    if (result == SDL_APP_FAILURE) {
        hostLog(std::string("ShioriKrkr: startup failed: ") + SDL_GetError());
        return SHIORIKRKR_ERROR_START;
    }
    h.started = true;
    h.finished = result == SDL_APP_SUCCESS;
    return SHIORIKRKR_OK;
}

SHIORIKRKR_EXPORT int shiorikrkr_tick(void) {
    Host &h = host();
    if (!h.started || h.finished) return 0;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (SDL_AppEvent(nullptr, &event) != SDL_APP_CONTINUE) h.finished = true;
    }
    if (!h.finished) {
        try {
            if (SDL_AppIterate(nullptr) != SDL_APP_CONTINUE) h.finished = true;
        } catch (const TJS::eTJS &e) {
            hostLog("ShioriKrkr: frame threw: " + e.GetMessage().AsStdString());
        } catch (...) {
            hostLog("ShioriKrkr: frame threw");
        }
    }
    return h.finished ? 0 : 1;
}

SHIORIKRKR_EXPORT void *shiorikrkr_take_view(void) {
    SDL_Window *window = engineWindow();
    if (!window) return nullptr;
    SDL_PropertiesID props = SDL_GetWindowProperties(window);
#if TARGET_OS_IPHONE
    UIWindow *uiWindow = (__bridge UIWindow *)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr);
    UIView *view = uiWindow.rootViewController.view;
    if (view && !host().viewTaken) {
        host().viewTaken = true;
        // SDL made its own window over the app's; the host shows the view in its own.
        uiWindow.hidden = YES;
    }
    return (__bridge void *)view;
#else
    NSWindow *nsWindow = (__bridge NSWindow *)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
    return (__bridge void *)nsWindow.contentView;
#endif
}

SHIORIKRKR_EXPORT void shiorikrkr_canvas_size(int32_t *width, int32_t *height) {
    std::string w, h;
    const bool ok = evaluate("Window.mainWindow !== void ? Window.mainWindow.innerWidth : 0", w) == 0 &&
                    evaluate("Window.mainWindow !== void ? Window.mainWindow.innerHeight : 0", h) == 0;
    if (width) *width = ok ? std::atoi(w.c_str()) : 0;
    if (height) *height = ok ? std::atoi(h.c_str()) : 0;
}

SHIORIKRKR_EXPORT void shiorikrkr_pointer(int32_t phase, float x, float y, int32_t button) {
    if (!host().started) return;
    int px = 0, py = 0;
    windowPoint(x, y, px, py);
    switch (phase) {
    case SHIORIKRKR_POINTER_DOWN:
        krkrsdl3::KRKR_Trig_MouseMove(px, py);
        krkrsdl3::KRKR_Trig_MouseDown(mouseButton(button), px, py);
        break;
    case SHIORIKRKR_POINTER_MOVED:
        krkrsdl3::KRKR_Trig_MouseMove(px, py);
        break;
    case SHIORIKRKR_POINTER_UP:
        krkrsdl3::KRKR_Trig_MouseUp(mouseButton(button), px, py);
        break;
    }
}

SHIORIKRKR_EXPORT void shiorikrkr_wheel(float dy, float x, float y) {
    if (!host().started) return;
    int px = 0, py = 0;
    windowPoint(x, y, px, py);
    krkrsdl3::KRKR_Trig_MouseScroll(0, static_cast<int>(dy), px, py);
}

SHIORIKRKR_EXPORT void shiorikrkr_key(int32_t virtualKey, int32_t down) {
    if (!host().started) return;
    const SDL_Scancode code = scancodeForVirtualKey(virtualKey);
    if (code == SDL_SCANCODE_UNKNOWN) return;
    if (down) krkrsdl3::KRKR_Trig_KeyDown(code);
    else krkrsdl3::KRKR_Trig_KeyUp(code);
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
        " l.saveLayerImage(path, 'bmp'); invalidate l; return 'ok'; })(\"" + escaped + "\")";
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
    SDL_AppQuit(nullptr, SDL_APP_SUCCESS);
    TVPSetXP3ArchiveExtractionFilter(nullptr);
    TVPSetOnLog(nullptr);
    h.cx.reset();
    h.started = false;
    h.finished = true;
}
