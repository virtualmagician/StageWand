/**
 * simbridge.c — LVGL host port for the macOS simulator.
 *
 * Mirrors the device profile of the Waveshare ESP32-C6-Touch-AMOLED-1.8 BSP:
 *   - RGB565 rendering
 *   - a single partial draw buffer of 368 x 100 lines (the BSP default)
 *   - flush areas rounded to even x/y boundaries (SH8601/CO5300 QSPI quirk)
 * so what redraws here is what the QSPI bus would carry on hardware. Every
 * flush is measured and expressed as estimated device transfer time.
 */
#include <string.h>
#include <time.h>

#include "lvgl.h"
#include "simbridge.h"
#include "showui/showui.h"
#include "showui/showui_hal.h"
#include "showui/showlink.h"

/* 40 MHz QSPI, 4 data lines -> 20 MB/s peak (transfer only, no overhead) */
#define QSPI_BYTES_PER_SEC (20.0 * 1000.0 * 1000.0)
/* Mirrors the BSP default CONFIG_BSP_DISPLAY_LVGL_BUF_HEIGHT = 100 lines */
#define DRAW_BUF_LINES 100

static lv_display_t *s_disp;
static lv_indev_t *s_indev;

static uint8_t s_draw_buf[SIM_HOR_RES * DRAW_BUF_LINES * 2];
static uint8_t s_fb[SIM_HOR_RES * SIM_VER_RES * 4]; /* RGBX, R first */
static uint32_t s_frame_gen;

/* Stats for the frame currently being flushed */
static uint32_t s_acc_flushes, s_acc_bytes, s_acc_rect_count;
static sim_rect_t s_acc_rects[SIM_MAX_FLUSH_RECTS];
static sim_stats_t s_stats;

static struct { bool pressed; int32_t x, y; } s_touch;

static struct {
    int32_t battery_pct; bool charging;
    float ax, ay, az, gx, gy, gz;
    bool wifi;
    int32_t hour, minute, second;
    bool btn_boot, btn_pwr;
    uint8_t brightness;
} s_state = {
    .battery_pct = 84,
    .az = 1.0f,
    .wifi = true,
    .brightness = 255,
};

static uint32_t tick_ms_cb(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

/* SH8601/CO5300 QSPI panels require flush areas aligned to even coordinates;
 * the Waveshare BSP applies exactly this rounding, so the simulator does too. */
static void rounder_cb(lv_event_t *e)
{
    lv_area_t *a = lv_event_get_param(e);
    a->x1 &= ~1;
    a->y1 &= ~1;
    a->x2 |= 1;
    a->y2 |= 1;
    if (a->x2 >= SIM_HOR_RES) a->x2 = SIM_HOR_RES - 1;
    if (a->y2 >= SIM_VER_RES) a->y2 = SIM_VER_RES - 1;
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    const uint16_t *src = (const uint16_t *)px_map;
    const int32_t w = area->x2 - area->x1 + 1;
    const int32_t h = area->y2 - area->y1 + 1;

    for (int32_t row = 0; row < h; row++) {
        uint8_t *dst = &s_fb[(((size_t)(area->y1 + row)) * SIM_HOR_RES + (size_t)area->x1) * 4];
        const uint16_t *s = &src[(size_t)row * (size_t)w];
        for (int32_t col = 0; col < w; col++) {
            const uint16_t px = s[col];
            const uint8_t r5 = (px >> 11) & 0x1F;
            const uint8_t g6 = (px >> 5) & 0x3F;
            const uint8_t b5 = px & 0x1F;
            dst[0] = (uint8_t)((r5 << 3) | (r5 >> 2));
            dst[1] = (uint8_t)((g6 << 2) | (g6 >> 4));
            dst[2] = (uint8_t)((b5 << 3) | (b5 >> 2));
            dst[3] = 0xFF;
            dst += 4;
        }
    }

    s_acc_flushes++;
    s_acc_bytes += (uint32_t)(w * h * 2); /* what the QSPI bus would carry */
    if (s_acc_rect_count < SIM_MAX_FLUSH_RECTS) {
        s_acc_rects[s_acc_rect_count++] = (sim_rect_t){ area->x1, area->y1, w, h };
    }

    if (lv_display_flush_is_last(disp)) {
        s_stats.frame_count++;
        s_stats.flushes_last_frame = s_acc_flushes;
        s_stats.bytes_last_frame = s_acc_bytes;
        s_stats.est_device_ms = (double)s_acc_bytes / QSPI_BYTES_PER_SEC * 1000.0;
        double cap = (s_stats.est_device_ms > 0.0) ? 1000.0 / s_stats.est_device_ms : 60.0;
        s_stats.est_device_fps_cap = (cap > 60.0) ? 60.0 : cap;
        s_stats.rect_count = s_acc_rect_count;
        memcpy(s_stats.rects, s_acc_rects, sizeof(s_acc_rects));
        s_acc_flushes = 0;
        s_acc_bytes = 0;
        s_acc_rect_count = 0;
        s_frame_gen++;
    }

    lv_display_flush_ready(disp);
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point.x = s_touch.x;
    data->point.y = s_touch.y;
    data->state = s_touch.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

/* Lifecycle ---------------------------------------------------------------- */

void sim_init(void)
{
    lv_init();
    lv_tick_set_cb(tick_ms_cb);

    s_disp = lv_display_create(SIM_HOR_RES, SIM_VER_RES);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_disp, s_draw_buf, NULL, sizeof(s_draw_buf),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_display_add_event_cb(s_disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    s_indev = lv_indev_create();
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, touch_read_cb);
    lv_indev_set_display(s_indev, s_disp);

    showui_create();
}

uint32_t sim_step(void)
{
    return lv_timer_handler();
}

/* Output ------------------------------------------------------------------- */

const uint8_t *sim_framebuffer(void) { return s_fb; }
uint32_t sim_frame_generation(void) { return s_frame_gen; }

void sim_get_stats(sim_stats_t *out)
{
    if (out) *out = s_stats;
}

/* Input -------------------------------------------------------------------- */

void sim_touch(bool pressed, int32_t x, int32_t y)
{
    if (x < 0) x = 0;
    if (x >= SIM_HOR_RES) x = SIM_HOR_RES - 1;
    if (y < 0) y = 0;
    if (y >= SIM_VER_RES) y = SIM_VER_RES - 1;
    s_touch.pressed = pressed;
    s_touch.x = x;
    s_touch.y = y;
}

/* Simulated peripherals ---------------------------------------------------- */

void sim_set_battery(int32_t pct, bool charging)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    s_state.battery_pct = pct;
    s_state.charging = charging;
}

void sim_set_imu(float ax, float ay, float az, float gx, float gy, float gz)
{
    s_state.ax = ax; s_state.ay = ay; s_state.az = az;
    s_state.gx = gx; s_state.gy = gy; s_state.gz = gz;
}

void sim_set_clock(int32_t hour, int32_t minute, int32_t second)
{
    s_state.hour = hour;
    s_state.minute = minute;
    s_state.second = second;
}

void sim_set_wifi(bool connected) { s_state.wifi = connected; }

void sim_set_button(int32_t which, bool pressed)
{
    if (which == SIM_BUTTON_BOOT) s_state.btn_boot = pressed;
    else if (which == SIM_BUTTON_PWR) s_state.btn_pwr = pressed;
}

uint8_t sim_get_brightness(void) { return s_state.brightness; }

/* StageWizard link --------------------------------------------------------- */

void sim_set_link(bool enabled, const char *host_ip, uint16_t osc_port, uint16_t http_port)
{
    showlink_configure(host_ip, osc_port, http_port, enabled);
}

void sim_goto_tile(int32_t index)
{
    showui_goto_tile((int)index);
}

void sim_get_link_state(sim_link_state_t *out)
{
    if (!out) return;
    showlink_state_t st;
    showlink_get_state(&st);
    out->enabled = st.enabled;
    out->online = st.online;
    memcpy(out->standing_by_number, st.standing_by_number, sizeof(out->standing_by_number));
    memcpy(out->standing_by_name, st.standing_by_name, sizeof(out->standing_by_name));
    out->running_count = st.running_count;
    out->show_mode = st.show_mode;
    out->panicking = st.panicking;
    out->last_status_age_ms = st.last_status_age_ms;
}

/* showui_hal implementation (the Mac side of the portable HAL) ------------- */

void showui_hal_get_inputs(showui_inputs_t *out)
{
    if (!out) return;
    out->battery_pct = s_state.battery_pct;
    out->charging = s_state.charging;
    out->accel_x = s_state.ax;
    out->accel_y = s_state.ay;
    out->accel_z = s_state.az;
    out->gyro_x = s_state.gx;
    out->gyro_y = s_state.gy;
    out->gyro_z = s_state.gz;
    out->wifi_connected = s_state.wifi;
    out->hour = s_state.hour;
    out->minute = s_state.minute;
    out->second = s_state.second;
    out->button_boot = s_state.btn_boot;
    out->button_pwr = s_state.btn_pwr;
}

void showui_hal_set_brightness(uint8_t level)
{
    s_state.brightness = level; /* device: MIPI-DCS 0x51 via the BSP */
}
