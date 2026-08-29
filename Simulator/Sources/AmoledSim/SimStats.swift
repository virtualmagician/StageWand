// SimStats.swift — Swift-native mirror of `sim_stats_t`.

import Foundation
import SimCore

/// One flush rectangle reported by SimCore for the last completed frame, in device pixel
/// coordinates (matching `sim_rect_t`).
struct SimRect: Identifiable {
    let id = UUID()
    var x: Int
    var y: Int
    var w: Int
    var h: Int
}

/// Mirrors `sim_stats_t` with Swift-native types, unpacking the fixed-size C array `rects`
/// (which the Clang importer exposes as a 32-tuple of `sim_rect_t`) into `[SimRect]`.
struct SimStats {
    var frameCount: UInt32 = 0
    var flushesLastFrame: UInt32 = 0
    var bytesLastFrame: UInt32 = 0
    var estDeviceMS: Double = 0
    var estDeviceFPSCap: Double = 0
    var rects: [SimRect] = []

    init() {}

    init(_ raw: sim_stats_t) {
        frameCount = raw.frame_count
        flushesLastFrame = raw.flushes_last_frame
        bytesLastFrame = raw.bytes_last_frame
        estDeviceMS = raw.est_device_ms
        estDeviceFPSCap = raw.est_device_fps_cap

        let count = min(Int(raw.rect_count), 32)
        guard count > 0 else { return }

        rects = withUnsafeBytes(of: raw.rects) { rawBuffer -> [SimRect] in
            let buffer = rawBuffer.bindMemory(to: sim_rect_t.self)
            return (0..<count).map { i in
                let r = buffer[i]
                return SimRect(x: Int(r.x), y: Int(r.y), w: Int(r.w), h: Int(r.h))
            }
        }
    }
}
