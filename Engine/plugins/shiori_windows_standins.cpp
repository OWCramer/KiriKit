/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

// Stand-ins for Windows-only parts of plug-ins that KiriKiri Z games expect, written from what
// the games' scripts call (no plug-in code read). (gamepad.dll is plugins/shiori_gamepad.cpp.)
#include "tjsCommHead.h"
#include <ncbind.hpp>

// windowEx.dll's Window.registerHotKey/unregisterHotKey (system-wide hot keys, e.g. a game's
// PrtSc snapshot key in snapcapture.tjs, which stops the game's start-up when registering
// fails): there are no system hot keys here, so registering succeeds and the key never fires.
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
