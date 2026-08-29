// AmoledSimApp.swift — SwiftUI app shell. No @main here: main.swift calls
// AmoledSimApp.main() directly so it can branch into headless snapshot mode first.

import AppKit
import SwiftUI

struct AmoledSimApp: App {
    @StateObject private var engine = SimEngine()

    var body: some Scene {
        WindowGroup("AMOLED 1.8 Simulator — Waveshare ESP32-C6") {
            ContentView()
                .environmentObject(engine)
                .onAppear {
                    engine.start()
                    // `swift run` launches without a bundle, so AppKit doesn't front the
                    // window on its own — do it explicitly.
                    NSApplication.shared.setActivationPolicy(.regular)
                    NSApp.activate(ignoringOtherApps: true)
                }
        }
    }
}
