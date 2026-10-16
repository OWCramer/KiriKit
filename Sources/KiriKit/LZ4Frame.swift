// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
// If a copy of the MPL was not distributed with this file, You can obtain one at
// https://mozilla.org/MPL/2.0/.

/// LZ4 frames (the format's public specification): what lzfs.dll stores, NEKOPARA Extra's
/// E-mote models among them. Several frames may follow one another.
enum LZ4Frame {
    struct Failure: Error, CustomStringConvertible {
        let description: String
    }

    static func isFrame(_ b: [UInt8]) -> Bool {
        b.count >= 7 && b[0] == 0x04 && b[1] == 0x22 && b[2] == 0x4D && b[3] == 0x18
    }

    static func decode(_ b: [UInt8]) throws -> [UInt8] {
        var out: [UInt8] = []
        out.reserveCapacity(b.count * 3)
        var p = 0
        func u32(_ at: Int) throws -> Int {
            guard at + 4 <= b.count else { throw Failure(description: "LZ4: truncated") }
            return Int(b[at]) | Int(b[at + 1]) << 8 | Int(b[at + 2]) << 16 | Int(b[at + 3]) << 24
        }
        while p + 4 <= b.count {
            let magic = try u32(p)
            if magic & 0xFFFF_FFF0 == 0x184D_2A50 { // a skippable frame
                p += 8 + (try u32(p + 4))
                continue
            }
            guard magic == 0x184D_2204, p + 6 < b.count else { throw Failure(description: "LZ4: not a frame") }
            let flags = b[p + 4]
            let blockChecksums = flags & 0x10 != 0, hasSize = flags & 0x08 != 0
            let contentChecksum = flags & 0x04 != 0, hasDictionary = flags & 0x01 != 0
            p += 6 + (hasSize ? 8 : 0) + (hasDictionary ? 4 : 0) + 1 // descriptor and its checksum
            while true {
                let word = try u32(p)
                p += 4
                if word == 0 { break }
                let size = word & 0x7FFF_FFFF
                guard p + size <= b.count else { throw Failure(description: "LZ4: truncated block") }
                if word & 0x8000_0000 != 0 {
                    out.append(contentsOf: b[p..<(p + size)])
                } else {
                    try block(b, p, p + size, into: &out)
                }
                p += size + (blockChecksums ? 4 : 0)
            }
            if contentChecksum { p += 4 }
        }
        return out
    }

    /// One compressed block: sequences of literals then a match (offset back into the output,
    /// which spans blocks: frames' blocks may be linked).
    private static func block(_ b: [UInt8], _ start: Int, _ end: Int, into out: inout [UInt8]) throws {
        var p = start
        while p < end {
            let token = Int(b[p]); p += 1
            var literals = token >> 4
            if literals == 15 {
                while p < end { let x = Int(b[p]); p += 1; literals += x; if x != 255 { break } }
            }
            guard p + literals <= end else { throw Failure(description: "LZ4: bad literals") }
            out.append(contentsOf: b[p..<(p + literals)])
            p += literals
            if p >= end { break } // the last sequence has no match
            guard p + 2 <= end else { throw Failure(description: "LZ4: bad offset") }
            let offset = Int(b[p]) | Int(b[p + 1]) << 8; p += 2
            var length = token & 15
            if length == 15 {
                while p < end { let x = Int(b[p]); p += 1; length += x; if x != 255 { break } }
            }
            length += 4
            guard offset > 0, offset <= out.count else { throw Failure(description: "LZ4: bad match") }
            // Byte by byte: appending a slice of the output to itself would copy all of it
            // first (a copy per match; NEKOPARA Extra's models took a minute to load).
            let from = out.count - offset
            for i in 0..<length { out.append(out[from + i]) }
        }
    }
}
