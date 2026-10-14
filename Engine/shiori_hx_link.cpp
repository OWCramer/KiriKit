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

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <unordered_map>

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
        member.Hash = entry.directoryHash + entry.nameHash;
        member.Id = entry.id;
        member.Key = entry.key;
        members.push_back(std::move(member));
    }
    if (hxLog)
        hxLog("ShioriKrkr: Hx index of " + name + ": " + std::to_string(entries.size()) + " members, " +
              std::to_string(named) + " with names");
    return true;
}

// A normalized in-archive name's index hashes: its directory (with the trailing slash, or the
// root) and its file name.
bool hashName(const ttstr &name, std::string &hash) {
    // Called for every name looked up in every auto path (often more than a hundred times for
    // one name a game probes for), so both halves are cached: directories for the session,
    // the file name for as long as the same one is asked again on this thread.
    static std::mutex lock;
    static std::unordered_map<std::u16string, std::string> directories;
    thread_local std::u16string lastFile;
    thread_local std::string lastFileHash;
    const std::u16string full(reinterpret_cast<const char16_t *>(name.c_str()), size_t(name.GetLen()));
    const size_t slash = full.rfind(u'/');
    const std::u16string directory = slash == std::u16string::npos ? std::u16string() : full.substr(0, slash + 1);
    const std::u16string file = slash == std::u16string::npos ? full : full.substr(slash + 1);
    if (file.empty()) return false;
    {
        std::lock_guard<std::mutex> hold(lock);
        auto found = directories.find(directory);
        if (found == directories.end())
            found = directories.emplace(directory, shiorikrkr::hxDirectoryHash(directory)).first;
        hash = found->second;
    }
    if (file != lastFile) {
        lastFile = file;
        lastFileHash = shiorikrkr::hxFileNameHash(file);
    }
    hash += lastFileHash;
    static const bool trace = std::getenv("SHIORIKRKR_TRACE_HX") != nullptr;
    if (trace) fprintf(stderr, "hx: %s\n", name.AsStdString().c_str());
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
    TVPShioriHxHashName = hashName;
    return true;
}

void hxUninstall() {
    TVPShioriHxReadIndex = nullptr;
    TVPShioriHxDecrypt = nullptr;
    TVPShioriHxHashName = nullptr;
    state().reset();
}

} // namespace shiorikrkr
