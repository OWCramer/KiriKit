/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

// Hx archives, as GARbro's HxCrypt reads them (ArcFormats/KiriKiri/HxCrypt.cs, MIT,
// Copyright (C) 2018 by morkt): the index's ChaCha20 and serialized object tree, and the
// per-entry filter. Ported for ShioriKrkr; see shiori_hx.h and PROVENANCE.md.
#include "shiori_hx.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <set>
#include <zlib.h>

namespace shiorikrkr {

namespace {

uint32_t le32(const uint8_t *p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
uint64_t le64(const uint8_t *p) { return uint64_t(le32(p)) | uint64_t(le32(p + 4)) << 32; }

std::string lower(std::string s) {
    for (char &c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string hex(const uint8_t *p, size_t n) {
    static const char digits[] = "0123456789ABCDEF";
    std::string s(n * 2, '0');
    for (size_t i = 0; i < n; ++i) {
        s[2 * i] = digits[p[i] >> 4];
        s[2 * i + 1] = digits[p[i] & 15];
    }
    return s;
}

// MARK: ChaCha20 (GARbro's HxChachaDecryptor: 20 rounds, 64-bit block counter from 1)

uint32_t rotl(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

void chachaBlock(const uint32_t in[16], uint8_t out[64]) {
    uint32_t z[16];
    std::memcpy(z, in, sizeof z);
    auto quarter = [&](int a, int b, int c, int d) {
        z[a] += z[b]; z[d] = rotl(z[d] ^ z[a], 16);
        z[c] += z[d]; z[b] = rotl(z[b] ^ z[c], 12);
        z[a] += z[b]; z[d] = rotl(z[d] ^ z[a], 8);
        z[c] += z[d]; z[b] = rotl(z[b] ^ z[c], 7);
    };
    for (int i = 0; i < 10; ++i) {
        quarter(0, 4, 8, 12); quarter(1, 5, 9, 13); quarter(2, 6, 10, 14); quarter(3, 7, 11, 15);
        quarter(0, 5, 10, 15); quarter(1, 6, 11, 12); quarter(2, 7, 8, 13); quarter(3, 4, 9, 14);
    }
    for (int i = 0; i < 16; ++i) {
        const uint32_t v = z[i] + in[i];
        out[4 * i] = uint8_t(v); out[4 * i + 1] = uint8_t(v >> 8);
        out[4 * i + 2] = uint8_t(v >> 16); out[4 * i + 3] = uint8_t(v >> 24);
    }
}

std::vector<uint8_t> chacha(const HxIndexKey &key, const uint8_t *data, size_t size) {
    static const char constant[] = "expand 32-byte k";
    uint32_t state[16];
    for (int i = 0; i < 4; ++i) state[i] = le32(reinterpret_cast<const uint8_t *>(constant) + 4 * i);
    for (int i = 0; i < 8; ++i) state[4 + i] = le32(key.key1.data() + 4 * i);
    state[12] = 1;
    state[13] = 0;
    state[14] = le32(key.key2.data());
    state[15] = le32(key.key2.data() + 4);
    std::vector<uint8_t> out(size);
    uint8_t stream[64];
    for (size_t at = 0; at < size; at += 64) {
        chachaBlock(state, stream);
        const size_t n = std::min<size_t>(64, size - at);
        for (size_t i = 0; i < n; ++i) out[at + i] = data[at + i] ^ stream[i];
        if (++state[12] == 0) ++state[13];
    }
    return out;
}

bool inflateAll(const uint8_t *data, size_t size, std::vector<uint8_t> &out) {
    z_stream z{};
    if (inflateInit(&z) != Z_OK) return false;
    z.next_in = const_cast<Bytef *>(data);
    z.avail_in = uInt(size);
    out.clear();
    uint8_t chunk[65536];
    int status;
    do {
        z.next_out = chunk;
        z.avail_out = sizeof chunk;
        status = inflate(&z, Z_NO_FLUSH);
        if (status != Z_OK && status != Z_STREAM_END) {
            inflateEnd(&z);
            return false;
        }
        out.insert(out.end(), chunk, chunk + (sizeof chunk - z.avail_out));
    } while (status != Z_STREAM_END && (z.avail_in > 0 || z.avail_out == 0));
    inflateEnd(&z);
    return status == Z_STREAM_END;
}

// MARK: The index's object tree (GARbro's HxIndexDeserializer; big-endian counts)

struct Node {
    enum Kind { Null, String, Bytes, Integer, Array, Dictionary } kind = Null;
    std::vector<uint8_t> bytes;
    int64_t integer = 0;
    std::vector<Node> items; // array items, or dictionary values
};

struct Reader {
    const uint8_t *p;
    size_t size, at = 0;
    bool fail = false;
    bool need(size_t n) {
        if (fail || size - at < n) fail = true;
        return !fail;
    }
    int32_t i32() {
        if (!need(4)) return 0;
        const uint32_t v = uint32_t(p[at]) << 24 | uint32_t(p[at + 1]) << 16 | uint32_t(p[at + 2]) << 8 | p[at + 3];
        at += 4;
        return int32_t(v);
    }
    int64_t i64() {
        const uint64_t hi = uint32_t(i32());
        const uint64_t lo = uint32_t(i32());
        return int64_t(hi << 32 | lo);
    }
    bool skip(size_t n) {
        if (!need(n)) return false;
        at += n;
        return true;
    }
    bool read(Node &node, int depth) {
        if (depth > 16 || !need(1)) return fail = true, false;
        const uint8_t type = p[at++];
        switch (type) {
        case 0x00: case 0x01: node.kind = Node::Null; return true;
        case 0x02: { // UTF-16 string, length in characters
            const int32_t n = i32();
            node.kind = Node::String;
            return n >= 0 && skip(size_t(n) * 2);
        }
        case 0x03: {
            const int32_t n = i32();
            if (n < 0 || !need(size_t(n))) return fail = true, false;
            node.kind = Node::Bytes;
            node.bytes.assign(p + at, p + at + n);
            at += size_t(n);
            return true;
        }
        case 0x04: case 0x05:
            node.kind = Node::Integer;
            node.integer = i64();
            return !fail;
        case 0x81: case 0xC1: {
            const int32_t n = i32();
            if (n < 0 || size_t(n) > size - at) return fail = true, false;
            node.kind = type == 0x81 ? Node::Array : Node::Dictionary;
            node.items.resize(size_t(n));
            for (auto &item : node.items) {
                if (type == 0xC1) {
                    const int32_t length = i32();
                    if (length < 0 || !skip(size_t(length) * 2)) return false;
                }
                if (!read(item, depth + 1)) return false;
            }
            return true;
        }
        default: return fail = true, false;
        }
    }
};

bool parseIndex(const HxScheme &scheme, const std::vector<uint8_t> &tree, std::vector<HxEntry> &out) {
    Reader reader{tree.data(), tree.size()};
    Node root;
    if (!reader.read(root, 0) || root.kind != Node::Array) return false;
    out.clear();
    for (size_t i = 0; i + 1 < root.items.size(); i += 2) {
        const Node &directory = root.items[i], &members = root.items[i + 1];
        if (directory.kind != Node::Bytes || members.kind != Node::Array) continue;
        const std::string directoryHash = hex(directory.bytes.data(), directory.bytes.size());
        const auto directoryName = scheme.directoryNames.find(directoryHash);
        for (size_t j = 0; j + 1 < members.items.size(); j += 2) {
            const Node &nameHash = members.items[j], &info = members.items[j + 1];
            if (nameHash.kind != Node::Bytes || info.kind != Node::Array || info.items.size() < 2) continue;
            if (info.items[0].kind != Node::Integer || info.items[1].kind != Node::Integer) continue;
            HxEntry entry;
            entry.id = uint64_t(info.items[0].integer);
            entry.key = uint64_t(info.items[1].integer);
            entry.directoryHash = directoryHash;
            entry.nameHash = hex(nameHash.bytes.data(), nameHash.bytes.size());
            const auto fileName = scheme.fileNames.find(entry.nameHash);
            if (fileName != scheme.fileNames.end()) {
                std::string path = directoryName != scheme.directoryNames.end() ? directoryName->second : "";
                if (!path.empty() && path.back() != '/' && path.back() != '\\') path += '/';
                entry.name = path + fileName->second;
            }
            out.push_back(std::move(entry));
        }
    }
    return true;
}

} // namespace

// MARK: - Scheme

bool HxScheme::parse(const std::vector<uint8_t> &ext, HxScheme &out, std::string &error) {
    // "HXv4", u32 random type, u64 filter key, u8 has-default, 32 + 16 default key,
    // u32 archive count {u16 length, UTF-8 name, 32 + 16 key}, then two name tables
    // (directories: 8-byte hashes, files: 32-byte hashes) {u32 count {hash, u16 length, UTF-8}}.
    if (ext.size() < 4 || std::memcmp(ext.data(), "HXv4", 4) != 0) return false;
    size_t at = 4;
    auto need = [&](size_t n) { return ext.size() - at >= n; };
    auto u16 = [&]() { const uint32_t v = ext[at] | ext[at + 1] << 8; at += 2; return v; };
    auto key = [&](HxIndexKey &k) {
        std::memcpy(k.key1.data(), &ext[at], 32);
        std::memcpy(k.key2.data(), &ext[at + 32], 16);
        at += 48;
    };
    if (!need(4 + 8 + 1 + 48 + 4)) return error = "Hx section too short", false;
    out = HxScheme{};
    out.randomType = int(le32(&ext[at])); at += 4;
    out.filterKey = le64(&ext[at]); at += 8;
    out.hasDefaultKey = ext[at++] != 0;
    key(out.defaultKey);
    uint32_t count = le32(&ext[at]); at += 4;
    for (uint32_t i = 0; i < count; ++i) {
        if (!need(2)) return error = "Hx archive keys truncated", false;
        const uint32_t length = u16();
        if (!need(length + 48)) return error = "Hx archive keys truncated", false;
        std::string name(reinterpret_cast<const char *>(&ext[at]), length);
        at += length;
        HxIndexKey k;
        key(k);
        out.archiveKeys.emplace_back(lower(name), k);
    }
    for (auto *table : {&out.directoryNames, &out.fileNames}) {
        const size_t hashSize = table == &out.directoryNames ? 8 : 32;
        if (!need(4)) return error = "Hx names truncated", false;
        count = le32(&ext[at]); at += 4;
        for (uint32_t i = 0; i < count; ++i) {
            if (!need(hashSize + 2)) return error = "Hx names truncated", false;
            const std::string hash = hex(&ext[at], hashSize);
            at += hashSize;
            const uint32_t length = u16();
            if (!need(length)) return error = "Hx names truncated", false;
            (*table)[hash] = std::string(reinterpret_cast<const char *>(&ext[at]), length);
            at += length;
        }
    }
    return true;
}

// MARK: - Index

bool hxReadIndex(const HxScheme &scheme, const std::string &archiveName, const uint8_t *blob, size_t size,
                 std::vector<HxEntry> &out, std::string &error) {
    if (size <= 20) return error = "Hx index too small", false;
    std::vector<const HxIndexKey *> keys;
    const std::string name = lower(archiveName);
    for (const auto &[archive, key] : scheme.archiveKeys)
        if (archive == name) keys.push_back(&key);
    if (scheme.hasDefaultKey) keys.push_back(&scheme.defaultKey);
    std::set<std::vector<uint8_t>> tried;
    for (const auto &[archive, key] : scheme.archiveKeys) keys.push_back(&key);
    for (const HxIndexKey *key : keys) {
        std::vector<uint8_t> id(key->key1.begin(), key->key1.end());
        id.insert(id.end(), key->key2.begin(), key->key2.end());
        if (!tried.insert(id).second) continue;
        // 16 bytes before the encrypted part; 4 more before the zlib stream.
        const std::vector<uint8_t> plain = chacha(*key, blob + 16, size - 16);
        std::vector<uint8_t> tree;
        if (plain.size() > 4 && inflateAll(plain.data() + 4, plain.size() - 4, tree) && parseIndex(scheme, tree, out))
            return true;
    }
    error = "no Hx index key opens " + archiveName;
    return false;
}

std::u16string hxEntryName(uint64_t id) {
    uint32_t hash = uint32_t(id);
    std::u16string name;
    do {
        name.push_back(char16_t((hash & 0x3fff) + 0x5000));
        hash >>= 14;
    } while (hash != 0);
    return name;
}

// MARK: - Filter

HxFilter::HxFilter(const CxDecryptor &cx, uint32_t cxMask, uint32_t cxOffset, uint64_t filterKey,
                   const HxEntry &entry)
    : HxFilter(cx, cxMask, cxOffset, filterKey, entry.id, entry.key) {}

HxFilter::HxFilter(const CxDecryptor &cx, uint32_t cxMask, uint32_t cxOffset, uint64_t filterKey, uint64_t id,
                   uint64_t key) {
    uint64_t entryKey = key;
    if (!(id & 0x100000000ull)) entryKey ^= filterKey;
    const uint64_t headerSeed = ~entryKey;
    auto run = [&](uint32_t argument) {
        uint32_t first, second;
        cx.xcode(argument, first, second);
        return uint64_t(first) | uint64_t(second) << 32;
    };
    spans_[0].init(run(uint32_t(entryKey)));
    spans_[1].init(run(uint32_t(entryKey >> 32)));
    split_ = uint32_t(cxOffset + ((entryKey >> 16) & cxMask));
    uint64_t v = ~run(uint32_t(headerSeed));
    for (int i = 0; i < 8; ++i) header_[i] = uint8_t(v >> (56 - 8 * i));
    v = ~run(uint32_t(v));
    for (int i = 0; i < 8; ++i) header_[8 + i] = uint8_t(v >> (56 - 8 * i));
}

void HxFilter::Span::init(uint64_t key) {
    key1 = uint8_t(key >> 8);
    key2 = uint8_t(key >> 16);
    position1 = (key >> 48) & 0xffff;
    position2 = (key >> 32) & 0xffff;
    if (position1 == position2) position2 += 1;
    uint32_t first = uint32_t(key & 0xff);
    if (first == 0) first = 0xa5;
    fill = first * 0x01010101u;
}

void HxFilter::Span::decrypt(uint64_t position, uint8_t *buffer, size_t count) const {
    if (count == 0) return;
    const uint8_t byte = uint8_t(fill);
    for (size_t i = 0; i < count; ++i) buffer[i] ^= byte;
    if (key1 && position1 >= position && position1 < position + count) buffer[position1 - position] ^= key1;
    if (key2 && position2 >= position && position2 < position + count) buffer[position2 - position] ^= key2;
}

void HxFilter::decrypt(uint64_t offset, uint8_t *buffer, size_t count) const {
    if (count == 0) return;
    if (offset < header_.size()) {
        const size_t end = size_t(std::min<uint64_t>(header_.size(), offset + count));
        for (size_t at = size_t(offset); at < end; ++at) buffer[at - offset] ^= header_[at];
    }
    if (split_ > offset) {
        if (split_ < offset + count) {
            const size_t head = size_t(split_ - offset);
            spans_[0].decrypt(offset, buffer, head);
            spans_[1].decrypt(split_, buffer + head, count - head);
        } else {
            spans_[0].decrypt(offset, buffer, count);
        }
    } else {
        spans_[1].decrypt(offset, buffer, count);
    }
}

} // namespace shiorikrkr
