# StageWand day-one bring-up checklist

Waveshare ESP32-C6-Touch-AMOLED-1.8 (SKU 33305) boards, bench checklist from
unboxing to a side-by-side comparison against the simulator. Follow in order.

## 1. Unboxing & identification

Two hardware revisions exist in the wild — **do not assume which one you
have**:

| | V1 | V2 |
|---|---|---|
| Display driver | SH8601 | CO5300 |
| Touch controller | FT-family | CST820 |
| Touch I2C address | `0x38` | `0x15` |

- [ ] For **each board**, before anything else, run Waveshare's official
      `examples/esp-idf/00_board_check` example on it. Confirm: chip/flash
      variant, **16 MB flash**, **no PSRAM**, and the detected touch address.
- [ ] Record V1 or V2 per board and **sticker it** — the BSP auto-detects at
      runtime by probing the touch I2C address, but you still want to know
      what's on the bench without re-flashing.
- [ ] macOS needs **no driver install** — the C6's native USB-Serial-JTAG
      shows up on its own as `/dev/cu.usbmodem*`. Run `ls /dev/cu.usbmodem*`
      to find the port.

## 2. Toolchain

- [ ] ESP-IDF **≥ 5.5** installed. Activate it:
  ```sh
  . ~/esp/esp-idf/export.sh
  ```
- [ ] Set the target:
  ```sh
  cd firmware/showcontroller
  idf.py set-target esp32c6
  ```
- [ ] Configure StageWand:
  ```sh
  idf.py menuconfig
  ```
  Under the **StageWand** menu, set:
  - Wi-Fi SSID / password
  - Host IP — leave **empty** for Bonjour discovery of `_stagewizard._udp`
  - OSC port **53100**, HTTP port **53200** (defaults; only change if the
    host is non-default)

## 3. Build, flash, monitor

```sh
idf.py build
idf.py -p <PORT> flash monitor
```

**Expected healthy serial log** — these are the exact strings the firmware
logs; grep for them, in roughly this order:

- [ ] `Starting ShowController on` … V1 or V2 — must match the sticker from
      step 1 (this is `bsp_board_detect()` reporting)
- [ ] the BSP's display/touch init lines with no `E (` errors after them
- [ ] `Wi-Fi connected, got IP:`
- [ ] `StageWizard host resolved via Bonjour:` (or `Using configured
      StageWizard host` if you set a static IP)
- [ ] `StageWizard link enabled ->`
- [ ] `heap: free=… min_free_ever=… largest_free_block(8BIT)=…` — repeats
      every 10 s; **write these numbers down** (see §6)

Reconnect behavior you may also see and should *not* worry about:
`Wi-Fi disconnected (reason=…), retry N in M ms` with M doubling up to 30 s,
and `no _stagewizard._udp responder yet, retrying in 5000 ms` until
StageWizard is running with OSC enabled.

If any marker is missing or the board reboots first, stop and diagnose
before moving to step 4 — do not chase it against the simulator instead.

## 4. StageWizard side

- [ ] StageWizard **v1.7.0+** (or dev build **D28**) installed on the host
      Mac.
- [ ] In **Show Settings**, OSC control is **enabled**.
- [ ] The wand appears as a subscriber once its `/stagewand/ping` keepalive
      reaches the host (within ~1 s of link-enabled in the serial log).
- [ ] The GO page on the physical wand shows the **standing-by cue** —
      matching what StageWizard currently has queued.

## 5. Simulator side-by-side

No real host handy? Run the mock in one terminal:

```sh
python3 tools/mock_stagewizard.py
```

For **each of the four pages** (0 = GO, 1 = cues, 2 = transport, 3 = setup),
capture the equivalent simulator frame and hold it next to the physical
panel:

```sh
# against the mock
swift run AmoledSim --snapshot go.png        --tile 0 --link 127.0.0.1
swift run AmoledSim --snapshot cues.png      --tile 1 --link 127.0.0.1
swift run AmoledSim --snapshot transport.png --tile 2 --link 127.0.0.1
swift run AmoledSim --snapshot setup.png     --tile 3 --link 127.0.0.1

# or against the real host, with its IP
swift run AmoledSim --snapshot go.png --tile 0 --link <host-ip>
```

Compare, page by page:

- [ ] **Colors** — AMOLED true black vs. the Mac's backlit display; the
      MagicLab-blue accent and the six cue-tag tints should read the same
      hue, just deeper/blacker on the AMOLED.
- [ ] **Text size** at the panel's true **322 ppi** — check nothing that
      reads fine on the Mac's simulated canvas is too small to read at arm's
      length on the real panel.
- [ ] **Tap target feel** — PREV/NEXT, GO, cue rows; confirm nothing near an
      edge is hard to hit with a thumb.
- [ ] **Even-coordinate flush artifacts** — the simulator reproduces the
      device's single 368×100 draw buffer and even-coordinate flush
      rounding; look for any 1px seam or misaligned redraw on the real panel
      that the simulator already predicted.
- [ ] **Touch offset / calibration** — tap a known target (e.g. a cue row
      edge) and check the registered touch point matches where you tapped;
      note any consistent offset for later calibration.

## 6. Measurements to capture on day one

- [ ] **Free heap + largest block**, read off the 10 s diagnostics line.
      This is the number that decides whether NimBLE's ~40-80 KB footprint
      fits for the BLE fallback transport — write it down per board.
- [ ] **Observed responsiveness** (page swipes, tap-to-fire latency) vs. the
      simulator's QSPI estimate (~16.5 ms to push a full 322 KB frame at
      20 MB/s) — note if real-world redraw feels slower than that budget
      predicts.
- [ ] **Battery runtime**, if a 3.7 V LiPo cell is fitted to the MX1.25
      connector — rough runtime to first low-battery indication.

## 7. Known hazards

- [ ] **Display and microSD share SPI2** on different pins — they are
      **mutually exclusive**, never active together. If a card needs
      mounting, do it before `bsp_display_start()`, display-free.
- [ ] **No PSRAM** — the C6 has no PSRAM interface at all; this is why the
      UI tiles the frame instead of holding a full 322 KB buffer.
- [ ] Brightness goes through the AMOLED controller's **MIPI-DCS `0x51`**
      "Set Display Brightness" command — don't hand-roll a different one.
- [ ] **Never bypass V1/V2 auto-detect.** The BSP picks the driver by
      probing the touch I2C address at runtime; don't hardcode a variant
      even "just for testing."
- [ ] Keep the boards on a **trusted network** — the OSC link is
      **unauthenticated**.

## 8. What comes after day one

- BLE fallback transport, implemented on the wand side against
  StageWizard's shipped `BLEWandLink` (dev D28), with the corrected
  characteristic directions: the host **writes** feedback frames to
  `...0002`, the wand **notifies** commands (and pings) on `...0003`.
  Wand-side NimBLE bring-up is the last open item and needs the heap
  numbers from step 6 first.
- OTA, once BLE is stable.
