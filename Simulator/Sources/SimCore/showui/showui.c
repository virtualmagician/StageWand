/**
 * showui.c — StageWand HMI for the Waveshare ESP32-C6-Touch-AMOLED-1.8.
 *
 * Portable: compiles unchanged in the macOS simulator and the ESP-IDF
 * firmware. Depends only on LVGL, showui_hal.h, and showlink.h.
 *
 * The wand carries NO show content of its own — every cue on screen comes
 * from StageWizard over showlink. Offline is an explicit empty state with
 * the transport controls disabled.
 *
 * Designed for the board's real constraints (no PSRAM, single core, QSPI
 * partial refresh): screens are built from small widgets that update
 * independently on stable black backgrounds — no full-screen animations.
 *
 * Layout: 4 horizontal tiles — [Cues] [GO] [Faders] [Setup] — swipe to
 * navigate, page dots at the bottom, status bar on the top layer.
 * The physical BOOT button doubles as a hardware GO; PWR toggles display
 * sleep (blackout).
 */
#include <stdio.h>
#include <string.h>
#include "lvgl.h"
#include "showui.h"
#include "showui_hal.h"
#include "showlink.h"

/* --- palette (AMOLED true-black theme, amber accent) ---------------------- */
#define COL_BG       lv_color_black()
#define COL_SURFACE  lv_color_hex(0x14181D)
#define COL_LINE     lv_color_hex(0x2A323C)
#define COL_TEXT     lv_color_hex(0xE8EAED)
#define COL_MUTED    lv_color_hex(0x8A93A0)
#define COL_AMBER    lv_color_hex(0xFFB454)
#define COL_GO       lv_color_hex(0x35C45F)
#define COL_RED      lv_color_hex(0xFF5C5C)

#define STATUS_H 30
#define TILE_COUNT 4
#define TILE_CUELIST 0
#define TILE_CUE     1
#define TILE_FADERS  2
#define TILE_SETUP   3

/* Submaster mapping is an open StageWizard product decision (P3); the
 * faders are live UI but drive nothing yet. */
static const char *FADER_NAMES[4] = { "HSE", "KEY", "FX", "AUD" };

static bool s_asleep = false;  /* PWR: display blackout */

/* --- widgets we update at runtime ----------------------------------------- */
static lv_obj_t *s_tv;
static lv_obj_t *s_dots[TILE_COUNT];
static lv_obj_t *s_time_label, *s_batt_label, *s_wifi_label, *s_link_dot;
static lv_obj_t *s_cue_eyebrow, *s_cue_num, *s_cue_name, *s_cue_next, *s_cue_pos;
static lv_obj_t *s_cue_prog;
static lv_obj_t *s_go_btn, *s_go_label, *s_prev_btn, *s_next_btn;
static lv_obj_t *s_cuelist, *s_notes_label;
static lv_obj_t *s_stopall_btn, *s_panic_btn;
static lv_obj_t *s_imu_label, *s_bri_value;
static lv_obj_t *s_net_wifi, *s_net_link, *s_net_host, *s_net_mode;
static lv_obj_t *s_sleep_overlay;

static bool s_prev_boot = false, s_prev_pwr = false;
static bool s_link_live = false;

/* Cue-list render bookkeeping: rebuild rows only when the source changed. */
static uint32_t s_rendered_rev = 0;
static char s_rendered_sig[160];
static char s_row_numbers[SHOWLINK_MAX_CUES][SHOWLINK_NUM_MAX];
static char s_sb_highlighted[SHOWLINK_NUM_MAX];

/* --- helpers -------------------------------------------------------------- */

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, text);
    return l;
}

/* Update a label only when its text actually changes, so link refreshes
 * don't invalidate (and re-flush) unchanged screen regions every 500 ms. */
static void label_set_if_changed(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
}

static void set_enabled(lv_obj_t *btn, bool enabled)
{
    if (enabled) lv_obj_remove_state(btn, LV_STATE_DISABLED);
    else lv_obj_add_state(btn, LV_STATE_DISABLED);
}

/* --- events ---------------------------------------------------------------- */

static void go_clicked_cb(lv_event_t *e)
{
    (void)e;
    if (s_link_live && !s_asleep) showlink_send_go();
}

static void prev_clicked_cb(lv_event_t *e) { (void)e; showlink_send_prev(); }
static void next_clicked_cb(lv_event_t *e) { (void)e; showlink_send_next(); }
static void stopall_clicked_cb(lv_event_t *e) { (void)e; showlink_send_stopall(); }
static void panic_clicked_cb(lv_event_t *e) { (void)e; showlink_send_panic(); }

/* Tap a cue row: arm it on the host (select, never fire). The row's number
 * lives in the static pool s_row_numbers, passed as user_data. */
static void cue_row_clicked_cb(lv_event_t *e)
{
    const char *number = lv_event_get_user_data(e);
    if (number && number[0]) showlink_send_select_cue(number);
}

static void tv_scroll_end_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_t *active = lv_tileview_get_tile_active(s_tv);
    for (int i = 0; i < TILE_COUNT; i++) {
        bool on = (lv_obj_get_child(s_tv, i) == active);
        lv_obj_set_style_bg_color(s_dots[i], on ? COL_AMBER : COL_LINE, 0);
    }
}

/* --- cue list rendering ---------------------------------------------------- */

typedef struct { const char *num; const char *name; } cue_entry_t;

static lv_obj_t *make_cue_row(const char *number, const char *name, int pool_idx)
{
    lv_obj_t *rowc = lv_obj_create(s_cuelist);
    lv_obj_remove_style_all(rowc);
    lv_obj_set_size(rowc, LV_PCT(100), 44);
    lv_obj_set_style_bg_color(rowc, COL_BG, 0);
    lv_obj_set_style_bg_opa(rowc, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(rowc, 8, 0);
    lv_obj_set_style_border_width(rowc, 1, 0);
    lv_obj_set_style_border_color(rowc, COL_LINE, 0);
    lv_obj_set_style_pad_hor(rowc, 12, 0);
    lv_obj_set_flex_flow(rowc, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(rowc, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(rowc, 12, 0);

    strncpy(s_row_numbers[pool_idx], number, SHOWLINK_NUM_MAX - 1);
    s_row_numbers[pool_idx][SHOWLINK_NUM_MAX - 1] = '\0';
    lv_obj_add_flag(rowc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(rowc, cue_row_clicked_cb, LV_EVENT_CLICKED, s_row_numbers[pool_idx]);

    lv_obj_t *num = make_label(rowc, &lv_font_montserrat_16, COL_MUTED, number);
    lv_obj_set_width(num, 42);
    lv_obj_t *nm = make_label(rowc, &lv_font_montserrat_14, COL_TEXT, name);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_flex_grow(nm, 1);
    return rowc;
}

static void cuelist_show_message(const char *msg)
{
    lv_obj_clean(s_cuelist);
    lv_obj_t *l = make_label(s_cuelist, &lv_font_montserrat_14, COL_MUTED, msg);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_margin_top(l, 40, 0);
    s_rendered_sig[0] = '\0';
    s_sb_highlighted[0] = '\0';
}

/* Rebuild rows when the underlying cue set changed; restyle the standing-by
 * highlight (and scroll it into view) when only the playhead moved. */
static void cuelist_update(const showlink_state_t *link)
{
    /* Gather entries: the full host list when it exists, else the window. */
    cue_entry_t entries[SHOWLINK_MAX_CUES];
    static char nums[SHOWLINK_MAX_CUES][SHOWLINK_NUM_MAX];
    static char names[SHOWLINK_MAX_CUES][SHOWLINK_NAME_MAX];
    int count = 0;
    int32_t host_count = showlink_cue_count();
    if (host_count > 0) {
        for (int32_t i = 0; i < host_count && count < SHOWLINK_MAX_CUES; i++) {
            if (showlink_get_cue(i, nums[count], SHOWLINK_NUM_MAX,
                                 names[count], SHOWLINK_NAME_MAX)) {
                entries[count].num = nums[count];
                entries[count].name = names[count];
                count++;
            }
        }
    } else {
        const char *wn[3] = { link->prev_number, link->standing_by_number, link->next_number };
        const char *wm[3] = { link->prev_name, link->standing_by_name, link->next_name };
        for (int i = 0; i < 3; i++) {
            if (wn[i][0]) {
                entries[count].num = wn[i];
                entries[count].name = wm[i];
                count++;
            }
        }
    }

    if (count == 0) {
        if (s_rendered_sig[0] != '\1') {
            cuelist_show_message("Linked. Waiting for cue data...");
            s_rendered_sig[0] = '\1';
            s_rendered_sig[1] = '\0';
        }
        return;
    }

    /* Signature: revision + first/last numbers + count — cheap change check. */
    char sig[160];
    snprintf(sig, sizeof(sig), "%u|%d|%s|%s", (unsigned)showlink_cuelist_revision(),
             count, entries[0].num, entries[count - 1].num);
    if (strcmp(sig, s_rendered_sig) != 0) {
        strncpy(s_rendered_sig, sig, sizeof(s_rendered_sig) - 1);
        s_rendered_sig[sizeof(s_rendered_sig) - 1] = '\0';
        s_sb_highlighted[0] = '\0';
        lv_obj_clean(s_cuelist);
        for (int i = 0; i < count; i++) {
            make_cue_row(entries[i].num, entries[i].name, i);
        }
        s_rendered_rev = showlink_cuelist_revision();
    }

    /* Standing-by highlight */
    const char *sb = link->standing_by_number;
    if (strcmp(sb, s_sb_highlighted) != 0) {
        strncpy(s_sb_highlighted, sb, sizeof(s_sb_highlighted) - 1);
        s_sb_highlighted[sizeof(s_sb_highlighted) - 1] = '\0';
        uint32_t n = lv_obj_get_child_count(s_cuelist);
        for (uint32_t i = 0; i < n; i++) {
            lv_obj_t *row = lv_obj_get_child(s_cuelist, (int32_t)i);
            const char *num = s_row_numbers[i];
            bool is_sb = sb[0] && strcmp(num, sb) == 0;
            lv_obj_set_style_bg_color(row, is_sb ? COL_SURFACE : COL_BG, 0);
            lv_obj_set_style_border_color(row, is_sb ? COL_AMBER : COL_LINE, 0);
            lv_obj_t *numlbl = lv_obj_get_child(row, 0);
            if (numlbl) lv_obj_set_style_text_color(numlbl, is_sb ? COL_AMBER : COL_MUTED, 0);
            if (is_sb) lv_obj_scroll_to_view(row, LV_ANIM_ON);
        }
    }
}

/* --- link-driven screen state ---------------------------------------------- */

static void apply_offline_state(void)
{
    label_set_if_changed(s_cue_eyebrow, "STANDING BY");
    label_set_if_changed(s_cue_num, "-");
    label_set_if_changed(s_cue_name, "No StageWizard link");
    label_set_if_changed(s_cue_next, "Enable the link on the Setup page");
    label_set_if_changed(s_cue_pos, "");
    lv_obj_set_style_text_color(s_cue_name, COL_MUTED, 0);
    lv_obj_set_style_text_color(s_cue_num, COL_LINE, 0);
    lv_obj_add_flag(s_cue_prog, LV_OBJ_FLAG_HIDDEN);
    set_enabled(s_go_btn, false);
    /* State styles don't propagate to child labels — recolor explicitly. */
    lv_obj_set_style_text_color(s_go_label, COL_LINE, 0);
    set_enabled(s_prev_btn, false);
    set_enabled(s_next_btn, false);
    set_enabled(s_stopall_btn, false);
    set_enabled(s_panic_btn, false);
    cuelist_show_message("No StageWizard link.\nEnable it on the Setup page.");
    s_rendered_sig[0] = '\2';  /* distinct from the linked "waiting" marker */
    s_rendered_sig[1] = '\0';
    label_set_if_changed(s_notes_label, "-");
}

static void apply_live_state(const showlink_state_t *link)
{
    char buf[128];

    label_set_if_changed(s_cue_eyebrow, "STANDING BY");
    label_set_if_changed(s_cue_num,
        link->standing_by_number[0] ? link->standing_by_number : "-");
    if (link->panicking) {
        label_set_if_changed(s_cue_name, "PANIC - all stopped");
        lv_obj_set_style_text_color(s_cue_name, COL_RED, 0);
        lv_obj_set_style_text_color(s_cue_num, COL_RED, 0);
    } else {
        label_set_if_changed(s_cue_name,
            link->standing_by_name[0] ? link->standing_by_name : "(no cue standing by)");
        lv_obj_set_style_text_color(s_cue_name, COL_MUTED, 0);
        lv_obj_set_style_text_color(s_cue_num, COL_TEXT, 0);
    }

    if (link->next_number[0]) {
        snprintf(buf, sizeof(buf), "NEXT  %s - %s", link->next_number, link->next_name);
    } else {
        snprintf(buf, sizeof(buf), "RUNNING  %d", (int)link->running_count);
    }
    label_set_if_changed(s_cue_next, buf);

    if (link->window_total > 0) {
        snprintf(buf, sizeof(buf), "%d/%d " LV_SYMBOL_BULLET " RUN %d",
                 (int)link->window_index, (int)link->window_total, (int)link->running_count);
    } else {
        snprintf(buf, sizeof(buf), "RUN %d", (int)link->running_count);
    }
    label_set_if_changed(s_cue_pos, buf);

    /* Progress bar: only while the host streams elapsed for a finite cue */
    if (link->elapsed_fresh && link->duration_s > 0.0f) {
        int32_t v = (int32_t)(link->elapsed_s / link->duration_s * 1000.0f);
        if (v < 0) v = 0;
        if (v > 1000) v = 1000;
        lv_bar_set_value(s_cue_prog, v, LV_ANIM_OFF);
        lv_obj_remove_flag(s_cue_prog, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_cue_prog, LV_OBJ_FLAG_HIDDEN);
    }

    set_enabled(s_go_btn, true);
    lv_obj_set_style_text_color(s_go_label, lv_color_black(), 0);
    set_enabled(s_prev_btn, true);
    set_enabled(s_next_btn, true);
    set_enabled(s_stopall_btn, true);
    set_enabled(s_panic_btn, true);

    cuelist_update(link);
    label_set_if_changed(s_notes_label, link->notes[0] ? link->notes : "-");
}

/* --- periodic status ------------------------------------------------------- */

static const char *battery_symbol(int pct)
{
    if (pct > 87) return LV_SYMBOL_BATTERY_FULL;
    if (pct > 62) return LV_SYMBOL_BATTERY_3;
    if (pct > 37) return LV_SYMBOL_BATTERY_2;
    if (pct > 12) return LV_SYMBOL_BATTERY_1;
    return LV_SYMBOL_BATTERY_EMPTY;
}

static void status_timer_cb(lv_timer_t *t)
{
    (void)t;
    showui_inputs_t in;
    showui_hal_get_inputs(&in);
    char buf[128];

    lv_label_set_text_fmt(s_time_label, "%02d:%02d:%02d", (int)in.hour, (int)in.minute, (int)in.second);

    if (in.battery_pct < 0) {
        /* Negative = gauge not available (e.g. firmware PMU driver TODO). */
        lv_label_set_text(s_batt_label, LV_SYMBOL_BATTERY_EMPTY " --%");
        lv_obj_set_style_text_color(s_batt_label, COL_MUTED, 0);
    } else {
        lv_label_set_text_fmt(s_batt_label, "%s%s %d%%",
                              in.charging ? LV_SYMBOL_CHARGE " " : "",
                              battery_symbol(in.battery_pct), (int)in.battery_pct);
        lv_obj_set_style_text_color(s_batt_label,
                                    in.battery_pct <= 20 && !in.charging ? COL_RED : COL_MUTED, 0);
    }

    lv_obj_set_style_text_color(s_wifi_label, in.wifi_connected ? COL_TEXT : COL_LINE, 0);

    /* C-library snprintf, NOT lv_label_set_text_fmt: LVGL's built-in printf
     * ships with float support off (LV_SPRINTF_USE_FLOAT 0) and renders %f
     * literally — same default as the device build, so keep floats out of it. */
    char imu_buf[96];
    snprintf(imu_buf, sizeof(imu_buf),
             "ACC  %+.2f  %+.2f  %+.2f g\nGYR  %+.0f  %+.0f  %+.0f dps",
             (double)in.accel_x, (double)in.accel_y, (double)in.accel_z,
             (double)in.gyro_x, (double)in.gyro_y, (double)in.gyro_z);
    label_set_if_changed(s_imu_label, imu_buf);

    /* Physical buttons: BOOT = hardware GO, PWR = display sleep toggle. */
    if (in.button_boot && !s_prev_boot && s_link_live && !s_asleep) showlink_send_go();
    s_prev_boot = in.button_boot;

    if (in.button_pwr && !s_prev_pwr) {
        s_asleep = !s_asleep;
        if (s_asleep) lv_obj_remove_flag(s_sleep_overlay, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_sleep_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    s_prev_pwr = in.button_pwr;

    /* --- StageWizard link: status dot + screens --------------------------- */
    showlink_state_t link;
    showlink_get_state(&link);
    bool live = link.enabled && link.online;

    lv_obj_set_style_bg_color(s_link_dot,
        !link.enabled ? COL_LINE : (live ? COL_GO : COL_RED), 0);

    if (live) {
        apply_live_state(&link);
        s_link_live = true;
    } else if (s_link_live || s_rendered_sig[0] != '\2') {
        s_link_live = false;
        apply_offline_state();
    }

    /* Setup tile: network & OSC health */
    label_set_if_changed(s_net_wifi, in.wifi_connected ? "Wi-Fi   connected" : "Wi-Fi   offline");
    snprintf(buf, sizeof(buf), "Host    %s", link.enabled ? link.host : "--");
    label_set_if_changed(s_net_host, buf);
    if (!link.enabled) {
        snprintf(buf, sizeof(buf), "Link    off");
    } else if (link.transport == SHOWLINK_TRANSPORT_OSC) {
        unsigned age_s = (unsigned)(link.last_status_age_ms / 1000u);
        /* "quiet" = healthy subscription, host just has nothing new to say */
        snprintf(buf, sizeof(buf), "Link    OSC %s " LV_SYMBOL_BULLET " %us",
                 age_s >= 3 ? "quiet" : "feedback", age_s);
    } else if (link.transport == SHOWLINK_TRANSPORT_HTTP) {
        snprintf(buf, sizeof(buf), "Link    HTTP poll " LV_SYMBOL_BULLET " %us",
                 (unsigned)(link.last_status_age_ms / 1000u));
    } else {
        snprintf(buf, sizeof(buf), "Link    searching...");
    }
    label_set_if_changed(s_net_link, buf);
    if (live) {
        snprintf(buf, sizeof(buf), "Show    %s " LV_SYMBOL_BULLET " RUN %d",
                 link.show_mode ? "SHOW MODE" : "EDIT MODE", (int)link.running_count);
    } else {
        snprintf(buf, sizeof(buf), "Show    --");
    }
    label_set_if_changed(s_net_mode, buf);
}

static void link_tick_timer_cb(lv_timer_t *t)
{
    (void)t;
    showlink_tick(lv_tick_get());
}

/* --- tiles ----------------------------------------------------------------- */

static lv_obj_t *make_tile(int idx)
{
    lv_obj_t *tile = lv_tileview_add_tile(s_tv, idx, 0, LV_DIR_HOR);
    lv_obj_set_style_bg_color(tile, COL_BG, 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_top(tile, STATUS_H + 4, 0);
    lv_obj_set_style_pad_bottom(tile, 22, 0);
    lv_obj_set_style_pad_hor(tile, 14, 0);
    return tile;
}

static lv_obj_t *make_pill_button(lv_obj_t *parent, const char *text, lv_color_t color,
                                  lv_event_cb_t cb, int32_t w)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, 40);
    lv_obj_set_style_radius(btn, 20, 0);
    lv_obj_set_style_bg_color(btn, COL_SURFACE, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, color, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_border_color(btn, COL_LINE, LV_STATE_DISABLED);
    lv_obj_set_style_bg_color(btn, COL_BG, LV_STATE_DISABLED);
    lv_obj_t *l = make_label(btn, &lv_font_montserrat_14, color, text);
    lv_obj_center(l);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

static void build_cue_tile(void)
{
    lv_obj_t *t = make_tile(TILE_CUE);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(t, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(t, 2, 0);
    lv_obj_set_scrollbar_mode(t, LV_SCROLLBAR_MODE_OFF);

    s_cue_eyebrow = make_label(t, &lv_font_montserrat_14, COL_AMBER, "STANDING BY");

    s_cue_num = make_label(t, &lv_font_montserrat_48, COL_TEXT, "-");

    s_cue_name = make_label(t, &lv_font_montserrat_16, COL_MUTED, "");
    lv_label_set_long_mode(s_cue_name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(s_cue_name, 320);
    lv_obj_set_style_text_align(s_cue_name, LV_TEXT_ALIGN_CENTER, 0);

    s_cue_next = make_label(t, &lv_font_montserrat_12, COL_MUTED, "");
    lv_label_set_long_mode(s_cue_next, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(s_cue_next, 320);
    lv_obj_set_style_text_align(s_cue_next, LV_TEXT_ALIGN_CENTER, 0);

    /* Playback progress of the running cue (host elapsed/duration feedback) */
    s_cue_prog = lv_bar_create(t);
    lv_obj_set_size(s_cue_prog, 280, 4);
    lv_bar_set_range(s_cue_prog, 0, 1000);
    lv_obj_set_style_bg_color(s_cue_prog, COL_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_cue_prog, COL_GO, LV_PART_INDICATOR);
    lv_obj_set_style_margin_bottom(s_cue_prog, 4, 0);
    lv_obj_add_flag(s_cue_prog, LV_OBJ_FLAG_HIDDEN);

    /* GO — the one big control */
    s_go_btn = lv_button_create(t);
    lv_obj_set_size(s_go_btn, 158, 158);
    lv_obj_set_style_radius(s_go_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_go_btn, COL_GO, 0);
    lv_obj_set_style_bg_color(s_go_btn, lv_color_hex(0x2AA04E), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(s_go_btn, COL_SURFACE, LV_STATE_DISABLED);
    lv_obj_set_style_shadow_width(s_go_btn, 0, 0);
    s_go_label = make_label(s_go_btn, &lv_font_montserrat_28, lv_color_black(), "GO");
    lv_obj_center(s_go_label);
    lv_obj_add_event_cb(s_go_btn, go_clicked_cb, LV_EVENT_CLICKED, NULL);

    /* PREV / position / NEXT row. Height fits the 40px pills INSIDE the
     * padded content area — LVGL clips children to the parent. */
    lv_obj_t *row = lv_obj_create(t);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 320, 60);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(row, 14, 0);

    s_prev_btn = make_pill_button(row, LV_SYMBOL_UP " PREV", COL_MUTED, prev_clicked_cb, 108);
    s_cue_pos = make_label(row, &lv_font_montserrat_14, COL_MUTED, "");
    s_next_btn = make_pill_button(row, "NEXT " LV_SYMBOL_DOWN, COL_MUTED, next_clicked_cb, 108);
}

static void build_cuelist_tile(void)
{
    lv_obj_t *t = make_tile(TILE_CUELIST);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(t, 8, 0);
    lv_obj_set_scrollbar_mode(t, LV_SCROLLBAR_MODE_OFF);

    make_label(t, &lv_font_montserrat_14, COL_AMBER, "GO SEQUENCE");

    /* The scrollable cue list: drag/flick vertically; tap a row to arm it. */
    s_cuelist = lv_obj_create(t);
    lv_obj_remove_style_all(s_cuelist);
    lv_obj_set_width(s_cuelist, LV_PCT(100));
    lv_obj_set_flex_grow(s_cuelist, 1);
    lv_obj_set_flex_flow(s_cuelist, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_cuelist, 8, 0);
    lv_obj_add_flag(s_cuelist, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_cuelist, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_cuelist, LV_SCROLLBAR_MODE_AUTO);

    /* Standing-by cue notes, pinned under the list */
    make_label(t, &lv_font_montserrat_12, COL_AMBER, "NOTES");
    s_notes_label = make_label(t, &lv_font_montserrat_12, COL_MUTED, "-");
    lv_label_set_long_mode(s_notes_label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(s_notes_label, LV_PCT(100));
    lv_obj_set_height(s_notes_label, 34);  /* two lines, clipped */
}

static void fader_changed_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target_obj(e);
    lv_obj_t *value = lv_event_get_user_data(e);
    lv_label_set_text_fmt(value, "%d", (int)lv_slider_get_value(slider));
}

static void build_faders_tile(void)
{
    lv_obj_t *t = make_tile(TILE_FADERS);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(t, 6, 0);

    make_label(t, &lv_font_montserrat_14, COL_AMBER, "SUBMASTERS");

    lv_obj_t *bank = lv_obj_create(t);
    lv_obj_remove_style_all(bank);
    lv_obj_set_size(bank, LV_PCT(100), 284);
    lv_obj_set_flex_flow(bank, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bank, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    for (int i = 0; i < 4; i++) {
        lv_obj_t *col = lv_obj_create(bank);
        lv_obj_remove_style_all(col);
        lv_obj_set_size(col, 70, 284);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(col, 10, 0);

        lv_obj_t *value = make_label(col, &lv_font_montserrat_16, COL_TEXT, "0");

        lv_obj_t *slider = lv_slider_create(col);
        lv_obj_set_size(slider, 30, 176);
        lv_slider_set_range(slider, 0, 100);
        lv_slider_set_value(slider, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(slider, COL_SURFACE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(slider, COL_AMBER, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(slider, COL_TEXT, LV_PART_KNOB);
        lv_obj_set_style_pad_all(slider, 4, LV_PART_KNOB);
        lv_obj_add_event_cb(slider, fader_changed_cb, LV_EVENT_VALUE_CHANGED, value);

        make_label(col, &lv_font_montserrat_14, COL_MUTED, FADER_NAMES[i]);
    }

    /* Transport row: host-wide controls over OSC */
    lv_obj_t *transport = lv_obj_create(t);
    lv_obj_remove_style_all(transport);
    lv_obj_set_size(transport, LV_PCT(100), 46);
    lv_obj_set_flex_flow(transport, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(transport, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_stopall_btn = make_pill_button(transport, "STOP ALL", COL_AMBER, stopall_clicked_cb, 150);
    s_panic_btn = make_pill_button(transport, "PANIC", COL_RED, panic_clicked_cb, 150);
}

static void brightness_changed_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target_obj(e);
    int v = (int)lv_slider_get_value(slider);
    showui_hal_set_brightness((uint8_t)v);
    lv_label_set_text_fmt(s_bri_value, "%d%%", v * 100 / 255);
}

static void build_setup_tile(void)
{
    lv_obj_t *t = make_tile(TILE_SETUP);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(t, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(t, 10, 0);

    make_label(t, &lv_font_montserrat_14, COL_AMBER, "SETUP");

    /* Brightness — routed through the HAL (DCS 0x51 on hardware) */
    lv_obj_t *bri_head = lv_obj_create(t);
    lv_obj_remove_style_all(bri_head);
    lv_obj_set_size(bri_head, LV_PCT(100), 22);
    lv_obj_set_flex_flow(bri_head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bri_head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    make_label(bri_head, &lv_font_montserrat_14, COL_TEXT, "Panel brightness");
    s_bri_value = make_label(bri_head, &lv_font_montserrat_14, COL_MUTED, "100%");

    lv_obj_t *bri = lv_slider_create(t);
    lv_obj_set_size(bri, LV_PCT(100), 16);
    lv_slider_set_range(bri, 10, 255); /* keep a floor so the panel never goes fully dark */
    lv_slider_set_value(bri, 255, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bri, COL_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bri, COL_AMBER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bri, COL_TEXT, LV_PART_KNOB);
    lv_obj_add_event_cb(bri, brightness_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_set_style_margin_bottom(bri, 8, 0);

    /* Network & OSC health */
    make_label(t, &lv_font_montserrat_14, COL_TEXT, "Network");
    s_net_wifi = make_label(t, &lv_font_montserrat_12, COL_MUTED, "Wi-Fi   --");
    s_net_host = make_label(t, &lv_font_montserrat_12, COL_MUTED, "Host    --");
    s_net_link = make_label(t, &lv_font_montserrat_12, COL_MUTED, "Link    off");
    s_net_mode = make_label(t, &lv_font_montserrat_12, COL_MUTED, "Show    --");
    lv_obj_set_style_margin_bottom(s_net_mode, 8, 0);

    make_label(t, &lv_font_montserrat_14, COL_TEXT, "Motion (QMI8658)");
    s_imu_label = make_label(t, &lv_font_montserrat_12, COL_MUTED, "ACC  --\nGYR  --");

    lv_obj_t *spacer = lv_obj_create(t);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_size(spacer, 1, 1);
    lv_obj_set_flex_grow(spacer, 1);

    make_label(t, &lv_font_montserrat_12, COL_MUTED,
               "BOOT button = GO   |   PWR = blackout");
    make_label(t, &lv_font_montserrat_12, COL_LINE,
               "StageWand on Waveshare ESP32-C6 AMOLED 1.8");
}

/* --- public ---------------------------------------------------------------- */

void showui_create(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);

    /* Tileview: swipeable pages */
    s_tv = lv_tileview_create(scr);
    lv_obj_set_size(s_tv, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_tv, COL_BG, 0);
    lv_obj_set_scrollbar_mode(s_tv, LV_SCROLLBAR_MODE_OFF);
    /* VALUE_CHANGED (not SCROLL_END): fires only when the tileview actually
     * commits a new active tile, so the dots can never read a stale tile. */
    lv_obj_add_event_cb(s_tv, tv_scroll_end_cb, LV_EVENT_VALUE_CHANGED, NULL);

    build_cuelist_tile();
    build_cue_tile();
    build_faders_tile();
    build_setup_tile();

    /* Status bar on the top layer: always visible, never scrolls */
    lv_obj_t *bar = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(100), STATUS_H);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, COL_BG, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(bar, 12, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);

    s_time_label = make_label(bar, &lv_font_montserrat_14, COL_TEXT, "--:--:--");
    lv_obj_align(s_time_label, LV_ALIGN_LEFT_MID, 0, 0);

    s_batt_label = make_label(bar, &lv_font_montserrat_14, COL_MUTED, LV_SYMBOL_BATTERY_3 " --%");
    lv_obj_align(s_batt_label, LV_ALIGN_RIGHT_MID, 0, 0);

    s_wifi_label = make_label(bar, &lv_font_montserrat_14, COL_TEXT, LV_SYMBOL_WIFI);
    lv_obj_align(s_wifi_label, LV_ALIGN_RIGHT_MID, -78, 0);

    /* StageWizard link dot: gray = link off, red = searching, green = online */
    s_link_dot = lv_obj_create(bar);
    lv_obj_remove_style_all(s_link_dot);
    lv_obj_set_size(s_link_dot, 8, 8);
    lv_obj_set_style_radius(s_link_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_link_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_link_dot, COL_LINE, 0);
    lv_obj_align(s_link_dot, LV_ALIGN_RIGHT_MID, -106, 0);

    /* Page dots on the top layer, bottom center */
    lv_obj_t *dots = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(dots);
    lv_obj_set_size(dots, 120, 14);
    lv_obj_align(dots, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(dots, 10, 0);
    lv_obj_remove_flag(dots, LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < TILE_COUNT; i++) {
        s_dots[i] = lv_obj_create(dots);
        lv_obj_remove_style_all(s_dots[i]);
        lv_obj_set_size(s_dots[i], 6, 6);
        lv_obj_set_style_radius(s_dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(s_dots[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s_dots[i], i == TILE_CUE ? COL_AMBER : COL_LINE, 0);
    }

    /* Sleep overlay (PWR button): full black, swallows touches */
    s_sleep_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_sleep_overlay);
    lv_obj_set_size(s_sleep_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_sleep_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_sleep_overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_sleep_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_sleep_overlay, LV_OBJ_FLAG_HIDDEN);

    /* Start on the GO tile */
    lv_tileview_set_tile_by_index(s_tv, TILE_CUE, 0, LV_ANIM_OFF);

    apply_offline_state();

    showlink_init();
    lv_timer_create(status_timer_cb, 500, NULL);
    lv_timer_create(link_tick_timer_cb, 100, NULL);
}

void showui_goto_tile(int index)
{
    if (!s_tv || index < 0 || index >= TILE_COUNT) return;
    lv_tileview_set_tile_by_index(s_tv, (uint32_t)index, 0, LV_ANIM_OFF);
    tv_scroll_end_cb(NULL);  /* refresh the page dots (cb ignores its arg) */
}
