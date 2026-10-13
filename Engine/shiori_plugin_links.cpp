/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

// Built-in plug-ins whose upstream entry point is a link function rather than ncbind
// registrations: when a game links the DLL name (Plugins.link), the function runs, as the DLL's
// V2Link would on Windows.
#include "tjsCommHead.h"
#include <ncbind.hpp>

bool onV2Link_extrans();
bool onV2Unlink_extrans();

namespace {

void linkExtrans() { onV2Link_extrans(); }
void unlinkExtrans() { onV2Unlink_extrans(); }
void linkKAGParserEx() {}

} // namespace

#undef NCB_MODULE_NAME
#define NCB_MODULE_NAME TJS_W("extrans.dll")
NCB_POST_REGIST_CALLBACK(linkExtrans);
NCB_PRE_UNREGIST_CALLBACK(unlinkExtrans);

#undef NCB_MODULE_NAME
#define NCB_MODULE_NAME TJS_W("KAGParserEx.dll")
NCB_POST_REGIST_CALLBACK(linkKAGParserEx);

// DLLs games link whose work is done elsewhere (audio decoders and the movie back end are in the
// core, Steam and Windows integration have no meaning here) or not yet: linking them succeeds and
// adds nothing.
#define SHIORI_EMPTY_PLUGIN(tag, dll)                                                                \
    namespace {                                                                                      \
    void shioriEmpty_##tag() {}                                                                      \
    }                                                                                                \
    static ncbCallbackAutoRegister shioriEmptyRegister_##tag(TJS_W(dll), ncbAutoRegister::PostRegist, \
                                                             &shioriEmpty_##tag, nullptr);
SHIORI_EMPTY_PLUGIN(wuvorbis, "wuvorbis.dll")
SHIORI_EMPTY_PLUGIN(wuopus, "wuopus.dll")
SHIORI_EMPTY_PLUGIN(krmovie, "krmovie.dll")
SHIORI_EMPTY_PLUGIN(krkrsteam, "krkrsteam.dll")
SHIORI_EMPTY_PLUGIN(SteamDrawDevice, "SteamDrawDevice.dll")
SHIORI_EMPTY_PLUGIN(drawdeviceD3DZ, "drawdeviceD3DZ.dll")
SHIORI_EMPTY_PLUGIN(win32ole, "win32ole.dll")
SHIORI_EMPTY_PLUGIN(wsh, "wsh.dll")
SHIORI_EMPTY_PLUGIN(messenger, "messenger.dll")
SHIORI_EMPTY_PLUGIN(wumsadp, "wumsadp.dll")
