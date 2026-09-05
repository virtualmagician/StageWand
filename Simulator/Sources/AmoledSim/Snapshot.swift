// Snapshot.swift — headless PNG snapshot mode.
//
//   AmoledSim --snapshot <outPath> [--frames N] [--tap x,y] [--link host] [--link-ports osc,http]
//
// No AppKit windows are created, so this works without a GUI session (e.g. over SSH or in
// CI). `main.swift` routes here before any SwiftUI/App machinery is touched.

import Foundation
import SimCore

func runSnapshotMode(arguments: [String]) {
    guard let outPath = argumentValue(after: "--snapshot", in: arguments) else {
        printError("--snapshot requires an output path, e.g. --snapshot out.png")
        exit(1)
    }

    var frameCount = 150
    if let framesString = argumentValue(after: "--frames", in: arguments) {
        guard let parsed = Int(framesString), parsed > 0 else {
            printError("--frames expects a positive integer, got '\(framesString)'")
            exit(1)
        }
        frameCount = parsed
    }

    var tapPoint: (x: Int32, y: Int32)?
    if let tapString = argumentValue(after: "--tap", in: arguments) {
        let parts = tapString.split(separator: ",")
        if parts.count == 2,
           let x = Int32(parts[0].trimmingCharacters(in: .whitespaces)),
           let y = Int32(parts[1].trimmingCharacters(in: .whitespaces)) {
            tapPoint = (x, y)
        } else {
            printStderr("warning: ignoring malformed --tap value '\(tapString)', expected x,y")
        }
    }

    // Tap roughly a third of the way through the run; hold for --tap-hold
    // frames (default 3 ≈ a tap; ~80 ≈ a long press for hold-to-confirm
    // controls). Clamp the release inside the loop so a tiny --frames value
    // can never leave the touch stuck pressed in the captured PNG.
    let holdFrames = argumentValue(after: "--tap-hold", in: arguments).flatMap { Int($0) } ?? 3
    let pressFrame = frameCount / 3
    let releaseFrame = min(pressFrame + max(holdFrames, 1), max(frameCount - 1, 0))

    let linkHost = argumentValue(after: "--link", in: arguments)

    var linkOSCPort: UInt16 = 53100
    var linkHTTPPort: UInt16 = 53200
    if let portsString = argumentValue(after: "--link-ports", in: arguments) {
        let parts = portsString.split(separator: ",")
        if parts.count == 2,
           let osc = UInt16(parts[0].trimmingCharacters(in: .whitespaces)),
           let http = UInt16(parts[1].trimmingCharacters(in: .whitespaces)) {
            linkOSCPort = osc
            linkHTTPPort = http
        } else {
            printStderr("warning: ignoring malformed --link-ports value '\(portsString)', expected osc,http")
        }
    }

    sim_init()

    // Configure the StageWizard link before the frame loop starts so the very first
    // sim_step() already reflects it.
    if let linkHost {
        sim_set_link(true, linkHost, linkOSCPort, linkHTTPPort)
    }

    // --tile N: capture a specific page (0 GO, 1 cues, 2 transport, 3 setup).
    if let tileString = argumentValue(after: "--tile", in: arguments) {
        if let tile = Int32(tileString), (0...3).contains(tile) {
            sim_goto_tile(tile)
        } else {
            printStderr("warning: ignoring malformed --tile value '\(tileString)', expected 0-3")
        }
    }

    for frameIndex in 0..<frameCount {
        feedWallClock()

        if let tap = tapPoint {
            if frameIndex == pressFrame {
                sim_touch(true, tap.x, tap.y)
            } else if frameIndex == releaseFrame {
                sim_touch(false, tap.x, tap.y)
            }
        }

        _ = sim_step()
        usleep(8000)
    }

    guard let pointer = sim_framebuffer(), let image = FramebufferImage.makeCGImage(from: pointer) else {
        printError("framebuffer unavailable")
        exit(1)
    }

    let outURL = URL(fileURLWithPath: outPath)
    guard FramebufferImage.writePNG(image, to: outURL) else {
        printError("failed to write PNG to \(outPath)")
        exit(1)
    }

    var rawStats = sim_stats_t()
    sim_get_stats(&rawStats)
    let stats = SimStats(rawStats)

    print("Wrote snapshot to \(outPath) (frame_count=\(stats.frameCount), bytes_last_frame=\(stats.bytesLastFrame))")
}

private func feedWallClock() {
    let components = Calendar.current.dateComponents([.hour, .minute, .second], from: Date())
    sim_set_clock(Int32(components.hour ?? 0), Int32(components.minute ?? 0), Int32(components.second ?? 0))
}

private func argumentValue(after flag: String, in arguments: [String]) -> String? {
    guard let flagIndex = arguments.firstIndex(of: flag), flagIndex + 1 < arguments.count else {
        return nil
    }
    let value = arguments[flagIndex + 1]
    guard !value.hasPrefix("--") else { return nil }
    return value
}

private func printError(_ message: String) {
    printStderr("error: \(message)")
}

private func printStderr(_ message: String) {
    FileHandle.standardError.write(Data("\(message)\n".utf8))
}
