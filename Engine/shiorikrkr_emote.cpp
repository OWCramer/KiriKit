/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

// E-mote characters for ShioriKrkr: the host's E-mote player (shiorikrkr_emote_host_t; in the
// app, Shiori's Swift player on CatSystemKit's renderer), shared helpers for the Motion classes
// (shiorikrkr_motion.cpp), and drawdeviceD3D.dll.
//
// drawdeviceD3D.dll links, but defines no DrawDeviceD3D class: NEKO WORKs' scripts then take
// their plain-layer path (Window.isD3D false), where E-mote characters are Motion.EmotePlayer
// drawing into ordinary layers, which Shiori's compositor composes with everything else.
#include "tjsCommHead.h"
#include <ncbind.hpp>
#include "StorageIntf.h"
#include "UtilStreams.h"
#include "DebugIntf.h"

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "shiorikrkr_emote.h"


namespace shiorikrkr {

namespace {
shiorikrkr_emote_host_t g_host{};
bool g_hasHost = false;
} // namespace

void setEmoteHost(const shiorikrkr_emote_host_t *host) {
    g_hasHost = host && host->struct_size >= sizeof(shiorikrkr_emote_host_t) && host->create && host->command &&
                host->query && host->draw && host->destroy;
    if (g_hasHost) g_host = *host;
    else g_host = {};
}

const shiorikrkr_emote_host_t *emoteHost() { return g_hasHost ? &g_host : nullptr; }

} // namespace shiorikrkr

namespace shiorikrkr::util {

std::string utf8(const ttstr &s) { return s.AsStdString(); }

// The storage's bytes: by its name as the game gives it (found through the auto paths).
bool readStorage(const ttstr &name, std::vector<uint8_t> &out, std::string &error) {
    ttstr placed = TVPGetPlacedPath(name);
    if (placed.IsEmpty()) {
        error = "not found: " + utf8(name);
        return false;
    }
    tTJSBinaryStream *stream = nullptr;
    try {
        stream = TVPCreateStream(placed, TJS_BS_READ);
    } catch (...) {
        stream = nullptr;
    }
    if (!stream) {
        error = "can't open: " + utf8(name);
        return false;
    }
    const tjs_uint64 size = stream->GetSize();
    out.resize(size_t(size));
    size_t done = 0;
    while (done < out.size()) {
        const tjs_uint chunk = tjs_uint(std::min<size_t>(out.size() - done, 1 << 24));
        const tjs_uint read = stream->Read(out.data() + done, chunk);
        if (read == 0) break;
        done += read;
    }
    delete stream;
    if (done != out.size()) {
        error = "short read: " + utf8(name);
        return false;
    }
    return true;
}

double number(tTJSVariant **param, tjs_int count, tjs_int index, double fallback) {
    if (index >= count || !param[index] || param[index]->Type() == tvtVoid) return fallback;
    return param[index]->AsReal();
}

std::string text(tTJSVariant **param, tjs_int count, tjs_int index) {
    if (index >= count || !param[index] || param[index]->Type() == tvtVoid) return {};
    return utf8(ttstr(*param[index]));
}

} // namespace shiorikrkr::util
using namespace shiorikrkr::util;

#define NCB_MODULE_NAME TJS_W("drawdeviceD3D.dll")
static void ShioriDrawDeviceD3DModule() { ncbAutoRegister::LoadModule(TJS_W("emoteplayer.dll")); }
NCB_PRE_REGIST_CALLBACK(ShioriDrawDeviceD3DModule);
