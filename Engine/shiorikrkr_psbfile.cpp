/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

// psbfile.dll for ShioriKrkr: the PSBFile class NEKO WORKs' scripts read their scenario
// files (.scn), snapshots and packed images with: new PSBFile(storage or octet), then .root,
// the file's tree as TJS dictionaries, arrays, numbers, strings and octets (resources).
//
// The PSB layout is the one Shiori's own reader knows (CatSystemKit's PSB.swift): a header of
// offsets, a key trie in three integer arrays, string offsets, resource offsets and lengths,
// and tagged values. Written fresh from that; nothing from krkr2's or krkrsdl3's psbfile
// plug-ins (PROVENANCE.md).
#include "tjsCommHead.h"
#include <ncbind.hpp>
#include "tjsArray.h"
#include "tjsDictionary.h"
#include "StorageIntf.h"
#include "UtilStreams.h"
#include "MsgIntf.h"

#include <zlib.h>
#include <cstring>
#include <string>
#include <vector>

#define NCB_MODULE_NAME TJS_W("psbfile.dll")

namespace {

class PSBReader {
public:
    explicit PSBReader(std::vector<uint8_t> data) : b_(std::move(data)) {}

    bool parse(tTJSVariant &root, std::string &error) {
        if (b_.size() >= 16 && !std::memcmp(b_.data(), "PSZ\0", 4)) {
            // PSB under zlib: packed length, inflated length, reserved, stream.
            const uint32_t packed = u32(4), size = u32(8);
            if (16 + uint64_t(packed) > b_.size()) return fail(error, "psz truncated");
            std::vector<uint8_t> out(size);
            uLongf length = size;
            if (uncompress(out.data(), &length, b_.data() + 16, packed) != Z_OK) return fail(error, "psz inflate");
            out.resize(length);
            b_.swap(out);
        }
        if (b_.size() < 0x28 || std::memcmp(b_.data(), "PSB\0", 4)) return fail(error, "not a PSB");
        const int flags = b_[6] | b_[7] << 8;
        if (flags & 1) return fail(error, "encrypted PSB header");
        const size_t names = u32(12), strings = u32(16);
        stringData_ = u32(20);
        const size_t chunkOffsets = u32(24), chunkLengths = u32(28);
        chunkData_ = u32(32);
        const size_t rootAt = u32(36);
        size_t p = names;
        std::vector<uint64_t> charset, nameData, tree;
        if (!integers(p, charset) || !integers(p, nameData) || !integers(p, tree)) return fail(error, "name table");
        names_.reserve(tree.size());
        for (uint64_t index : tree) {
            std::string out;
            if (index < nameData.size()) {
                uint64_t chr = nameData[index];
                while (chr != 0 && chr < nameData.size() && out.size() < 1024) {
                    const uint64_t code = nameData[chr];
                    if (code >= charset.size()) break;
                    out.push_back(char(uint8_t(chr - charset[code])));
                    chr = code;
                }
            }
            names_.emplace_back(out.rbegin(), out.rend());
        }
        p = strings;
        if (!integers(p, stringOffsets_)) return fail(error, "string table");
        p = chunkOffsets;
        if (!integers(p, chunkOffsets_)) return fail(error, "resource offsets");
        p = chunkLengths;
        if (!integers(p, chunkLengths_)) return fail(error, "resource lengths");
        if (!value(rootAt, root, 0)) return fail(error, "value at " + std::to_string(failedAt_));
        return true;
    }

private:
    static bool fail(std::string &error, const std::string &what) {
        error = what;
        return false;
    }

    uint32_t u32(size_t o) const {
        return o + 4 <= b_.size() ? uint32_t(b_[o]) | uint32_t(b_[o + 1]) << 8 | uint32_t(b_[o + 2]) << 16 |
                                        uint32_t(b_[o + 3]) << 24
                                  : 0;
    }

    uint64_t uint(size_t p, int n) const {
        uint64_t v = 0;
        for (int i = 0; i < n && p + i < b_.size(); ++i) v |= uint64_t(b_[p + i]) << (8 * i);
        return v;
    }

    bool integers(size_t &p, std::vector<uint64_t> &out) const {
        if (p >= b_.size()) return false;
        const int countSize = int(b_[p]) - 0xC;
        if (countSize < 1 || countSize > 8) return false;
        const uint64_t count = uint(p + 1, countSize);
        const size_t widthAt = p + 1 + countSize;
        if (widthAt >= b_.size()) return false;
        const int width = int(b_[widthAt]) - 0xC;
        if (width < 1 || width > 8) return false;
        const size_t start = widthAt + 1;
        if (start + count * width > b_.size()) return false;
        out.resize(size_t(count));
        for (uint64_t i = 0; i < count; ++i) out[size_t(i)] = uint(start + i * width, width);
        p = start + count * width;
        return true;
    }

    ttstr string(uint64_t index) const {
        if (index >= stringOffsets_.size()) return ttstr();
        const size_t start = stringData_ + stringOffsets_[size_t(index)];
        size_t end = start;
        while (end < b_.size() && b_[end]) ++end;
        return ttstr(std::string(reinterpret_cast<const char *>(b_.data()) + std::min(start, b_.size()),
                                 end > start ? end - start : 0)
                         .c_str());
    }

    tTJSVariant resource(uint64_t index) const {
        if (index >= chunkOffsets_.size() || index >= chunkLengths_.size()) return tTJSVariant();
        const size_t start = chunkData_ + chunkOffsets_[size_t(index)];
        const size_t length = chunkLengths_[size_t(index)];
        if (start + length > b_.size()) return tTJSVariant();
        return tTJSVariant(b_.data() + start, tjs_uint(length));
    }

    bool value(size_t p, tTJSVariant &out, int depth) {
        if (p >= b_.size() || depth > 256) {
            failedAt_ = p;
            return false;
        }
        const int type = b_[p];
        switch (type) {
        case 0x0: case 0x1: out = tTJSVariant(); return true;
        case 0x2: out = tTJSVariant(false); return true;
        case 0x3: out = tTJSVariant(true); return true;
        case 0x4: out = tTJSVariant(tjs_int64(0)); return true;
        case 0x5: case 0x6: case 0x7: case 0x8: case 0x9: case 0xA: case 0xB: case 0xC: {
            const int n = type - 4;
            int64_t v = int64_t(uint(p + 1, n));
            if (n < 8 && (v & (int64_t(1) << (8 * n - 1)))) v -= int64_t(1) << (8 * n);
            out = tTJSVariant(tjs_int64(v));
            return true;
        }
        case 0xD: case 0xE: case 0xF: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: {
            size_t q = p;
            std::vector<uint64_t> list;
            if (!integers(q, list)) break;
            iTJSDispatch2 *array = TJSCreateArrayObject();
            for (size_t i = 0; i < list.size(); ++i) {
                tTJSVariant v = tTJSVariant((tjs_int64)list[i]);
                array->PropSetByNum(TJS_MEMBERENSURE, tjs_int(i), &v, array);
            }
            out = tTJSVariant(array, array);
            array->Release();
            return true;
        }
        case 0x15: case 0x16: case 0x17: case 0x18: out = tTJSVariant(string(uint(p + 1, type - 0x14))); return true;
        case 0x19: case 0x1A: case 0x1B: case 0x1C: out = resource(uint(p + 1, type - 0x18)); return true;
        case 0x1D: out = tTJSVariant(tjs_real(0)); return true;
        case 0x1E: {
            float f;
            const uint32_t bits = u32(p + 1);
            std::memcpy(&f, &bits, 4);
            out = tTJSVariant(tjs_real(f));
            return true;
        }
        case 0x1F: {
            const uint64_t bits = uint64_t(u32(p + 1)) | uint64_t(u32(p + 5)) << 32;
            double d;
            std::memcpy(&d, &bits, 8);
            out = tTJSVariant(tjs_real(d));
            return true;
        }
        case 0x20: {
            size_t q = p + 1;
            std::vector<uint64_t> offsets;
            if (!integers(q, offsets)) break;
            iTJSDispatch2 *array = TJSCreateArrayObject();
            bool ok = true;
            for (size_t i = 0; i < offsets.size() && ok; ++i) {
                tTJSVariant v;
                ok = value(q + offsets[i], v, depth + 1);
                if (ok) array->PropSetByNum(TJS_MEMBERENSURE, tjs_int(i), &v, array);
            }
            out = tTJSVariant(array, array);
            array->Release();
            return ok;
        }
        case 0x21: {
            size_t q = p + 1;
            std::vector<uint64_t> keys, offsets;
            if (!integers(q, keys) || !integers(q, offsets)) break;
            iTJSDispatch2 *dict = TJSCreateDictionaryObject();
            bool ok = true;
            for (size_t i = 0; i < keys.size() && i < offsets.size() && ok; ++i) {
                if (keys[i] >= names_.size()) continue;
                tTJSVariant v;
                ok = value(q + offsets[i], v, depth + 1);
                if (ok) {
                    ttstr key(names_[size_t(keys[i])].c_str());
                    dict->PropSet(TJS_MEMBERENSURE, key.c_str(), nullptr, &v, dict);
                }
            }
            out = tTJSVariant(dict, dict);
            dict->Release();
            return ok;
        }
        case 0x22: case 0x23: case 0x24: case 0x25: out = tTJSVariant(); return true; // extra resources
        default: break;
        }
        failedAt_ = p;
        return false;
    }

    std::vector<uint8_t> b_;
    std::vector<std::string> names_;
    std::vector<uint64_t> stringOffsets_, chunkOffsets_, chunkLengths_;
    size_t stringData_ = 0, chunkData_ = 0, failedAt_ = 0;
};

} // namespace

class ShioriPSBFile {
public:
    ShioriPSBFile() {}

    static tjs_error factory(ShioriPSBFile **result, tjs_int count, tTJSVariant **param, iTJSDispatch2 *) {
        auto *self = new ShioriPSBFile();
        if (count >= 1 && param[0] && param[0]->Type() != tvtVoid) {
            try {
                self->load(*param[0]);
            } catch (...) {
                delete self;
                throw;
            }
        }
        *result = self;
        return TJS_S_OK;
    }

    void load(tTJSVariant source) {
        std::vector<uint8_t> bytes;
        ttstr name;
        if (source.Type() == tvtOctet) {
            if (tTJSVariantOctet *o = source.AsOctetNoAddRef())
                bytes.assign(o->GetData(), o->GetData() + o->GetLength());
            name = TJS_W("(octet)");
        } else {
            name = ttstr(source);
            std::unique_ptr<tTJSBinaryStream> stream(TVPCreateStream(TVPGetPlacedPath(name), TJS_BS_READ));
            bytes.resize(size_t(stream->GetSize()));
            if (!bytes.empty()) stream->ReadBuffer(bytes.data(), tjs_uint(bytes.size()));
        }
        PSBReader reader(std::move(bytes));
        std::string error;
        tTJSVariant root;
        if (!reader.parse(root, error))
            TVPThrowExceptionMessage((ttstr(TJS_W("PSBFile: ")) + name + TJS_W(": ") + ttstr(error.c_str())).c_str());
        root_ = root;
    }

    tTJSVariant getRoot() const { return root_; }

private:
    tTJSVariant root_;
};

static tjs_error ShioriPSBFileFactory(ShioriPSBFile **result, tjs_int count, tTJSVariant **param, iTJSDispatch2 *objthis) {
    return ShioriPSBFile::factory(result, count, param, objthis);
}

NCB_REGISTER_CLASS_DIFFER(PSBFile, ShioriPSBFile) {
    RawCallback(ShioriPSBFileFactory);
    NCB_METHOD(load);
    NCB_PROPERTY_RO(root, getRoot);
}
