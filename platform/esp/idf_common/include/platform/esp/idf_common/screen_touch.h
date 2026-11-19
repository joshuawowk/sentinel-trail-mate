#pragma once

#include "lvgl.h"

// Screen-power handling of a touch panel without a keyboard (the Tab5).
//
// While the screen sleeps or shows the screen saver, a touch belongs to the screen-power
// layer, not to the page underneath: it wakes the screen into the saver preview, and a
// swipe up resumes the page that was active before sleep (ResumeMethod::SwipeUp). Without
// this the saver asked for a SPACE key the Tab5 does not have, and a tap on a dark screen
// reached the controls under it.
namespace platform::esp::idf_common::screen_touch
{

// Apply the screen-power rules to one pointer sample (raw panel coordinates, as a read
// callback produces them). Returns true when the sample was consumed; its state is then
// LV_INDEV_STATE_RELEASED.
bool filter(lv_indev_t* indev, lv_indev_data_t* data);

// Wrap the pointer device's read callback with filter(). Call once, with the LVGL lock held.
void install(lv_indev_t* indev);

} // namespace platform::esp::idf_common::screen_touch
