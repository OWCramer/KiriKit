/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

// motionplayer.dll: the same Motion classes as emoteplayer.dll (shiorikrkr_motion.cpp), under
// the name later games link.
#include "tjsCommHead.h"
#include "ncbind/ncbind.hpp"

#define NCB_MODULE_NAME TJS_N("motionplayer.dll")

static void ShioriMotionPlayerModule() { ncbAutoRegister::LoadModule(TJS_N("emoteplayer.dll")); }

NCB_PRE_REGIST_CALLBACK(ShioriMotionPlayerModule);
