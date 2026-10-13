/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

// Shiori's window layer for krkr2's core: one per KiriKiri Window object (krkr2's iWindowLayer,
// which its cocos2d-x scene used to implement). It keeps the window's size, zoom, visibility and
// the texture the layer manager composed for it; the host presents that texture and sends input
// here. Keyboard and mouse button state (System.getKeyState) lives here too.
#include "shiori_environ.h"

#include "tjsCommHead.h"
#include "WindowImpl.h"
#include "WindowIntf.h"
#include "EventIntf.h"
#include "RenderManager.h"
#include "TickCount.h"
#include "SysInitIntf.h"
#include "vkdefine.h"
#include "Application.h"
#include "Random.h"
#include "VelocityTracker.h"

#import <Foundation/Foundation.h>
#include <algorithm>
#include <cmath>

void (*shiori::cursorChanged)(int cursor, const std::string &storage) = nullptr;

namespace {

// Key and mouse button state by VK code: bit 0 down now, bit 4 pressed since last asked.
tjs_uint8 scancode[0x200];
int lastX = 0, lastY = 0;

class ShioriWindowLayer;
std::vector<ShioriWindowLayer *> &windows() {
    static std::vector<ShioriWindowLayer *> list;
    return list;
}
ShioriWindowLayer *current = nullptr;

void adjustRatio(tjs_int &n, tjs_int &d) {
    tjs_int a = n, b = d;
    while (b) {
        tjs_int t = b;
        b = a % b;
        a = t;
    }
    if (a) {
        n /= a;
        d /= a;
    }
}

tjs_uint32 mouseButtonState() {
    tjs_uint32 s = 0;
    if (scancode[VK_LBUTTON] & 1) s |= ssLeft;
    if (scancode[VK_RBUTTON] & 1) s |= ssRight;
    if (scancode[VK_MBUTTON] & 1) s |= ssMiddle;
    return s;
}

int vkForButton(tTVPMouseButton b) {
    switch (b) {
    case mbRight: return VK_RBUTTON;
    case mbMiddle: return VK_MBUTTON;
    default: return VK_LBUTTON;
    }
}

} // namespace

tjs_uint32 TVPGetCurrentShiftKeyState() {
    tjs_uint32 f = 0;
    if (scancode[VK_SHIFT] & 1) f |= ssShift;
    if (scancode[VK_MENU] & 1) f |= ssAlt;
    if (scancode[VK_CONTROL] & 1) f |= ssCtrl;
    return f | mouseButtonState();
}

bool TVPGetKeyMouseAsyncState(tjs_uint keycode, bool getcurrent) {
    if (keycode >= sizeof(scancode)) return false;
    tjs_uint8 code = scancode[keycode];
    scancode[keycode] &= 1;
    return code & (getcurrent ? 1 : 0x10);
}

bool TVPGetJoyPadAsyncState(tjs_uint keycode, bool getcurrent) { return TVPGetKeyMouseAsyncState(keycode, getcurrent); }

class shiori::WindowLayer : public iWindowLayer {};

namespace {

class ShioriWindowLayer final : public shiori::WindowLayer {
public:
    tTJSNI_Window *Instance;
    tjs_int Width = 0, Height = 0;           // the window's (inner) size
    tjs_int LayerWidth = 0, LayerHeight = 0; // the paint box: the primary layer's size
    tjs_int ActualZoomNumer = 1, ActualZoomDenom = 1;
    bool Visible = false;
    std::string Caption;
    iTVPTexture2D *Texture = nullptr;
    uint64_t Serial = 0;
    bool UseMouseKey = false;
    bool Closing = false, ProgramClosing = false, CanCloseWork = false;
    bool InModal = false;
    int ModalResult = 0;
    tTVPImeMode DefaultImeMode = ::imDisable;
    VelocityTracker MouseVelocity;
    tjs_int Cursor = 0;

    explicit ShioriWindowLayer(tTJSNI_Window *w) : Instance(w) { windows().push_back(this); }

    ~ShioriWindowLayer() override {
        auto &list = windows();
        list.erase(std::remove(list.begin(), list.end(), this), list.end());
        if (current == this) current = list.empty() ? nullptr : list.back();
        if (Texture) Texture->Release();
    }

    // ---- size and zoom ----
    void SetPaintBoxSize(tjs_int w, tjs_int h) override {
        LayerWidth = w;
        LayerHeight = h;
    }
    bool GetFormEnabled() override { return Visible; }
    void SetDefaultMouseCursor() override { SetMouseCursor(0); }
    void GetCursorPos(tjs_int &x, tjs_int &y) override {
        x = lastX;
        y = lastY;
    }
    void SetCursorPos(tjs_int x, tjs_int y) override {
        lastX = x;
        lastY = y;
    }
    void SetHintText(const ttstr &) override {}
    void SetAttentionPoint(tjs_int, tjs_int, const struct tTVPFont *) override {}
    void ZoomRectangle(tjs_int &left, tjs_int &top, tjs_int &right, tjs_int &bottom) override {
        left = tjs_int64(left) * ActualZoomNumer / ActualZoomDenom;
        top = tjs_int64(top) * ActualZoomNumer / ActualZoomDenom;
        right = tjs_int64(right) * ActualZoomNumer / ActualZoomDenom;
        bottom = tjs_int64(bottom) * ActualZoomNumer / ActualZoomDenom;
    }
    void BringToFront() override {
        if (current != this) {
            if (current && current->Instance) current->Instance->OnReleaseCapture();
            current = this;
        }
    }
    void ShowWindowAsModal() override {
        InModal = true;
        ModalResult = 0;
        SetVisible(true);
        BringToFront();
        // The host keeps ticking the engine (its display link runs inside this loop) until the
        // window closes.
        while (current == this && !ModalResult) {
            if (::Application->IsTarminate() || shiori::exitRequested()) {
                ModalResult = mrCancel;
                break;
            }
            @autoreleasepool {
                CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.005, true);
            }
        }
        InModal = false;
    }
    bool GetVisible() override { return Visible; }
    void SetVisible(bool b) override {
        Visible = b;
        if (b) BringToFront();
        else if (current == this) {
            current = nullptr;
            for (auto it = windows().rbegin(); it != windows().rend(); ++it)
                if (*it != this && (*it)->Visible) {
                    current = *it;
                    break;
                }
        }
    }
    const char *GetCaption() override { return Caption.c_str(); }
    void SetCaption(const std::string &s) override { Caption = s; }
    void SetWidth(tjs_int w) override { Width = w; }
    void SetHeight(tjs_int h) override { Height = h; }
    void SetSize(tjs_int w, tjs_int h) override {
        Width = w;
        Height = h;
    }
    void GetSize(tjs_int &w, tjs_int &h) override {
        w = Width;
        h = Height;
    }
    tjs_int GetWidth() const override { return Width; }
    tjs_int GetHeight() const override { return Height; }
    void GetWinSize(tjs_int &w, tjs_int &h) override {
        w = Width;
        h = Height;
    }
    void SetZoom(tjs_int numer, tjs_int denom) override {
        adjustRatio(numer, denom);
        ZoomNumer = ActualZoomNumer = numer;
        ZoomDenom = ActualZoomDenom = denom;
    }

    // ---- drawing ----
    void UpdateDrawBuffer(iTVPTexture2D *tex) override {
        if (!tex) return;
        if (tex != Texture) {
            tex->AddRef();
            if (Texture) Texture->Release();
            Texture = tex;
        }
        ++Serial;
    }

    // ---- closing ----
    void InvalidateClose() override {
        Instance = nullptr;
        SetVisible(false);
        delete this;
    }
    bool GetWindowActive() override { return current == this; }

    bool OnCloseQuery() {
        if (TVPGetBreathing()) return false;
        if (Instance && (ModalResult == 0 || ModalResult == mrCancel)) {
            iTJSDispatch2 *obj = Instance->GetOwnerNoAddRef();
            if (!obj) return true;
            if (!ProgramClosing) {
                TVPPostInputEvent(new tTVPOnCloseInputEvent(Instance));
                Closing = true;
                return false;
            }
            CanCloseWork = true;
            tTJSVariant arg[1] = {true};
            static ttstr eventname(TJS_W("onCloseQuery"));
            TVPPostEvent(obj, obj, eventname, 0, TVP_EPT_IMMEDIATE, 1, arg);
            return CanCloseWork;
        }
        return true;
    }

    void Close() override {
        if (Closing) return;
        ProgramClosing = true;
        try {
            if (InModal) {
                ModalResult = mrCancel;
            } else if (OnCloseQuery()) {
                if (Instance) {
                    const bool isMain = Instance->IsMainWindow();
                    iTJSDispatch2 *obj = Instance->GetOwnerNoAddRef();
                    Instance->NotifyWindowClose();
                    tTJSNI_Window *instance = Instance;
                    Instance = nullptr;
                    SetVisible(false);
                    if (obj) obj->Invalidate(0, nullptr, nullptr, obj);
                    (void)instance;
                    (void)isMain;
                }
            }
        } catch (...) {
            ProgramClosing = false;
            throw;
        }
        ProgramClosing = false;
    }

    void OnCloseQueryCalled(bool b) override {
        if (!ProgramClosing) {
            if (b) {
                if (InModal) ModalResult = 1;
                else SetVisible(false);
                Closing = false;
                if (Instance) {
                    if (Instance->IsMainWindow()) {
                        iTJSDispatch2 *obj = Instance->GetOwnerNoAddRef();
                        obj->Invalidate(0, nullptr, nullptr, obj);
                    }
                }
            } else {
                Closing = false;
            }
        } else {
            CanCloseWork = b;
        }
    }

    // ---- keys ----
    void InternalKeyDown(tjs_uint16 key, tjs_uint32 shift) override {
        tjs_uint32 tick = TVPGetRoughTickCount32();
        TVPPushEnvironNoise(&tick, sizeof(tick));
        TVPPushEnvironNoise(&key, sizeof(key));
        if (Instance) TVPPostInputEvent(new tTVPOnKeyDownInputEvent(Instance, key, shift));
    }
    void OnKeyUp(tjs_uint16 vk, int shift) override {
        if (Instance) TVPPostInputEvent(new tTVPOnKeyUpInputEvent(Instance, vk, shift | mouseButtonState()));
    }
    void OnKeyPress(tjs_uint16 vk, int, bool, bool) override {
        if (Instance && vk) TVPPostInputEvent(new tTVPOnKeyPressInputEvent(Instance, vk));
    }
    tTVPImeMode GetDefaultImeMode() const override { return DefaultImeMode; }
    void SetImeMode(tTVPImeMode) override {}
    void ResetImeMode() override {}

    void UpdateWindow(tTVPUpdateType) override {
        if (!Instance) return;
        tTVPRect r(0, 0, LayerWidth, LayerHeight);
        Instance->NotifyWindowExposureToLayer(r);
        TVPDeliverWindowUpdateEvents();
    }
    void SetVisibleFromScript(bool b) override { SetVisible(b); }
    void SetUseMouseKey(bool b) override { UseMouseKey = b; }
    bool GetUseMouseKey() const override { return UseMouseKey; }
    void ResetMouseVelocity() override { MouseVelocity.clear(); }
    void ResetTouchVelocity(tjs_int) override {}
    bool GetMouseVelocity(float &x, float &y, float &speed) const override {
        if (MouseVelocity.getVelocity(x, y)) {
            speed = hypotf(x, y);
            return true;
        }
        return false;
    }
    void TickBeat() override {}
    cocos2d::Node *GetPrimaryArea() override { return nullptr; }

    void SetMouseCursor(tjs_int handle) override {
        if (handle == Cursor) return;
        Cursor = handle;
        if (shiori::cursorChanged && current == this) {
            auto &storages = TVPCursorStorages();
            auto found = storages.find(handle);
            shiori::cursorChanged(int(handle), found != storages.end() ? found->second.AsStdString() : std::string());
        }
    }

    // ---- pointer input (paint-box pixels) ----
    void mouseMove(int x, int y) {
        lastX = x;
        lastY = y;
        if (!Instance) return;
        MouseVelocity.addMovement(TVPGetRoughTickCount32(), float(x), float(y));
        TVPPostInputEvent(new tTVPOnMouseMoveInputEvent(Instance, x, y, TVPGetCurrentShiftKeyState()),
                          TVP_EPT_DISCARDABLE);
        int pos = (y << 16) + x;
        TVPPushEnvironNoise(&pos, sizeof(pos));
    }
    void mouseDown(tTVPMouseButton button, int x, int y) {
        lastX = x;
        lastY = y;
        scancode[vkForButton(button)] = 0x11;
        if (!Instance) return;
        MouseVelocity.addMovement(TVPGetRoughTickCount32(), float(x), float(y));
        TVPPostInputEvent(new tTVPOnMouseDownInputEvent(Instance, x, y, button, TVPGetCurrentShiftKeyState()));
    }
    void mouseUp(tTVPMouseButton button, int x, int y, bool click) {
        lastX = x;
        lastY = y;
        const tjs_uint32 shift = TVPGetCurrentShiftKeyState();
        scancode[vkForButton(button)] &= 0x10;
        if (!Instance) return;
        if (click) TVPPostInputEvent(new tTVPOnClickInputEvent(Instance, x, y));
        TVPPostInputEvent(new tTVPOnMouseUpInputEvent(Instance, x, y, button, shift));
    }
    void mouseWheel(int delta, int x, int y) {
        if (Instance) TVPPostInputEvent(new tTVPOnMouseWheelInputEvent(Instance, TVPGetCurrentShiftKeyState(), delta, x, y));
    }
};

ShioriWindowLayer *target() {
    if (current && current->Instance) return current;
    for (auto it = windows().rbegin(); it != windows().rend(); ++it)
        if ((*it)->Instance && (*it)->Visible) return *it;
    return windows().empty() ? nullptr : windows().back();
}

tTVPMouseButton buttonFor(int button) {
    switch (button) {
    case 1: return mbRight;
    case 2: return mbMiddle;
    default: return mbLeft;
    }
}

} // namespace

iWindowLayer *TVPCreateAndAddWindow(tTJSNI_Window *w) { return new ShioriWindowLayer(w); }

void TVPRemoveWindowLayer(iWindowLayer *layer) { delete static_cast<ShioriWindowLayer *>(layer); }

tTJSNI_Window *TVPGetActiveWindow() { return current ? current->Instance : nullptr; }

// ---- the host's side ----
shiori::WindowLayer *shiori::mainWindowLayer() {
    for (ShioriWindowLayer *w : windows())
        if (w->Instance && w->Instance->IsMainWindow()) return w;
    return nullptr;
}

shiori::WindowLayer *shiori::activeWindowLayer() {
    // A modal window over the main one is what shows.
    if (current && current->Instance && current->Visible && current->Texture) return current;
    if (WindowLayer *main = mainWindowLayer()) return main;
    return target();
}

iTVPTexture2D *shiori::windowTexture(WindowLayer *layer) {
    return layer ? static_cast<ShioriWindowLayer *>(layer)->Texture : nullptr;
}

void shiori::windowCanvasSize(WindowLayer *layer, int &width, int &height) {
    auto *w = static_cast<ShioriWindowLayer *>(layer);
    width = w ? w->LayerWidth : 0;
    height = w ? w->LayerHeight : 0;
}

uint64_t shiori::windowFrameSerial(WindowLayer *layer) {
    return layer ? static_cast<ShioriWindowLayer *>(layer)->Serial : 0;
}

tTJSNI_Window *shiori::windowInstance(WindowLayer *layer) {
    return layer ? static_cast<ShioriWindowLayer *>(layer)->Instance : nullptr;
}

void shiori::pointerMove(int x, int y) {
    if (auto *w = target()) w->mouseMove(x, y);
}

void shiori::pointerDown(int button, int x, int y) {
    if (auto *w = target()) w->mouseDown(buttonFor(button), x, y);
}

void shiori::pointerUp(int button, int x, int y) {
    if (auto *w = target()) w->mouseUp(buttonFor(button), x, y, true);
}

void shiori::wheel(int delta, int x, int y) {
    if (auto *w = target()) w->mouseWheel(delta, x, y);
}

void shiori::keyDown(int vk) {
    if (vk <= 0 || vk >= 0x200) return;
    const bool repeat = scancode[vk] & 1;
    scancode[vk] = 0x11;
    if (auto *w = target()) w->InternalKeyDown(tjs_uint16(vk), TVPGetCurrentShiftKeyState() | (repeat ? ssRepeat : 0));
}

void shiori::keyUp(int vk) {
    if (vk <= 0 || vk >= 0x200) return;
    const bool wasDown = scancode[vk] & 1;
    scancode[vk] &= 0x10;
    if (wasDown)
        if (auto *w = target()) w->OnKeyUp(tjs_uint16(vk), TVPGetCurrentShiftKeyState());
}

void shiori::lastPointer(int &x, int &y) {
    x = lastX;
    y = lastY;
}

void shiori::setLastPointer(int x, int y) {
    lastX = x;
    lastY = y;
}
