# showcontroller — ESP-IDF firmware for the Waveshare ESP32-C6-Touch-AMOLED-1.8 (SKU 33305).
#
# UNTESTED: written without hardware or an ESP-IDF toolchain present. See
# firmware/README.md before building or flashing.
#
# Standard ESP-IDF top-level project file. Build from this directory with
# `idf.py set-target esp32c6 && idf.py build flash monitor`.
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(showcontroller)
