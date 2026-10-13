/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

// Temporary: no movie player yet.
#include "tjsCommHead.h"
#include <cstdint>
class tTJSNI_VideoOverlay;
class iTVPVideoOverlay;
struct IStream;
void GetVideoOverlayObject(tTJSNI_VideoOverlay *, IStream *, const tjs_char *, const tjs_char *, uint64_t, iTVPVideoOverlay **out) { *out = nullptr; }
void GetVideoLayerObject(tTJSNI_VideoOverlay *, IStream *, const tjs_char *, const tjs_char *, uint64_t, iTVPVideoOverlay **out) { *out = nullptr; }
void GetMixingVideoOverlayObject(tTJSNI_VideoOverlay *, IStream *, const tjs_char *, const tjs_char *, uint64_t, iTVPVideoOverlay **out) { *out = nullptr; }
void GetMFVideoOverlayObject(tTJSNI_VideoOverlay *, IStream *, const tjs_char *, const tjs_char *, uint64_t, iTVPVideoOverlay **out) { *out = nullptr; }
