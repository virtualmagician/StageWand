/**
 * wifi_link.c — see wifi_link.h.
 *
 * Builds clean for esp32c6 (ESP-IDF v5.5.5) but has not run on hardware yet.
 *
 * Shape: esp_event handlers only ever do two things -- flip a flag /
 * publish the HAL snapshot, and wake link_task() -- because both Wi-Fi
 * (re)connect backoff and (worse) an mDNS browse can block for seconds,
 * and neither is safe to do from the system event-loop task. link_task()
 * is where all the blocking happens, and it's the only place that calls
 * showlink_configure() (always under bsp_display_lock()/unlock(), per the
 * concurrency rule in wifi_link.h).
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "showlink.h"
#include "showui_hal_device.h"
#include "wifi_link.h"

static const char *TAG = "wifi_link";

/* Backoff for esp_wifi_connect() retries after WIFI_EVENT_STA_DISCONNECTED. */
#define WIFI_LINK_BACKOFF_MIN_MS 1000
#define WIFI_LINK_BACKOFF_MAX_MS 30000

/* How often to re-browse Bonjour while no _stagewizard._udp host has
 * answered yet. */
#define WIFI_LINK_MDNS_RETRY_MS 5000
#define WIFI_LINK_MDNS_QUERY_TIMEOUT_MS 3000
#define WIFI_LINK_MDNS_MAX_RESULTS 8

static esp_timer_handle_t s_reconnect_timer;
static uint32_t s_retry_count;
static uint32_t s_backoff_ms = WIFI_LINK_BACKOFF_MIN_MS;

/* Set true only between IP_EVENT_STA_GOT_IP and the next disconnect.
 * link_task() re-reads this every time it wakes rather than trusting the
 * event that woke it, so a disconnect that lands mid-resolution is never
 * missed. */
static volatile bool s_sta_connected;

/* Level-triggered wakeup: every state change (connect, disconnect, got-IP)
 * gives this; link_task() just re-evaluates s_sta_connected each time, so
 * coalesced gives from a flappy link are harmless. */
static SemaphoreHandle_t s_wake_sem;

static bool s_mdns_started;

/* Every showlink_configure() call funnels through here so the
 * bsp_display_lock()/unlock() pairing (the concurrency rule in
 * wifi_link.h) can't be gotten wrong at a call site, and a failed lock
 * never unlocks a mutex it doesn't hold. */
static void configure_link_locked(const char *host_ip, uint16_t osc_port, uint16_t http_port, bool enabled)
{
    if (!bsp_display_lock(0)) {
        ESP_LOGE(TAG, "Failed to lock LVGL for showlink_configure(); link state not updated");
        return;
    }
    showlink_configure(host_ip, osc_port, http_port, enabled);
    bsp_display_unlock();
}

static void reconnect_timer_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Wi-Fi reconnect attempt (retry %" PRIu32 ")", s_retry_count);
    esp_wifi_connect();
}

static void schedule_reconnect(void)
{
    if (CONFIG_STAGEWAND_WIFI_MAX_RETRY != 0 && s_retry_count > CONFIG_STAGEWAND_WIFI_MAX_RETRY) {
        ESP_LOGE(TAG, "giving up after %" PRIu32 " Wi-Fi connect retries", s_retry_count - 1);
        return;
    }
    ESP_ERROR_CHECK(esp_timer_start_once(s_reconnect_timer, (uint64_t)s_backoff_ms * 1000));
    if (s_backoff_ms < WIFI_LINK_BACKOFF_MAX_MS) {
        s_backoff_ms = s_backoff_ms * 2 > WIFI_LINK_BACKOFF_MAX_MS ? WIFI_LINK_BACKOFF_MAX_MS : s_backoff_ms * 2;
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = (const wifi_event_sta_disconnected_t *)event_data;
        s_sta_connected = false;
        showui_hal_device_set_wifi_connected(false);
        s_retry_count++;
        ESP_LOGW(TAG, "Wi-Fi disconnected (reason=%d), retry %" PRIu32 " in %" PRIu32 " ms",
                 d != NULL ? d->reason : -1, s_retry_count, s_backoff_ms);
        schedule_reconnect();
        xSemaphoreGive(s_wake_sem);
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = (const ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Wi-Fi connected, got IP:" IPSTR, IP2STR(&e->ip_info.ip));
        s_retry_count = 0;
        s_backoff_ms = WIFI_LINK_BACKOFF_MIN_MS;
        s_sta_connected = true;
        showui_hal_device_set_wifi_connected(true);
        xSemaphoreGive(s_wake_sem);
        return;
    }
}

static void wifi_init_sta(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                          &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                          &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };
    strlcpy((char *)wifi_config.sta.ssid, CONFIG_STAGEWAND_WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, CONFIG_STAGEWAND_WIFI_PASSWORD, sizeof(wifi_config.sta.password));
    if (strlen(CONFIG_STAGEWAND_WIFI_PASSWORD) == 0) {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi station starting, SSID \"%s\"", CONFIG_STAGEWAND_WIFI_SSID);
}

/**
 * Resolve the StageWizard host, blocking until found or Wi-Fi drops.
 *
 * Returns true with out_ip filled in, or false if s_sta_connected went
 * false before a host was found (caller should give up on this attempt
 * and let link_task()'s outer loop re-evaluate).
 */
static bool resolve_host_ip(char *out_ip, size_t cap)
{
    if (strlen(CONFIG_STAGEWAND_HOST_IP) > 0) {
        strlcpy(out_ip, CONFIG_STAGEWAND_HOST_IP, cap);
        ESP_LOGI(TAG, "Using configured StageWizard host %s (mDNS discovery skipped)", out_ip);
        return true;
    }

    if (!s_mdns_started) {
        esp_err_t err = mdns_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "mdns_init failed: %s", esp_err_to_name(err));
            return false;
        }
        s_mdns_started = true;
    }

    while (s_sta_connected) {
        mdns_result_t *results = NULL;
        esp_err_t err = mdns_query_ptr("_stagewizard", "_udp", WIFI_LINK_MDNS_QUERY_TIMEOUT_MS,
                                        WIFI_LINK_MDNS_MAX_RESULTS, &results);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "mdns_query_ptr(_stagewizard._udp) failed: %s", esp_err_to_name(err));
        }

        bool found = false;
        for (mdns_result_t *r = results; r != NULL && !found; r = r->next) {
            ESP_LOGI(TAG, "mDNS: found %s.%s.%s (host %s, port %u)",
                     r->instance_name ? r->instance_name : "?", r->service_type ? r->service_type : "?",
                     r->proto ? r->proto : "?", r->hostname ? r->hostname : "?", r->port);
            for (mdns_ip_addr_t *a = r->addr; a != NULL; a = a->next) {
                if (a->addr.type == ESP_IPADDR_TYPE_V4) {
                    snprintf(out_ip, cap, IPSTR, IP2STR(&a->addr.u_addr.ip4));
                    found = true;
                    break;
                }
            }
        }
        if (results != NULL) {
            mdns_query_results_free(results);
        }
        if (found) {
            ESP_LOGI(TAG, "StageWizard host resolved via Bonjour: %s", out_ip);
            return true;
        }

        ESP_LOGW(TAG, "no _stagewizard._udp responder yet, retrying in %d ms", WIFI_LINK_MDNS_RETRY_MS);
        for (int waited = 0; waited < WIFI_LINK_MDNS_RETRY_MS && s_sta_connected; waited += 500) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
    return false;
}

static void link_task(void *arg)
{
    (void)arg;

    for (;;) {
        xSemaphoreTake(s_wake_sem, portMAX_DELAY);

        if (!s_sta_connected) {
            configure_link_locked("", 0, 0, false);
            ESP_LOGI(TAG, "StageWizard link disabled (Wi-Fi down)");
            continue;
        }

        char host_ip[SHOWLINK_HOST_MAX] = {0};
        if (!resolve_host_ip(host_ip, sizeof(host_ip))) {
            /* Wi-Fi dropped mid-resolution; the disconnect handler already
             * gave s_wake_sem, so looping back picks that up next. */
            continue;
        }
        if (!s_sta_connected) {
            /* Resolved via the static-IP fast path, but Wi-Fi dropped
             * before we got here. */
            continue;
        }

        configure_link_locked(host_ip, CONFIG_STAGEWAND_OSC_PORT, CONFIG_STAGEWAND_HTTP_PORT, true);
        ESP_LOGI(TAG, "StageWizard link enabled -> %s (OSC %d, HTTP %d)", host_ip, CONFIG_STAGEWAND_OSC_PORT,
                 CONFIG_STAGEWAND_HTTP_PORT);
    }
}

void wifi_link_start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    s_wake_sem = xSemaphoreCreateBinary();

    const esp_timer_create_args_t reconnect_timer_args = {
        .callback = &reconnect_timer_cb,
        .name = "wifi_reconnect",
    };
    ESP_ERROR_CHECK(esp_timer_create(&reconnect_timer_args, &s_reconnect_timer));

    wifi_init_sta();

    /* 6 KB stack: link_task does the mDNS browse + IPSTR logging; 4 KB is the
     * usual minimum and a stack overflow here would be an ugly day-one bug. */
    xTaskCreate(link_task, "stagewand_link", 6144, NULL, 4, NULL);
}
