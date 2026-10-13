/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

// Shiori's environment for krkr2's core: what krkr2's cocos2d-x environ used to provide
// (window layers, input state, audio output, platform paths, configuration), and the
// internal API between those parts and the host interface (shiorikrkr_host.mm).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class iTVPTexture2D;
class tTJSNI_Window;

namespace shiori {

// ---- configuration (shiori_config.cpp) ----
void setConfigValue(const std::string &name, const std::string &value);
void clearConfigValues();

// ---- host services (shiori_platform.mm) ----
struct HostServices {
    std::function<void(const std::string &)> log;
    // Returns the button index, or -1.
    std::function<int(const std::string &text, const std::string &caption, const std::vector<std::string> &buttons)>
        messageBox;
    std::string gameRoot;   // the folder the engine opens, ends with '/'
    std::string exeName;    // the PC executable's name, never opened
    std::string dataPath;   // where the engine may write its own files (ends with '/')
};
HostServices &host();
void log(const std::string &line);
// Set when the game asked to quit (System.terminate, the main window closing).
bool exitRequested();
void requestExit(int code);
void resetExit();

// ---- window layers (shiori_window.mm) ----
// The window the host shows: the main window if there is one, else the last visible one.
class WindowLayer;
WindowLayer *activeWindowLayer();
WindowLayer *mainWindowLayer();
// The texture the window last showed (the layer manager's composed buffer), or null.
iTVPTexture2D *windowTexture(WindowLayer *layer);
// The canvas size of the window (its inner size in the game's pixels).
void windowCanvasSize(WindowLayer *layer, int &width, int &height);
// A serial that changes whenever the window was shown again.
uint64_t windowFrameSerial(WindowLayer *layer);
tTJSNI_Window *windowInstance(WindowLayer *layer);

// Input, in the canvas's pixels. Delivered to the active window.
void pointerMove(int x, int y);
void pointerDown(int button, int x, int y);
void pointerUp(int button, int x, int y);
void wheel(int delta, int x, int y);
void keyDown(int virtualKey);
void keyUp(int virtualKey);
void lastPointer(int &x, int &y);
// Keeps the pointer position the scripts read (Window.cursorX/Y, System.getCursorPos).
void setLastPointer(int x, int y);

// ---- the cursor the game chose ----
// cursor: KiriKiri's cursor number (0 arrow, -1 none ...); storage: the cursor file's
// storage name when the number is one the game loaded from a file, else empty.
extern void (*cursorChanged)(int cursor, const std::string &storage);

// ---- audio (shiori_audio.mm) ----
void audioSuspend(bool suspended);
void audioShutdown();

} // namespace shiori
