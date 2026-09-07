# showcontroller — ESP-IDF firmware for the Waveshare ESP32-C6-Touch-AMOLED-1.8 (SKU 33305).
#
# Builds green for esp32c6 (ESP-IDF v5.5.5); first light on hardware
# 2026-09-07 (StageWand-FB9C, V2). See firmware/README.md and docs/bringup.md.
#
# Standard ESP-IDF top-level project file. Build from this directory with
# `idf.py set-target esp32c6 && idf.py build flash monitor`.
cmake_minimum_required(VERSION 3.16)

# Layered defaults: base config + the measured memory trim (see sdkconfig.memtrim).
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.memtrim")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(showcontroller)
