/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

// Stand-ins for Windows-only parts of plug-ins that KiriKiri Z games expect, written from what
// the games' scripts call (no plug-in code read).
//
// gamepad.dll (DirectInput/XInput pads, KiriKiri Z): a stand-in with no pads attached. Scripts
// that link it (exgamepad.tjs) expect its `GamepadPort` class to be there and ask it for
// devices; with none, they keep their keyboard mapping. On Shiori a controller already reaches
// the game as keys (the host's controller mapping), so nothing is lost.
#include "tjsCommHead.h"
#include <ncbind.hpp>

#define NCB_MODULE_NAME TJS_W("gamepad.dll")

struct ShioriGamepadPort {
    tjs_int getCount() const { return 0; } // pads attached
    static tjs_error zero(tTJSVariant *r, tjs_int, tTJSVariant **, iTJSDispatch2 *) {
        if (r) *r = tjs_int(0);
        return TJS_S_OK;
    }
    static tjs_error one(tTJSVariant *r, tjs_int, tTJSVariant **, iTJSDispatch2 *) {
        if (r) *r = tjs_int(1);
        return TJS_S_OK;
    }
    static tjs_error nothing(tTJSVariant *r, tjs_int, tTJSVariant **, iTJSDispatch2 *) {
        if (r) r->Clear();
        return TJS_S_OK;
    }
};

NCB_REGISTER_CLASS_DIFFER(GamepadPort, ShioriGamepadPort) {
    Constructor();
    RawCallback("initialize", &ShioriGamepadPort::one, 0); // set up (with no pads)
    RawCallback("update", &ShioriGamepadPort::nothing, 0);
    Property(TJS_W("count"), &ShioriGamepadPort::getCount, (int)0);
}

// windowEx.dll's Window.registerHotKey/unregisterHotKey (system-wide hot keys, e.g. a game's
// PrtSc snapshot key in snapcapture.tjs, which stops the game's start-up when registering
// fails): there are no system hot keys here, so registering succeeds and the key never fires.
#undef NCB_MODULE_NAME
#define NCB_MODULE_NAME TJS_W("windowEx.dll")

struct ShioriWindowHotKeys {
    static tjs_error registered(tTJSVariant *r, tjs_int, tTJSVariant **, iTJSDispatch2 *) {
        if (r) *r = true;
        return TJS_S_OK;
    }
};

NCB_ATTACH_CLASS(ShioriWindowHotKeys, Window) {
    RawCallback("registerHotKey", &ShioriWindowHotKeys::registered, 0);
    RawCallback("unregisterHotKey", &ShioriWindowHotKeys::registered, 0);
}
