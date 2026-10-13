/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

// krkrsdl3 plug-ins name krkr2's core headers by krkrsdl3's own file names; this maps one.
#pragma once
#include "FontImpl.h"
#include "FontSystem.h"
#include "BinaryStream.h"
#include "UtilStreams.h"
#include <vector>
void TVPGetAllFontList(std::vector<ttstr> &list);
const ttstr &TVPGetDefaultFontName();
// krkrsdl3 reads its bundled fallback font by this name; Shiori has the host's default font.
inline tTJSBinaryStream *GetResourceStream(const ttstr &) { return TVPCreateFontStream(TVPGetDefaultFontName()); }
