/**
 * ble_link.c — see ble_link.h.
 *
 * UNTESTED: written without hardware or an ESP-IDF toolchain present.
 *
 * Modeled on ESP-IDF's NimBLE examples (v5.5.5):
 *   examples/bluetooth/nimble/bleprph/main/{main.c,bleprph.h}     -- host
 *     init (nimble_port_init/ble_hs_cfg/nimble_port_freertos_init), GAP
 *     event handling, advertising.
 *   examples/bluetooth/nimble/ble_spp/spp_server/main/main.c      -- the
 *     write+notify characteristic shape (ble_gatt_svc_def table,
 *     ble_hs_mbuf_from_flat()+ble_gatts_notify_custom() for outbound,
 *     ble_hs_mbuf_to_flat() for inbound) this module's RX/TX pair mirrors
 *     almost exactly, swapping the example's UART bytes for showlink's.
 * API signatures verified against the exact NimBLE sources ESP-IDF v5.5.5
 * vendors (submodule espressif/esp-nimble @ 685675c0128deafdd201c9eb82e61d227364646c):
 * host/{ble_hs.h,ble_gap.h,ble_gatt.h,ble_att.h,ble_hs_adv.h,ble_hs_mbuf.h,
 * ble_uuid.h,ble_hs_id.h,util/util.h}, nimble/{nimble_port.h,ble.h}.
 */
#include <inttypes.h>
#include <string.h>

#include "esp_log.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "bsp/esp-bsp.h"

#include "ble_link.h"
#include "showlink.h"
#include "showui_hal.h"

static const char *TAG = "ble_link";

/* Service 8B0F4F44-5A5B-4EC1-A0E9-77616E640001 / RX ...0002 / TX ...0003 --
 * the contract in docs/showlink.md's "BLE as-built correction" (host shipped
 * dev D28, BLEWandLink.swift). NimBLE 128-bit UUIDs are stored least-
 * significant-byte first (on-air order), i.e. reversed from the UUID's
 * usual hex-string form -- these three arrays are the same 16 bytes with
 * only the last-group tail (...0001/0002/0003) changed, byte-reversed. */
static const ble_uuid128_t s_svc_uuid = BLE_UUID128_INIT(
    0x01, 0x00, 0x64, 0x6e, 0x61, 0x77, 0xe9, 0xa0,
    0xc1, 0x4e, 0x5b, 0x5a, 0x44, 0x4f, 0x0f, 0x8b);
static const ble_uuid128_t s_rx_chr_uuid = BLE_UUID128_INIT(
    0x02, 0x00, 0x64, 0x6e, 0x61, 0x77, 0xe9, 0xa0,
    0xc1, 0x4e, 0x5b, 0x5a, 0x44, 0x4f, 0x0f, 0x8b);
static const ble_uuid128_t s_tx_chr_uuid = BLE_UUID128_INIT(
    0x03, 0x00, 0x64, 0x6e, 0x61, 0x77, 0xe9, 0xa0,
    0xc1, 0x4e, 0x5b, 0x5a, 0x44, 0x4f, 0x0f, 0x8b);

/* ATT attribute values are capped at 512 bytes by the spec; that's the most
 * a single RX write could ever legally carry, regardless of MTU. showlink's
 * own chunking keeps real writes to (negotiated MTU - 3), far smaller. */
#define BLE_LINK_RX_BUF_MAX 512

static uint8_t s_rx_buf[BLE_LINK_RX_BUF_MAX];

static uint8_t s_own_addr_type;
static bool s_host_synced;
static bool s_wifi_up; /* mirrors the last ble_link_set_wifi_up() request */

static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_tx_val_handle;
static bool s_subscribed;

static int ble_link_gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                                 struct ble_gatt_access_ctxt *ctxt, void *arg);
static int ble_link_gap_event(struct ble_gap_event *event, void *arg);
static void ble_link_start_advertising(void);

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        /*** showlink BLE transport service ***/
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                /* RX: host writes its feedback frames here. */
                .uuid = &s_rx_chr_uuid.u,
                .access_cb = ble_link_gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                /* TX: wand notifies commands/pings here. CCCD is added
                 * automatically by NimBLE because of the NOTIFY flag --
                 * ble_gatt_chr_def's own doc comment says not to declare it
                 * ourselves. */
                .uuid = &s_tx_chr_uuid.u,
                .access_cb = ble_link_gatt_access,
                .val_handle = &s_tx_val_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY,
            },
            {
                0, /* No more characteristics in this service. */
            },
        },
    },
    {
        0, /* No more services. */
    },
};

/**
 * Logs a peer's Bluetooth address for a connection event, MSB first (NimBLE
 * stores ble_addr_t.val LSB first, same on-air convention as the 128-bit
 * UUIDs above).
 */
static void ble_link_log_peer_addr(uint16_t conn_handle, const char *what)
{
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn_handle, &desc) != 0) {
        ESP_LOGI(TAG, "%s (conn_handle=%d)", what, conn_handle);
        return;
    }
    const uint8_t *a = desc.peer_id_addr.val;
    ESP_LOGI(TAG, "%s: peer=%02x:%02x:%02x:%02x:%02x:%02x conn_handle=%d",
             what, a[5], a[4], a[3], a[2], a[1], a[0], conn_handle);
}

/**
 * showlink_ble_send_fn -- called BY showlink, on the LVGL task, already
 * under the display lock (see ble_link.h). Must not block: a GATT
 * notification is fire-and-forget from the host's point of view, so this
 * only allocates an mbuf and hands it to the stack.
 */
static bool ble_link_send(const uint8_t *data, uint32_t len, void *ctx)
{
    (void)ctx;

    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || !s_subscribed) {
        return false;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, (uint16_t)len);
    if (om == NULL) {
        ESP_LOGW(TAG, "ble_hs_mbuf_from_flat failed (len=%" PRIu32 ")", len);
        return false;
    }

    /* Consumes om regardless of outcome -- nothing left for us to free. */
    int rc = ble_gatts_notify_custom(s_conn_handle, s_tx_val_handle, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gatts_notify_custom failed: rc=%d", rc);
        return false;
    }
    return true;
}

/**
 * GATT access callback shared by RX and TX. TX has no READ/WRITE flag, so
 * the ATT layer rejects any read/write attempt on it before ever reaching
 * here -- only RX's writes are expected. CCCD subscribe/unsubscribe is
 * handled entirely by NimBLE and surfaces as BLE_GAP_EVENT_SUBSCRIBE
 * instead, not through this callback.
 */
static int ble_link_gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                                 struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len == 0) {
        return 0;
    }
    if (len > sizeof(s_rx_buf)) {
        ESP_LOGW(TAG, "RX write too large (%u > %u bytes); dropped",
                 (unsigned)len, (unsigned)sizeof(s_rx_buf));
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    uint16_t copied = 0;
    int rc = ble_hs_mbuf_to_flat(ctxt->om, s_rx_buf, sizeof(s_rx_buf), &copied);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_hs_mbuf_to_flat failed: rc=%d", rc);
        return BLE_ATT_ERR_UNLIKELY;
    }

    /* Concurrency rule (ble_link.h / wifi_link.h): every call into
     * showlink_ble_* from a NimBLE host callback is wrapped in the display
     * lock, exactly like wifi_link.c's configure_link_locked(). */
    if (bsp_display_lock(0)) {
        showlink_ble_receive(s_rx_buf, copied);
        bsp_display_unlock();
    } else {
        ESP_LOGE(TAG, "Failed to lock LVGL for showlink_ble_receive(); RX frame dropped");
    }

    return 0;
}

static void ble_link_attach_locked(uint16_t conn_handle)
{
    uint16_t mtu = ble_att_mtu(conn_handle);
    uint32_t max_chunk = (mtu > 3) ? (uint32_t)(mtu - 3) : (BLE_ATT_MTU_DFLT - 3);

    if (bsp_display_lock(0)) {
        showlink_ble_attach(ble_link_send, NULL, max_chunk);
        bsp_display_unlock();
        ESP_LOGI(TAG, "showlink attached over BLE (conn_handle=%d, mtu=%u, max_chunk=%" PRIu32 ")",
                 conn_handle, (unsigned)mtu, max_chunk);
    } else {
        ESP_LOGE(TAG, "Failed to lock LVGL for showlink_ble_attach(); staying detached");
    }
}

static void ble_link_detach_locked(void)
{
    if (bsp_display_lock(0)) {
        showlink_ble_detach();
        bsp_display_unlock();
        ESP_LOGI(TAG, "showlink detached from BLE");
    } else {
        ESP_LOGE(TAG, "Failed to lock LVGL for showlink_ble_detach()");
    }
}

static void ble_link_stop_advertising(void)
{
    if (!ble_gap_adv_active()) {
        return;
    }
    int rc = ble_gap_adv_stop();
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gap_adv_stop failed: rc=%d", rc);
        return;
    }
    ESP_LOGI(TAG, "BLE advertising stopped");
}

/**
 * Starts (or restarts) advertising: general discoverable, undirected
 * connectable, fast interval. The device name doesn't fit alongside the
 * complete 128-bit service UUID in the 31-byte legacy ADV_IND payload
 * (flags(3) + tx_pwr(3) + uuid128(18) = 24 bytes already; adding the
 * "StageWand-XXXX" name would push it over), so the name goes in the scan
 * response instead -- CoreBluetooth (StageWizard's central) merges both
 * into one advertisementData dictionary, so this split is invisible to the
 * app on the other end.
 */
static void ble_link_start_advertising(void)
{
    if (!s_host_synced || ble_gap_adv_active()) {
        return;
    }

    char name[32];
    showui_hal_get_device_name(name, sizeof(name));
    ble_svc_gap_device_name_set(name);

    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.uuids128 = &s_svc_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields failed: rc=%d", rc);
        return;
    }

    struct ble_hs_adv_fields rsp_fields;
    memset(&rsp_fields, 0, sizeof(rsp_fields));
    rsp_fields.name = (const uint8_t *)name;
    rsp_fields.name_len = (uint8_t)strlen(name);
    rsp_fields.name_is_complete = 1;

    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_rsp_set_fields failed: rc=%d", rc);
        return;
    }

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv_params.itvl_min = BLE_GAP_ADV_FAST_INTERVAL1_MIN;
    adv_params.itvl_max = BLE_GAP_ADV_FAST_INTERVAL1_MAX;

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &adv_params,
                           ble_link_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start failed: rc=%d", rc);
        return;
    }

    ESP_LOGI(TAG, "BLE advertising started as \"%s\"", name);
}

static int ble_link_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_subscribed = false;
            ble_link_log_peer_addr(s_conn_handle, "BLE connected");

            /* Fast connection interval for GO latency: 7.5-15 ms in 1.25 ms
             * units (6/12); no direct BLE_GAP_CONN_ITVL_MS() equivalent for
             * a fractional 7.5 ms, so these are written out numerically. */
            struct ble_gap_upd_params params = {
                .itvl_min = 6,
                .itvl_max = 12,
                .latency = 0,
                .supervision_timeout = BLE_GAP_INITIAL_SUPERVISION_TIMEOUT,
                .min_ce_len = BLE_GAP_INITIAL_CONN_MIN_CE_LEN,
                .max_ce_len = BLE_GAP_INITIAL_CONN_MAX_CE_LEN,
            };
            int rc = ble_gap_update_params(s_conn_handle, &params);
            if (rc != 0) {
                ESP_LOGW(TAG, "ble_gap_update_params failed: rc=%d", rc);
            }
        } else {
            ESP_LOGW(TAG, "BLE connect attempt failed; status=%d", event->connect.status);
            if (!s_wifi_up) {
                ble_link_start_advertising();
            }
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE disconnected; reason=%d", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        if (s_subscribed) {
            s_subscribed = false;
            ble_link_detach_locked();
        }
        if (!s_wifi_up) {
            ble_link_start_advertising();
        }
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "BLE subscribe event: conn_handle=%d attr_handle=%d cur_notify=%d",
                 event->subscribe.conn_handle, event->subscribe.attr_handle,
                 event->subscribe.cur_notify);
        if (event->subscribe.cur_notify && !s_subscribed) {
            s_subscribed = true;
            ble_link_attach_locked(event->subscribe.conn_handle);
        } else if (!event->subscribe.cur_notify && s_subscribed) {
            s_subscribed = false;
            ble_link_detach_locked();
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "BLE MTU updated: conn_handle=%d mtu=%d",
                 event->mtu.conn_handle, event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        /* ble_gap_adv_start() below is called with BLE_HS_FOREVER, so this
         * only fires if advertising was stopped by something other than a
         * successful connection (e.g. an internal host restart) -- resume
         * unless Wi-Fi has since come up. */
        if (!s_wifi_up) {
            ble_link_start_advertising();
        }
        return 0;

    default:
        return 0;
    }
}

static void ble_link_on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE host reset; reason=%d", reason);
}

static void ble_link_on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_util_ensure_addr failed: rc=%d", rc);
        return;
    }

    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed: rc=%d", rc);
        return;
    }

    s_host_synced = true;
    ESP_LOGI(TAG, "NimBLE host synced (own_addr_type=%d)", s_own_addr_type);

    if (!s_wifi_up) {
        ble_link_start_advertising();
    }
}

static void ble_link_host_task(void *param)
{
    (void)param;
    ESP_LOGI(TAG, "NimBLE host task started");
    /* Returns only once nimble_port_stop() runs, which this module never
     * calls -- the BLE stack lives for the lifetime of the firmware. */
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void ble_link_gatt_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg)
{
    (void)arg;
    char uuid_buf[BLE_UUID_STR_LEN];

    switch (ctxt->op) {
    case BLE_GATT_REGISTER_OP_SVC:
        ESP_LOGI(TAG, "registered service %s handle=%d",
                 ble_uuid_to_str(ctxt->svc.svc_def->uuid, uuid_buf), ctxt->svc.handle);
        break;
    case BLE_GATT_REGISTER_OP_CHR:
        ESP_LOGI(TAG, "registered characteristic %s def_handle=%d val_handle=%d",
                 ble_uuid_to_str(ctxt->chr.chr_def->uuid, uuid_buf),
                 ctxt->chr.def_handle, ctxt->chr.val_handle);
        break;
    default:
        break;
    }
}

static int ble_link_gatt_svr_init(void)
{
    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) {
        return rc;
    }
    return ble_gatts_add_svcs(s_gatt_svcs);
}

void ble_link_init(void)
{
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return;
    }

    ble_hs_cfg.reset_cb = ble_link_on_reset;
    ble_hs_cfg.sync_cb = ble_link_on_sync;
    ble_hs_cfg.gatts_register_cb = ble_link_gatt_register_cb;
    /* No pairing/bonding in this contract (sdkconfig.defaults disables the
     * Security Manager outright, CONFIG_BT_NIMBLE_SECURITY_ENABLE=n) --
     * ble_hs_cfg.sm_* is left untouched at its all-zero default. */

    int rc = ble_link_gatt_svr_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_link_gatt_svr_init failed: rc=%d", rc);
        return;
    }

    /* nimble_port_freertos_init() creates the NimBLE host task at the stack
     * size Kconfig gives it (CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE, 4096
     * bytes by default) -- ble_link_on_sync() above runs on that task, as
     * does every GAP/GATT callback in this file. */
    nimble_port_freertos_init(ble_link_host_task);

    ESP_LOGI(TAG, "BLE link initialized; advertising deferred until Wi-Fi is down and the host syncs");
}

void ble_link_set_wifi_up(bool up)
{
    s_wifi_up = up;

    if (!s_host_synced) {
        /* ble_link_on_sync() re-reads s_wifi_up once it finishes; nothing
         * else to do until then. */
        return;
    }

    if (up) {
        ble_link_stop_advertising();
        if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            ESP_LOGI(TAG, "Wi-Fi up; terminating BLE connection (coexistence switchover)");
            /* showlink_ble_detach() follows from the resulting
             * BLE_GAP_EVENT_DISCONNECT, under the display lock. */
            int rc = ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            if (rc != 0) {
                ESP_LOGW(TAG, "ble_gap_terminate failed: rc=%d", rc);
            }
        }
    } else {
        /* Can't already be connected here: BLE is never advertising (hence
         * never connectable) while Wi-Fi is up. */
        ble_link_start_advertising();
    }
}
