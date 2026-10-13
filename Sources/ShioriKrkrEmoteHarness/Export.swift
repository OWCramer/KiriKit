// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
// If a copy of the MPL was not distributed with this file, You can obtain one at
// https://mozilla.org/MPL/2.0/.

// The Mac harness's way to Shiori's E-mote player: it dlopens this library and asks for
// the table to hand the engine (the app links ShioriKrkrEmote directly).
import CShioriKrkr
import ShioriKrkrEmote

nonisolated(unsafe) private var table = KrkrEmoteHost.makeTable()

@_cdecl("shiorikrkr_emote_harness_table")
public func shiorikrkrEmoteHarnessTable() -> UnsafeRawPointer {
    withUnsafePointer(to: &table) { UnsafeRawPointer($0) }
}
