/**
 * showui_hal.h — the boundary between the portable UI and the machine it
 * runs on.
 *
 * macOS: implemented by simbridge.c from the inspector panel's fake values.
 * Board: implemented over the Waveshare BSP (AXP2101 / PCF85063 / QMI8658 /
 *        BOOT+PWR buttons / DCS 0x51 brightness).
 */
#ifndef SHOWUI_HAL_H
#define SHOWUI_HAL_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int32_t battery_pct;      /* 0..100 */
    bool    charging;
    float   accel_x, accel_y, accel_z;  /* g */
    float   gyro_x, gyro_y, gyro_z;     /* deg/s */
    bool    wifi_connected;
    int32_t hour, minute, second;       /* RTC time of day */
    bool    button_boot;      /* physical BOOT button held */
    bool    button_pwr;       /* physical PWR button held */
} showui_inputs_t;

void showui_hal_get_inputs(showui_inputs_t *out);

/* AMOLED brightness: MIPI-DCS 0x51 on hardware. 0..255. */
void showui_hal_set_brightness(uint8_t level);

#ifdef __cplusplus
}
#endif

#endif /* SHOWUI_HAL_H */
