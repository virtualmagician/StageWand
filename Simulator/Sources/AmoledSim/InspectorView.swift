// InspectorView.swift — the right-hand control panel: peripherals, buttons, display
// options, and a read-only "device honesty" panel of flush/timing stats.

import AppKit
import SwiftUI
import UniformTypeIdentifiers

struct InspectorView: View {
    @EnvironmentObject private var engine: SimEngine

    @Binding var scale: DisplayScale
    @Binding var showFlushRects: Bool
    @Binding var batteryPercent: Double
    @Binding var charging: Bool
    @Binding var pitch: Double
    @Binding var roll: Double
    @Binding var wifiConnected: Bool

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 14) {
                powerSection
                motionSection
                connectivitySection
                buttonsSection
                displaySection
                honestySection
            }
            .padding(14)
        }
        .background(Color(white: 0.1))
    }

    // MARK: - Power

    private var powerSection: some View {
        GroupBox("Power") {
            VStack(alignment: .leading, spacing: 8) {
                HStack {
                    Text("Battery")
                    Spacer()
                    Text("\(Int(batteryPercent))%")
                        .monospacedDigit()
                        .foregroundStyle(.secondary)
                }
                Slider(value: $batteryPercent, in: 0...100, step: 1)
                Toggle("Charging", isOn: $charging)
            }
            .padding(.top, 4)
        }
    }

    // MARK: - Motion

    private var motionSection: some View {
        GroupBox("Motion") {
            VStack(alignment: .leading, spacing: 8) {
                HStack {
                    Text("Pitch")
                    Spacer()
                    Text("\(Int(pitch))°")
                        .monospacedDigit()
                        .foregroundStyle(.secondary)
                }
                Slider(value: $pitch, in: -90...90, step: 1)

                HStack {
                    Text("Roll")
                    Spacer()
                    Text("\(Int(roll))°")
                        .monospacedDigit()
                        .foregroundStyle(.secondary)
                }
                Slider(value: $roll, in: -90...90, step: 1)

                Button("Level") {
                    pitch = 0
                    roll = 0
                }
                .frame(maxWidth: .infinity, alignment: .trailing)
            }
            .padding(.top, 4)
        }
    }

    // MARK: - Connectivity

    private var connectivitySection: some View {
        GroupBox("Connectivity") {
            Toggle("Wi-Fi", isOn: $wifiConnected)
                .padding(.top, 4)
        }
    }

    // MARK: - Buttons

    private var buttonsSection: some View {
        GroupBox("Buttons") {
            HStack(spacing: 12) {
                MomentaryButton(title: "BOOT") { pressed in
                    engine.setButton(0, pressed: pressed)
                }
                MomentaryButton(title: "PWR") { pressed in
                    engine.setButton(1, pressed: pressed)
                }
            }
            .padding(.top, 4)
        }
    }

    // MARK: - Display

    private var displaySection: some View {
        GroupBox("Display") {
            VStack(alignment: .leading, spacing: 8) {
                HStack {
                    Text("Brightness")
                    Spacer()
                    Text("\(Int(engine.brightness * 100))%")
                        .monospacedDigit()
                        .foregroundStyle(.secondary)
                }
                ProgressView(value: engine.brightness)

                Picker("Scale", selection: $scale) {
                    ForEach(DisplayScale.allCases) { option in
                        Text(option.label).tag(option)
                    }
                }
                .pickerStyle(.segmented)

                Toggle("Show Flush Rects", isOn: $showFlushRects)

                Button("Save Screenshot…") {
                    saveScreenshot()
                }
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            .padding(.top, 4)
        }
    }

    // MARK: - Device honesty

    private var honestySection: some View {
        GroupBox("Device Honesty") {
            VStack(alignment: .leading, spacing: 6) {
                statRow("LVGL fps", String(format: "%.1f", engine.lvglFPS))
                statRow("Flushes/frame", "\(engine.stats.flushesLastFrame)")
                statRow("KB/frame", String(format: "%.1f", Double(engine.stats.bytesLastFrame) / 1024.0))
                statRow("Est. QSPI transfer", String(format: "%.2f ms", engine.stats.estDeviceMS))
                statRow("Est. device fps cap", String(format: "%.1f", engine.stats.estDeviceFPSCap))

                Text("Estimates assume 40 MHz QSPI ≈ 20 MB/s, transfer only.")
                    .font(.caption2)
                    .foregroundStyle(.secondary)
                    .padding(.top, 4)
            }
            .padding(.top, 4)
        }
    }

    private func statRow(_ label: String, _ value: String) -> some View {
        HStack {
            Text(label)
            Spacer()
            Text(value)
                .monospacedDigit()
                .foregroundStyle(.secondary)
        }
    }

    // MARK: - Screenshot

    private func saveScreenshot() {
        guard let cgImage = engine.frame else { return }

        let panel = NSSavePanel()
        panel.allowedContentTypes = [.png]
        panel.nameFieldStringValue = "amoled-sim.png"
        panel.canCreateDirectories = true

        guard panel.runModal() == .OK, let url = panel.url else { return }
        FramebufferImage.writePNG(cgImage, to: url)
    }
}

/// A momentary press button (BOOT / PWR): fires `onPress(true)` on press-down and
/// `onPress(false)` on release, wherever the release happens to land.
private struct MomentaryButton: View {
    let title: String
    let onPress: (Bool) -> Void

    @State private var isPressed = false

    var body: some View {
        Text(title)
            .font(.system(.body, design: .monospaced))
            .frame(maxWidth: .infinity)
            .padding(.vertical, 8)
            .background(isPressed ? Color.white.opacity(0.25) : Color.white.opacity(0.08))
            .clipShape(RoundedRectangle(cornerRadius: 6, style: .continuous))
            .overlay(
                RoundedRectangle(cornerRadius: 6, style: .continuous)
                    .strokeBorder(Color.white.opacity(0.15), lineWidth: 1)
            )
            .contentShape(Rectangle())
            .gesture(
                DragGesture(minimumDistance: 0)
                    .onChanged { _ in
                        if !isPressed {
                            isPressed = true
                            onPress(true)
                        }
                    }
                    .onEnded { _ in
                        isPressed = false
                        onPress(false)
                    }
            )
    }
}
