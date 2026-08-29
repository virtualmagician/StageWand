// main.swift — top-level entry point.
//
// This file has no top-level type, so it is the implicit script entry point;
// AmoledSimApp itself carries no @main attribute so that we can branch into
// headless snapshot mode BEFORE any AppKit/SwiftUI machinery spins up.

import Foundation

if CommandLine.arguments.contains("--snapshot") {
    runSnapshotMode(arguments: CommandLine.arguments)
    exit(0)
}

AmoledSimApp.main()
