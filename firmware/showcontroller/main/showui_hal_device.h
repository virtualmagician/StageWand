/**
 * showui_hal_device.h — internal glue between the sensor-polling task in
 * main.c and the showui_hal_* functions implemented in showui_hal_device.c.
 *
 * This is firmware-only plumbing, NOT part of the portable showui/ sources
 * (Simulator/Sources/SimCore/showui/showui_hal.h) -- it exists because
 * showui_hal_get_inputs() must return instantly (it is called from LVGL
 * code running under the BSP display lock) while the actual sensor reads
 * (I2C to the RTC/IMU, GPIO for buttons) happen on their own cadence in a
 * background FreeRTOS task.
 *
 * UNTESTED: written without hardware or an ESP-IDF toolchain present.
 */
#pragma once

#include "showui_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Create the mutex guarding the published input snapshot.
 *
 * Call once from app_main(), before starting the sensor-polling task and
 * before any LVGL/showui code can call showui_hal_get_inputs().
 */
void showui_hal_device_init(void);

/**
 * Publish a fresh input snapshot, overwriting the previous one -- except
 * wifi_connected, which this call leaves untouched (see
 * showui_hal_device_set_wifi_connected() below). That keeps the 5 Hz
 * sensor-polling task in main.c, which knows nothing about Wi-Fi, from
 * clobbering the flag wifi_link.c maintains on its own schedule.
 *
 * Called from the sensor-polling task in main.c. Thread-safe against a
 * concurrent showui_hal_get_inputs() call from the LVGL task.
 */
void showui_hal_device_publish(const showui_inputs_t *in);

/**
 * Update just the wifi_connected flag in the published snapshot, leaving
 * every other field alone.
 *
 * Called from wifi_link.c's Wi-Fi event handler (IP_EVENT_STA_GOT_IP /
 * WIFI_EVENT_STA_DISCONNECTED) -- a separate entry point from
 * showui_hal_device_publish() specifically so that call site and the
 * sensor-polling task's full-snapshot publish never race to clobber each
 * other's idea of Wi-Fi state. Thread-safe.
 */
void showui_hal_device_set_wifi_connected(bool connected);

#ifdef __cplusplus
}
#endif
