#include "ui/app_registry.h"

#include "app/app_config.h"
#include "app/app_facade_access.h"
#include "chat/infra/mesh_protocol_utils.h"
#include "platform/ui/device_runtime.h"
#include "platform/ui/lora_runtime.h"
#include "platform/ui/pack_repository_runtime.h"
#include "platform/ui/route_storage.h"
#include "platform/ui/sstv_runtime.h"
#include "platform/ui/tracker_runtime.h"
#include "platform/ui/usb_support_runtime.h"
#include "platform/ui/walkie_runtime.h"
#include "platform/ui/wireless_companion_runtime.h"
#include "ui/app_catalog.h"
#include "ui/app_catalog_builder.h"
#include "ui/app_screen.h"
#include "ui/callback_app_screen.h"
#include "ui/localization.h"
#include "ui/ui_theme.h"

#if defined(TRAIL_MATE_SENTINEL)
#include "platform/esp/idf_common/sentinel_runtime.h"
#include "ui/app_runtime.h"
#include "ui/assets/fonts/font_utils.h"
#include "ui/page/page_profile.h"
#endif

#include <cstdio>

namespace ui::assets
{
extern "C" const lv_image_dsc_t Setting;
#if defined(TRAIL_MATE_SENTINEL)
extern "C" const lv_image_dsc_t sentinel_home;
#endif
}

namespace
{

#define APP_REG_LOG(...) std::printf("[UI][Registry] " __VA_ARGS__)

using ui::assets::Setting;

struct CompanionPageState
{
    lv_obj_t* root = nullptr;
};

CompanionPageState s_companion_page_state;

void add_label(lv_obj_t* parent,
               const char* text,
               const lv_font_t* font,
               lv_color_t color)
{
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text ? text : "");
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
}

void add_status_line(lv_obj_t* parent, const char* label, const char* value)
{
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s: %s", label ? label : "", value ? value : "");
    add_label(parent, buf, &lv_font_montserrat_14, ui::theme::text());
}

void add_u32_line(lv_obj_t* parent, const char* label, unsigned long value)
{
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s: %lu", label ? label : "", value);
    add_label(parent, buf, &lv_font_montserrat_14, ui::theme::text());
}

void add_hex_line(lv_obj_t* parent, const char* label, uint32_t value)
{
    char buf[48];
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(value));
    add_status_line(parent, label, buf);
}

void companion_enter(void* user_data, lv_obj_t* parent)
{
    auto* state = static_cast<CompanionPageState*>(user_data);
    if (!state || !parent || (state->root && lv_obj_is_valid(state->root)))
    {
        return;
    }

    state->root = lv_obj_create(parent);
    lv_obj_set_size(state->root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(state->root, ui::theme::white(), 0);
    lv_obj_set_style_bg_opa(state->root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(state->root, 0, 0);
    lv_obj_set_style_radius(state->root, 0, 0);
    lv_obj_set_style_pad_left(state->root, 18, 0);
    lv_obj_set_style_pad_right(state->root, 18, 0);
    lv_obj_set_style_pad_top(state->root, 18, 0);
    lv_obj_set_style_pad_bottom(state->root, 18, 0);
    lv_obj_set_flex_flow(state->root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(state->root, 8, 0);

    add_label(state->root, ::ui::i18n::tr("C6 Companion"), &lv_font_montserrat_14, ui::theme::text());

    const auto st = platform::ui::wireless_companion::status();
    add_status_line(state->root, "State", platform::ui::wireless_companion::state_name(st.state));
    add_status_line(state->root, "Message", st.message);
    add_status_line(state->root, "Detail", st.detail);
    add_status_line(state->root, "Board capable", st.board_capable ? "yes" : "no");
    add_status_line(state->root, "Started", st.started ? "yes" : "no");
    add_status_line(state->root, "Present", st.present ? "yes" : "no");
    add_u32_line(state->root, "Protocol min", st.protocol_min);
    add_u32_line(state->root, "Protocol max", st.protocol_max);
    add_u32_line(state->root, "Selected protocol", st.selected_protocol);

    add_hex_line(state->root, "Supported features", st.supported_features);
    add_hex_line(state->root, "Enabled features", st.enabled_features);
    add_u32_line(state->root, "Config seq", st.config_seq);
    add_u32_line(state->root, "Config error", st.config_error);
    add_u32_line(state->root, "Selected MTU", st.selected_mtu);
    add_status_line(state->root,
                    "BLE",
                    platform::ui::wireless_companion::service_state_name(st.ble_state));
    add_status_line(state->root,
                    "ESP-NOW",
                    platform::ui::wireless_companion::service_state_name(st.espnow_state));
    add_status_line(state->root,
                    "Wi-Fi",
                    platform::ui::wireless_companion::service_state_name(st.wifi_state));

    add_u32_line(state->root, "BLE uplink", st.ble_uplink_count);
    add_u32_line(state->root, "BLE events", st.ble_event_count);
    add_u32_line(state->root, "ESP-NOW uplink", st.espnow_uplink_count);
    add_u32_line(state->root, "ESP-NOW events", st.espnow_event_count);
    add_u32_line(state->root, "Wi-Fi events", st.wifi_event_count);
    add_u32_line(state->root, "Firmware", st.firmware_version);
    add_u32_line(state->root, "Free heap", st.free_heap);
}

void companion_exit(void* user_data, lv_obj_t* parent)
{
    (void)parent;
    auto* state = static_cast<CompanionPageState*>(user_data);
    if (!state || !state->root || !lv_obj_is_valid(state->root))
    {
        if (state)
        {
            state->root = nullptr;
        }
        return;
    }
    lv_obj_del(state->root);
    state->root = nullptr;
}

ui::CallbackAppScreen s_companion_app("c6_companion",
                                      "C6 Companion",
                                      &Setting,
                                      companion_enter,
                                      companion_exit,
                                      &s_companion_page_state);

#if defined(TRAIL_MATE_SENTINEL)
// "Return to Sentinel": a house tile at the end of the main menu that asks first, then
// hands the Tab5 back to the Sentinel Bootloader launcher. Esc or Cancel closes the dialog.
constexpr uint32_t kSentinelPanelBg = 0xFFF6E2;
constexpr uint32_t kSentinelAccent = 0xF0701E;
constexpr uint32_t kSentinelAccentDark = 0xC4550F;
constexpr uint32_t kSentinelLine = 0xE7C98F;
constexpr uint32_t kSentinelText = 0x2B2E5E;
constexpr uint32_t kSentinelTextDim = 0x5A5D86;

lv_obj_t* s_sentinel_modal = nullptr;
lv_group_t* s_sentinel_group = nullptr;
lv_group_t* s_sentinel_prev_group = nullptr;

void sentinel_close_modal()
{
    if (s_sentinel_modal != nullptr)
    {
        lv_obj_del(s_sentinel_modal);
        s_sentinel_modal = nullptr;
    }
    set_default_group(s_sentinel_prev_group != nullptr ? s_sentinel_prev_group : menu_g);
    s_sentinel_prev_group = nullptr;
    ui_set_overlay_active(false);
}

void sentinel_confirm_cb(lv_event_t* e)
{
    (void)e;
    APP_REG_LOG("return to Sentinel confirmed\n");
    platform::esp::idf_common::sentinel::return_to_sentinel();
}

void sentinel_cancel_cb(lv_event_t* e)
{
    (void)e;
    sentinel_close_modal();
}

void sentinel_key_cb(lv_event_t* e)
{
    if (lv_event_get_key(e) == LV_KEY_ESC)
    {
        sentinel_close_modal();
    }
}

void sentinel_set_label(lv_obj_t* label, const char* text, uint32_t color, const lv_font_t* font)
{
    const char* localized = ::ui::i18n::tr(text);
    lv_label_set_text(label, localized);
    lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
    ::ui::fonts::apply_localized_font(label, localized, font);
}

lv_obj_t* sentinel_button(lv_obj_t* parent, const lv_image_dsc_t* image, const char* text, lv_event_cb_t cb,
                          bool primary)
{
    lv_obj_t* btn = lv_btn_create(parent);
    lv_obj_set_height(btn, LV_MAX(ui::page_profile::resolve_control_button_height(), 52));
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(primary ? kSentinelAccent : kSentinelPanelBg), LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, lv_color_hex(primary ? kSentinelAccentDark : kSentinelLine), LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 10, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(btn, 3, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_color(btn, lv_color_hex(kSentinelText), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_bg_color(btn, lv_color_hex(primary ? kSentinelAccentDark : kSentinelLine), LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(btn, sentinel_key_cb, LV_EVENT_KEY, nullptr);

    lv_obj_t* row = lv_obj_create(btn);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(row);
    const uint32_t color = primary ? 0xFFFFFF : kSentinelText;
    if (image != nullptr)
    {
        // The tile's house icon, scaled to the button (64 px source, 40 px shown).
        lv_obj_t* icon = lv_image_create(row);
        lv_image_set_src(icon, image);
        lv_image_set_scale(icon, 160);
        lv_obj_set_size(icon, 40, 40);
        lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CENTER);
    }
    lv_obj_t* label = lv_label_create(row);
    sentinel_set_label(label, text, color, ui::page_profile::resolve_body_font());

    if (s_sentinel_group != nullptr)
    {
        lv_group_add_obj(s_sentinel_group, btn);
    }
    return btn;
}

void sentinel_enter(void*, lv_obj_t* parent)
{
    if (parent == nullptr || s_sentinel_modal != nullptr)
    {
        return;
    }

    s_sentinel_prev_group = lv_group_get_default();
    if (s_sentinel_group == nullptr)
    {
        s_sentinel_group = lv_group_create();
    }
    lv_group_remove_all_objs(s_sentinel_group);
    set_default_group(s_sentinel_group);
    ui_set_overlay_active(true);

    s_sentinel_modal = lv_obj_create(parent);
    lv_obj_set_size(s_sentinel_modal, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_sentinel_modal, 0, 0);
    lv_obj_set_style_bg_color(s_sentinel_modal, lv_color_hex(kSentinelText), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_sentinel_modal, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_sentinel_modal, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_sentinel_modal, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_sentinel_modal, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_sentinel_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_sentinel_modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(s_sentinel_modal);

    const auto size = ui::page_profile::resolve_modal_size(560, 300, parent);
    lv_obj_t* dialog = lv_obj_create(s_sentinel_modal);
    lv_obj_set_size(dialog, size.width, size.height);
    lv_obj_center(dialog);
    lv_obj_set_style_bg_color(dialog, lv_color_hex(kSentinelPanelBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dialog, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(dialog, 3, LV_PART_MAIN);
    lv_obj_set_style_border_color(dialog, lv_color_hex(kSentinelAccent), LV_PART_MAIN);
    lv_obj_set_style_radius(dialog, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_all(dialog, 22, LV_PART_MAIN);
    lv_obj_set_style_pad_row(dialog, 14, LV_PART_MAIN);
    lv_obj_set_flex_flow(dialog, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(dialog, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(dialog, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* head = lv_obj_create(dialog);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, 16, LV_PART_MAIN);
    lv_obj_t* house = lv_image_create(head);
    lv_image_set_src(house, &ui::assets::sentinel_home);
    lv_obj_t* title = lv_label_create(head);
    sentinel_set_label(title, "Return to Sentinel", kSentinelText, ui::page_profile::resolve_title_font());

    lv_obj_t* body = lv_label_create(dialog);
    lv_obj_set_width(body, LV_PCT(100));
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    sentinel_set_label(body,
                       "Close Trail Mate and go back to the Sentinel launcher? "
                       "Messages and settings are already saved.",
                       kSentinelTextDim,
                       ui::page_profile::resolve_body_font());
    lv_obj_set_flex_grow(body, 1);

    lv_obj_t* row = lv_obj_create(dialog);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 16, LV_PART_MAIN);

    lv_obj_t* cancel_btn = sentinel_button(row, nullptr, "Cancel", sentinel_cancel_cb, false);
    (void)sentinel_button(row, &ui::assets::sentinel_home, "Return to Sentinel", sentinel_confirm_cb, true);
    lv_group_focus_obj(cancel_btn);
}

void sentinel_exit(void*, lv_obj_t* parent)
{
    (void)parent;
}

ui::CallbackAppScreen s_sentinel_app("sentinel_return",
                                     "Return to Sentinel",
                                     &ui::assets::sentinel_home,
                                     sentinel_enter,
                                     sentinel_exit,
                                     nullptr,
                                     ui::AppLaunchMode::MenuOverlay);
#endif

struct IdfCatalogState
{
    ui::AppCatalog base{};
    AppScreen* companion = nullptr;
    AppScreen* sentinel = nullptr;
};

std::size_t idf_catalog_count(void* user_data)
{
    auto* state = static_cast<IdfCatalogState*>(user_data);
    if (state == nullptr)
    {
        return 0;
    }
    return ui::catalogCount(state->base) + (state->companion != nullptr ? 1U : 0U) +
           (state->sentinel != nullptr ? 1U : 0U);
}

AppScreen* idf_catalog_at(void* user_data, std::size_t index)
{
    auto* state = static_cast<IdfCatalogState*>(user_data);
    if (state == nullptr)
    {
        return nullptr;
    }

    const std::size_t base_count = ui::catalogCount(state->base);
    if (index < base_count)
    {
        return ui::catalogAt(state->base, index);
    }
    std::size_t next = base_count;
    if (state->companion != nullptr)
    {
        if (index == next)
        {
            return state->companion;
        }
        ++next;
    }
    if (state->sentinel != nullptr && index == next)
    {
        return state->sentinel;
    }
    return nullptr;
}

ui::app_catalog_builder::FeatureFlags buildFeatureFlags()
{
    ui::app_catalog_builder::FeatureFlags flags{};
    flags.profile = ui::app_catalog_builder::CatalogProfile::IdfDefault;
    flags.include_gps_map = platform::ui::device::gps_supported();
    flags.include_gnss_skyplot = platform::ui::device::gps_supported();
    flags.include_tracker = platform::ui::route_storage::is_supported() ||
                            platform::ui::tracker::is_supported();
    flags.include_energy_sweep = platform::ui::lora::is_supported();
    flags.include_sstv = platform::ui::sstv::is_supported();
    flags.include_usb = platform::ui::usb_support::is_supported() &&
                        platform::ui::device::sd_ready();
    flags.include_extensions = ui::runtime::packs::is_supported();
    flags.include_network = app::hasAppFacade() &&
                            chat::infra::isReticulumMeshProtocol(
                                app::configFacade().readConfig().mesh_protocol);
    flags.include_walkie_talkie = platform::ui::walkie::is_supported();
#if defined(TRAIL_MATE_ESP_BOARD_T_DISPLAY_P4)
    flags.include_power_off = true;
#endif
    APP_REG_LOG(
        "flags profile=idf gps_map=%d skyplot=%d tracker=%d chat=%d sweep=%d sstv=%d usb=%d extensions=%d network=%d walkie=%d power_off=%d gps_supported=%d gps_ready=%d sd_ready=%d\n",
        flags.include_gps_map ? 1 : 0,
        flags.include_gnss_skyplot ? 1 : 0,
        flags.include_tracker ? 1 : 0,
        flags.include_chat ? 1 : 0,
        flags.include_energy_sweep ? 1 : 0,
        flags.include_sstv ? 1 : 0,
        flags.include_usb ? 1 : 0,
        flags.include_extensions ? 1 : 0,
        flags.include_network ? 1 : 0,
        flags.include_walkie_talkie ? 1 : 0,
        flags.include_power_off ? 1 : 0,
        platform::ui::device::gps_supported() ? 1 : 0,
        platform::ui::device::gps_ready() ? 1 : 0,
        platform::ui::device::sd_ready() ? 1 : 0);
    return flags;
}

IdfCatalogState s_catalog_state;
ui::AppCatalog s_catalog{&s_catalog_state, idf_catalog_count, idf_catalog_at};

ui::AppCatalog buildCatalog()
{
    s_catalog_state.base = ui::app_catalog_builder::build(buildFeatureFlags());
    s_catalog_state.companion =
        platform::ui::wireless_companion::is_supported() ? &s_companion_app : nullptr;
#if defined(TRAIL_MATE_SENTINEL)
    s_catalog_state.sentinel = &s_sentinel_app;
#endif
    return s_catalog;
}

} // namespace

namespace ui
{

AppCatalog appCatalog()
{
    return buildCatalog();
}

} // namespace ui
