/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

// Scripts.loadDataPack (PackinOne.dll): KiriKiri data packs ("TJS/ns0", "TJS/4s0"), the
// typed value trees Ninja Girl keeps its layered character pictures' layouts in (.pbd).
//
// A pack: "TJS/", a compression byte ('n' none, '4' LZ4), "s0\0", a 32-bit seed, an
// encryption flag, an IV and its length, then the payload. Encrypted payloads are XORed
// with a keystream built from ChaCha (8 rounds) under a BLAKE2s key of the seed and IV,
// each 1 KiB of keystream being one ChaCha block followed by words that xorshift the word
// 64 bytes before them; their LZ4 comes as linked blocks with 16-bit sizes. The values carry
// a check byte each from a small byte-shift generator seeded by the pack's seed, and a final
// check word follows. Behaviour notes in PROVENANCE.md (the format was learnt by reading
// Aether's decoder for guidance; this is Shiori's own implementation).
#include "tjsCommHead.h"
#include "ncbind/ncbind.hpp"
#include "tjsArray.h"
#include "tjsDictionary.h"
#include "TVPStorage.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "shiorikrkr_emote.h"

#define NCB_MODULE_NAME TJS_N("PackinOne.dll")

namespace {

using Bytes = std::vector<uint8_t>;

uint32_t le32(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint32_t rotl(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }
uint32_t rotr(uint32_t v, int n) { return (v >> n) | (v << (32 - n)); }

// XXH32 (the public xxHash specification).
uint32_t xxh32(const uint8_t *p, size_t n, uint32_t seed) {
    const uint32_t P1 = 2654435761u, P2 = 2246822519u, P3 = 3266489917u, P4 = 668265263u, P5 = 374761393u;
    auto round = [&](uint32_t acc, uint32_t lane) { return rotl(acc + lane * P2, 13) * P1; };
    size_t i = 0;
    uint32_t h;
    if (n >= 16) {
        uint32_t a = seed + P1 + P2, b = seed + P2, c = seed, d = seed - P1;
        for (; i + 16 <= n; i += 16) {
            a = round(a, le32(p + i));
            b = round(b, le32(p + i + 4));
            c = round(c, le32(p + i + 8));
            d = round(d, le32(p + i + 12));
        }
        h = rotl(a, 1) + rotl(b, 7) + rotl(c, 12) + rotl(d, 18);
    } else {
        h = seed + P5;
    }
    h += uint32_t(n);
    for (; i + 4 <= n; i += 4) h = rotl(h + le32(p + i) * P3, 17) * P4;
    for (; i < n; ++i) h = rotl(h + p[i] * P5, 11) * P1;
    h ^= h >> 15;
    h *= P2;
    h ^= h >> 13;
    h *= P3;
    h ^= h >> 16;
    return h;
}

// BLAKE2s-256 with a key (RFC 7693).
class Blake2s {
public:
    explicit Blake2s(const Bytes &key) {
        h_ = kIV;
        h_[0] ^= 0x01010000u ^ uint32_t(key.size() << 8) ^ 32u;
        if (!key.empty()) {
            uint8_t block[64] = {};
            std::memcpy(block, key.data(), key.size());
            update(block, 64);
        }
    }
    void update(const uint8_t *p, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            if (fill_ == 64) {
                count_ += 64;
                compress(false);
                fill_ = 0;
            }
            buffer_[fill_++] = p[i];
        }
    }
    std::array<uint8_t, 32> digest() {
        count_ += fill_;
        std::fill(buffer_ + fill_, buffer_ + 64, 0);
        compress(true);
        std::array<uint8_t, 32> out;
        for (int i = 0; i < 8; ++i)
            for (int b = 0; b < 4; ++b) out[size_t(i * 4 + b)] = uint8_t(h_[size_t(i)] >> (8 * b));
        return out;
    }

private:
    static constexpr std::array<uint32_t, 8> kIV = {0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
                                                    0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u};
    void compress(bool last) {
        static const uint8_t sigma[10][16] = {
            {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}, {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
            {11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4}, {7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8},
            {9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13}, {2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9},
            {12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11}, {13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10},
            {6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5}, {10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0}};
        uint32_t v[16], m[16];
        for (int i = 0; i < 8; ++i) {
            v[i] = h_[size_t(i)];
            v[i + 8] = kIV[size_t(i)];
        }
        v[12] ^= uint32_t(count_);
        v[13] ^= uint32_t(count_ >> 32);
        if (last) v[14] = ~v[14];
        for (int i = 0; i < 16; ++i) m[i] = le32(buffer_ + i * 4);
        auto g = [&](int a, int b, int c, int d, uint32_t x, uint32_t y) {
            v[a] += v[b] + x;
            v[d] = rotr(v[d] ^ v[a], 16);
            v[c] += v[d];
            v[b] = rotr(v[b] ^ v[c], 12);
            v[a] += v[b] + y;
            v[d] = rotr(v[d] ^ v[a], 8);
            v[c] += v[d];
            v[b] = rotr(v[b] ^ v[c], 7);
        };
        for (const auto &s : sigma) {
            g(0, 4, 8, 12, m[s[0]], m[s[1]]);
            g(1, 5, 9, 13, m[s[2]], m[s[3]]);
            g(2, 6, 10, 14, m[s[4]], m[s[5]]);
            g(3, 7, 11, 15, m[s[6]], m[s[7]]);
            g(0, 5, 10, 15, m[s[8]], m[s[9]]);
            g(1, 6, 11, 12, m[s[10]], m[s[11]]);
            g(2, 7, 8, 13, m[s[12]], m[s[13]]);
            g(3, 4, 9, 14, m[s[14]], m[s[15]]);
        }
        for (int i = 0; i < 8; ++i) h_[size_t(i)] ^= v[i] ^ v[i + 8];
    }

    std::array<uint32_t, 8> h_;
    uint8_t buffer_[64] = {};
    size_t fill_ = 0;
    uint64_t count_ = 0;
};

// The pack's keystream, applied in place.
void decrypt(Bytes &payload, uint32_t seed, const Bytes &iv) {
    const Bytes seedKey = {uint8_t(seed), uint8_t(seed >> 8), uint8_t(seed >> 16), uint8_t(seed >> 24)};
    Blake2s keyHash(seedKey);
    keyHash.update(iv.data(), iv.size());
    const auto key = keyHash.digest();
    uint32_t input[16] = {0x61707865u, 0x3320646Eu, 0x79622D32u, 0x6B206574u};
    for (int i = 0; i < 8; ++i) input[4 + i] = le32(key.data() + i * 4);
    const uint32_t nonce = xxh32(iv.data(), iv.size(), seed);
    input[12] = 0;
    input[13] = 0;
    input[14] = nonce;
    input[15] = seed;
    uint32_t zeroWord = seed ^ nonce;
    if (zeroWord == 0) zeroWord = seed ? seed : 0xFFFFFFFFu;

    uint32_t words[256];
    for (size_t at = 0; at < payload.size(); at += sizeof(words)) {
        uint32_t x[16];
        std::memcpy(x, input, sizeof(x));
        auto quarter = [&](int a, int b, int c, int d) {
            x[a] += x[b];
            x[d] = rotl(x[d] ^ x[a], 16);
            x[c] += x[d];
            x[b] = rotl(x[b] ^ x[c], 12);
            x[a] += x[b];
            x[d] = rotl(x[d] ^ x[a], 8);
            x[c] += x[d];
            x[b] = rotl(x[b] ^ x[c], 7);
        };
        for (int round = 0; round < 4; ++round) {
            quarter(0, 4, 8, 12);
            quarter(1, 5, 9, 13);
            quarter(2, 6, 10, 14);
            quarter(3, 7, 11, 15);
            quarter(0, 5, 10, 15);
            quarter(1, 6, 11, 12);
            quarter(2, 7, 8, 13);
            quarter(3, 4, 9, 14);
        }
        for (int i = 0; i < 16; ++i) words[i] = x[i] + input[i];
        for (int i = 16; i < 256; ++i) {
            uint32_t w = words[i - 16];
            w ^= w << 13;
            w ^= w >> 17;
            w ^= w << 5;
            words[i] = w ? w : zeroWord;
        }
        const size_t n = std::min(sizeof(words), payload.size() - at);
        for (size_t i = 0; i < n; ++i) payload[at + i] ^= uint8_t(words[i / 4] >> (8 * (i % 4)));
        if (++input[12] == 0) ++input[13];
    }
}

// One LZ4 block (the public block format), appended to out; matches may reach back into
// what out already holds.
bool lz4Block(const uint8_t *p, size_t n, Bytes &out) {
    size_t i = 0;
    while (i < n) {
        const uint8_t token = p[i++];
        size_t literals = token >> 4;
        if (literals == 15)
            for (uint8_t x = 255; x == 255 && i < n;) literals += (x = p[i++]);
        if (literals > n - i) return false;
        out.insert(out.end(), p + i, p + i + literals);
        i += literals;
        if (i >= n) break;
        if (n - i < 2) return false;
        const size_t offset = size_t(p[i]) | size_t(p[i + 1]) << 8;
        i += 2;
        size_t length = token & 15;
        if (length == 15)
            for (uint8_t x = 255; x == 255 && i < n;) length += (x = p[i++]);
        length += 4;
        if (offset == 0 || offset > out.size()) return false;
        const size_t from = out.size() - offset;
        for (size_t k = 0; k < length; ++k) out.push_back(out[from + k]);
    }
    return true;
}

// LZ4 as the packs store it: linked blocks with 16-bit sizes (encrypted packs), or the
// unpacked size and one block.
Bytes unpack(const Bytes &packed, bool blocksFirst) {
    auto blocks = [&](Bytes &out) {
        out.clear();
        for (size_t i = 0; i < packed.size();) {
            if (packed.size() - i < 2) return false;
            const size_t size = size_t(packed[i]) | size_t(packed[i + 1]) << 8;
            i += 2;
            if (size == 0 || size > packed.size() - i || !lz4Block(packed.data() + i, size, out)) return false;
            i += size;
        }
        return !out.empty();
    };
    auto sized = [&](Bytes &out) {
        out.clear();
        if (packed.size() < 4) return false;
        const size_t size = le32(packed.data());
        return lz4Block(packed.data() + 4, packed.size() - 4, out) && out.size() == size;
    };
    Bytes out;
    if (blocksFirst ? (blocks(out) || sized(out)) : (sized(out) || blocks(out))) return out;
    throw std::runtime_error("data pack: bad LZ4");
}

// Each value's check byte.
class CheckBytes {
public:
    explicit CheckBytes(uint32_t seed) {
        for (int i = 0; i < 4; ++i) b_[i] = uint8_t(seed >> (8 * i));
        b_[0] = uint8_t(seed ^ (seed >> 24));
    }
    uint8_t next(uint8_t type) {
        if (type != 0) step(b_);
        return b_[2];
    }
    uint32_t final() const {
        uint8_t b[4] = {b_[0], b_[1], b_[2], b_[3]};
        step(b);
        step(b);
        step(b);
        std::swap(b[0], b[2]);
        return uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24;
    }

private:
    static void step(uint8_t *b) {
        const uint8_t t = uint8_t(b[0] ^ uint8_t(b[0] << 1));
        const uint8_t n = uint8_t(((((t >> 2) ^ b[2]) >> 3) ^ b[2]) ^ t);
        b[0] = b[1];
        b[1] = b[2];
        b[2] = n;
    }
    uint8_t b_[4];
};

class ValueReader {
public:
    ValueReader(const Bytes &b, uint32_t seed) : b_(b), check_(seed) {}

    tTJSVariant value(int depth = 0) {
        if (depth > 128) throw std::runtime_error("data pack: too deep");
        const uint16_t tag = u16();
        const uint8_t type = uint8_t(tag);
        if (uint8_t(tag >> 8) != check_.next(type)) throw std::runtime_error("data pack: check byte");
        switch (type) {
        case 0: return tTJSVariant();
        case 2: return tTJSVariant(string());
        case 4: {
            const uint64_t v = u64();
            return tTJSVariant(tjs_int64(v));
        }
        case 5: {
            const uint64_t bits = u64();
            double d;
            std::memcpy(&d, &bits, 8);
            return tTJSVariant(d);
        }
        case 0x81: {
            const uint32_t count = u32();
            iTJSDispatch2 *array = TJSCreateArrayObject();
            tTJSVariant result(array, array);
            array->Release();
            for (uint32_t i = 0; i < count; ++i) {
                tTJSVariant v = value(depth + 1);
                array->PropSetByNum(TJS_MEMBERENSURE, tjs_int(i), &v, array);
            }
            return result;
        }
        case 0xC1: {
            const uint32_t count = u32();
            iTJSDispatch2 *dict = TJSCreateDictionaryObject();
            tTJSVariant result(dict, dict);
            dict->Release();
            for (uint32_t i = 0; i < count; ++i) {
                const ttstr key = string();
                tTJSVariant v = value(depth + 1);
                dict->PropSet(TJS_MEMBERENSURE, key.c_str(), nullptr, &v, dict);
            }
            return result;
        }
        default: throw std::runtime_error("data pack: unknown value type");
        }
    }

    bool checksOut() {
        const uint32_t stored = u32(), expected = check_.final();
        // Some packs store the word with its top byte cleared.
        return stored == expected || stored == (expected & 0x00FFFFFFu);
    }

private:
    void need(size_t n) {
        if (n > b_.size() - at_) throw std::runtime_error("data pack: truncated");
    }
    uint16_t u16() {
        need(2);
        const uint16_t v = uint16_t(b_[at_] | b_[at_ + 1] << 8);
        at_ += 2;
        return v;
    }
    uint32_t u32() {
        need(4);
        const uint32_t v = le32(b_.data() + at_);
        at_ += 4;
        return v;
    }
    uint64_t u64() {
        const uint64_t lo = u32();
        return lo | uint64_t(u32()) << 32;
    }
    ttstr string() {
        const uint32_t n = u32();
        need(size_t(n) * 2);
        // UTF-16 in the pack; TJS strings here are UTF-8.
        std::string s;
        s.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t c = uint32_t(b_[at_ + i * 2] | b_[at_ + i * 2 + 1] << 8);
            if (c >= 0xD800 && c < 0xDC00 && i + 1 < n) {
                const uint32_t low = uint32_t(b_[at_ + i * 2 + 2] | b_[at_ + i * 2 + 3] << 8);
                if (low >= 0xDC00 && low < 0xE000) {
                    c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
                    ++i;
                }
            }
            if (c < 0x80) {
                s += char(c);
            } else if (c < 0x800) {
                s += char(0xC0 | c >> 6);
                s += char(0x80 | (c & 63));
            } else if (c < 0x10000) {
                s += char(0xE0 | c >> 12);
                s += char(0x80 | (c >> 6 & 63));
                s += char(0x80 | (c & 63));
            } else {
                s += char(0xF0 | c >> 18);
                s += char(0x80 | (c >> 12 & 63));
                s += char(0x80 | (c >> 6 & 63));
                s += char(0x80 | (c & 63));
            }
        }
        at_ += size_t(n) * 2;
        return ttstr(s.c_str());
    }

    const Bytes &b_;
    size_t at_ = 0;
    CheckBytes check_;
};

bool decodePack(const Bytes &b, const std::string &outerIV, tTJSVariant &result) {
    if (b.size() < 16 || std::memcmp(b.data(), "TJS/", 4) != 0 || b[5] != 's' || b[6] != '0' || b[7] != 0) return false;
    const uint8_t compression = b[4];
    const uint32_t seed = le32(b.data() + 8);
    const uint16_t encrypted = uint16_t(b[12] | b[13] << 8), ivLength = uint16_t(b[14] | b[15] << 8);
    if (16 + size_t(ivLength) > b.size() || (compression != 'n' && compression != '4') || encrypted > 1)
        throw std::runtime_error("data pack: unsupported header");
    Bytes payload(b.begin() + 16 + ivLength, b.end());
    if (encrypted) {
        const Bytes iv = outerIV.empty() ? Bytes(b.begin() + 16, b.begin() + 16 + ivLength) : Bytes(outerIV.begin(), outerIV.end());
        decrypt(payload, seed, iv);
    }
    if (compression == '4') payload = unpack(payload, encrypted != 0);
    ValueReader reader(payload, seed);
    result = reader.value();
    if (!reader.checksOut()) throw std::runtime_error("data pack: final check");
    return true;
}

} // namespace

// Scripts.loadDataPack(storage, options): options.outeriv replaces the pack's own IV.
static tjs_error ShioriLoadDataPack(tTJSVariant *result, tjs_int count, tTJSVariant **param, iTJSDispatch2 *) {
    if (count < 1 || !param[0]) return TJS_E_BADPARAMCOUNT;
    const ttstr name(*param[0]);
    std::string outerIV;
    if (count > 1 && param[1] && param[1]->Type() == tvtObject && param[1]->AsObjectNoAddRef()) {
        tTJSVariant iv;
        iTJSDispatch2 *options = param[1]->AsObjectNoAddRef();
        if (TJS_SUCCEEDED(options->PropGet(0, "outeriv", nullptr, &iv, options)) && iv.Type() == tvtString)
            outerIV = ttstr(iv).AsStdString();
    }
    Bytes bytes;
    std::string error;
    if (!shiorikrkr::util::readStorage(name, bytes, error)) TVPThrowExceptionMessage(ttstr(error.c_str()).c_str());
    tTJSVariant value;
    try {
        if (!decodePack(bytes, outerIV, value)) TVPThrowExceptionMessage((ttstr("not a data pack: ") + name).c_str());
    } catch (const std::runtime_error &e) {
        TVPThrowExceptionMessage((ttstr(e.what()) + ttstr(": ") + name).c_str());
    }
    if (result) *result = value;
    return TJS_S_OK;
}

NCB_ATTACH_FUNCTION(loadDataPack, Scripts, ShioriLoadDataPack);
