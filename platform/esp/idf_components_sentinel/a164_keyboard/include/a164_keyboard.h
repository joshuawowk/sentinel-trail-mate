/*
 * M5Stack Tab5 A164 keyboard (SKU A164) driver for ESP-IDF.
 *
 * Ported from the user's M5MonsterC5-Tab5 project
 * (components/a164_keyboard) into Trail Mate as a Sentinel-only component.
 * Compared with the original it is trimmed down for Trail Mate's UI: it no
 * longer owns a private LVGL group, a routing shim or a navigation timer.
 * Trail Mate already builds and populates menu/app/page keypad groups and
 * steers every KEYPAD indev onto the active one (see set_default_group), so
 * this driver just registers a KEYPAD indev on the default group and lets the
 * shared machinery deliver keys to the focused widget.
 *
 * The A164 is a 70-key keyboard expansion for the Tab5 with an STM32F030 MCU
 * that exposes an I2C slave (addr 0x6D) on the Tab5's internal ExtPort1/J9:
 *   SDA = GPIO0 (J9-7)   SCL = GPIO1 (J9-8)   INT = GPIO50 (J9-10, unused)
 * The driver creates its OWN i2c_master bus with i2c_port = -1 (auto-select a
 * free controller), so it does not conflict with the board's other I2C users.
 * It is completely independent of the USB-A host port.
 *
 * Gated entirely on TRAIL_MATE_SENTINEL by the build system; only the Tab5
 * Sentinel image compiles and links this component.
 */
#pragma once

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Bring up the A164 keyboard: create its private I2C bus, an LVGL KEYPAD
     *        indev bound to the current default group, and a polling task. Safe to
     *        call once, after the display/LVGL are started. If the keyboard is not
     *        attached yet, the task keeps re-probing so it works when docked later
     *        (hot-plug).
     *
     * @param disp  the LVGL display the keypad indev is associated with.
     * @return ESP_OK on success (even if the keyboard is not currently attached).
     */
    esp_err_t a164_keyboard_init(lv_display_t* disp);

    /** @return true if the A164 answered on I2C at least once (i.e. is docked). */
    bool a164_kbd_present(void);

#ifdef __cplusplus
}
#endif
