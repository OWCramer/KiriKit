/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

// Pieces of krkr2's environment that have no meaning in Shiori: its launcher's file selector,
// pop-up menus and patch-library link, and the COM GUID its storage streams compare against.
#include "shiori_environ.h"

#include "tjsCommHead.h"
#include "combase.h"
#include "SystemIntf.h"

#include <string>

extern "C" const IID IID_IUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

std::string TVPShowFileSelector(const std::string &, const std::string &, std::string, bool) { return {}; }

class tTJSNI_MenuItem;
void TVPShowPopMenu(tTJSNI_MenuItem *) {}

void TVPOpenPatchLibUrl() {}


// Compressed GPU texture files (PVR) aren't used by Shiori's games.
#include <functional>
class iTVPTexture2D;
iTVPTexture2D *TVPLoadPVRv3(tTJSBinaryStream *, const std::function<void(const ttstr &, const tTJSVariant &)> &) {
    return nullptr;
}
