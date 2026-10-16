/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

// KiriKiri Z "Hx" archives (Hxv4 index chunk), ported from GARbro's HxCrypt
// (ArcFormats/KiriKiri/HxCrypt.cs, MIT, Copyright (C) 2018 by morkt; see README.md (Licences)).
// An Hx archive's File entries carry numbered names; its Hxv4 chunk points at an index,
// encrypted with ChaCha20 and compressed, that gives each entry its directory and file name
// hashes and its file key. Each entry is decrypted with a filter built from that key through
// the title's CxDec program (cx_decrypt.h). Keys come only from GARbro's public database
// (the SHCX v2 Hx section, GARbroDatabase.swift / garbro-cx.py), never from the executable;
// everything is decrypted in memory.
#pragma once

#include "cx_decrypt.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace shiorikrkr {

struct HxIndexKey {
    std::array<uint8_t, 32> key1{}; // ChaCha20 key
    std::array<uint8_t, 16> key2{}; // its first 8 bytes are the nonce
};

struct HxScheme {
    int randomType = 0;      // GARbro's RandomType (0: the old generator)
    uint64_t filterKey = 0;
    bool hasDefaultKey = false;
    HxIndexKey defaultKey;
    std::vector<std::pair<std::string, HxIndexKey>> archiveKeys; // by archive file name
    // GARbro's names list, when it has one for the title: upper-case hex of a directory hash
    // (8 bytes) or file name hash (32 bytes) -> the name.
    std::unordered_map<std::string, std::string> directoryNames, fileNames;

    /// Reads the Hx section of an SHCX v2 file (CxScheme::extension). False when there is
    /// none (a plain CxDec scheme) or it's malformed (`error` set).
    static bool parse(const std::vector<uint8_t> &extension, HxScheme &out, std::string &error);
};

struct HxEntry {
    uint64_t id = 0;   // the File entry's number (its name is hxEntryName(id))
    uint64_t key = 0;  // the entry's file key
    std::string directoryHash, nameHash; // upper-case hex
    std::string name;  // directory/name when the names list knows the name, else empty
};

/// Decrypts and reads an Hxv4 index (`blob`: the bytes the chunk points at). The key is the
/// archive's own (by `archiveName`, compared without case), else the scheme's default, else
/// each of the title's other keys in turn (an archive added by an update may share one).
bool hxReadIndex(const HxScheme &scheme, const std::string &archiveName, const uint8_t *blob, size_t size,
                 std::vector<HxEntry> &out, std::string &error);

/// The name an Hx archive's File entry carries for an index entry: the id in 14-bit groups,
/// each as U+5000 + group, low group first.
std::u16string hxEntryName(uint64_t id);

// The index's name hashes (published facts about Hx, written here from the algorithms'
// specifications: see README.md (Licences)). Both hash UTF-16LE text followed by "xp3hnp".
/// SipHash-2-4 (Aumasson & Bernstein, reference specification) of `message` under `key`.
uint64_t sipHash24(const uint8_t key[16], const uint8_t *message, size_t size);
/// BLAKE2s-256, unkeyed (RFC 7693).
std::array<uint8_t, 32> blake2s256(const uint8_t *message, size_t size);
/// A directory's hash, upper-case hex of its 8 little-endian bytes: SipHash-2-4 with a zero
/// key of the directory with its trailing slash ("image/sys/"); the root ("" or "/") hashes
/// the constant alone.
std::string hxDirectoryHash(const std::u16string &directory);
/// A file name's hash (no directory), upper-case hex of BLAKE2s-256.
std::string hxFileNameHash(const std::u16string &name);

/// One entry's decryption, from its file key (GARbro's HxFilter).
class HxFilter {
public:
    HxFilter(const CxDecryptor &cx, uint32_t cxMask, uint32_t cxOffset, uint64_t filterKey, const HxEntry &entry);
    HxFilter(const CxDecryptor &cx, uint32_t cxMask, uint32_t cxOffset, uint64_t filterKey, uint64_t id,
             uint64_t key);

    /// Decrypts `count` bytes at `offset` in the entry's decompressed contents.
    void decrypt(uint64_t offset, uint8_t *buffer, size_t count) const;

private:
    struct Span {
        uint32_t fill = 0;          // the byte every byte is XORed with, in all four lanes
        uint8_t key1 = 0, key2 = 0; // single bytes at two positions
        uint64_t position1 = 0, position2 = 0;
        void init(uint64_t key);
        void decrypt(uint64_t position, uint8_t *buffer, size_t count) const;
    };
    Span spans_[2];
    uint64_t split_ = 0;
    std::array<uint8_t, 16> header_{}; // XORed over the first 16 bytes
};

} // namespace shiorikrkr
