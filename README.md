# StageWand

Handheld hardware cue controllers for
[StageWizard](https://github.com/virtualmagician/StageWizard), built on the
Waveshare **ESP32-C6-Touch-AMOLED-1.8** board (SKU 33305). A native macOS
simulator runs the *identical* LVGL C UI code the firmware runs on-device,
so screens are designed and debugged on a laptop before ever touching
hardware. Show integration will ride StageWizard's OSC/MIDI remote hooks
over the board's Wi-Fi 6.

## Repository layout

```
Simulator/                  Swift package — the macOS simulator
├── Package.swift
├── Sources/
│   ├── SimCore/             C target: vendored LVGL 9.5.0 + simbridge + showui
│   │   ├── lvgl/             vendored LVGL 9.5.0 (unmodified upstream source)
│   │   ├── include/simbridge.h   the only header Swift talks to
│   │   └── showui/           device-portable UI — see "The rule that matters" below
│   └── AmoledSim/            SwiftUI app: window chrome, inspector panel, --snapshot mode
└── scripts/build_app.sh      packages a release build into dist/AmoledSim.app

firmware/                   UNTESTED ESP-IDF skeleton for the real board
└── showcontroller/           see firmware/README.md before building

docs/plan.html               the full research & plan document
```

## Quick start (simulator)

Needs only Xcode Command Line Tools — no Xcode project, no CocoaPods/SPM
registry access, nothing else to install.

```sh
cd Simulator
swift run AmoledSim              # windowed simulator, 368x448 canvas + inspector panel
```

Packaged `.app` (for double-clicking, or handing to someone else):
```sh
scripts/build_app.sh             # → dist/AmoledSim.app
```

Headless, for scripting/CI (renders one frame and exits):
```sh
swift run AmoledSim --snapshot out.png
```

## The rule that matters

Screens live **only** in `Simulator/Sources/SimCore/showui/` (`showui.h`,
`showui_hal.h`, `showui.c`), and that C code is portable: it depends on
nothing but LVGL and `showui_hal.h`. It is compiled twice — once into the
macOS simulator, once into the firmware — from the same files on disk (the
firmware references `showui.c` by relative path; it is never copied).

`showui_hal.h` is the *entire* boundary to the machine it runs on: battery,
IMU, RTC time, Wi-Fi status, BOOT/PWR buttons in; brightness out. Each
platform implements that HAL once — `simbridge.c` on macOS (fed by the
inspector panel's fake values), `firmware/showcontroller/main/showui_hal_device.c`
on-device (fed by real sensors). **Never add `#ifdef`-platform code inside a
screen.** If a screen needs to behave differently per platform, that
difference belongs in the HAL implementation, not in `showui.c`.

## Hardware cheat sheet

| | |
|---|---|
| MCU | ESP32-C6, single-core RISC-V, 160 MHz |
| RAM | 512 KB HP SRAM — **no PSRAM** |
| Flash | 16 MB |
| Display | 368×448 AMOLED over QSPI (SPI2) |
| Display driver | V1: SH8601 · V2: CO5300 — BSP auto-detects by touch I2C address |
| Touch | V1: FT-family @ I2C 0x38 · V2: CST820 @ I2C 0x15 |
| IMU | QMI8658 (accel + gyro) |
| RTC | PCF85063A |
| Audio codec | ES8311 |
| PMU | AXP2101 |
| IO expander | TCA9554 |
| Shared I2C bus | GPIO7/GPIO8¹ — TCA9554, AXP2101, QMI8658, PCF85063A, ES8311, touch all share it |
| SPI2 contention | Display and microSD share SPI2 on different pins — **mutually exclusive**, cannot be active together |
| Full-frame cost | 368×448×2 bytes = **322 KB** — larger than all of SRAM, must be tiled |
| QSPI push time | ~16.5 ms to push a full 322 KB frame at 20 MB/s → caps full-frame redraw well under 60 fps |

¹ The board's marketing pinout lists GPIO7 as SDA / GPIO8 as SCL. The
official BSP header (`bsp/esp32_c6_touch_amoled_1_8.h`, fetched 2026-08-29)
defines it the other way — `BSP_I2C_SCL = GPIO7`, `BSP_I2C_SDA = GPIO8` —
and the official `01_AXP2101`/`02_PCF85063` examples' Kconfig defaults agree
with the header. Firmware code should always go through `bsp_i2c_get_handle()`
rather than hard-coding either pin, which sidesteps the question entirely;
if you're wiring by hand, trust the header over the marketing pinout and
verify with a continuity check.

## Key links

- Board docs — <https://docs.waveshare.com/ESP32-C6-Touch-AMOLED-1.8>
- Official examples repo — <https://github.com/waveshareteam/ESP32-C6-Touch-AMOLED-1.8>
- BSP on the ESP Component Registry — <https://components.espressif.com/components/waveshare/esp32_c6_touch_amoled_1_8>
- Community: chayuto's board notes — <https://github.com/chayuto/ESP32-C6-Touch-AMOLED-1.8>
- Community: espressif/esp-brookesia (the launcher framework used by the official `03_esp-brookesia` example) — <https://github.com/espressif/esp-brookesia>
- LVGL — <https://github.com/lvgl/lvgl>
