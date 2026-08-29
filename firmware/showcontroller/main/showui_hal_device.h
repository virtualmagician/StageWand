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
 * Publish a fresh input snapshot, overwriting the previous one.
 *
 * Called from the sensor-polling task in main.c. Thread-safe against a
 * concurrent showui_hal_get_inputs() call from the LVGL task.
 */
void showui_hal_device_publish(const showui_inputs_t *in);

#ifdef __cplusplus
}
#endif
