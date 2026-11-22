#include "platform/esp/idf_common/sentinel_keyboard.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "a164_keyboard.h"
#include "bsp/m5stack_tab5.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "platform/ui/screen_runtime.h"
#include "src/misc/lv_event_private.h"
#include "ui/app_runtime.h"
#include "ui/menu/menu_runtime.h"

extern "C" void trail_mate_idf_note_user_activity(void);

namespace platform::esp::idf_common::sentinel_keyboard
{
namespace
{

constexpr const char* kTag = "sentinel-kbd";
constexpr int64_t kRepeatDelayUs = 450000; // a held key repeats after this ...
constexpr int64_t kRepeatEveryUs = 60000;  // ... this often

// HID usage ids
enum : uint8_t
{
    kHidEnter = 0x28,
    kHidEsc = 0x29,
    kHidBackspace = 0x2A,
    kHidTab = 0x2B,
    kHidSpace = 0x2C,
    kHidHome = 0x4A,
    kHidDelete = 0x4C,
    kHidEnd = 0x4D,
    kHidRight = 0x4F,
    kHidLeft = 0x50,
    kHidDown = 0x51,
    kHidUp = 0x52,
    kHidKeypadEnter = 0x58,
};

// How a pressed key is being delivered.
enum class Held : uint8_t
{
    None,
    Lvgl,     // reported PRESSED to LVGL, whose group machinery delivers (and repeats) it
    Proxy,    // sent to a widget directly; this adapter repeats it while held
    Consumed, // fully handled at the press (Esc, screen wake); nothing more until release
};

lv_indev_t* s_indev = nullptr;
volatile Route s_route = Route::Smart;

Held s_held = Held::None;
uint32_t s_held_key = 0;
bool s_held_wakes = false; // the press woke the screen: its release goes to the saver too
lv_obj_t* s_proxy_target = nullptr;
int64_t s_next_repeat_us = 0;
bool s_pending_valid = false;
a164_kbd_event_t s_pending{};

// ---------------------------------------------------------------------------------------
// Keys

char hid_to_ascii(uint8_t code, bool shift)
{
    static const char kPlain[] = "abcdefghijklmnopqrstuvwxyz1234567890";
    static const char kShifted[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()";
    static const char kSym[] = " -=[]\\\\;'`,./"; // 0x2C .. 0x38
    static const char kSymShifted[] = " _+{}||:\"~<>?";
    if (code >= 0x04 && code <= 0x27)
    {
        return shift ? kShifted[code - 0x04] : kPlain[code - 0x04];
    }
    if (code >= kHidSpace && code <= 0x38)
    {
        return shift ? kSymShifted[code - kHidSpace] : kSym[code - kHidSpace];
    }
    return 0;
}

uint32_t hid_to_lv_key(const a164_kbd_event_t& ev)
{
    const bool shift = (ev.modifier & A164_MOD_SHIFT) != 0;
    switch (ev.keycode)
    {
    case kHidEnter:
    case kHidKeypadEnter:
        return LV_KEY_ENTER;
    case kHidEsc:
        return LV_KEY_ESC;
    case kHidBackspace:
        return LV_KEY_BACKSPACE;
    case kHidDelete:
        return LV_KEY_DEL;
    case kHidTab:
        return shift ? LV_KEY_PREV : LV_KEY_NEXT;
    case kHidRight:
        return LV_KEY_RIGHT;
    case kHidLeft:
        return LV_KEY_LEFT;
    case kHidDown:
        return LV_KEY_DOWN;
    case kHidUp:
        return LV_KEY_UP;
    case kHidHome:
        return LV_KEY_HOME;
    case kHidEnd:
        return LV_KEY_END;
    default:
        break;
    }
    // Ctrl/Alt chords are not text: do not type the letter.
    if ((ev.modifier & (A164_MOD_CTRL | A164_MOD_ALT)) != 0)
    {
        return 0;
    }
    return static_cast<uint8_t>(hid_to_ascii(ev.keycode, shift));
}

bool is_printable(uint32_t key)
{
    return key >= 0x20 && key <= 0x7E;
}

bool is_arrow(uint32_t key)
{
    return key == LV_KEY_UP || key == LV_KEY_DOWN || key == LV_KEY_LEFT || key == LV_KEY_RIGHT;
}

bool is_text_key(uint32_t key)
{
    return is_printable(key) || is_arrow(key) || key == LV_KEY_BACKSPACE || key == LV_KEY_DEL ||
           key == LV_KEY_ENTER || key == LV_KEY_HOME || key == LV_KEY_END;
}

bool repeats(uint32_t key)
{
    return is_printable(key) || is_arrow(key) || key == LV_KEY_BACKSPACE || key == LV_KEY_DEL;
}

// ---------------------------------------------------------------------------------------
// Where keys go (modelled on the T-Display-P4 keyboard's routing)

bool visible(lv_obj_t* obj)
{
    for (lv_obj_t* o = obj; o != nullptr; o = lv_obj_get_parent(o))
    {
        if (!lv_obj_is_valid(o) || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN))
        {
            return false;
        }
    }
    return true;
}

lv_obj_t* root_of(lv_obj_t* obj)
{
    lv_obj_t* root = obj;
    while (root != nullptr && lv_obj_get_parent(root) != nullptr)
    {
        root = lv_obj_get_parent(root);
    }
    return root;
}

bool on_screen(lv_obj_t* obj)
{
    if (obj == nullptr || !lv_obj_is_valid(obj) || !visible(obj))
    {
        return false;
    }
    lv_obj_t* root = root_of(obj);
    return root == lv_screen_active() || root == lv_layer_top();
}

lv_obj_t* find_focused_textarea(lv_obj_t* root)
{
    if (root == nullptr || !lv_obj_is_valid(root) || lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN))
    {
        return nullptr;
    }
    if (lv_obj_check_type(root, &lv_textarea_class) && lv_obj_has_state(root, LV_STATE_FOCUSED))
    {
        return root;
    }
    const uint32_t count = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < count; ++i)
    {
        if (lv_obj_t* ta = find_focused_textarea(lv_obj_get_child(root, i)))
        {
            return ta;
        }
    }
    return nullptr;
}

// Overlays (dialogs, editors, pickers) live on the top layer and sit in front of the page.
lv_obj_t* focused_textarea()
{
    if (lv_obj_t* ta = find_focused_textarea(lv_layer_top()))
    {
        return ta;
    }
    return find_focused_textarea(lv_screen_active());
}

void count_textareas(lv_obj_t* root, lv_obj_t** only, uint32_t* count)
{
    if (root == nullptr || !lv_obj_is_valid(root) || lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN) || *count > 1)
    {
        return;
    }
    if (lv_obj_check_type(root, &lv_textarea_class) && !lv_obj_has_state(root, LV_STATE_DISABLED))
    {
        *only = root;
        ++*count;
        return;
    }
    const uint32_t n = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < n; ++i)
    {
        count_textareas(lv_obj_get_child(root, i), only, count);
    }
}

// Is anything showing on the top layer (a dialog, editor or picker in front of the page)?
// Hidden children do not count: some stay there for the app's lifetime.
bool overlay_open()
{
    lv_obj_t* top = lv_layer_top();
    const uint32_t n = lv_obj_get_child_count(top);
    for (uint32_t i = 0; i < n; ++i)
    {
        if (!lv_obj_has_flag(lv_obj_get_child(top, i), LV_OBJ_FLAG_HIDDEN))
        {
            return true;
        }
    }
    return false;
}

// The one text field in front, if there is exactly one.
lv_obj_t* only_textarea()
{
    lv_obj_t* only = nullptr;
    uint32_t count = 0;
    count_textareas(overlay_open() ? lv_layer_top() : lv_screen_active(), &only, &count);
    return count == 1 ? only : nullptr;
}

bool has_key_handler(lv_obj_t* obj)
{
    if (obj == nullptr || !lv_obj_is_valid(obj))
    {
        return false;
    }
    const uint32_t n = lv_obj_get_event_count(obj);
    for (uint32_t i = 0; i < n; ++i)
    {
        const uint32_t filter = lv_obj_get_event_dsc(obj, i)->filter & ~static_cast<uint32_t>(LV_EVENT_PREPROCESS);
        if (filter == LV_EVENT_KEY)
        {
            return true;
        }
    }
    return false;
}

// Pages that take keys themselves install a KEY handler on their root container.
lv_obj_t* find_page_key_target(lv_obj_t* root)
{
    if (root == nullptr || !lv_obj_is_valid(root) || lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN))
    {
        return nullptr;
    }
    if (lv_obj_check_type(root, &lv_obj_class) && lv_obj_get_group(root) == nullptr && has_key_handler(root))
    {
        return root;
    }
    for (uint32_t i = lv_obj_get_child_count(root); i > 0; --i)
    {
        if (lv_obj_t* target = find_page_key_target(lv_obj_get_child(root, i - 1)))
        {
            return target;
        }
    }
    return nullptr;
}

// The group whose focused control is on screen: the one Trail Mate last pointed the keypads
// at (a page's or dialog's group), then the default, then the app or menu group.
lv_group_t* active_key_group()
{
    lv_group_t* candidates[] = {s_indev != nullptr ? lv_indev_get_group(s_indev) : nullptr, lv_group_get_default(),
                                ui_get_active_app() != nullptr ? app_g : menu_g};
    for (lv_group_t* group : candidates)
    {
        if (group != nullptr && on_screen(lv_group_get_focused(group)))
        {
            return group;
        }
    }
    return nullptr;
}

lv_obj_t* focused_in(lv_group_t* group)
{
    return group != nullptr ? lv_group_get_focused(group) : nullptr;
}

// Arrow keys on a page that does not handle them: the nearest control of the group in
// that direction, preferring ones in line (as Sentinel's launcher does).
bool move_focus(lv_group_t* group, uint32_t key)
{
    lv_obj_t* from = focused_in(group);
    if (from == nullptr)
    {
        return false;
    }
    lv_area_t a;
    lv_obj_get_coords(from, &a);
    lv_obj_t* best = nullptr;
    int64_t best_d = INT64_MAX;
    const uint32_t n = lv_group_get_obj_count(group);
    for (uint32_t i = 0; i < n; ++i)
    {
        lv_obj_t* c = lv_group_get_obj_by_index(group, i);
        if (c == from || !on_screen(c) || lv_obj_has_state(c, LV_STATE_DISABLED))
        {
            continue;
        }
        lv_area_t b;
        lv_obj_get_coords(c, &b);
        const int64_t dx = static_cast<int64_t>(b.x1) + b.x2 - a.x1 - a.x2;
        const int64_t dy = static_cast<int64_t>(b.y1) + b.y2 - a.y1 - a.y2;
        const int64_t fwd = key == LV_KEY_RIGHT ? dx : key == LV_KEY_LEFT ? -dx
                                                   : key == LV_KEY_DOWN   ? dy
                                                                          : -dy;
        const int64_t cross = (key == LV_KEY_LEFT || key == LV_KEY_RIGHT) ? dy : dx;
        if (fwd <= 0)
        {
            continue;
        }
        const int64_t d = fwd * fwd + 4 * cross * cross;
        if (d < best_d)
        {
            best_d = d;
            best = c;
        }
    }
    if (best == nullptr)
    {
        return false;
    }
    lv_group_focus_obj(best);
    lv_obj_scroll_to_view_recursive(best, LV_ANIM_ON);
    return true;
}

void send_key(lv_obj_t* target, uint32_t key)
{
    uint32_t parameter = key;
    (void)lv_obj_send_event(target, LV_EVENT_KEY, &parameter);
}

// Deliver a newly pressed key; returns how it is held from now on.
Held dispatch(uint32_t key)
{
    lv_group_t* group = active_key_group();
    if (group != nullptr)
    {
        lv_indev_set_group(s_indev, group);
    }
    if (s_route == Route::Group)
    {
        return Held::Lvgl;
    }

    lv_obj_t* focus = focused_in(group);
    // A dialog, picker or page that installed its own group owns the keys while it is up (as
    // on the T-Display-P4): the group hands them to its focused control, a text field
    // included, so nothing reaches a field or page behind it.
    const bool own_group = group != nullptr && group != app_g && group != menu_g;

    // The menu's own physical-key shortcuts, as on Trail Mate's other keyboards; not while a
    // dialog over the menu has the keys.
    if (!own_group && ui_get_active_app() == nullptr && is_printable(key))
    {
        const char c = static_cast<char>(key);
        if (::ui::menu_runtime::handleWalkieKey(c, 1) || ::ui::menu_runtime::handleShortcutKey(c, 1))
        {
            return Held::Consumed;
        }
    }

    lv_obj_t* page = find_page_key_target(lv_screen_active());
    lv_obj_t* ta = own_group ? nullptr : focused_textarea();

    // Esc steps back: a dialog's or page's own handling first, then out of the text field,
    // then out of the app.
    if (key == LV_KEY_ESC)
    {
        if (has_key_handler(focus))
        {
            send_key(focus, key);
            return Held::Consumed;
        }
        if (page != nullptr)
        {
            send_key(page, key);
            return Held::Consumed;
        }
        if (ta != nullptr)
        {
            (void)lv_obj_send_event(ta, LV_EVENT_CANCEL, nullptr);
            if (lv_obj_is_valid(ta))
            {
                lv_obj_remove_state(ta, LV_STATE_FOCUSED);
            }
            return Held::Consumed;
        }
        if (ui_get_active_app() != nullptr)
        {
            ui_request_exit_to_menu();
            return Held::Consumed;
        }
        return group != nullptr ? Held::Lvgl : Held::Consumed;
    }
    if (own_group)
    {
        return Held::Lvgl;
    }

    // Typing goes to the focused text field; with none focused, to the only one in front.
    if (ta == nullptr && is_printable(key) && ui_get_active_app() != nullptr)
    {
        ta = only_textarea();
        if (ta != nullptr)
        {
            if (lv_obj_get_group(ta) != nullptr)
            {
                lv_group_focus_obj(ta);
            }
            lv_obj_add_state(ta, LV_STATE_FOCUSED);
            (void)lv_obj_send_event(ta, LV_EVENT_FOCUSED, nullptr);
        }
    }
    if (ta != nullptr && is_text_key(key))
    {
        s_proxy_target = ta;
        send_key(ta, key);
        return Held::Proxy;
    }

    if (group != nullptr)
    {
        // Activation and Tab/Shift-Tab belong to the group; so does any key the focused
        // control handles itself.
        if (key == LV_KEY_ENTER || key == LV_KEY_NEXT || key == LV_KEY_PREV || has_key_handler(focus))
        {
            return Held::Lvgl;
        }
        if (page == nullptr)
        {
            if (is_arrow(key))
            {
                (void)move_focus(group, key);
                return Held::Consumed;
            }
            return Held::Lvgl;
        }
    }
    if (page != nullptr)
    {
        s_proxy_target = page;
        send_key(page, key);
        return Held::Proxy;
    }
    return Held::Consumed;
}

// ---------------------------------------------------------------------------------------
// The input device

void release_held(lv_indev_data_t* data)
{
    if (s_held == Held::Lvgl)
    {
        data->key = s_held_key;
        data->state = LV_INDEV_STATE_RELEASED;
    }
    if (s_held_wakes)
    {
        ::platform::ui::screen::handle_input_release();
    }
    s_held = Held::None;
    s_held_key = 0;
    s_held_wakes = false;
    s_proxy_target = nullptr;
}

void keep_held(lv_indev_data_t* data)
{
    if (s_held == Held::Lvgl)
    {
        data->key = s_held_key;
        data->state = LV_INDEV_STATE_PRESSED;
        return;
    }
    if (s_held == Held::Proxy && repeats(s_held_key) && esp_timer_get_time() >= s_next_repeat_us)
    {
        s_next_repeat_us += kRepeatEveryUs;
        if (s_proxy_target != nullptr && on_screen(s_proxy_target))
        {
            send_key(s_proxy_target, s_held_key);
        }
    }
}

void read_cb(lv_indev_t*, lv_indev_data_t* data)
{
    data->continue_reading = false;
    data->state = LV_INDEV_STATE_RELEASED;
    data->key = s_held_key;

    a164_kbd_event_t ev{};
    if (s_pending_valid)
    {
        ev = s_pending;
        s_pending_valid = false;
    }
    else if (!a164_kbd_get(&ev))
    {
        keep_held(data);
        return;
    }
    data->continue_reading = true; // there may be more queued; the next read says

    if (ev.keycode == 0) // the key that was down is released
    {
        if (s_held != Held::None)
        {
            release_held(data);
        }
        return;
    }
    const uint32_t key = hid_to_lv_key(ev);
    if (key == 0)
    {
        return; // not a key Trail Mate uses (its release finds nothing held)
    }
    if (s_held != Held::None)
    {
        // A second key went down before the first came up: release that one on this
        // read, take the new one on the next. LVGL's keypad is a single-key device.
        s_pending = ev;
        s_pending_valid = true;
        release_held(data);
        return;
    }

    s_held_key = key;
    s_next_repeat_us = esp_timer_get_time() + kRepeatDelayUs;
    if (::platform::ui::screen::is_sleeping() || ::platform::ui::screen::is_saver_active())
    {
        // Like a touch: any key wakes the preview; Space or Enter resumes the page.
        if (key == ' ' || key == LV_KEY_ENTER)
        {
            ::platform::ui::screen::request_resume();
        }
        else
        {
            ::platform::ui::screen::handle_input();
            s_held_wakes = true;
        }
        s_held = Held::Consumed;
        return;
    }

    trail_mate_idf_note_user_activity();
    lv_display_trigger_activity(nullptr);
    s_held = dispatch(key);
    if (s_held == Held::Lvgl)
    {
        data->key = key;
        data->state = LV_INDEV_STATE_PRESSED;
    }
}

const char* describe_obj(lv_obj_t* obj, char* buf, std::size_t len)
{
    if (obj == nullptr)
    {
        return "none";
    }
    if (!lv_obj_is_valid(obj))
    {
        return "invalid";
    }
    const char* type = lv_obj_check_type(obj, &lv_textarea_class) ? "textarea"
                       : lv_obj_check_type(obj, &lv_button_class) ? "button"
                       : lv_obj_check_type(obj, &lv_label_class)  ? "label"
                                                                  : "obj";
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    std::snprintf(buf, len, "%s %p at %ld,%ld %ldx%ld%s%s", type, static_cast<void*>(obj), static_cast<long>(a.x1),
                  static_cast<long>(a.y1), static_cast<long>(lv_area_get_width(&a)),
                  static_cast<long>(lv_area_get_height(&a)), has_key_handler(obj) ? " key-handler" : "",
                  on_screen(obj) ? "" : " (not on screen)");
    return buf;
}

const char* group_name(lv_group_t* group)
{
    if (group == nullptr) return "none";
    if (group == app_g) return "app_g";
    if (group == menu_g) return "menu_g";
    return "page";
}

} // namespace

void start(lv_display_t* disp)
{
    if (s_indev != nullptr)
    {
        return;
    }
    if (a164_kbd_start() != ESP_OK)
    {
        ESP_LOGE(kTag, "keyboard driver did not start; no physical keyboard input");
        return;
    }
    bsp_display_lock(0);
    s_indev = lv_indev_create();
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(s_indev, read_cb);
    if (disp != nullptr)
    {
        lv_indev_set_display(s_indev, disp);
    }
    // Trail Mate points every keypad at the active page's group as pages change
    // (set_default_group); start on the current one.
    lv_indev_set_group(s_indev, lv_group_get_default());
    bsp_display_unlock();
    ESP_LOGI(kTag, "Tab5 Keyboard input ready");
}

bool present()
{
    return s_indev != nullptr && a164_kbd_present();
}

lv_indev_t* indev()
{
    return s_indev;
}

Route route()
{
    return s_route;
}

void set_route(Route route)
{
    s_route = route;
}

bool ascii_to_hid(char c, uint8_t* keycode, uint8_t* modifier)
{
    if (keycode == nullptr || modifier == nullptr)
    {
        return false;
    }
    for (uint8_t code = 0x04; code <= 0x38; ++code)
    {
        for (int shift = 0; shift < 2; ++shift)
        {
            if (code > 0x27 && code < kHidSpace)
            {
                continue; // Enter, Esc, Backspace, Tab: not characters
            }
            if (hid_to_ascii(code, shift != 0) == c)
            {
                *keycode = code;
                *modifier = shift != 0 ? 0x02 : 0x00;
                return true;
            }
        }
    }
    return false;
}

void describe_focus(void (*emit)(const char* line, void* ctx), void* ctx)
{
    if (emit == nullptr)
    {
        return;
    }
    // Console-task scratch (only the console calls this): kept off the task stack.
    static char line[200];
    static char obj[120];
    if (!bsp_display_lock(1000))
    {
        emit("display busy", ctx);
        return;
    }
    AppScreen* app = ui_get_active_app();
    lv_group_t* active = active_key_group();
    const char* screen = ::platform::ui::screen::is_sleeping()       ? "asleep"
                         : ::platform::ui::screen::is_saver_active() ? "saver"
                                                                     : "awake";
    std::snprintf(line, sizeof(line), "screen %s  app %s  indev group %s  default group %s  active group %s", screen,
                  app != nullptr ? app->name() : "(menu)", group_name(s_indev ? lv_indev_get_group(s_indev) : nullptr),
                  group_name(lv_group_get_default()), group_name(active));
    emit(line, ctx);
    std::snprintf(line, sizeof(line), "focused %s", describe_obj(focused_in(active), obj, sizeof(obj)));
    emit(line, ctx);
    lv_obj_t* ta = focused_textarea();
    if (ta != nullptr)
    {
        const char* text = lv_textarea_get_text(ta);
        std::snprintf(line, sizeof(line), "textarea %s text \"%.60s\"", describe_obj(ta, obj, sizeof(obj)),
                      text != nullptr ? text : "");
    }
    else
    {
        std::snprintf(line, sizeof(line), "textarea none (only one in front: %s)",
                      describe_obj(only_textarea(), obj, sizeof(obj)));
    }
    emit(line, ctx);
    std::snprintf(line, sizeof(line), "page key target %s  overlay %s",
                  describe_obj(find_page_key_target(lv_screen_active()), obj, sizeof(obj)),
                  overlay_open() ? "open" : "none");
    emit(line, ctx);
    bsp_display_unlock();
}

} // namespace platform::esp::idf_common::sentinel_keyboard
