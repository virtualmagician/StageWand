/**
 * simbridge.h — the only API the Swift app talks to.
 *
 * Everything is main-thread-only (LVGL is not thread-safe); the Swift side
 * calls all of these from the main run loop.
 */
#ifndef SIMBRIDGE_H
#define SIMBRIDGE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SIM_HOR_RES 368
#define SIM_VER_RES 448
#define SIM_MAX_FLUSH_RECTS 32

typedef struct {
    int32_t x, y, w, h;
} sim_rect_t;

typedef struct {
    uint32_t frame_count;        /* completed LVGL frames since start */
    uint32_t flushes_last_frame; /* flush callbacks in the last completed frame */
    uint32_t bytes_last_frame;   /* RGB565 bytes pushed in the last completed frame */
    double   est_device_ms;      /* estimated QSPI transfer time on real hardware, ms
                                    (bytes / 20 MB/s: 40 MHz quad SPI, transfer only) */
    double   est_device_fps_cap; /* achievable device fps if every frame cost this much */
    uint32_t rect_count;         /* valid entries in rects[] */
    sim_rect_t rects[SIM_MAX_FLUSH_RECTS]; /* flush areas of the last completed frame */
} sim_stats_t;

/* Lifecycle ---------------------------------------------------------------- */
void sim_init(void);              /* lv_init + display + touch + ShowUI; call once */
uint32_t sim_step(void);          /* run lv_timer_handler once; returns ms until next due */

/* Output ------------------------------------------------------------------- */
const uint8_t *sim_framebuffer(void); /* SIM_HOR_RES*SIM_VER_RES*4, RGBX (R first, X=255) */
uint32_t sim_frame_generation(void);  /* bumps whenever framebuffer content changed */
void sim_get_stats(sim_stats_t *out);

/* Input -------------------------------------------------------------------- */
void sim_touch(bool pressed, int32_t x, int32_t y);   /* device pixel coords */

/* Simulated peripherals (Swift -> device) ---------------------------------- */
void sim_set_battery(int32_t pct, bool charging);
void sim_set_imu(float ax, float ay, float az, float gx, float gy, float gz);
void sim_set_clock(int32_t hour, int32_t minute, int32_t second);
void sim_set_wifi(bool connected);

#define SIM_BUTTON_BOOT 0
#define SIM_BUTTON_PWR  1
void sim_set_button(int32_t which, bool pressed);

/* Device -> Swift ---------------------------------------------------------- */
uint8_t sim_get_brightness(void); /* 0..255 as last set by the UI (DCS 0x51 equivalent) */

/* StageWizard link (showlink) ---------------------------------------------- */
typedef struct {
    bool enabled;
    bool online;
    char standing_by_number[16];
    char standing_by_name[64];
    int32_t running_count;
    bool show_mode;
    bool panicking;
    uint32_t last_status_age_ms;
} sim_link_state_t;

/* Configure the StageWizard link (host is dotted IPv4). */
void sim_set_link(bool enabled, const char *host_ip, uint16_t osc_port, uint16_t http_port);
void sim_get_link_state(sim_link_state_t *out);

/* Jump the UI to a tile: 0 = cues, 1 = GO, 2 = transport, 3 = setup. */
void sim_goto_tile(int32_t index);

#ifdef __cplusplus
}
#endif

#endif /* SIMBRIDGE_H */
