#include "ui/dark_mode.h"

#include "lvgl.h"

#include <cstdint>

namespace ui::dark_mode
{
namespace
{

bool s_enabled = false;
lv_color_filter_dsc_t s_filter;
bool s_filter_ready = false;

uint8_t clamp_u8(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return static_cast<uint8_t>(v);
}

// Lightness-complement shift: move every channel by (255 - (max + min)). This flips a
// colour between light and dark while keeping its hue, so cream backgrounds become dark
// warm, dark-brown text becomes light, and the amber accent stays amber but a touch
// darker. It is integer-only and runs per style colour, not per pixel, so the cost is
// negligible. Greys invert correctly because max == min there.
lv_color_t invert_lightness(const lv_color_filter_dsc_t*, lv_color_t c, lv_opa_t)
{
    const int r = c.red;
    const int g = c.green;
    const int b = c.blue;
    int mx = r > g ? r : g;
    if (b > mx) mx = b;
    int mn = r < g ? r : g;
    if (b < mn) mn = b;
    const int delta = 255 - (mx + mn);
    return lv_color_make(clamp_u8(r + delta), clamp_u8(g + delta), clamp_u8(b + delta));
}

void ensure_filter()
{
    if (!s_filter_ready)
    {
        lv_color_filter_dsc_init(&s_filter, invert_lightness);
        s_filter_ready = true;
    }
}

} // namespace

bool default_enabled()
{
#if defined(TRAIL_MATE_SENTINEL)
    return true;
#else
    return false;
#endif
}

bool enabled()
{
    return s_enabled;
}

void apply(bool on)
{
    s_enabled = on;
    ensure_filter();

    lv_obj_t* screen = lv_screen_active();
    if (screen == nullptr)
    {
        return;
    }

    if (on)
    {
        lv_obj_set_style_color_filter_dsc(screen, &s_filter, LV_PART_MAIN);
        lv_obj_set_style_color_filter_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    }
    else
    {
        // Leave the (inheritable) descriptor in place but switch the filter off; a zero
        // opacity makes LVGL skip it everywhere.
        lv_obj_set_style_color_filter_opa(screen, LV_OPA_TRANSP, LV_PART_MAIN);
    }
    lv_obj_invalidate(screen);
}

} // namespace ui::dark_mode
