# ShowController firmware — Waveshare ESP32-C6-Touch-AMOLED-1.8 (SKU 33305)

> **UNTESTED.** This skeleton was written without any hardware or ESP-IDF
> toolchain in the loop — nothing here has been compiled, flashed, or run.
> Function names and dependency versions were confirmed by fetching the
> official Waveshare source (examples repo + BSP repo, see "Sources" below)
> on 2026-08-29, but the code itself is unverified. Expect to fix build
> errors on first `idf.py build`. Two spots are left as marked `TODO`
> because no clean official API exists yet (see below).

## What's here

```
firmware/showcontroller/
├── CMakeLists.txt              # top-level ESP-IDF project file
└── main/
    ├── CMakeLists.txt          # pulls in showui.c by relative path (see below)
    ├── idf_component.yml       # BSP + RTC + IMU component deps
    ├── main.c                  # app_main(): display bring-up + sensor task
    ├── showui_hal_device.c     # implements showui_hal_get_inputs / set_brightness
    └── showui_hal_device.h     # private glue between the two above
```

`showui_create()` (the actual screens) is **not** in this tree — see
"Shared ShowUI sources" below.

## Setup

1. Install **ESP-IDF ≥ 5.5** (the BSP's `idf_component.yml` requires it; CI
   for the official BSP tests 5.5.5 and 6.0.2). The easiest path is
   Espressif's **ESP-IDF Installation Manager** (`eim`):
   <https://github.com/espressif/idf-im-cli> — or a manual
   `install.sh esp32c6` / `. export.sh` from a cloned `esp-idf` checkout.
2. From this directory:
   ```sh
   cd firmware/showcontroller
   idf.py set-target esp32c6
   idf.py build
   idf.py -p <PORT> flash monitor
   ```
   The first `idf.py build` will resolve `main/idf_component.yml` and pull
   `waveshare/esp32_c6_touch_amoled_1_8`, `waveshare/pcf85063a`, and
   `waveshare/qmi8658` from the ESP Component Registry automatically.

## V1 vs V2 boards

The board shipped in two touch-controller generations (V1: SH8601 + FT-family
touch at I2C 0x38; V2: CO5300 + CST820 touch at I2C 0x15). The BSP's
`bsp_board_detect()` probes the touch controller and picks the right driver
automatically — `app_main()` here calls it first thing and logs the result.

**When each board arrives**, run the official
`examples/esp-idf/00_board_check` example on it *before* flashing this
firmware, to confirm chip/flash/variant/capabilities match what's expected
(16 MB flash, no PSRAM, correct variant detected) and catch a DOA board or
wrong-variant assumption early.

## SPI2 / microSD exclusivity

The AMOLED panel and the microSD slot share the ESP32-C6's **SPI2** host on
different pin sets and **cannot be active at the same time**. If this
firmware ever needs the SD card, it must be mounted *before*
`bsp_display_start()` in a display-free flow — this skeleton does not touch
the SD card at all. See `bsp_sdcard_mount()`'s doc comment in the BSP header
for the same warning from the source.

## Shared ShowUI sources

The portable UI (`showui_create()` and everything it draws) lives in the
Simulator package, **not** in `firmware/`:

```
Simulator/Sources/SimCore/showui/
├── showui.h        # public API: showui_create()
├── showui_hal.h    # the HAL boundary both platforms implement
└── showui.c        # the actual screens (portable LVGL C)
```

`firmware/showcontroller/main/CMakeLists.txt` references that directory by
relative path:

```cmake
set(SHOWUI_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../Simulator/Sources/SimCore/showui")
idf_component_register(
    SRCS "main.c" "showui_hal_device.c" "${SHOWUI_DIR}/showui.c"
    INCLUDE_DIRS "." "${SHOWUI_DIR}"
    ...
)
```

It is **compiled from that location**, not copied — the firmware and the
macOS simulator (`Simulator/Sources/SimCore`) always build the exact same
`showui.c`/`showui.h`. Never fork or duplicate those files into `firmware/`;
if a screen needs to behave differently on-device vs. in the simulator, that
difference belongs behind `showui_hal.h`, implemented once in
`showui_hal_device.c` (firmware) and once in `simbridge.c` (simulator) — not
as `#ifdef` branches inside the screens themselves.

`main/showui_hal_device.c` implements that HAL:
- `showui_hal_get_inputs()` returns the latest snapshot gathered by a 5 Hz
  FreeRTOS task in `main.c` (battery/IMU/RTC/buttons).
- `showui_hal_set_brightness()` maps the UI's `0..255` level onto the BSP's
  `bsp_display_brightness_set(0..100)`, which itself sends the AMOLED
  controller's MIPI-DCS `0x51` "Set Display Brightness" command.

## Known TODOs

Two inputs have no clean official API to build on yet and are left as
placeholders (`battery_pct = -1` as an explicit "unknown" sentinel — ShowUI
renders it as `--%` — and `charging = false`) with a `TODO` comment and exact
file references in `main.c`:

- **`battery_pct` / `charging`** (AXP2101 PMU) — the BSP exposes
  `BSP_CAPS_PMU` but no `bsp_pmu_*` accessor, and there is no
  `waveshare/axp2101` registry component. The only official reference is
  `examples/esp-idf/01_AXP2101/main/{main.cpp,port_axp2101.cpp}`, which
  vendors XPowersLib as a local component talking to the AXP2101 over its
  *own* I2C bus — that would collide with the BSP's shared bus, so it needs
  adapting (vendor XPowersLib, but drive it through `bsp_i2c_get_handle()`
  instead of a second bus). See the comment above `sensor_init()` in
  `main.c` for the exact steps.
- **`button_pwr`** — also PMU-side (the AXP2101 power key surfaces as IRQ
  status bits, not a GPIO); same example is the reference.

`button_boot` reads GPIO9 directly (the ESP32-C6's conventional BOOT strap
pin) — this is standard practice but wasn't exercised by any of the fetched
official examples, so treat it as unverified until confirmed on real
hardware.

## Sources consulted (2026-08-29)

- `github.com/waveshareteam/ESP32-C6-Touch-AMOLED-1.8`,
  `examples/esp-idf/{00_board_check,00_bsp_quickstart,01_AXP2101,02_PCF85063,04_QMI8658,05_LVGL_WITH_RAM}`
- `github.com/waveshareteam/Waveshare-ESP32-components`,
  `bsp/esp32_c6_touch_amoled_1_8/` (BSP) and `sensor/{pcf85063a,qmi8658}/` (component sources)
- ESP Component Registry: `waveshare/esp32_c6_touch_amoled_1_8`
