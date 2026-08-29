/**
 * showlink.h — the StageWand <-> StageWizard link. Portable C.
 *
 * Rides StageWizard's existing remote hooks (dev branch, v1.5.x) unmodified:
 *   commands OUT: OSC 1.0 over UDP (default port 53100) — address-only
 *                 messages: /stagewizard/go, /stopall, /next, /prev,
 *                 /toggle, /panic, /stagewizard/cue/<number>/fire
 *   state IN:     the web remote's GET /status (default port 53200) polled
 *                 at 2 Hz — standing-by cue, running count, show mode, panic.
 *
 * Implementation is plain non-blocking BSD sockets, so the same file
 * compiles on macOS (simulator) and ESP-IDF/lwIP (firmware). Single-threaded:
 * call everything from the LVGL loop thread. No LVGL dependency.
 */
#ifndef SHOWLINK_H
#define SHOWLINK_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHOWLINK_HOST_MAX  64
#define SHOWLINK_NUM_MAX   16
#define SHOWLINK_NAME_MAX  64

#define SHOWLINK_DEFAULT_OSC_PORT  53100
#define SHOWLINK_DEFAULT_HTTP_PORT 53200

typedef struct {
    bool enabled;               /* link switched on by the operator */
    bool online;                /* a good /status arrived within the last 2.5 s */
    char standing_by_number[SHOWLINK_NUM_MAX];  /* "" when none/unknown */
    char standing_by_name[SHOWLINK_NAME_MAX];   /* "" when none/unknown */
    int32_t running_count;
    bool show_mode;
    bool panicking;
    uint32_t last_status_age_ms;  /* ms since last good /status; UINT32_MAX if never */
} showlink_state_t;

void showlink_init(void);

/* Reconfigure at any time; enabled=false closes sockets. host_ip is a dotted
 * IPv4 string (the simulator defaults to 127.0.0.1). */
void showlink_configure(const char *host_ip, uint16_t osc_port,
                        uint16_t http_port, bool enabled);

/* Pump sockets + the /status poll state machine. Call at ~10 Hz from the UI
 * loop thread with a monotonic millisecond clock (e.g. lv_tick_get()). */
void showlink_tick(uint32_t now_ms);

void showlink_get_state(showlink_state_t *out);

/* Commands — OSC, fire-and-forget; silent no-ops while disabled. */
void showlink_send_go(void);
void showlink_send_stopall(void);
void showlink_send_panic(void);
void showlink_send_next(void);
void showlink_send_prev(void);
void showlink_send_toggle(void);
void showlink_send_fire_cue(const char *number);  /* number: no slashes, per host rule */

#ifdef __cplusplus
}
#endif

#endif /* SHOWLINK_H */
