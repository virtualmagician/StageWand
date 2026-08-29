// FramebufferImage.swift — turns the raw SimCore framebuffer into a CGImage, and CGImages
// into PNG files. Shared by the live preview (SimEngine), the "Save Screenshot…" button
// (InspectorView), and headless snapshot mode (Snapshot.swift).

import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers

enum FramebufferImage {
    static let width = 368
    static let height = 448
    private static let bytesPerPixel = 4
    static let bytesPerRow = width * bytesPerPixel

    /// Builds an immutable CGImage from the SimCore framebuffer: 368x448 RGBX8888
    /// (R, G, B, 255), copying the bytes at `pointer` via CFData + CGDataProvider.
    static func makeCGImage(from pointer: UnsafePointer<UInt8>) -> CGImage? {
        let byteCount = width * height * bytesPerPixel
        guard let data = CFDataCreate(nil, pointer, byteCount),
              let provider = CGDataProvider(data: data) else {
            return nil
        }

        return CGImage(
            width: width,
            height: height,
            bitsPerComponent: 8,
            bitsPerPixel: 32,
            bytesPerRow: bytesPerRow,
            space: CGColorSpaceCreateDeviceRGB(),
            bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipLast.rawValue),
            provider: provider,
            decode: nil,
            shouldInterpolate: false,
            intent: .defaultIntent
        )
    }

    /// Writes `image` to `url` as a PNG file. Returns false (without throwing) on failure.
    @discardableResult
    static func writePNG(_ image: CGImage, to url: URL) -> Bool {
        guard let destination = CGImageDestinationCreateWithURL(
            url as CFURL,
            UTType.png.identifier as CFString,
            1,
            nil
        ) else {
            return false
        }
        CGImageDestinationAddImage(destination, image, nil)
        return CGImageDestinationFinalize(destination)
    }
}
