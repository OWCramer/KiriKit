/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

#include "nekoworks.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <map>
#include <vector>
#include <zlib.h>

namespace shiorikrkr {

namespace {

uint8_t fold(uint32_t hash) { return uint8_t(hash ^ (hash >> 8) ^ (hash >> 16) ^ (hash >> 24)); }

uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }
uint64_t u64(const uint8_t *p) { return uint64_t(u32(p)) | uint64_t(u32(p + 4)) << 32; }

struct Sample {
    uint32_t hash;
    uint8_t plain[2];
    uint8_t cipher[2];
};

/// The archive's index (inflated), or empty.
std::vector<uint8_t> readIndex(FILE *f) {
    uint8_t header[40];
    if (std::fread(header, 1, sizeof header, f) != sizeof header || std::memcmp(header, "XP3\r\n \n\x1a", 8) != 0) return {};
    uint64_t offset = u64(header + 11);
    if (offset == 0x17) offset = u64(header + 0x20); // the newer header points on
    for (int guard = 0; guard < 8; ++guard) {
        if (fseeko(f, off_t(offset), SEEK_SET) != 0) return {};
        uint8_t flag;
        if (std::fread(&flag, 1, 1, f) != 1) return {};
        if (flag == 0x80) { // a pointer to the real index
            uint8_t pointer[16];
            if (std::fread(pointer, 1, 16, f) != 16) return {};
            offset = u64(pointer + 8);
            continue;
        }
        if (flag & 1) {
            uint8_t sizes[16];
            if (std::fread(sizes, 1, 16, f) != 16) return {};
            const uint64_t packed = u64(sizes), size = u64(sizes + 8);
            if (packed > (64u << 20) || size > (256u << 20)) return {};
            std::vector<uint8_t> in(packed), out(size);
            if (std::fread(in.data(), 1, in.size(), f) != in.size()) return {};
            uLongf length = uLongf(size);
            if (uncompress(out.data(), &length, in.data(), uLong(in.size())) != Z_OK) return {};
            out.resize(length);
            return out;
        }
        uint8_t sizeBytes[8];
        if (std::fread(sizeBytes, 1, 8, f) != 8) return {};
        const uint64_t size = u64(sizeBytes);
        if (size > (256u << 20)) return {};
        std::vector<uint8_t> out(size);
        if (std::fread(out.data(), 1, out.size(), f) != out.size()) return {};
        return out;
    }
    return {};
}

/// 'neko' / 'eliF' names by hash, and samples from protected PNG/Ogg entries.
bool scan(const std::string &path, std::vector<Sample> &samples, bool &newLayout) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    const std::vector<uint8_t> index = readIndex(f);
    std::map<uint32_t, std::u16string> names;
    bool neko = false, elif = false;
    struct Entry {
        uint32_t hash = 0, flags = 0;
        uint64_t start = 0;
        bool compressed = true;
    };
    std::vector<Entry> entries;
    for (size_t p = 0; p + 12 <= index.size();) {
        const uint64_t size = u64(&index[p + 4]);
        if (size > index.size() - p - 12) break;
        const uint8_t *body = &index[p + 12];
        if (!std::memcmp(&index[p], "neko", 4) || !std::memcmp(&index[p], "eliF", 4)) {
            (index[p] == 'n' ? neko : elif) = true;
            if (size >= 6) {
                const size_t length = body[4] | body[5] << 8;
                if (6 + length * 2 <= size) {
                    std::u16string name;
                    for (size_t i = 0; i < length; ++i) name.push_back(char16_t(body[6 + i * 2] | body[7 + i * 2] << 8));
                    names[u32(body)] = name;
                }
            }
        } else if (!std::memcmp(&index[p], "File", 4)) {
            Entry entry;
            for (size_t q = 0; q + 12 <= size;) {
                const uint64_t length = u64(body + q + 4);
                if (length > size - q - 12) break;
                const uint8_t *chunk = body + q + 12;
                if (!std::memcmp(body + q, "info", 4) && length >= 4) entry.flags = u32(chunk);
                if (!std::memcmp(body + q, "adlr", 4) && length >= 4) entry.hash = u32(chunk);
                if (!std::memcmp(body + q, "segm", 4) && length >= 28) {
                    entry.compressed = (u32(chunk) & 7) != 0;
                    entry.start = u64(chunk + 4);
                }
                q += 12 + length;
            }
            entries.push_back(entry);
        }
        p += 12 + size;
    }
    if (!neko && !elif) {
        std::fclose(f);
        return false;
    }
    newLayout = neko;
    auto endsWith = [](const std::u16string &name, const char *ext) {
        const size_t n = std::strlen(ext);
        if (name.size() < n) return false;
        for (size_t i = 0; i < n; ++i)
            if (std::tolower(int(name[name.size() - n + i])) != ext[i]) return false;
        return true;
    };
    for (const Entry &entry : entries) {
        if (!(entry.flags & 0x80000000u) || entry.compressed) continue;
        auto named = names.find(entry.hash);
        if (named == names.end()) continue;
        Sample sample{entry.hash, {0, 0}, {0, 0}};
        if (endsWith(named->second, ".png")) {
            sample.plain[0] = 0x89, sample.plain[1] = 'P';
        } else if (endsWith(named->second, ".ogg")) {
            sample.plain[0] = 'O', sample.plain[1] = 'g';
        } else {
            continue;
        }
        if (fseeko(f, off_t(entry.start), SEEK_SET) != 0 || std::fread(sample.cipher, 1, 2, f) != 2) continue;
        samples.push_back(sample);
        if (samples.size() >= 400) break;
    }
    std::fclose(f);
    return true;
}

} // namespace

bool detectNekoWorksKey(const std::string &gameRoot, NekoWorksKey &key, std::string &log) {
    std::string root = gameRoot;
    if (!root.empty() && root.back() != '/') root += '/';
    std::vector<std::string> archives{root + "data.xp3"};
    if (DIR *dir = opendir(root.c_str())) {
        while (dirent *e = readdir(dir)) {
            std::string name = e->d_name;
            if (name.size() > 4 && name.compare(name.size() - 4, 4, ".xp3") == 0 && name != "data.xp3")
                archives.push_back(root + name);
        }
        closedir(dir);
    }
    for (const std::string &archive : archives) {
        std::vector<Sample> samples;
        bool newLayout = true;
        if (!scan(archive, samples, newLayout)) {
            if (archive == root + "data.xp3") return false; // not NEKO WORKs
            continue;
        }
        // Majority vote over the samples (the user's rule): keys that fold to zero use fallbacks.
        std::map<std::pair<uint8_t, uint8_t>, int> votes;
        for (const Sample &s : samples) {
            const uint8_t x = s.cipher[1] ^ s.plain[1];
            const uint8_t first = newLayout ? uint8_t(s.cipher[0] ^ s.plain[0] ^ x) : 0;
            votes[{uint8_t(fold(s.hash) ^ x), newLayout ? uint8_t(uint8_t(s.hash) ^ first) : uint8_t(0)}]++;
        }
        int best = 0;
        NekoWorksKey found;
        for (const auto &[constants, count] : votes)
            if (count > best) best = count, found.foldXor = constants.first, found.firstXor = constants.second;
        if (best < 3 || best * 2 <= int(samples.size())) continue;
        found.valid = true;
        found.firstByte = newLayout;
        bool haveZeroKey = false, haveZeroFirst = !newLayout;
        for (const Sample &s : samples) {
            const uint8_t folded = fold(s.hash) ^ found.foldXor;
            const uint8_t x = folded ? folded : uint8_t(s.cipher[1] ^ s.plain[1]);
            if (!folded && !haveZeroKey) found.zeroKey = x, haveZeroKey = true;
            if (!haveZeroFirst && uint8_t(s.hash ^ found.firstXor) == 0 && (folded || haveZeroKey))
                found.zeroFirst = s.cipher[0] ^ s.plain[0] ^ x, haveZeroFirst = true;
        }
        key = found;
        char line[200];
        std::snprintf(line, sizeof line, "NEKO WORKs archives (%s layout): key %02x/%02x from %d of %zu samples in %s",
                      newLayout ? "neko" : "eliF", found.foldXor, found.firstXor, best, samples.size(),
                      archive.substr(root.size()).c_str());
        log = line;
        return true;
    }
    log = "NEKO WORKs archives, but no key found";
    return false;
}

void decodeNekoWorks(const NekoWorksKey &key, uint32_t hash, uint64_t offset, uint8_t *buffer, size_t size) {
    if (!size) return;
    uint8_t x = fold(hash) ^ key.foldXor;
    if (x == 0) x = key.zeroKey;
    if (offset == 0 && key.firstByte) {
        uint8_t first = uint8_t(hash) ^ key.firstXor;
        if (first == 0) first = key.zeroFirst;
        buffer[0] ^= first;
    }
    for (size_t i = 0; i < size; ++i) buffer[i] ^= x;
}

} // namespace shiorikrkr
