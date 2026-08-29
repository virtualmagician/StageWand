/**
 * showui.h — the device-portable show-controller UI.
 *
 * This target compiles unchanged on the Mac simulator and in the ESP-IDF
 * firmware. It depends only on LVGL ("lvgl.h") and showui_hal.h.
 * Call showui_create() once after LVGL and the display are up.
 */
#ifndef SHOWUI_H
#define SHOWUI_H

#ifdef __cplusplus
extern "C" {
#endif

void showui_create(void);

#ifdef __cplusplus
}
#endif

#endif /* SHOWUI_H */
