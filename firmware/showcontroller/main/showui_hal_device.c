/**
 * showui_hal_device.c — ESP32-C6-Touch-AMOLED-1.8 implementation of the
 * showui_hal.h boundary (Simulator/Sources/SimCore/showui/showui_hal.h).
 *
 * UNTESTED: written without hardware or an ESP-IDF toolchain present.
 *
 * showui_hal_get_inputs() returns the latest snapshot published by the
 * sensor-polling task in main.c (see sensor_poll_task() and
 * showui_hal_device_publish()) -- it never touches I2C/GPIO itself, so it
 * is safe to call from LVGL code running under the BSP display lock.
 *
 * showui_hal_set_brightness() maps the portable UI's 0..255 level onto
 * bsp_display_brightness_set()'s 0..100 percent range. That BSP function
 * is the one that actually issues the AMOLED controller's MIPI-DCS 0x51
 * "Set Display Brightness" command -- see bsp_display_brightness_set() in
 * Waveshare-ESP32-components' bsp/esp32_c6_touch_amoled_1_8/esp32_c6_touch_amoled_1_8.c
 * (fetched 2026-08-29: `{0x51, &param, 1, ...}` over esp_lcd_panel_io).
 */
#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "showui_hal_device.h"

static const char *TAG = "showui_hal_device";

static SemaphoreHandle_t s_lock;
static showui_inputs_t s_latest;

void showui_hal_device_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    memset(&s_latest, 0, sizeof(s_latest));
    s_latest.battery_pct = -1; /* unknown until the PMU TODO in main.c lands */
}

void showui_hal_device_publish(const showui_inputs_t *in)
{
    if (in == NULL || s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_latest = *in;
    xSemaphoreGive(s_lock);
}

void showui_hal_get_inputs(showui_inputs_t *out)
{
    if (out == NULL) {
        return;
    }
    if (s_lock == NULL) {
        /* showui_hal_device_init() has not run yet -- return a safe zeroed
         * snapshot rather than garbage stack/static memory. */
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_latest;
    xSemaphoreGive(s_lock);
}

void showui_hal_set_brightness(uint8_t level)
{
    /* 0..255 -> 0..100 percent, rounded to nearest. */
    int percent = ((int)level * 100 + 127) / 255;
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }

    esp_err_t err = bsp_display_brightness_set(percent);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "bsp_display_brightness_set(%d) failed: %s", percent, esp_err_to_name(err));
    }
}
