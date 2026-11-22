/**
 * @file dark_mode.h
 * @brief Runtime dark mode for the LVGL UI (GitHub issue #10).
 *
 * Trail Mate paints its screens with hundreds of hard-coded warm "paper" colours
 * spread across several independent colour systems (ui::theme, two_pane_styles and
 * per-screen literals), so there is no single palette to swap. Instead this module
 * attaches an inheritable LVGL colour filter to the one active screen. LVGL marks
 * COLOR_FILTER_DSC/OPA as inheritable style properties, so every descendant resolves
 * the filter at draw time and has its solid colours (backgrounds, text, borders,
 * lines) lightness-inverted, while raw image bitmaps -- the colourful app icons and
 * map tiles -- are left untouched. Foreground/background contrast is preserved
 * because every colour is transformed the same way.
 */
#pragma once

namespace ui::dark_mode
{

/// Compile-time default: dark out of the box on the Sentinel Tab5 image, light elsewhere.
bool default_enabled();

/// Whether dark mode is currently applied.
bool enabled();

/**
 * @brief Apply @p on now: attach or detach the lightness-invert colour filter on the
 *        active screen and invalidate it. Safe to call before any page is built (the
 *        filter is inherited by content created later) and cheap enough to toggle live.
 *        Must be called with the LVGL lock held.
 */
void apply(bool on);

} // namespace ui::dark_mode
