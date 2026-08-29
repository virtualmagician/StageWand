// swift-tools-version: 5.9
// AmoledSim — native macOS simulator for the Waveshare ESP32-C6-Touch-AMOLED-1.8 (368x448).
// Builds with Command Line Tools alone: `swift build` / `swift run AmoledSim`.
import PackageDescription

let package = Package(
    name: "AmoledSim",
    platforms: [.macOS(.v14)],
    targets: [
        // Single C target: vendored LVGL v9.5.0 + sim bridge + the device-portable ShowUI screens.
        // Swift only ever talks to include/simbridge.h; LVGL stays an internal detail.
        .target(
            name: "SimCore",
            path: "Sources/SimCore",
            exclude: [
                "lvgl/LICENCE.txt",
                // Display/indev drivers for other platforms (SDL, X11, Arduino GFX stacks...)
                "lvgl/src/drivers",
                "lvgl/src/debugging/vg_lite_tvg",
                // Optional C++ engines and ports we do not compile (config-disabled anyway)
                "lvgl/src/libs/thorvg",
                "lvgl/src/libs/gltf",
                "lvgl/src/libs/freetype",
                "lvgl/src/libs/vg_lite_driver",
                "lvgl/src/libs/FT800-FT813",
                "lvgl/src/libs/fsdrv/lv_fs_arduino_esp_littlefs.cpp",
                "lvgl/src/libs/fsdrv/lv_fs_arduino_sd.cpp",
                // Cortex-M Helium assembly, not applicable on macOS arm64
                "lvgl/src/draw/sw/blend/helium/lv_blend_helium.S",
                // License texts riding along in the vendored tree
                "lvgl/src/libs/tjpgd/LICENSE.txt",
                "lvgl/src/libs/frogfs/LICENSE.txt",
                "lvgl/src/libs/tiny_ttf/LICENSE.txt",
                "lvgl/src/libs/lz4/LICENSE.txt",
                "lvgl/src/libs/qrcode/LICENSE.txt",
                "lvgl/src/libs/lodepng/LICENSE.txt",
                "lvgl/src/libs/barcode/LICENSE.txt",
                "lvgl/src/libs/nanovg/LICENSE.txt",
                "lvgl/src/libs/gif/LICENSE",
                "lvgl/src/stdlib/builtin/LICENSE_SPRINTF.txt",
                "lvgl/src/stdlib/builtin/LICENSE_TLSF.txt",
            ],
            publicHeadersPath: "include",
            cSettings: [
                .define("LV_CONF_INCLUDE_SIMPLE"),
                .headerSearchPath("."),      // lv_conf.h
                .headerSearchPath("lvgl"),   // lvgl.h — same include name the ESP-IDF build uses
            ]
        ),
        .executableTarget(
            name: "AmoledSim",
            dependencies: ["SimCore"],
            path: "Sources/AmoledSim"
        ),
    ]
)
