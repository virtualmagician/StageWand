// LinkState.swift — Swift-native mirror of `sim_link_state_t`, the StageWizard link status
// reported back from SimCore.

import Foundation
import SimCore

/// Mirrors `sim_link_state_t` with Swift-native types, unpacking the fixed-size C char arrays
/// `standing_by_number` (16 bytes) and `standing_by_name` (64 bytes) — which the Clang importer
/// exposes as tuples, same as `sim_stats_t.rects` — into Swift `String`s.
struct LinkState {
    var enabled = false
    var online = false
    var standingByNumber = ""
    var standingByName = ""
    var runningCount = 0
    var showMode = false
    var panicking = false
    var lastStatusAgeMS: UInt32 = 0

    init() {}

    init(_ raw: sim_link_state_t) {
        enabled = raw.enabled
        online = raw.online
        standingByNumber = LinkState.string(from: raw.standing_by_number)
        standingByName = LinkState.string(from: raw.standing_by_name)
        runningCount = Int(raw.running_count)
        showMode = raw.show_mode
        panicking = raw.panicking
        lastStatusAgeMS = raw.last_status_age_ms
    }

    /// Extracts a NUL-terminated C string out of a fixed-size tuple field (e.g. the
    /// `(CChar, CChar, ...)` the Clang importer produces for `char foo[N]`).
    private static func string<Tuple>(from tuple: Tuple) -> String {
        withUnsafeBytes(of: tuple) { rawBuffer -> String in
            let buffer = rawBuffer.bindMemory(to: CChar.self)
            return String(cString: buffer.baseAddress!)
        }
    }
}
