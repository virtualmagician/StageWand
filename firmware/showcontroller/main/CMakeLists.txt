# The device-portable ShowUI UI lives in the Simulator package and is referenced
# here by relative path -- it is NOT copied into firmware/. The macOS simulator
# (Simulator/Sources/SimCore) and this firmware compile the exact same
# showui.c/showui.h/showui_hal.h.
set(SHOWUI_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../Simulator/Sources/SimCore/showui")

idf_component_register(
    SRCS
        "main.c"
        "showui_hal_device.c"
        "${SHOWUI_DIR}/showui.c"
    INCLUDE_DIRS
        "."
        "${SHOWUI_DIR}"
    REQUIRES
        esp_driver_i2c
        esp_driver_gpio
        pcf85063a
        qmi8658
)
