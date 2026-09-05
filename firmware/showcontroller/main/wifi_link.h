/**
 * wifi_link.h — Wi-Fi station bring-up + StageWizard host discovery for
 * StageWand.
 *
 * UNTESTED: written without hardware or an ESP-IDF toolchain present.
 *
 * Owns everything between "board has power" and "showlink is enabled and
 * pointed at the right host": NVS/esp_netif/esp_wifi station init, connect
 * with backoff on WIFI_EVENT_STA_DISCONNECTED, publishing
 * showui_inputs_t.wifi_connected, resolving the StageWizard host (static IP
 * from Kconfig, or Bonjour "_stagewizard._udp" browse via the espressif/mdns
 * component), and calling showlink_configure() to enable/disable the link.
 *
 * Concurrency: showlink is single-threaded and ticked from the LVGL task
 * (see main.c). Every showlink_configure() call this module makes runs on
 * its own background task, never directly from a Wi-Fi/event-loop
 * callback, and is wrapped in bsp_display_lock()/bsp_display_unlock().
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Bring up NVS + Wi-Fi station mode and start the background task that
 * resolves the StageWizard host and drives showlink_configure().
 *
 * Call once from app_main(), after showui_hal_device_init() (so the
 * wifi_connected flag has somewhere to land) and after the display/LVGL
 * bring-up that owns bsp_display_lock()/bsp_display_unlock() (so this
 * module can use them once Wi-Fi comes up).
 */
void wifi_link_start(void);

#ifdef __cplusplus
}
#endif
