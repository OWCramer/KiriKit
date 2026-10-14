/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

// kztouch.dll: KiriKiri Z's touch input on a KiriKiri 2 window, as Yuzusoft's scripts use it
// (gesture.tjs, dragutil.tjs): linking it gives `Window.TouchMouse` (an object the window
// keeps; on Windows it joins a finger's touch events to the mouse events the same finger
// makes), and the scripts then set `window.enableTouch` and read `touchPointCount`. Those and
// the onTouchDown/onTouchMove/onTouchUp events are krkr2's own Window members; Shiori backs them
// with the host's fingers (environ/shiori_window.mm), sent beside the pointer events once the
// game has turned touch on. Written from the members the scripts use; no plug-in code read.
#include "tjsCommHead.h"
#include "ScriptMgnIntf.h"
#include <ncbind.hpp>

namespace {

struct ShioriTouchMouse {
    static tjs_error factory(ShioriTouchMouse **result, tjs_int, tTJSVariant **, iTJSDispatch2 *) {
        *result = new ShioriTouchMouse();
        return TJS_S_OK;
    }
};

// Window.TouchMouse, beside the window's other classes (as Window.BasicDrawDevice).
void linkTouch() {
    iTJSDispatch2 *global = TVPGetScriptDispatch();
    if (!global) return;
    tTJSVariant cls, window;
    if (TJS_SUCCEEDED(global->PropGet(0, TJS_W("TouchMouse"), nullptr, &cls, global)) &&
        TJS_SUCCEEDED(global->PropGet(0, TJS_W("Window"), nullptr, &window, global)) && window.Type() == tvtObject) {
        iTJSDispatch2 *w = window.AsObjectNoAddRef();
        w->PropSet(TJS_MEMBERENSURE | TJS_IGNOREPROP | TJS_STATICMEMBER, TJS_W("TouchMouse"), nullptr, &cls, w);
    }
    global->Release();
}

} // namespace

#define NCB_MODULE_NAME TJS_W("kztouch.dll")
NCB_REGISTER_CLASS_DIFFER(TouchMouse, ShioriTouchMouse) { Factory(&ClassT::factory); }
NCB_POST_REGIST_CALLBACK(linkTouch);
