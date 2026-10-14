/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

// Links Hx archives (shiori_hx.h) into krkr2's XP3 reader: the reader hands each 'Hxv4'
// chunk's index here and asks for its members' reads to be decrypted (TVPShioriHx* hooks in
// XP3Archive.h, Shiori's patch). Everything stays in memory.
#include "shiori_hx_link.h"

#include "shiori_hx.h"

#include "tjsCommHead.h"
#include "XP3Archive.h"

#include <memory>

namespace {

struct HxState {
    shiorikrkr::CxScheme cx;
    shiorikrkr::HxScheme hx;
    std::unique_ptr<shiorikrkr::CxDecryptor> decryptor;
};

std::unique_ptr<HxState> &state() {
    static std::unique_ptr<HxState> instance;
    return instance;
}

void (*hxLog)(const std::string &) = nullptr;

bool readIndex(const ttstr &archive, const tjs_uint8 *blob, tjs_uint size,
               std::vector<tTVPShioriHxMember> &members) {
    const HxState *s = state().get();
    if (!s) return false;
    const std::string name = archive.AsStdString();
    std::vector<shiorikrkr::HxEntry> entries;
    std::string error;
    if (!shiorikrkr::hxReadIndex(s->hx, name, blob, size, entries, error)) {
        if (hxLog) hxLog("ShioriKrkr: Hx: " + error);
        return false;
    }
    size_t named = 0;
    members.clear();
    members.reserve(entries.size());
    for (const auto &entry : entries) {
        tTVPShioriHxMember member;
        const std::u16string number = shiorikrkr::hxEntryName(entry.id);
        member.Number = ttstr(reinterpret_cast<const tjs_char *>(number.c_str()));
        if (!entry.name.empty()) {
            member.Name = ttstr(entry.name.c_str());
            ++named;
        }
        member.Id = entry.id;
        member.Key = entry.key;
        members.push_back(std::move(member));
    }
    if (hxLog)
        hxLog("ShioriKrkr: Hx index of " + name + ": " + std::to_string(entries.size()) + " members, " +
              std::to_string(named) + " with names");
    return true;
}

void decrypt(tjs_uint64 id, tjs_uint64 key, tjs_uint64 offset, tjs_uint8 *buffer, tjs_uint size) {
    const HxState *s = state().get();
    if (!s) return;
    shiorikrkr::HxFilter(*s->decryptor, s->cx.mask, s->cx.offset, s->hx.filterKey, id, key)
        .decrypt(offset, buffer, size);
}

} // namespace

namespace shiorikrkr {

bool hxInstall(const CxScheme &scheme, void (*log)(const std::string &), std::string &line) {
    hxUninstall();
    auto s = std::make_unique<HxState>();
    std::string error;
    if (!HxScheme::parse(scheme.extension, s->hx, error)) {
        if (!error.empty()) line = "Hx scheme: " + error;
        return false;
    }
    s->cx = scheme;
    s->cx.random = s->hx.randomType == 0 ? 1 : 2;
    s->decryptor = std::make_unique<CxDecryptor>(s->cx);
    line = "Hx scheme loaded (" + std::to_string(s->hx.archiveKeys.size()) + " archive keys, " +
           std::to_string(s->hx.fileNames.size()) + " file names)";
    state() = std::move(s);
    hxLog = log;
    TVPShioriHxReadIndex = readIndex;
    TVPShioriHxDecrypt = decrypt;
    return true;
}

void hxUninstall() {
    TVPShioriHxReadIndex = nullptr;
    TVPShioriHxDecrypt = nullptr;
    state().reset();
}

} // namespace shiorikrkr
