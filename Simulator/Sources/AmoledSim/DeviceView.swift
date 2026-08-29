// DeviceView.swift — the "physical device" stage: framebuffer image on a bezel, AMOLED
// dimming overlay, optional flush-rect debug overlay, and touch input.

import SwiftUI

struct DeviceView: View {
    @EnvironmentObject private var engine: SimEngine

    let scale: DisplayScale
    let showFlushRects: Bool

    private let deviceWidth = CGFloat(FramebufferImage.width)
    private let deviceHeight = CGFloat(FramebufferImage.height)
    private let bezelMargin: CGFloat = 28

    @State private var lastTouchPoint: CGPoint = .zero

    var body: some View {
        let displayWidth = deviceWidth * scale.rawValue
        let displayHeight = deviceHeight * scale.rawValue

        ZStack {
            bezel(displayWidth: displayWidth, displayHeight: displayHeight)
            screen(displayWidth: displayWidth, displayHeight: displayHeight)
        }
    }

    // MARK: - Bezel

    @ViewBuilder
    private func bezel(displayWidth: CGFloat, displayHeight: CGFloat) -> some View {
        RoundedRectangle(cornerRadius: 30, style: .continuous)
            .fill(Color(red: 0.06, green: 0.06, blue: 0.065))
            .frame(width: displayWidth + bezelMargin * 2, height: displayHeight + bezelMargin * 2)
            .overlay(
                RoundedRectangle(cornerRadius: 30, style: .continuous)
                    .strokeBorder(Color.white.opacity(0.08), lineWidth: 1)
            )
            .shadow(color: .black.opacity(0.6), radius: 24, y: 12)
    }

    // MARK: - Screen

    @ViewBuilder
    private func screen(displayWidth: CGFloat, displayHeight: CGFloat) -> some View {
        ZStack {
            if let frame = engine.frame {
                Image(decorative: frame, scale: 1, orientation: .up)
                    .interpolation(.none)
                    .resizable()
                    .frame(width: displayWidth, height: displayHeight)
            } else {
                Color.black
                    .frame(width: displayWidth, height: displayHeight)
            }

            // AMOLED dimming: fully off at brightness 1.0, fully black at brightness 0.0.
            Color.black
                .opacity(1 - engine.brightness)
                .allowsHitTesting(false)

            if showFlushRects {
                flushRectOverlay(displayWidth: displayWidth, displayHeight: displayHeight)
                    .allowsHitTesting(false)
            }
        }
        .frame(width: displayWidth, height: displayHeight)
        .clipShape(RoundedRectangle(cornerRadius: 10, style: .continuous))
        .overlay(
            RoundedRectangle(cornerRadius: 10, style: .continuous)
                .strokeBorder(Color.black.opacity(0.9), lineWidth: 2)
        )
        .contentShape(Rectangle())
        .gesture(touchGesture)
    }

    @ViewBuilder
    private func flushRectOverlay(displayWidth: CGFloat, displayHeight: CGFloat) -> some View {
        Canvas { context, _ in
            let s = scale.rawValue
            for rect in engine.stats.rects {
                let scaledRect = CGRect(
                    x: CGFloat(rect.x) * s,
                    y: CGFloat(rect.y) * s,
                    width: CGFloat(rect.w) * s,
                    height: CGFloat(rect.h) * s
                )
                context.stroke(Path(scaledRect), with: .color(.red), lineWidth: 1)
            }
        }
        .frame(width: displayWidth, height: displayHeight)
    }

    // MARK: - Touch

    private var touchGesture: some Gesture {
        DragGesture(minimumDistance: 0, coordinateSpace: .local)
            .onChanged { value in
                let point = devicePoint(from: value.location)
                lastTouchPoint = point
                engine.touch(pressed: true, x: Int(point.x), y: Int(point.y))
            }
            .onEnded { _ in
                engine.touch(pressed: false, x: Int(lastTouchPoint.x), y: Int(lastTouchPoint.y))
            }
    }

    /// Maps a point in the (scaled) image's local coordinate space back to device pixels,
    /// clamped to the panel bounds.
    private func devicePoint(from location: CGPoint) -> CGPoint {
        let s = scale.rawValue
        let x = min(max(location.x / s, 0), deviceWidth - 1)
        let y = min(max(location.y / s, 0), deviceHeight - 1)
        return CGPoint(x: x, y: y)
    }
}
