/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

// The CxDec algorithm as GARbro implements it (ArcFormats/KiriKiri/KiriKiriCx.cs, MIT,
// Copyright (C) 2014-2016 morkt). Each file's key picks one of 128 small programs, which
// GARbro generates from the scheme's branch orders and control block the way the original
// TPM plug-in emits x86 code; here they run as a list of operations. The programs are built
// once at load, so decryption is lock-free from any loader thread. Hx titles build the same
// programs with their own random generators (GARbro's HxProgram, ArcFormats/KiriKiri/HxCrypt.cs,
// MIT, Copyright (C) 2018 by morkt).
#include "cx_decrypt.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace shiorikrkr {

namespace {

// Operations; IMMED-flagged ones are followed by a 32-bit word in the code list.
enum : uint32_t {
    kNop, kRet, kMovEdiArg, kPushEbx, kPopEbx, kPushEcx, kPopEcx, kMovEbxEax, kMovEcxEbx,
    kMovEaxEdi, kMovEaxIndirect, kAddEaxEbx, kSubEaxEbx, kImulEaxEbx, kAndEcx0F, kShrEbx1,
    kShlEax1, kShrEaxCl, kShlEaxCl, kOrEaxEbx, kNotEax, kNegEax, kDecEax, kIncEax,
    kImmediate = 0x100,
    kMovEaxImm, kAndEbxImm, kAndEaxImm, kXorEaxImm, kAddEaxImm, kSubEaxImm,
};

constexpr int kLengthLimit = 0x80;

uint32_t readLE32(const uint8_t *p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

} // namespace

bool CxScheme::load(const std::string &path, CxScheme &out, std::string &error) {
    FILE *file = std::fopen(path.c_str(), "rb");
    if (!file) {
        error = "can't open " + path;
        return false;
    }
    std::vector<uint8_t> data;
    uint8_t chunk[4096];
    size_t n;
    while ((n = std::fread(chunk, 1, sizeof chunk, file)) > 0) data.insert(data.end(), chunk, chunk + n);
    std::fclose(file);
    constexpr size_t kHeader = 16, kOrders = 20, kControl = 1024 * 4;
    const uint32_t version = data.size() >= 8 ? readLE32(&data[4]) : 0;
    if (data.size() < kHeader + kOrders + kControl || std::memcmp(data.data(), "SHCX", 4) != 0 ||
        (version == 1 && data.size() != kHeader + kOrders + kControl) || version < 1 || version > 2) {
        error = "not an SHCX v1 or v2 scheme";
        return false;
    }
    out.extension.assign(data.begin() + kHeader + kOrders + kControl, data.end());
    out.mask = readLE32(&data[8]);
    out.offset = readLE32(&data[12]);
    const uint8_t *orders = &data[kHeader];
    std::memcpy(out.prologOrder.data(), orders, 3);
    std::memcpy(out.oddBranchOrder.data(), orders + 3, 6);
    std::memcpy(out.evenBranchOrder.data(), orders + 9, 8);
    for (size_t i = 0; i < 1024; ++i) out.controlBlock[i] = readLE32(&data[kHeader + kOrders + i * 4]);
    return true;
}

// MARK: - Program building

bool CxDecryptor::Program::emitNop(int count) {
    if (length + count > kLengthLimit) return false;
    length += count;
    return true;
}

bool CxDecryptor::Program::emit(uint32_t op, int bytes) {
    if (length + bytes > kLengthLimit) return false;
    length += bytes;
    code.push_back(op);
    return true;
}

bool CxDecryptor::Program::emitWord(uint32_t word) {
    if (length + 4 > kLengthLimit) return false;
    length += 4;
    code.push_back(word);
    return true;
}

uint32_t CxDecryptor::Program::random() {
    if (kind == 1 || kind == 2) {
        // Hx's generators (GARbro's HxProgram.GetOldRandom/GetNewRandom, HxCrypt.cs, MIT).
        const uint64_t a = state[0], b = state[1];
        const uint32_t aLo = uint32_t(a), aHi = uint32_t(a >> 32);
        uint64_t t;
        if (kind == 1) {
            const uint32_t cLo = aHi ^ uint32_t(b >> 32), cHi = aLo ^ uint32_t(b);
            const uint64_t c = uint64_t(cLo) | uint64_t(cHi) << 32;
            const uint64_t e = uint64_t(cHi) | uint64_t(cLo) << 32;
            t = uint64_t(cHi) << 21;
            t ^= a >> 15;
            t ^= cHi;
            const uint32_t s0Lo = uint32_t(t);
            t = uint64_t(aHi >> 15);
            t |= uint64_t(aLo) << 17;
            t ^= e >> 11;
            t ^= cLo;
            const uint32_t s0Hi = uint32_t(t);
            state[0] = uint64_t(s0Lo) | uint64_t(s0Hi) << 32;
            state[1] = uint64_t(uint32_t(c >> 4)) | uint64_t(uint32_t(e >> 4)) << 32;
            const uint64_t d = a + b;
            t = d << 17;
            t |= uint32_t(d >> 32) >> 15;
            t += a;
        } else {
            const uint32_t cLo = aLo ^ uint32_t(b), cHi = aHi ^ uint32_t(b >> 32);
            const uint64_t c = uint64_t(cLo) | uint64_t(cHi) << 32;
            t = uint64_t(aLo) << 24;
            t |= aHi >> 8;
            t ^= uint64_t(cLo) << 16;
            t ^= cLo;
            const uint32_t s0Lo = uint32_t(t);
            t = c >> 16;
            t ^= a >> 8;
            t ^= cHi;
            const uint32_t s0Hi = uint32_t(t);
            t = cHi >> 27;
            t |= uint64_t(cLo) << 5;
            const uint32_t s1Hi = uint32_t(t);
            const uint32_t s1Lo = uint32_t(c >> 27);
            state[0] = uint64_t(s0Lo) | uint64_t(s0Hi) << 32;
            state[1] = uint64_t(s1Lo) | uint64_t(s1Hi) << 32;
            const uint64_t d = 5 * a;
            t = uint32_t(d >> 32) >> 25;
            t |= d << 7;
            t *= 9;
        }
        return uint32_t(t);
    }
    const uint32_t previous = seed;
    seed = 1103515245u * previous + 12345u;
    return seed ^ (previous << 16) ^ (previous >> 16);
}

namespace {
uint64_t splitMix(uint64_t &seed) {
    seed += 0x9e3779b97f4a7c15ull;
    uint64_t z = seed;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}
} // namespace

CxDecryptor::CxDecryptor(const CxScheme &scheme) : scheme_(scheme) {
    for (uint32_t index = 0; index < programs_.size(); ++index) {
        Program &program = programs_[index];
        program.seed = index;
        program.kind = scheme.random;
        if (program.kind == 1 || program.kind == 2) {
            // Hx seeds its generator from the program index and its complement.
            uint64_t s = uint64_t(index) | (~uint64_t(index)) << 32;
            program.state[0] = splitMix(s);
            program.state[1] = splitMix(s);
        }
        // The largest program that fits, from stage 5 down. A failed stage clears the code
        // but not the generator: the next stage continues its sequence (as GARbro's does).
        for (int stage = 5; stage > 0; --stage) {
            program.code.clear();
            program.length = 0;
            if (build(program, stage)) break;
        }
    }
}

bool CxDecryptor::build(Program &p, int stage) const {
    return p.emitNop(5) && p.emit(kMovEdiArg, 4) && body(p, stage) && p.emitNop(5) && p.emit(kRet);
}

bool CxDecryptor::body(Program &p, int stage) const {
    if (stage == 1) return prolog(p);
    if (!p.emit(kPushEbx)) return false;
    if (!((p.random() & 1) ? body(p, stage - 1) : body2(p, stage - 1))) return false;
    if (!p.emit(kMovEbxEax, 2)) return false;
    if (!((p.random() & 1) ? body(p, stage - 1) : body2(p, stage - 1))) return false;
    return oddBranch(p) && p.emit(kPopEbx);
}

bool CxDecryptor::body2(Program &p, int stage) const {
    if (stage == 1) return prolog(p);
    const bool ok = (p.random() & 1) ? body(p, stage - 1) : body2(p, stage - 1);
    return ok && evenBranch(p);
}

bool CxDecryptor::prolog(Program &p) const {
    switch (scheme_.prologOrder[p.random() % 3]) {
    case 0: // eax = random
        return p.emit(kMovEaxImm) && p.emitWord(p.random());
    case 1: // eax = argument
        return p.emit(kMovEaxEdi, 2);
    case 2: // eax = control[random & 0x3ff]
        return p.emitNop(5) && p.emit(kMovEaxImm, 2) && p.emitWord(p.random() & 0x3ff) &&
               p.emit(kMovEaxIndirect, 0);
    }
    return true;
}

bool CxDecryptor::evenBranch(Program &p) const {
    switch (scheme_.evenBranchOrder[p.random() & 7]) {
    case 0: return p.emit(kNotEax, 2);
    case 1: return p.emit(kDecEax);
    case 2: return p.emit(kNegEax, 2);
    case 3: return p.emit(kIncEax);
    case 4:
        return p.emitNop(5) && p.emit(kAndEaxImm) && p.emitWord(0x3ff) && p.emit(kMovEaxIndirect, 3);
    case 5: // swap odd and even bits
        return p.emit(kPushEbx) && p.emit(kMovEbxEax, 2) && p.emit(kAndEbxImm, 2) &&
               p.emitWord(0xaaaaaaaa) && p.emit(kAndEaxImm) && p.emitWord(0x55555555) &&
               p.emit(kShrEbx1, 2) && p.emit(kShlEax1, 2) && p.emit(kOrEaxEbx, 2) && p.emit(kPopEbx);
    case 6: return p.emit(kXorEaxImm) && p.emitWord(p.random());
    case 7: {
        const bool add = p.random() & 1;
        return p.emit(add ? kAddEaxImm : kSubEaxImm) && p.emitWord(p.random());
    }
    }
    return true;
}

bool CxDecryptor::oddBranch(Program &p) const {
    switch (scheme_.oddBranchOrder[p.random() % 6]) {
    case 0:
        return p.emit(kPushEcx) && p.emit(kMovEcxEbx, 2) && p.emit(kAndEcx0F, 3) && p.emit(kShrEaxCl, 2) &&
               p.emit(kPopEcx);
    case 1:
        return p.emit(kPushEcx) && p.emit(kMovEcxEbx, 2) && p.emit(kAndEcx0F, 3) && p.emit(kShlEaxCl, 2) &&
               p.emit(kPopEcx);
    case 2: return p.emit(kAddEaxEbx, 2);
    case 3: return p.emit(kNegEax, 2) && p.emit(kAddEaxEbx, 2);
    case 4: return p.emit(kImulEaxEbx, 3);
    case 5: return p.emit(kSubEaxEbx, 2);
    }
    return true;
}

// MARK: - Running

uint32_t CxDecryptor::Program::run(uint32_t argument, const std::array<uint32_t, 1024> &control) const {
    uint32_t eax = 0, ebx = 0, ecx = 0, edi = 0;
    uint32_t stack[64];
    int depth = 0;
    for (size_t pc = 0; pc < code.size(); ++pc) {
        const uint32_t op = code[pc];
        uint32_t immediate = 0;
        if (op & kImmediate) {
            if (++pc >= code.size()) return 0;
            immediate = code[pc];
        }
        switch (op) {
        case kNop: break;
        case kMovEdiArg: edi = argument; break;
        case kPushEbx: if (depth < 64) stack[depth++] = ebx; break;
        case kPopEbx: if (depth > 0) ebx = stack[--depth]; break;
        case kPushEcx: if (depth < 64) stack[depth++] = ecx; break;
        case kPopEcx: if (depth > 0) ecx = stack[--depth]; break;
        case kMovEbxEax: ebx = eax; break;
        case kMovEcxEbx: ecx = ebx; break;
        case kMovEaxEdi: eax = edi; break;
        case kMovEaxIndirect: eax = eax < control.size() ? ~control[eax] : 0; break;
        case kAddEaxEbx: eax += ebx; break;
        case kSubEaxEbx: eax -= ebx; break;
        case kImulEaxEbx: eax *= ebx; break;
        case kAndEcx0F: ecx &= 0x0f; break;
        case kShrEbx1: ebx >>= 1; break;
        case kShlEax1: eax <<= 1; break;
        case kShrEaxCl: eax >>= ecx; break;
        case kShlEaxCl: eax <<= ecx; break;
        case kOrEaxEbx: eax |= ebx; break;
        case kNotEax: eax = ~eax; break;
        case kNegEax: eax = 0u - eax; break;
        case kDecEax: --eax; break;
        case kIncEax: ++eax; break;
        case kMovEaxImm: eax = immediate; break;
        case kAndEbxImm: ebx &= immediate; break;
        case kAndEaxImm: eax &= immediate; break;
        case kXorEaxImm: eax ^= immediate; break;
        case kAddEaxImm: eax += immediate; break;
        case kSubEaxImm: eax -= immediate; break;
        case kRet: return eax;
        default: return eax;
        }
    }
    return eax;
}

void CxDecryptor::xcode(uint32_t key, uint32_t &first, uint32_t &second) const {
    const Program &program = programs_[key & 0x7f];
    const uint32_t argument = key >> 7;
    first = program.run(argument, scheme_.controlBlock);
    second = program.run(~argument, scheme_.controlBlock);
}

void CxDecryptor::decode(uint32_t key, uint64_t offset, uint8_t *buffer, size_t count) const {
    uint32_t first, second;
    xcode(key, first, second);
    uint32_t position1 = second >> 16;
    uint32_t position2 = second & 0xffff;
    uint8_t xorAll = uint8_t(first);
    if (position1 == position2) position2 += 1;
    if (xorAll == 0) xorAll = 1;
    if (position2 >= offset && position2 < offset + count) buffer[position2 - offset] ^= uint8_t(first >> 16);
    if (position1 >= offset && position1 < offset + count) buffer[position1 - offset] ^= uint8_t(first >> 8);
    for (size_t i = 0; i < count; ++i) buffer[i] ^= xorAll;
}

void CxDecryptor::decrypt(uint32_t hash, uint64_t offset, uint8_t *buffer, size_t count) const {
    // Bytes before the file's split point use the hash as it is; bytes after it, the hash
    // folded with its own high half.
    const uint64_t split = uint64_t(hash & scheme_.mask) + scheme_.offset;
    if (offset < split) {
        const size_t head = size_t(std::min<uint64_t>(split - offset, count));
        decode(hash, offset, buffer, head);
        offset += head;
        buffer += head;
        count -= head;
    }
    if (count > 0) decode((hash >> 16) ^ hash, offset, buffer, count);
}

} // namespace shiorikrkr
