/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

// NEKO WORKs XP3 archives (NEKOPARA and its family):
// - real names live in 'neko' chunks (Vol. 0, Extra, Vol. 4) or, in the older layout, 'eliF'
//   chunks (Vol. 1 and 2), each an adlr hash, a length and a UTF-16 name (Shiori's krkr2 patch
//   maps them like 'hnfn'); unnamed entries (a decoy startup.tjs, a warning) are skipped;
// - every protected entry is XORed with one byte folded from its hash, and in the newer layout
//   its first byte also with the hash's low byte, each mixed with a per-title constant;
// - the constants come from the archive's own PNG and Ogg entries, whose first bytes are known.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace shiorikrkr {

struct NekoWorksKey {
    bool valid = false;
    bool firstByte = true; // the newer 'neko' layout keys the first byte too
    uint8_t foldXor = 0;   // xored into the folded hash
    uint8_t firstXor = 0;  // xored into the hash's low byte (first byte)
    uint8_t zeroKey = 0;   // used when the folded key is 0
    uint8_t zeroFirst = 0; // used when the first-byte key is 0
};

/// Finds the key from a game folder's archives (data.xp3 first, then the others: a title's
/// archives share it). Returns false for archives that aren't NEKO WORKs'.
bool detectNekoWorksKey(const std::string &gameRoot, NekoWorksKey &key, std::string &log);

void decodeNekoWorks(const NekoWorksKey &key, uint32_t hash, uint64_t offset, uint8_t *buffer, size_t size);

} // namespace shiorikrkr
