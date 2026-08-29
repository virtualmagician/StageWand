/**
 * main.c — ShowController firmware entry point for the Waveshare
 * ESP32-C6-Touch-AMOLED-1.8 (SKU 33305).
 *
 * ============================== UNTESTED ================================
 * Written without hardware or an ESP-IDF toolchain present. Nothing here
 * has been compiled or run. See firmware/README.md before building/flashing.
 * ==========================================================================
 *
 * The display/LVGL bring-up sequence mirrors the official example
 *   examples/esp-idf/05_LVGL_WITH_RAM/main/example_qspi_with_ram.c
 * (github.com/waveshareteam/ESP32-C6-Touch-AMOLED-1.8) verbatim, except
 * lv_demo_music() is replaced with showui_create() -- the same portable
 * LVGL UI code the macOS simulator runs, see
 * Simulator/Sources/SimCore/showui/showui.h.
 *
 * A background FreeRTOS task polls real inputs at 5 Hz and hands them to
 * showui_hal_device.c (showui_hal_device_publish()), which serves them back
 * out through showui_hal_get_inputs() -- the HAL boundary declared in
 * Simulator/Sources/SimCore/showui/showui_hal.h.
 */
#include <string.h>

#include "bsp/esp-bsp.h"
#include "driver/gpio.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "pcf85063a.h"
#include "qmi8658.h"

#include "showui.h"
#include "showui_hal.h"
#include "showui_hal_device.h"

static const char *TAG = "showcontroller";

/* ---------------------------------------------------------------------- *
 * BOOT button
 *
 * GPIO9 is the ESP32-C6's conventional BOOT/strapping pin, pulled low
 * while the physical BOOT button is held -- standard across ESP32-C6 dev
 * boards. NONE of the official Waveshare examples fetched while writing
 * this skeleton (00_board_check, 00_bsp_quickstart, 01_AXP2101,
 * 02_PCF85063, 04_QMI8658, 05_LVGL_WITH_RAM) touch a button GPIO directly,
 * so this is not board-verified -- confirm against the schematic or
 * 00_board_check output on real hardware before trusting it.
 * ---------------------------------------------------------------------- */
#define SHOWCONTROLLER_BOOT_BUTTON_GPIO GPIO_NUM_9

/* Sensor-polling cadence for the showui_hal_inputs_t snapshot. */
#define SENSOR_POLL_HZ 5
#define SENSOR_POLL_PERIOD_MS (1000 / SENSOR_POLL_HZ)

static pcf85063a_dev_t s_rtc;
static bool s_rtc_ready = false;

static qmi8658_dev_t s_imu;
static bool s_imu_ready = false;

/**
 * Bring up the RTC and IMU on the BSP's shared I2C0 bus.
 *
 * TODO(PMU/AXP2101 + PWR button): the BSP header
 * (bsp/esp32_c6_touch_amoled_1_8.h) sets BSP_CAPS_PMU but exposes no
 * bsp_pmu_* accessor, and there is no `waveshare/axp2101` registry
 * component. The only official reference is
 * examples/esp-idf/01_AXP2101/main/{main.cpp,port_axp2101.cpp}, which
 * vendors XPowersLib as a local component (see that example's
 * components/XPowersLib/) and talks to the AXP2101 over ITS OWN I2C bus
 * on the same GPIO7/GPIO8 pins the BSP already owns -- that pattern
 * cannot be reused as-is here without a bus conflict. To wire up battery
 * state for real:
 *   1. Copy examples/esp-idf/01_AXP2101/main/components/XPowersLib into
 *      firmware/showcontroller/components/.
 *   2. Do NOT call that example's i2c_init(); instead implement
 *      pmu_register_read()/pmu_register_write_byte() (see
 *      port_axp2101.cpp) on top of bsp_i2c_get_handle() using
 *      i2c_master_transmit()/i2c_master_transmit_receive(), the same way
 *      pcf85063a.c and qmi8658.c do internally.
 *   3. PMU.begin(AXP2101_SLAVE_ADDRESS, ...) with AXP2101_SLAVE_ADDRESS
 *      == BSP_PMU_I2C_ADDRESS (0x34, from the BSP header).
 *   4. Feed showui_inputs_t.battery_pct/.charging from
 *      PMU.getBatteryPercent() / PMU.isCharging() (see main.cpp).
 * The PWR (power key) button is also PMU-side, not a plain GPIO: XPowersLib
 * surfaces it as IRQ status bits (XPOWERS_AXP2101_PKEY_SHORT_IRQ /
 * _PKEY_LONG_IRQ via PMU.getIrqStatus()) -- see port_axp2101.cpp's
 * pmu_isr_handler() for the pattern to crib from.
 *
 * Wi-Fi (showui_inputs_t.wifi_connected) is also left stubbed: bringing up
 * Wi-Fi is out of scope for this skeleton and none of the fetched official
 * examples exercise it.
 */
static void sensor_init(void)
{
    i2c_master_bus_handle_t i2c_bus = bsp_i2c_get_handle();
    if (i2c_bus == NULL) {
        ESP_LOGE(TAG, "BSP I2C bus not available; RTC/IMU disabled");
    } else {
        /* PCF85063A RTC -- mirrors examples/esp-idf/02_PCF85063/main/pcf85063.c,
         * but reuses the BSP's already-initialized I2C0 bus (bsp_i2c_get_handle())
         * instead of that example's private bus, since touch/IMU/PMU/RTC all
         * share the same GPIO7/GPIO8 pins. */
        esp_err_t err = pcf85063a_init(&s_rtc, i2c_bus, PCF85063A_ADDRESS);
        if (err == ESP_OK) {
            s_rtc_ready = true;
        } else {
            ESP_LOGW(TAG, "PCF85063A init failed: %s", esp_err_to_name(err));
        }

        /* QMI8658 IMU -- mirrors examples/esp-idf/04_QMI8658/main/test_qmi8658.c,
         * which already uses bsp_i2c_get_handle(), so this needs no adaptation. */
        memset(&s_imu, 0, sizeof(s_imu));
        err = qmi8658_init(&s_imu, i2c_bus, BSP_IMU_I2C_ADDRESS);
        if (err != ESP_OK) {
            err = qmi8658_init(&s_imu, i2c_bus, QMI8658_ADDRESS_LOW);
        }
        if (err == ESP_OK) {
            qmi8658_set_accel_range(&s_imu, QMI8658_ACCEL_RANGE_4G);
            qmi8658_set_accel_odr(&s_imu, QMI8658_ACCEL_ODR_250HZ);
            qmi8658_set_gyro_range(&s_imu, QMI8658_GYRO_RANGE_256DPS);
            qmi8658_set_gyro_odr(&s_imu, QMI8658_GYRO_ODR_250HZ);
            qmi8658_set_accel_unit_mps2(&s_imu, false); /* false = milli-g output; scaled to g at the read site */
            qmi8658_set_gyro_unit_dps(&s_imu, true);    /* deg/s, matches showui_inputs_t */
            esp_err_t enable_err = qmi8658_enable_sensors(&s_imu, QMI8658_ENABLE_ACCEL | QMI8658_ENABLE_GYRO);
            if (enable_err == ESP_OK) {
                s_imu_ready = true;
            } else {
                ESP_LOGW(TAG, "QMI8658 enable failed: %s", esp_err_to_name(enable_err));
            }
        } else {
            ESP_LOGW(TAG, "QMI8658 init failed: %s", esp_err_to_name(err));
        }
    }

    const gpio_config_t boot_btn_cfg = {
        .pin_bit_mask = 1ULL << SHOWCONTROLLER_BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&boot_btn_cfg);
}

static void sensor_poll_task(void *arg)
{
    (void)arg;
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        showui_inputs_t in = {0};

        /* TODO: AXP2101 battery/charging -- see sensor_init() comment. */
        in.battery_pct = -1;
        in.charging = false;

        if (s_imu_ready) {
            bool ready = false;
            if (qmi8658_is_data_ready(&s_imu, &ready) == ESP_OK && ready) {
                qmi8658_data_t data = {0};
                if (qmi8658_read_sensor_data(&s_imu, &data) == ESP_OK) {
                    /* Driver outputs milli-g in this mode; the HAL contract is g. */
                    in.accel_x = data.accelX / 1000.0f;
                    in.accel_y = data.accelY / 1000.0f;
                    in.accel_z = data.accelZ / 1000.0f;
                    in.gyro_x = data.gyroX;
                    in.gyro_y = data.gyroY;
                    in.gyro_z = data.gyroZ;
                }
            }
        }

        /* TODO: Wi-Fi is not brought up in this skeleton. */
        in.wifi_connected = false;

        if (s_rtc_ready) {
            pcf85063a_datetime_t dt = {0};
            if (pcf85063a_get_time_date(&s_rtc, &dt) == ESP_OK) {
                in.hour = dt.hour;
                in.minute = dt.min;
                in.second = dt.sec;
            }
        }

        in.button_boot = (gpio_get_level(SHOWCONTROLLER_BOOT_BUTTON_GPIO) == 0);
        /* TODO: PWR (PMU power key) button -- see sensor_init() comment. */
        in.button_pwr = false;

        showui_hal_device_publish(&in);

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(SENSOR_POLL_PERIOD_MS));
    }
}

void app_main(void)
{
    /* --- Display / LVGL bring-up: verbatim from 05_LVGL_WITH_RAM's app_main --- */
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0)
    esp_log_level_t i2c_log_level = esp_log_level_get("i2c.master");
    esp_log_level_set("i2c.master", ESP_LOG_NONE);
#endif
    const bsp_board_variant_t variant = bsp_board_detect();
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0)
    esp_log_level_set("i2c.master", i2c_log_level);
#endif
    ESP_LOGI(TAG, "Starting ShowController on %s", bsp_board_variant_to_name(variant));

    lv_display_t *display = bsp_display_start();
    if (display == NULL) {
        ESP_LOGE(TAG, "Display initialization failed");
        return;
    }
    ESP_ERROR_CHECK(bsp_display_backlight_on());

    /* --- Sensors: reuse the I2C0 bus bsp_board_detect()/bsp_display_start()
     * already brought up, so this must run after them. --- */
    showui_hal_device_init();
    sensor_init();
    xTaskCreate(sensor_poll_task, "sensor_poll", 4096, NULL, 5, NULL);

    /* --- Build the portable ShowUI screens: the exact same C code the
     * macOS simulator runs (Simulator/Sources/SimCore/showui/showui.c). --- */
    if (!bsp_display_lock(0)) {
        ESP_LOGE(TAG, "Failed to lock LVGL");
        return;
    }
    showui_create();
    bsp_display_unlock();

    ESP_LOGI(TAG, "ShowController UI is ready");
}
