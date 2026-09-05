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
#include <stdio.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "showui_hal_device.h"

static const char *TAG = "showui_hal_device";

static SemaphoreHandle_t s_lock;
static showui_inputs_t s_latest;
static bool s_wifi_connected; /* owned by wifi_link.c, not the sensor task -- see .h */

void showui_hal_device_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    memset(&s_latest, 0, sizeof(s_latest));
    s_latest.battery_pct = -1; /* unknown until the PMU TODO in main.c lands */
    s_wifi_connected = false;
}

void showui_hal_device_publish(const showui_inputs_t *in)
{
    if (in == NULL || s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_latest = *in;
    s_latest.wifi_connected = s_wifi_connected; /* preserve wifi_link.c's value */
    xSemaphoreGive(s_lock);
}

void showui_hal_device_set_wifi_connected(bool connected)
{
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_wifi_connected = connected;
    s_latest.wifi_connected = connected;
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

void showui_hal_get_device_name(char *out, uint32_t cap)
{
    if (out == NULL || cap == 0) {
        return;
    }

    uint8_t mac[6] = {0};
    /* esp_read_mac(..., ESP_MAC_WIFI_STA) is the station MAC Wi-Fi actually
     * uses; fall back to the raw base MAC (same value on boards that don't
     * derive a separate per-interface address) if that ever fails. */
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) {
        err = esp_efuse_mac_get_default(mac);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not read MAC for device name: %s", esp_err_to_name(err));
    }

    snprintf(out, cap, "StageWand-%02X%02X", mac[4], mac[5]);
}
