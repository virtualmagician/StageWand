/**
 * ble_link.h — NimBLE peripheral for the showlink BLE fallback transport.
 *
 * UNTESTED: written without hardware or an ESP-IDF toolchain present.
 *
 * StageWizard connects as a CoreBluetooth CENTRAL (dev D28, BLEWandLink.swift)
 * to a single GATT service this module advertises + serves:
 *
 *   Service 8B0F4F44-5A5B-4EC1-A0E9-77616E640001
 *     RX  ...0002  WRITE + WRITE_NO_RSP  -- host writes its feedback frames
 *     TX  ...0003  NOTIFY                -- wand notifies commands + pings
 *
 * Framing (u16 big-endian length + one OSC message; split/coalesce
 * reassembly) is entirely showlink's problem -- see
 * Simulator/Sources/SimCore/showui/showlink.h's "BLE transport hooks". This
 * module only pushes/pulls raw chunks through showlink_ble_attach() /
 * showlink_ble_receive() and never looks inside a frame.
 *
 * Coexistence: advertises ONLY while Wi-Fi is down (Espressif marks
 * Wi-Fi-connected + BLE-connected coexistence "performance unstable" --
 * see docs/showlink.md's BLE section). wifi_link.c calls
 * ble_link_set_wifi_up() from its IP_EVENT_STA_GOT_IP and
 * WIFI_EVENT_STA_DISCONNECTED handlers to drive the switchover.
 *
 * Concurrency: every call this module makes into showlink_ble_* happens on
 * a NimBLE host-task GAP/GATT callback and is wrapped in
 * bsp_display_lock(0)/bsp_display_unlock(), exactly like wifi_link.c's
 * configure_link_locked() -- see the rule in wifi_link.h. The showlink_ble_send_fn
 * this module hands to showlink_ble_attach() runs the other direction: it is
 * called BY showlink on the LVGL task (already under the display lock), so
 * it must not block -- see ble_link.c.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Bring up the NimBLE host (nimble_port_init, GAP/GATT service registration,
 * the host FreeRTOS task) and register the showlink GATT service. Does NOT
 * start advertising -- that only happens once ble_link_set_wifi_up(false)
 * is called (or if Wi-Fi is already down when this runs, see main.c's call
 * order).
 *
 * Call once from app_main(), after showui_create()/bsp_display_unlock() (so
 * showlink_init() has already run and this module can safely call
 * showlink_ble_attach()/_detach()/_receive() from GAP/GATT callbacks) and
 * before wifi_link_start()'s Wi-Fi events can race ble_link_set_wifi_up()
 * against a not-yet-initialized BLE stack.
 */
void ble_link_init(void);

/**
 * Drive the Wi-Fi/BLE coexistence switchover.
 *
 *   up == true:  stop advertising; if a central is connected, terminate the
 *                connection (showlink_ble_detach() follows from the
 *                resulting BLE_GAP_EVENT_DISCONNECT, under the display lock).
 *   up == false: start advertising, unless already connected (can't happen
 *                in practice -- BLE is never connectable while Wi-Fi is up).
 *
 * Called from wifi_link.c's Wi-Fi event handler (IP_EVENT_STA_GOT_IP /
 * WIFI_EVENT_STA_DISCONNECTED) -- see wifi_link.h's concurrency note; this
 * function itself only calls ble_gap_* APIs, which NimBLE documents as safe
 * to call from any task, so it needs no display lock of its own.
 */
void ble_link_set_wifi_up(bool up);

#ifdef __cplusplus
}
#endif
