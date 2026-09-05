# ShowController firmware — Waveshare ESP32-C6-Touch-AMOLED-1.8 (SKU 33305)

> **BUILDS CLEAN, NOT YET RUN.** As of 2026-09-05 this compiles green for
> `esp32c6` under ESP-IDF v5.5.5 with zero warnings — but it has never been
> flashed or run, because no board has been connected yet. Everything about
> runtime behaviour (display bring-up, touch, sensors, Wi-Fi, the StageWizard
> link) is still unverified. Two spots are left as marked `TODO` because no
> clean official API exists yet (see below).

## What's here

```
firmware/flash.sh                   # sources export.sh, then flash + monitor
firmware/showcontroller/
├── CMakeLists.txt              # top-level ESP-IDF project file
├── sdkconfig.defaults          # 16 MB flash, LVGL Montserrat sizes, 1 kHz tick
├── partitions.csv              # 4 MB factory app (the stock layouts are too small)
└── main/
    ├── CMakeLists.txt          # pulls in showui.c by relative path (see below)
    ├── idf_component.yml       # BSP + RTC + IMU + mDNS component deps
    ├── Kconfig.projbuild       # "StageWand" menuconfig: Wi-Fi + StageWizard link settings
    ├── main.c                  # app_main(): display bring-up + sensor task + diagnostics
    ├── showui_hal_device.c     # implements showui_hal_get_inputs / set_brightness / get_device_name
    ├── showui_hal_device.h     # private glue between the two above
    ├── wifi_link.c             # Wi-Fi station bring-up + StageWizard host discovery
    └── wifi_link.h             # public entry point: wifi_link_start()
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
   idf.py menuconfig   # StageWand -> set Wi-Fi SSID/password at minimum, see below
   idf.py build
   idf.py -p <PORT> flash monitor    # or: ../flash.sh
   ```
   `sdkconfig` is generated (and gitignored); the tracked source of truth is
   `sdkconfig.defaults`. After editing that file, `rm sdkconfig && idf.py build`
   to pick the new values up — an existing `sdkconfig` wins over the defaults.
   Verified build: ESP-IDF v5.5.5, app 0x176550 bytes, 63% of the 4 MB
   factory partition free.

   The first `idf.py build` will resolve `main/idf_component.yml` and pull
   `waveshare/esp32_c6_touch_amoled_1_8`, `waveshare/pcf85063a`,
   `waveshare/qmi8658`, and `espressif/mdns` from the ESP Component Registry
   automatically.

## Wi-Fi + StageWizard link setup

`idf.py menuconfig` → **StageWand** has:

| Setting | Default | Meaning |
|---|---|---|
| `STAGEWAND_WIFI_SSID` | `""` | Network StageWand joins in station mode. |
| `STAGEWAND_WIFI_PASSWORD` | `""` | WPA2/WPA3 passphrase; empty means an open network. |
| `STAGEWAND_HOST_IP` | `""` | StageWizard host's dotted IPv4 address, or blank to discover it. |
| `STAGEWAND_OSC_PORT` | `53100` | Matches `showlink.h`'s `SHOWLINK_DEFAULT_OSC_PORT`. |
| `STAGEWAND_HTTP_PORT` | `53200` | Matches `showlink.h`'s `SHOWLINK_DEFAULT_HTTP_PORT`. |
| `STAGEWAND_WIFI_MAX_RETRY` | `0` | Consecutive failed Wi-Fi connects before giving up; `0` = retry forever with backoff. |

**mDNS vs. static IP:** leaving `STAGEWAND_HOST_IP` blank is the normal
setting — `wifi_link.c` browses Bonjour for a `_stagewizard._udp` service
(via the `espressif/mdns` component) every 5 s until a host answers, then
takes its first IPv4 address. This means the StageWizard machine's IP can
change between shows without a re-flash. Set `STAGEWAND_HOST_IP` explicitly
only on networks where Bonjour multicast is unreliable (e.g. some
enterprise/isolated Wi-Fi setups) or to pin a specific host when more than
one `_stagewizard._udp` responder might be on the network — mDNS discovery
is then skipped entirely.

Once an IP is known (either way), `wifi_link.c` calls
`showlink_configure(ip, STAGEWAND_OSC_PORT, STAGEWAND_HTTP_PORT, true)` to
enable the link, and `showui_inputs_t.wifi_connected` tracks the Wi-Fi
station state independently of the sensor-polling task (see
`showui_hal_device.h`). On disconnect the link is disabled
(`showlink_configure("", 0, 0, false)`) and Wi-Fi reconnects with
exponential backoff (1 s up to 30 s) logging the disconnect reason each
time.

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
  FreeRTOS task in `main.c` (battery/IMU/RTC/buttons), with `wifi_connected`
  overlaid from `wifi_link.c`'s own state (see `showui_hal_device_set_wifi_connected()`
  in `showui_hal_device.h` — kept separate so the two publishers never race).
- `showui_hal_set_brightness()` maps the UI's `0..255` level onto the BSP's
  `bsp_display_brightness_set(0..100)`, which itself sends the AMOLED
  controller's MIPI-DCS `0x51` "Set Display Brightness" command.
- `showui_hal_get_device_name()` returns `"StageWand-XXXX"`, where `XXXX` is
  the last two bytes of the station MAC (`esp_read_mac`), uppercase hex.

`main/wifi_link.c` brings up Wi-Fi station mode, resolves the StageWizard
host, and drives `showlink_configure()` — see "Wi-Fi + StageWizard link
setup" above.

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

## Sources consulted (2026-08-29; Wi-Fi/mDNS bring-up added 2026-09-05)

- `github.com/waveshareteam/ESP32-C6-Touch-AMOLED-1.8`,
  `examples/esp-idf/{00_board_check,00_bsp_quickstart,01_AXP2101,02_PCF85063,04_QMI8658,05_LVGL_WITH_RAM}`
- `github.com/waveshareteam/Waveshare-ESP32-components`,
  `bsp/esp32_c6_touch_amoled_1_8/` (BSP) and `sensor/{pcf85063a,qmi8658}/` (component sources)
- ESP Component Registry: `waveshare/esp32_c6_touch_amoled_1_8`, `espressif/mdns` (v1.12.0, current major
  as of 2026-09-05)
- `github.com/espressif/esp-idf`, `examples/wifi/getting_started/station` (station bring-up pattern,
  v5.5 branch)
- `github.com/espressif/esp-protocols`, `components/mdns/examples/query_advertise` (mdns_result_t /
  `mdns_ip_addr_t` field usage, `IPSTR`/`IP2STR` for formatting a resolved address)
