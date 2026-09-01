// ContentView.swift — top-level layout: device stage on the left, inspector on the right.
// Holds the simulator-control state (battery, motion, wifi, display options) and forwards
// it to SimEngine's setter passthroughs.

import SwiftUI

/// Framebuffer display scale, applied on top of the native 368x448 device resolution.
enum DisplayScale: Double, CaseIterable, Identifiable {
    case x1 = 1.0
    case x1_5 = 1.5
    case x2 = 2.0

    var id: Double { rawValue }

    var label: String {
        switch self {
        case .x1: return "1x"
        case .x1_5: return "1.5x"
        case .x2: return "2x"
        }
    }
}

struct ContentView: View {
    @EnvironmentObject private var engine: SimEngine

    // Display
    // 1x by default (pixel-perfect native size); the choice persists across launches.
    @State private var scale: DisplayScale =
        DisplayScale(rawValue: UserDefaults.standard.double(forKey: "displayScale")) ?? .x1
    @State private var showFlushRects = false

    // Power
    @State private var batteryPercent: Double = 82
    @State private var charging = false

    // Motion
    @State private var pitch: Double = 0
    @State private var roll: Double = 0

    // Connectivity
    @State private var wifiConnected = true

    var body: some View {
        HStack(spacing: 0) {
            DeviceView(scale: scale, showFlushRects: showFlushRects)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .background(Color(white: 0.05))

            Divider()

            InspectorView(
                scale: $scale,
                showFlushRects: $showFlushRects,
                batteryPercent: $batteryPercent,
                charging: $charging,
                pitch: $pitch,
                roll: $roll,
                wifiConnected: $wifiConnected
            )
            .frame(width: 290)
        }
        .frame(minWidth: 760, minHeight: 560)
        .background(Color(white: 0.08))
        .preferredColorScheme(.dark)
        .onAppear {
            // Push the initial control state into SimCore once at launch.
            engine.setBattery(percent: Int(batteryPercent), charging: charging)
            engine.setMotion(pitchDegrees: pitch, rollDegrees: roll)
            engine.setWifi(connected: wifiConnected)
        }
        .onChange(of: scale) { _, newValue in
            UserDefaults.standard.set(newValue.rawValue, forKey: "displayScale")
        }
        .onChange(of: batteryPercent) { _, newValue in
            engine.setBattery(percent: Int(newValue), charging: charging)
        }
        .onChange(of: charging) { _, newValue in
            engine.setBattery(percent: Int(batteryPercent), charging: newValue)
        }
        .onChange(of: pitch) { _, newValue in
            engine.setMotion(pitchDegrees: newValue, rollDegrees: roll)
        }
        .onChange(of: roll) { _, newValue in
            engine.setMotion(pitchDegrees: pitch, rollDegrees: newValue)
        }
        .onChange(of: wifiConnected) { _, newValue in
            engine.setWifi(connected: newValue)
        }
    }
}
