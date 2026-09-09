#!/usr/bin/env python3
"""capture_serial.py — non-interactive serial log capture for the board.

    . ~/esp/esp-idf/export.sh            # pyserial lives in the IDF python env
    python tools/capture_serial.py /dev/cu.usbmodemXXXX 25 out.log

Reads the port at 115200 baud for N seconds and writes raw bytes to out.log.
Unlike `idf.py monitor` it needs no TTY, so an agent can flash, capture, and
grep the boot markers in one shell pipeline. DTR/RTS are left low so opening
the port does not reset the chip a second time.
"""
import sys
import time

import serial  # pyserial (from the ESP-IDF python env)

if len(sys.argv) != 4:
    sys.exit("usage: capture_serial.py <port> <seconds> <outfile>")
port, secs, out = sys.argv[1], float(sys.argv[2]), sys.argv[3]
s = serial.Serial(port, 115200, timeout=0.5)
s.setDTR(False)
s.setRTS(False)
end = time.time() + secs
with open(out, "wb") as f:
    while time.time() < end:
        d = s.read(4096)
        if d:
            f.write(d)
            f.flush()
s.close()
print(f"captured {secs:.0f}s from {port} -> {out}")
