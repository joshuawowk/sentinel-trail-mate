#pragma once

// The M5Stack Tab5 Keyboard (SKU A164) as Trail Mate input, Sentinel build only.
//
// The a164_keyboard component reads HID events off the keyboard's I2C bus. This adapter
// turns them into LVGL keys on a KEYPAD input device and delivers them the way Trail Mate's
// other physical keyboards are delivered (see the T-Display-P4 keyboard): typing goes to the
// focused text field, Esc steps back (a page's own Esc handling first, else out of the
// app), Enter activates, Tab/Shift-Tab move the focus, and the arrows move it on screen
// unless the page handles them itself. A key wakes the screen saver like a touch does;
// Space or Enter resumes it.

#include <cstddef>
#include <cstdint>

#include "lvgl.h"

namespace platform::esp::idf_common::sentinel_keyboard
{

enum class Route : uint8_t
{
    Smart, // route as described above (default)
    Group, // only through the LVGL group of the active page (diagnostics)
};

// Start the keyboard driver and create the KEYPAD input device on `disp`. Call once the
// display and LVGL are up. Safe with no keyboard attached: docking it later just works.
void start(lv_display_t* disp);

// True while a keyboard is docked and answering.
bool present();

// The keypad input device (nullptr before start()).
lv_indev_t* indev();

Route route();
void set_route(Route route);

// The HID usage id and modifier that type `c` on the A164 (console "kbd type").
bool ascii_to_hid(char c, uint8_t* keycode, uint8_t* modifier);

// What keys would reach right now, for the console: one line per call of `emit`.
// Takes the LVGL lock itself.
void describe_focus(void (*emit)(const char* line, void* ctx), void* ctx);

} // namespace platform::esp::idf_common::sentinel_keyboard
