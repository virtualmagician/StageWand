// SimEngine.swift — owns the SimCore lifecycle and republishes it for SwiftUI.
//
// Every sim_* call happens on the main thread (LVGL is not thread-safe), driven by a
// RunLoop timer rather than a background thread or DispatchSourceTimer.

import Combine
import CoreGraphics
import Foundation
import SimCore

@MainActor
final class SimEngine: ObservableObject {
    @Published private(set) var frame: CGImage?
    @Published private(set) var stats = SimStats()
    @Published private(set) var brightness: Double = 1.0
    @Published private(set) var lvglFPS: Double = 0

    // StageWizard link (OSC/UDP out, HTTP /status poll in). The port fields are Strings so
    // they can back TextFields directly; applyLinkConfig() parses them on every change.
    // Settings persist across launches; the link is ON by default (localhost host).
    @Published var linkEnabled = UserDefaults.standard.object(forKey: "linkEnabled") as? Bool ?? true {
        didSet {
            UserDefaults.standard.set(linkEnabled, forKey: "linkEnabled")
            applyLinkConfig()
        }
    }
    @Published var linkHost = UserDefaults.standard.string(forKey: "linkHost") ?? "127.0.0.1" {
        didSet {
            UserDefaults.standard.set(linkHost, forKey: "linkHost")
            applyLinkConfig()
        }
    }
    @Published var linkOSCPort = UserDefaults.standard.string(forKey: "linkOSCPort") ?? "53100" {
        didSet {
            UserDefaults.standard.set(linkOSCPort, forKey: "linkOSCPort")
            applyLinkConfig()
        }
    }
    @Published var linkHTTPPort = UserDefaults.standard.string(forKey: "linkHTTPPort") ?? "53200" {
        didSet {
            UserDefaults.standard.set(linkHTTPPort, forKey: "linkHTTPPort")
            applyLinkConfig()
        }
    }
    @Published private(set) var linkState = LinkState()

    private var timer: Timer?
    private var started = false
    private var lastGeneration: UInt32 = 0

    // Sliding 1-second window of frame-generation bumps, used to derive lvglFPS.
    private var generationTimestamps: [CFAbsoluteTime] = []

    // MARK: - Lifecycle

    func start() {
        guard !started else { return }
        started = true

        sim_init()

        // didSet doesn't fire for initial values, so push the (persisted or
        // default-on) link settings down once at startup.
        applyLinkConfig()

        // The timer is scheduled on the main run loop, so its callback is on the
        // main thread — assumeIsolated makes that explicit to the concurrency checker.
        let timer = Timer(timeInterval: 1.0 / 60.0, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated {
                self?.tick()
            }
        }
        RunLoop.main.add(timer, forMode: .common)
        self.timer = timer
    }

    func stop() {
        timer?.invalidate()
        timer = nil
    }

    private func tick() {
        feedWallClock()
        _ = sim_step()

        let generation = sim_frame_generation()
        if generation != lastGeneration {
            lastGeneration = generation
            recordGenerationTick()

            if let pointer = sim_framebuffer() {
                frame = FramebufferImage.makeCGImage(from: pointer)
            }

            var rawStats = sim_stats_t()
            sim_get_stats(&rawStats)
            stats = SimStats(rawStats)
        }

        // Brightness is device-controlled (the embedded UI can dim the panel at any time),
        // so it's refreshed every tick regardless of whether the framebuffer changed.
        brightness = Double(sim_get_brightness()) / 255.0

        // Cheap struct copy; the StageWizard link status can change independently of the
        // framebuffer (HTTP polling happens off the frame-generation clock), so refresh it
        // every tick too.
        var rawLinkState = sim_link_state_t()
        sim_get_link_state(&rawLinkState)
        linkState = LinkState(rawLinkState)
    }

    private func feedWallClock() {
        let components = Calendar.current.dateComponents([.hour, .minute, .second], from: Date())
        sim_set_clock(Int32(components.hour ?? 0), Int32(components.minute ?? 0), Int32(components.second ?? 0))
    }

    private func recordGenerationTick() {
        let now = CFAbsoluteTimeGetCurrent()
        generationTimestamps.append(now)
        let cutoff = now - 1.0
        generationTimestamps.removeAll { $0 < cutoff }
        lvglFPS = Double(generationTimestamps.count)
    }

    // MARK: - Input

    /// Device pixel coordinates, clamped to the 368x448 panel.
    func touch(pressed: Bool, x: Int, y: Int) {
        let clampedX = min(max(x, 0), FramebufferImage.width - 1)
        let clampedY = min(max(y, 0), FramebufferImage.height - 1)
        sim_touch(pressed, Int32(clampedX), Int32(clampedY))
    }

    // MARK: - Simulated peripherals

    func setBattery(percent: Int, charging: Bool) {
        sim_set_battery(Int32(percent), charging)
    }

    /// Converts a pitch/roll pair (degrees) into a plausible gravity vector for `sim_set_imu`.
    /// Gyro rates are left at zero — this simulator only models static tilt.
    func setMotion(pitchDegrees: Double, rollDegrees: Double) {
        let pitch = pitchDegrees * .pi / 180
        let roll = rollDegrees * .pi / 180

        let ax = Float(sin(roll))
        let ay = Float(sin(pitch))
        let az = Float(cos(pitch) * cos(roll))
        sim_set_imu(ax, ay, az, 0, 0, 0)
    }

    func setWifi(connected: Bool) {
        sim_set_wifi(connected)
    }

    func setButton(_ which: Int32, pressed: Bool) {
        sim_set_button(which, pressed)
    }

    // MARK: - StageWizard link

    /// Pushes the current link settings down to SimCore. Invalid port text falls back to
    /// StageWizard's own defaults (53100 OSC / 53200 HTTP) rather than failing silently.
    private func applyLinkConfig() {
        let oscPort = UInt16(linkOSCPort) ?? 53100
        let httpPort = UInt16(linkHTTPPort) ?? 53200
        sim_set_link(linkEnabled, linkHost, oscPort, httpPort)
    }
}
