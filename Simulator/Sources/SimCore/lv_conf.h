/**
 * lv_conf.h — LVGL v9.5.0 configuration for the AmoledSim macOS simulator.
 *
 * Kept deliberately close to the device profile of the Waveshare
 * ESP32-C6-Touch-AMOLED-1.8 (official BSP: RGB565, LVGL >=8 <10):
 * every option not set here falls back to the LVGL default, exactly as on
 * the ESP-IDF build. Anything that must differ between Mac and board
 * belongs in the port layers (simbridge.c / firmware), never in ShowUI.
 */
#ifndef LV_CONF_H
#define LV_CONF_H

/* Match the device: 16-bit RGB565 rendering (byte swapping is a QSPI bus
 * detail handled by the device BSP; the simulator consumes unswapped RGB565). */
#define LV_COLOR_DEPTH 16

/* Single-threaded: the Swift side drives lv_timer_handler() from the main
 * thread only, mirroring the single-core ESP32-C6. */
#define LV_USE_OS LV_OS_NONE

/* Roomy heap for the desktop build; the device build uses the BSP defaults.
 * Keep ShowUI's real usage well under the C6 budget regardless. */
#define LV_MEM_SIZE (256 * 1024U)

/* 1.8" 368x448 panel -> sqrt(368^2+448^2)/1.8 = 322 ppi */
#define LV_DPI_DEF 322

#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1

/* Fonts used by ShowUI (Montserrat ships with LVGL). */
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_48 1

#endif /* LV_CONF_H */
