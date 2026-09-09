# Agent handoff — building apps for the Waveshare ESP32-C6 Touch AMOLED 1.8

**Audience:** an AI coding agent (or engineer) starting on a **fresh macOS
machine**, tasked with building a *new* application for this board — not
necessarily StageWand, and not necessarily using Wi-Fi or Bluetooth. This repo
is the reference platform: a native macOS simulator that runs the board's real
LVGL C code, a firmware skeleton that builds green and has run on hardware, and
a list of traps already paid for. Read this file top to bottom before running
anything. Everything here was verified on real hardware on 2026-09-07
(`docs/bringup-logs/`).

Repository: https://github.com/virtualmagician/StageWand

---

## 1. The hardware you are targeting

| | |
|---|---|
| Board | Waveshare **ESP32-C6-Touch-AMOLED-1.8**, SKU 33305 (docs: https://docs.waveshare.com/ESP32-C6-Touch-AMOLED-1.8) |
| **Revision in hand** | **V2** — CO5300 AMOLED driver + **CST820** touch @ I2C `0x15`. (V1 boards use SH8601 + an FT3168 touch IC that the BSP drives with its FT5x06-class driver — grep `FT5X06`, not `FT3168` — @ `0x38`; the official BSP auto-detects by probing the touch address, so code is revision-agnostic — but verify each new board, see §7.) |
| MCU | ESP32-C6 rev v0.2: single 160 MHz RISC-V core, Wi-Fi 6, BLE 5.3 (LE only — **no Bluetooth Classic**), 802.15.4 |
| Memory | 512 KB SRAM, **no PSRAM** (the C6 has no PSRAM interface at all); 16 MB flash |
| Display | 1.8", **368 × 448**, QSPI, driven as RGB565 (16-bit). Brightness = MIPI-DCS `0x51` (not a backlight PWM) |
| Other chips | QMI8658 IMU, PCF85063A RTC, ES8311 codec + mic + speaker, AXP2101 PMU (battery, PWR button), TCA9554 IO expander |
| Buses | Shared I2C on GPIO7 (SCL) / GPIO8 (SDA) — always via `bsp_i2c_get_handle()`. **Display and microSD share SPI2: mutually exclusive.** |
| USB | Native USB-Serial-JTAG; appears on macOS as `/dev/cu.usbmodem*`, no driver |
| Buttons | BOOT (GPIO9, active low) and PWR (goes to the AXP2101, *not* a GPIO — long-press ~6 s is a hardware power-off on battery; on USB, unplug) |

**Graphics budget (measured/derived):** a full RGB565 frame is 322 KB — larger
than the free RAM budget, so LVGL renders through a partial draw buffer (60
lines = 44 KB, set in `sdkconfig.memtrim`). Full-screen redraws cost ~16.5 ms
of QSPI time at 40 MHz; design UIs as small, independently-updating widgets on
stable backgrounds. Flush areas must be even-aligned (the BSP rounds them; the
simulator reproduces this).

**Heap numbers on this board (free heap after init, measured):**
stock config **44 KB** (`bringup-logs/first-light-FB9C.log`) → with
`sdkconfig.memtrim` **153 KB** (`memtrim-FB9C.log`) → with NimBLE initialized
and a BLE connection up **92 KB** (`ble-first-FB9C.log`). Keep `sdkconfig.memtrim` unless
you know why you're dropping it.

---

## 2. What in this repo is platform vs. StageWand-specific

Keep the left column for any app; replace the right.

| Platform (reuse) | StageWand app (replace/remove) |
|---|---|
| `Simulator/` Swift package: `SimCore` C target (vendored **LVGL 9.5.0**, `simbridge.c` port layer, `lv_conf.h`) + `AmoledSim` SwiftUI app (device window, mouse-as-touch, inspector with fake peripherals, headless `--snapshot` mode) | `Simulator/Sources/SimCore/showui/showui.c` — the four StageWand screens |
| `Simulator/Sources/SimCore/showui/showui_hal.h` — the portable HAL boundary (inputs snapshot + brightness + device name) | `showui/showlink.[ch]` — the OSC/UDP/HTTP/BLE link to StageWizard (only if your app talks OSC) |
| `firmware/showcontroller/` — ESP-IDF 5.5.5 project on the official BSP, `sdkconfig.defaults` + `sdkconfig.memtrim` + `partitions.csv`, `main/showui_hal_device.c` (real sensors → HAL) | `main/wifi_link.c` (station Wi-Fi + Bonjour host discovery) — keep only if you need Wi-Fi |
| `firmware/showcontroller/main/ble_link.c` — a working NimBLE peripheral pattern (advertising, GATT service, notify/write, display-lock discipline) | Its UUIDs/characteristics are StageWizard's contract — change for your app |
| `tools/capture_serial.py`, `firmware/flash.sh`, `Simulator/scripts/build_app.sh` | `tools/mock_stagewizard.py`, `tools/test_link.sh` (OSC test rig) |
| `docs/bringup.md`, `docs/bringup-logs/` | `docs/plan.html`, `docs/stagewizard-osc-requests.html`, `docs/showlink.md` |

The architecture rule that makes the simulator worth anything: **screens live
only in `showui/`, depend only on LVGL + `showui_hal.h`, and are compiled
unchanged into both the Mac app and the firmware** (the firmware's
`main/CMakeLists.txt` references them by relative path; never copy). Platform
differences go into the HAL implementations (`simbridge.c` on macOS,
`showui_hal_device.c` on device), never as `#ifdef` inside a screen.

---

## 3. Fresh macOS setup — the simulator (10 minutes)

Requires only Xcode **Command Line Tools** (no Xcode). Paths in this Dropbox
folder contain spaces: always quote them.

```sh
xcode-select --install            # skip if already present; verify with: swift --version
git clone https://github.com/virtualmagician/StageWand.git
cd StageWand/Simulator
swift build --scratch-path "$HOME/Library/Caches/AmoledSimBuild"   # ~30 s first time (compiles LVGL)
"$HOME/Library/Caches/AmoledSimBuild/debug/AmoledSim" --snapshot /tmp/out.png --frames 150
```
Expected, literally: `Wrote snapshot to /tmp/out.png (frame_count=3, bytes_last_frame=8064)`
(numbers may differ slightly) and a 368×448 PNG of the GO page. `frame_count`
is how many frames LVGL actually redrew — a static screen settles in a few —
not the `--frames` loop count. The `--scratch-path` keeps ~1 GB of build
artifacts out of Dropbox sync; `scripts/build_app.sh` does the same and
produces `dist/AmoledSim.app` (ad-hoc signed, double-clickable).

**Headless flags** (`Sources/AmoledSim/Snapshot.swift`): `--snapshot <png>`
`--frames N` `--tap x,y` `--tap-hold <frames>` (≈80 = a long press)
`--tile N` (page to capture) `--link <host>` `--link-ports osc,http`
(StageWand-specific). Use snapshots as your rendering regression tests.

**The simulator is device-honest:** RGB565, the same partial draw buffer, the
even-coordinate flush rounding, and per-frame flush accounting shown in the
inspector as estimated QSPI cost on the real chip. Scale defaults to 1×
(pixel-exact); link settings persist in UserDefaults.

---

## 4. Fresh macOS setup — ESP-IDF and the firmware (20–30 minutes)

```sh
brew install cmake ninja dfu-util
git clone --depth 1 --recursive --shallow-submodules -b v5.5.5 \
    https://github.com/espressif/esp-idf.git "$HOME/esp/esp-idf"     # ~1 GB
cd "$HOME/esp/esp-idf" && ./install.sh esp32c6                       # toolchains → ~/.espressif
. "$HOME/esp/esp-idf/export.sh" && idf.py --version                  # expect: ESP-IDF v5.5.5
```
Use **v5.5.x** (the BSP requires ≥5.5; 5.5.5 is what everything here was built
and run with). Activate with the `export.sh` line in every new shell.

Build (managed components — the Waveshare BSP, RTC/IMU drivers, mDNS — are
fetched from the ESP Component Registry on the first build):
```sh
cd StageWand/firmware/showcontroller
idf.py set-target esp32c6
idf.py build          # green, zero warnings, ~0x1c8640 B app; 55% of the 4 MB app partition free
```
`sdkconfig` is generated and gitignored from two tracked files layered by
`CMakeLists.txt`: `sdkconfig.defaults` (16 MB flash, custom partitions,
Montserrat fonts, 1 kHz tick, NimBLE) and `sdkconfig.memtrim` (LVGL on the C
heap, 60-line draw buffer, trimmed Wi-Fi buffers). After editing either:
`rm sdkconfig && idf.py build`.

Flash and capture the boot log without an interactive monitor:
```sh
ls /dev/cu.usbmodem*                                   # board must be on a DATA cable
idf.py -p /dev/cu.usbmodemXXXX flash
python tools/capture_serial.py /dev/cu.usbmodemXXXX 25 /tmp/boot.log   # (repo root; IDF env active)
grep -a "Starting ShowController\|UI is ready\|heap:\|E (" /tmp/boot.log
```
Healthy markers, in order: `Detected board variant: V2 (CO5300 + CST820)` →
BSP display/touch init with no `E (` lines → `ShowController UI is ready` →
`heap: free=…` every 10 s. `firmware/flash.sh [port]` wraps flash+monitor
interactively. `esptool.py --port … flash_id` identifies chip/flash/MAC.

---

## 5. Making it YOUR app

1. **Screens:** rewrite `Simulator/Sources/SimCore/showui/showui.c` (keep
   `showui_create()` and `showui_goto_tile()` — `simbridge.c`, `main.c`, and
   the snapshot tool call them). Everything else in that file is StageWand.
2. **HAL:** `showui_hal.h` gives you battery/IMU/RTC/Wi-Fi/buttons in and
   brightness out, plus a device name. Extend it if your app needs more
   (e.g. audio, SD) — implement in `simbridge.c` (fake, driven by the inspector)
   and `showui_hal_device.c` (real). The inspector panel (`Sources/AmoledSim/
   InspectorView.swift`) is where you add controls for new fake inputs.
3. **Fonts:** LVGL fonts must be enabled in **both** `lv_conf.h` (simulator)
   and `sdkconfig.defaults` (`CONFIG_LV_FONT_MONTSERRAT_xx=y`, firmware). A
   font missing on the device side fails with `lv_font_montserrat_16 undeclared`.
4. **Connectivity, opt-in:** delete `wifi_link.c` (and its `Kconfig.projbuild`
   entries, `mdns` in `idf_component.yml`) if no Wi-Fi; delete `ble_link.c` and
   the `CONFIG_BT_*` lines if no BLE; delete `showlink.[ch]` if no OSC.
   Update `main/CMakeLists.txt` SRCS/REQUIRES accordingly.
5. **Radios rule:** the C6 has one 2.4 GHz radio. Wi-Fi-connected + BLE-connected
   at the same time is marked "performance unstable" by Espressif — design a
   switchover (StageWand advertises BLE only while Wi-Fi is down).
6. **Concurrency rule:** LVGL runs on the BSP's LVGL task. Anything that touches
   LVGL or shared UI state from another task (Wi-Fi/BLE/sensor callbacks) must be
   wrapped in `if (bsp_display_lock(0)) { …; bsp_display_unlock(); }`. See
   `wifi_link.c` (`configure_link_locked`) and `ble_link.c` for the pattern.
7. **Version + identity:** `showui_version.h` (`STAGEWAND_VERSION`) and
   `showui_hal_get_device_name()` ("…-XXXX" from the MAC) are shown on the
   Setup page so you always know which build is on which board.

---

## 6. Traps already paid for (don't rediscover these)

1. **LVGL's printf has no float support** by default (`LV_USE_FLOAT` is 0 in
   LVGL 9.5 — `CONFIG_LV_USE_FLOAT` unset on device): `lv_label_set_text_fmt(..., "%f")` prints a literal `f`.
   Format with C `snprintf` into a buffer, then `lv_label_set_text`.
2. **`LV_DPI_DEF` is 322** (true panel ppi) — the default theme scales paddings
   from it, so slider knobs and similar overhang and get clipped at screen
   edges. Set explicit small pads (`lv_obj_set_style_pad_all(s, 4, LV_PART_KNOB)`).
3. **Don't use `LV_STATE_DISABLED` for an "inert" look:** the default theme
   applies a grey colour filter that overrides local styles — pale fill,
   unreadable light text on the AMOLED. Swap colours explicitly on the normal
   state and remove `LV_OBJ_FLAG_CLICKABLE` (see `set_enabled` in `showui.c`).
4. **LVGL clips children to their parent:** a 40 px pill in a 48 px row with
   10 px top padding gets its bottom border shaved. Size rows to fit.
5. **Tileview page indicators:** react to `LV_EVENT_VALUE_CHANGED`, not
   `SCROLL_END` (the internal handler may not have committed the new tile yet).
6. **Montserrat has no emoji glyphs:** strip 4-byte UTF-8 from host-supplied
   text or you get tofu boxes (`copy_str` in `showlink.c` does this).
7. **macOS sockets:** a `send()` on a refused/reset TCP socket raises SIGPIPE
   and kills the simulator unless `SO_NOSIGPIPE` is set (lwIP has no SIGPIPE).
   Connected-UDP `ECONNREFUSED` arrives on *alternate* sends on macOS.
8. **Stock partition table leaves 0% app headroom** at this app size — that's
   why `partitions.csv` (4 MB factory app) exists.
9. **`CONFIG_FREERTOS_HZ` defaults to 100 Hz** → coarse touch/animation; set 1000.
10. **Memory:** the LVGL builtin 64 KB pool is both wasted baseline *and* a hard
    ceiling (≈700 B per list row). `CONFIG_LV_USE_CLIB_MALLOC=y` fixes both.
11. **NimBLE**: peripheral-only, 1 connection, MTU 256, SM off, msys 8/8 fits
    in the budget; the BLE MAC is the Wi-Fi MAC + 2. `Failed to persist local
    IRK (rc=8)` warnings are benign with NVS persistence off.
12. **`waveshare.com/wiki` blocks scripted fetches (403)** — use
    `docs.waveshare.com`; schematics/PDFs live under `files.waveshare.com`.

---

## 7. What needs the human (an agent cannot do these)

- Plugging boards in / moving them between USB and battery.
- **Per new board:** run Waveshare's `examples/esp-idf/00_board_check` once, sticker it V1/V2, and confirm 16 MB flash / no PSRAM. (Board 1, `StageWand-FB9C`, MAC `98:a3:16:a7:fb:9c`, is V2.)
- **Wi-Fi password:** enter it yourself via `idf.py menuconfig` → the app's menu; never paste credentials into files the agent writes.
- **macOS Bluetooth permission** prompts for any Mac app acting as a BLE central.
- Anything physical: touch feel, colour judgement on the AMOLED (the camera exaggerates brightness), audio.

---

## 8. Not done yet on this platform

- **AXP2101** (battery %, charging, PWR short-press events, software power-off): the BSP exposes no accessor; the plan is to vendor XPowersLib from Waveshare's `01_AXP2101` example and drive it over `bsp_i2c_get_handle()` — detailed in the TODO block in `main.c`.
- OTA updates (16 MB flash has room for an A/B layout; `partitions.csv` is single-factory today).
- ES8311 audio and the microSD path (remember SPI2 exclusivity with the display).
- Touch calibration/offset was not measured beyond "taps land where expected."

---

## 9. Verification checklist for your first hour

- [ ] `swift build` green; a `--snapshot` PNG shows your first screen.
- [ ] `idf.py build` green, zero warnings.
- [ ] Flash; log shows `V2 (CO5300 + CST820)` and `UI is ready` with no `E (` lines; note the `heap: free=` value.
- [ ] Your screen on the panel matches the simulator's PNG (hold them side by side — this is the whole point of the platform).
