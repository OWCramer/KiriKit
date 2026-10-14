/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

// KiriKiri CxDec archive decryption, the algorithm of GARbro's CxEncryption (MIT,
// Copyright (C) 2014-2016 morkt; see PROVENANCE.md) written for ShioriKrkr. Decrypts in
// memory only, from a scheme the host builds out of GARbro's database (SHCX layout,
// App/Sources/Engines/KiriKiri/GARbroDatabase.swift); nothing is read from the game's
// executable.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace shiorikrkr {

struct CxScheme {
    uint32_t mask = 0;
    uint32_t offset = 0;
    std::array<uint8_t, 3> prologOrder{};
    std::array<uint8_t, 6> oddBranchOrder{};
    std::array<uint8_t, 8> evenBranchOrder{};
    std::array<uint32_t, 1024> controlBlock{};
    /// The programs' random generator: 0 CxDec's own, 1 and 2 Hx's two (GARbro's HxProgram
    /// RandomType 0 and 1). Set by an SHCX v2 file's Hx section (shiori_hx.h).
    int random = 0;
    /// An SHCX v2 file's bytes after the Cx part (the Hx section), for shiori_hx.
    std::vector<uint8_t> extension;

    /// Reads an SHCX file: "SHCX", version 1 or 2, mask, offset, the prolog/odd/even branch
    /// orders (3 + 6 + 8 bytes, padded to 20), then 1024 little-endian control words.
    /// Version 2 may carry more after that (`extension`).
    static bool load(const std::string &path, CxScheme &out, std::string &error);
};

class CxDecryptor {
public:
    explicit CxDecryptor(const CxScheme &scheme);

    /// Decrypts `count` bytes of the file whose XP3 `adlr` hash is `hash`, starting at
    /// `offset` in the file's decompressed contents.
    void decrypt(uint32_t hash, uint64_t offset, uint8_t *buffer, size_t count) const;

    /// The scheme's program for `key` run on its argument and the argument's complement
    /// (GARbro's ExecuteXCode); Hx builds its file keys from it.
    void xcode(uint32_t key, uint32_t &first, uint32_t &second) const;

private:
    struct Program {
        std::vector<uint32_t> code;
        int length = 0;   // bytes the x86 original would take; capped at 128
        uint32_t seed = 0;
        int kind = 0;               // CxScheme::random
        uint64_t state[2] = {0, 0}; // Hx's generators
        bool emitNop(int count);
        bool emit(uint32_t op, int bytes = 1);
        bool emitWord(uint32_t word);
        uint32_t random();
        uint32_t run(uint32_t argument, const std::array<uint32_t, 1024> &control) const;
    };

    bool build(Program &program, int stage) const;
    bool body(Program &program, int stage) const;
    bool body2(Program &program, int stage) const;
    bool prolog(Program &program) const;
    bool oddBranch(Program &program) const;
    bool evenBranch(Program &program) const;
    void decode(uint32_t key, uint64_t offset, uint8_t *buffer, size_t count) const;

    CxScheme scheme_;
    std::array<Program, 128> programs_;
};

} // namespace shiorikrkr
