#!/usr/bin/env bash
# Flash showcontroller to the board and open the serial monitor.
#
#   ./firmware/flash.sh                    # auto-picks the first /dev/cu.usbmodem*
#   ./firmware/flash.sh /dev/cu.usbmodem14201
#
# Exit the monitor with Ctrl-].
set -euo pipefail

# shellcheck disable=SC1090
. "${IDF_EXPORT:-$HOME/esp/esp-idf/export.sh}"

cd "$(dirname "$0")/showcontroller"

idf.py -p "${1:-/dev/cu.usbmodem*}" flash monitor
