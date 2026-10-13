/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

// Included before every built-in plug-in source: the few definitions wamsoft's plug-in
// framework (tp_stub.h) gives plug-ins that krkr2's in-engine headers don't.
#pragma once

#include "tjsCommHead.h"
#include "typedefine.h"
#include "combase.h"

#include <cstdint>
#include <string>
#include "TickCount.h"
#include "tvpfontstruc.h"

#ifndef SHIORI_TJS_STRING_DEFINED
#define SHIORI_TJS_STRING_DEFINED
typedef std::u16string tjs_string;
#endif

#ifndef TJS_INTF_METHOD
#define TJS_INTF_METHOD
#endif
#ifndef STDCALL
#define STDCALL
#endif

#ifndef SHIORI_RGBQUAD_DEFINED
#define SHIORI_RGBQUAD_DEFINED
typedef struct tagRGBQUAD {
    uint8_t rgbBlue;
    uint8_t rgbGreen;
    uint8_t rgbRed;
    uint8_t rgbReserved;
} RGBQUAD;
#endif
