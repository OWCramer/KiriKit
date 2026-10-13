/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

// Development tool: lists or extracts members of a game's XP3 archive the way ShioriKrkr reads
// them (NEKO WORKs names and key, or a CxDec scheme), for reading a game's scripts while
// working on the engine. Output goes where it's told (a scratch folder), never into the game.
//
//   xp3dump <archive.xp3> <game folder> <out dir> [--scheme file.shcx] [name substring...]
#include "../nekoworks.h"
#include "../cx_decrypt.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <vector>
#include <zlib.h>

namespace {

uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }
uint64_t u64(const uint8_t *p) { return uint64_t(u32(p)) | uint64_t(u32(p + 4)) << 32; }

std::vector<uint8_t> readIndex(FILE *f) {
    uint8_t header[40];
    if (fread(header, 1, sizeof header, f) != sizeof header || memcmp(header, "XP3\r\n \n\x1a", 8) != 0) return {};
    uint64_t offset = u64(header + 11);
    if (offset == 0x17) offset = u64(header + 0x20);
    for (int guard = 0; guard < 8; ++guard) {
        fseeko(f, off_t(offset), SEEK_SET);
        uint8_t flag;
        if (fread(&flag, 1, 1, f) != 1) return {};
        if (flag == 0x80) {
            uint8_t pointer[16];
            if (fread(pointer, 1, 16, f) != 16) return {};
            offset = u64(pointer + 8);
            continue;
        }
        if (flag & 1) {
            uint8_t sizes[16];
            if (fread(sizes, 1, 16, f) != 16) return {};
            std::vector<uint8_t> in(u64(sizes)), out(u64(sizes + 8));
            if (fread(in.data(), 1, in.size(), f) != in.size()) return {};
            uLongf length = uLongf(out.size());
            if (uncompress(out.data(), &length, in.data(), uLong(in.size())) != Z_OK) return {};
            return out;
        }
        uint8_t sizeBytes[8];
        if (fread(sizeBytes, 1, 8, f) != 8) return {};
        std::vector<uint8_t> out(u64(sizeBytes));
        if (fread(out.data(), 1, out.size(), f) != out.size()) return {};
        return out;
    }
    return {};
}

std::string utf8(const std::u16string &s) {
    std::string out;
    for (char16_t c : s) {
        if (c < 0x80) out += char(c);
        else if (c < 0x800) out += char(0xC0 | (c >> 6)), out += char(0x80 | (c & 0x3F));
        else out += char(0xE0 | (c >> 12)), out += char(0x80 | ((c >> 6) & 0x3F)), out += char(0x80 | (c & 0x3F));
    }
    return out;
}

struct Segment {
    bool compressed;
    uint64_t start, orgSize, arcSize;
};
struct Entry {
    std::string name;
    uint32_t hash = 0, flags = 0;
    uint64_t size = 0;
    std::vector<Segment> segments;
};

} // namespace

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: xp3dump archive.xp3 gamefolder outdir [--scheme f] [filter...]\n");
        return 2;
    }
    std::string archive = argv[1], root = argv[2], out = argv[3];
    std::vector<std::string> filters;
    std::unique_ptr<shiorikrkr::CxDecryptor> cx;
    for (int i = 4; i < argc; ++i) {
        if (!strcmp(argv[i], "--scheme") && i + 1 < argc) {
            shiorikrkr::CxScheme scheme;
            std::string error;
            if (!shiorikrkr::CxScheme::load(argv[++i], scheme, error)) {
                fprintf(stderr, "scheme: %s\n", error.c_str());
                return 1;
            }
            cx = std::make_unique<shiorikrkr::CxDecryptor>(scheme);
        } else {
            filters.push_back(argv[i]);
        }
    }
    shiorikrkr::NekoWorksKey neko;
    std::string log;
    if (!cx) shiorikrkr::detectNekoWorksKey(root, neko, log);
    if (!log.empty()) fprintf(stderr, "%s\n", log.c_str());

    FILE *f = fopen(archive.c_str(), "rb");
    if (!f) return 1;
    const std::vector<uint8_t> index = readIndex(f);
    std::map<uint32_t, std::vector<std::string>> names;
    std::vector<Entry> entries;
    for (size_t p = 0; p + 12 <= index.size();) {
        const uint64_t size = u64(&index[p + 4]);
        const uint8_t *body = &index[p + 12];
        if (!memcmp(&index[p], "neko", 4) || !memcmp(&index[p], "eliF", 4) || !memcmp(&index[p], "hnfn", 4)) {
            const size_t length = body[4] | body[5] << 8;
            std::u16string name;
            for (size_t i = 0; i < length; ++i) name.push_back(char16_t(body[6 + i * 2] | body[7 + i * 2] << 8));
            names[u32(body)].push_back(utf8(name));
        } else if (!memcmp(&index[p], "File", 4)) {
            Entry e;
            for (size_t q = 0; q + 12 <= size;) {
                const uint64_t length = u64(body + q + 4);
                const uint8_t *chunk = body + q + 12;
                if (!memcmp(body + q, "info", 4)) {
                    e.flags = u32(chunk);
                    e.size = u64(chunk + 4);
                    const size_t n = chunk[20] | chunk[21] << 8;
                    std::u16string name;
                    for (size_t i = 0; i < n; ++i) name.push_back(char16_t(chunk[22 + i * 2] | chunk[23 + i * 2] << 8));
                    e.name = utf8(name);
                }
                if (!memcmp(body + q, "adlr", 4)) e.hash = u32(chunk);
                if (!memcmp(body + q, "segm", 4))
                    for (size_t s = 0; s + 28 <= length; s += 28)
                        e.segments.push_back({(u32(chunk + s) & 7) != 0, u64(chunk + s + 4), u64(chunk + s + 12),
                                              u64(chunk + s + 20)});
                q += 12 + length;
            }
            entries.push_back(e);
        }
        p += 12 + size;
    }
    std::map<uint32_t, size_t> used;
    mkdir(out.c_str(), 0755);
    for (Entry &e : entries) {
        auto named = names.find(e.hash);
        if (named != names.end()) {
            size_t &u = used[e.hash];
            e.name = named->second[std::min(u, named->second.size() - 1)];
            ++u;
        } else if (!names.empty()) {
            continue; // decoy
        }
        bool match = filters.empty();
        for (const auto &flt : filters)
            if (e.name.find(flt) != std::string::npos) match = true;
        if (!match) continue;
        std::vector<uint8_t> data;
        for (const Segment &s : e.segments) {
            std::vector<uint8_t> raw(s.arcSize);
            fseeko(f, off_t(s.start), SEEK_SET);
            fread(raw.data(), 1, raw.size(), f);
            if (s.compressed) {
                std::vector<uint8_t> plain(s.orgSize);
                uLongf length = uLongf(plain.size());
                uncompress(plain.data(), &length, raw.data(), uLong(raw.size()));
                raw.swap(plain);
            }
            data.insert(data.end(), raw.begin(), raw.end());
        }
        if (e.flags) {
            if (neko.valid) shiorikrkr::decodeNekoWorks(neko, e.hash, 0, data.data(), data.size());
            else if (cx) cx->decrypt(e.hash, 0, data.data(), data.size());
        }
        std::string path = out + "/" + e.name;
        for (size_t i = out.size() + 1; i < path.size(); ++i)
            if (path[i] == '/') {
                path[i] = 0;
                mkdir(path.c_str(), 0755);
                path[i] = '/';
            }
        if (FILE *o = fopen(path.c_str(), "wb")) {
            fwrite(data.data(), 1, data.size(), o);
            fclose(o);
        }
        printf("%s (%zu)\n", e.name.c_str(), data.size());
    }
    fclose(f);
}
