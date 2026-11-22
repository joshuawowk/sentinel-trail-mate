/*
 * M5Stack Tab5 Keyboard (SKU A164) driver for ESP-IDF: the raw I2C side only.
 *
 * The A164 is a 70-key keyboard with an STM32F030 that answers on its own I2C bus on the
 * Tab5's internal expansion port J9: SDA = GPIO0, SCL = GPIO1 (INT on GPIO50 is not used;
 * the driver polls). Register map from the keyboard's firmware (M5Tab5-Keyboard-Internal-FW,
 * user_i2c_reg.h); bring-up, polling and hand-back follow Sentinel Bootloader's driver
 * (firmware/components/board/keyboard.c), which is proven on this hardware.
 *
 * This component knows nothing about LVGL: it queues HID events {modifier, keycode}, with
 * keycode 0 meaning "the key that was down is released". Trail Mate's Sentinel keyboard
 * adapter (platform/esp/idf_common/src/sentinel_keyboard.cpp) turns them into LVGL keys.
 *
 * Only the Sentinel build adds this component directory, so the standalone Tab5 and
 * T-Display-P4 builds never compile or link it.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /** HID modifier bits as the A164 sends them (left | right). */
#define A164_MOD_CTRL 0x11
#define A164_MOD_SHIFT 0x22
#define A164_MOD_ALT 0x44

    typedef struct
    {
        uint8_t modifier;
        uint8_t keycode; /* HID usage id; 0 = released */
    } a164_kbd_event_t;

    typedef struct
    {
        bool present;
        int i2c_port;       /* controller the bus runs on, -1 before start */
        uint8_t version;    /* keyboard firmware version (register 0xFE) */
        uint8_t mode_found; /* the keyboard's mode when last found: 0 = as powered up */
        uint32_t connects;  /* times the keyboard was found */
        uint32_t events;    /* events read from the keyboard */
        uint32_t injected;  /* events queued by a164_kbd_inject() */
        uint32_t dropped;   /* events lost because the queue was full */
    } a164_kbd_status_t;

    /**
     * Create the keyboard's I2C bus and start the poll task. Safe when no keyboard is
     * attached: the task probes every half second, so docking it later just works.
     * Idempotent.
     */
    esp_err_t a164_kbd_start(void);

    /** True while the keyboard answers and is in HID mode. */
    bool a164_kbd_present(void);

    /** Take the next HID event, if any. Non-blocking. */
    bool a164_kbd_get(a164_kbd_event_t* ev);

    /** Queue a key press and its release as if read from the keyboard (console testing). */
    bool a164_kbd_inject(uint8_t keycode, uint8_t modifier);

    /** Queue one event as if read from the keyboard: keycode 0 is a release. */
    bool a164_kbd_inject_edge(uint8_t keycode, uint8_t modifier);

    void a164_kbd_get_status(a164_kbd_status_t* out);

    /** The last events read or injected, newest first, with their age. Returns the count. */
    int a164_kbd_history(a164_kbd_event_t* out, uint32_t* age_ms, int max);

#ifdef __cplusplus
}
#endif
