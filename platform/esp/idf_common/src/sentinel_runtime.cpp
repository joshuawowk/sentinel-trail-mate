#include "platform/esp/idf_common/sentinel_runtime.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "bsp/m5stack_tab5.h"
#include "esp_app_desc.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/usb_serial_jtag_ll.h"
#include "lvgl.h"
#include "sentinel_app.h"

// Where Sentinel Bootloader looks for updates of this firmware: the newest release of this
// repository that carries the asset, when its tag is newer than PROJECT_VER.
SENTINEL_APP_INFO("https://github.com/joshuawowk/sentinel-trail-mate", "trail-mate-tab5-sentinel.bin");

namespace platform::esp::idf_common::sentinel
{
namespace
{

constexpr const char* kTag = "sentinel";
constexpr const char* kAppName = "Trail Mate";
constexpr uint32_t kConsolePollMs = 20;
constexpr uint32_t kConsoleStackBytes = 6144;
constexpr std::size_t kLineMax = 160;
// One "@data" line carries 768 bytes: 1024 base64 characters.
constexpr std::size_t kBlobChunk = 768;

TaskHandle_t s_console_task = nullptr;

// Console task scratch. Only the console task uses these (no large automatic locals on the
// task stack; see AGENTS.md "ESP Stack Hygiene").
char s_line[kLineMax];
char s_out[kBlobChunk / 3 * 4 + 16];

// ---------------------------------------------------------------------------------------
// Replies. Every reply line is written with one fwrite on stdout so log lines from other
// tasks (which lock the same FILE) cannot split it.

void write_line(const char* prefix, const char* fmt, va_list args)
{
    int n = std::snprintf(s_out, sizeof(s_out), "%s", prefix);
    if (n < 0)
    {
        return;
    }
    int m = std::vsnprintf(s_out + n, sizeof(s_out) - static_cast<std::size_t>(n) - 1, fmt, args);
    if (m < 0)
    {
        m = 0;
    }
    std::size_t len = static_cast<std::size_t>(n) + static_cast<std::size_t>(m);
    if (len > sizeof(s_out) - 2)
    {
        len = sizeof(s_out) - 2;
    }
    s_out[len++] = '\n';
    std::fwrite(s_out, 1, len, stdout);
    std::fflush(stdout);
}

void reply(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    write_line("@ ", fmt, args);
    va_end(args);
}

void reply_ok(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    write_line("@ok ", fmt, args);
    va_end(args);
}

void reply_err(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    write_line("@err ", fmt, args);
    va_end(args);
}

void send_blob(const uint8_t* data, std::size_t len)
{
    static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (std::size_t off = 0; off < len; off += kBlobChunk)
    {
        const std::size_t chunk = (len - off) < kBlobChunk ? (len - off) : kBlobChunk;
        std::size_t o = 0;
        std::memcpy(s_out, "@data ", 6);
        o = 6;
        for (std::size_t i = 0; i < chunk; i += 3)
        {
            const uint8_t* p = data + off + i;
            const std::size_t rem = chunk - i;
            const uint32_t v = (static_cast<uint32_t>(p[0]) << 16) |
                               (rem > 1 ? static_cast<uint32_t>(p[1]) << 8 : 0U) |
                               (rem > 2 ? static_cast<uint32_t>(p[2]) : 0U);
            s_out[o++] = kB64[(v >> 18) & 0x3F];
            s_out[o++] = kB64[(v >> 12) & 0x3F];
            s_out[o++] = rem > 1 ? kB64[(v >> 6) & 0x3F] : '=';
            s_out[o++] = rem > 2 ? kB64[v & 0x3F] : '=';
        }
        s_out[o++] = '\n';
        std::fwrite(s_out, 1, o, stdout);
    }
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------------------
// shot: the frame buffer the panel scans, run-length encoded as Sentinel's console does:
// (count - 1) as u16 LE, then the RGB565 pixel as u16 LE.

std::size_t rle16(const uint16_t* px, std::size_t n, uint8_t* out, std::size_t cap)
{
    std::size_t o = 0;
    std::size_t i = 0;
    while (i < n)
    {
        const uint16_t c = px[i];
        std::size_t run = 1;
        while (i + run < n && px[i + run] == c && run < 65536)
        {
            ++run;
        }
        if (o + 4 > cap)
        {
            return 0;
        }
        out[o++] = static_cast<uint8_t>(run - 1);
        out[o++] = static_cast<uint8_t>((run - 1) >> 8);
        out[o++] = static_cast<uint8_t>(c);
        out[o++] = static_cast<uint8_t>(c >> 8);
        i += run;
    }
    return o;
}

void cmd_shot(int argc, char** argv)
{
    if (argc > 1 && std::strcmp(argv[1], "fb") != 0)
    {
        reply_err("only the frame buffer is available (shot)");
        return;
    }
#if CONFIG_BSP_LCD_COLOR_FORMAT_RGB888
    reply_err("shot supports RGB565 panels only");
#else
    esp_lcd_panel_handle_t panel = bsp_display_get_panel_handle();
    lv_display_t* disp = lv_display_get_default();
    void* fb = nullptr;
    if (panel == nullptr || disp == nullptr ||
        esp_lcd_dpi_panel_get_frame_buffer(panel, 1, &fb) != ESP_OK || fb == nullptr)
    {
        reply_err("no display");
        return;
    }
    const int w = BSP_LCD_H_RES;
    const int h = BSP_LCD_V_RES;
    const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    std::size_t cap = n * 4;
    auto* enc = static_cast<uint8_t*>(heap_caps_malloc(cap, MALLOC_CAP_SPIRAM));
    if (enc == nullptr)
    {
        cap = n * 2;
        enc = static_cast<uint8_t*>(heap_caps_malloc(cap, MALLOC_CAP_SPIRAM));
    }
    if (enc == nullptr)
    {
        reply_err("no memory");
        return;
    }
    // Hold the LVGL lock so no flush starts while the buffer is read, and drop stale cache
    // lines: the DSI driver's DMA writes the frame buffer behind the CPU's cache.
    if (!bsp_display_lock(2000))
    {
        heap_caps_free(enc);
        reply_err("display busy");
        return;
    }
    (void)esp_cache_msync(fb, n * 2, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    const std::size_t len = rle16(static_cast<const uint16_t*>(fb), n, enc, cap);
    const int rot = static_cast<int>(lv_display_get_rotation(disp));
    bsp_display_unlock();
    if (len == 0)
    {
        heap_caps_free(enc);
        reply_err("image too noisy to encode");
        return;
    }
    reply("image fb %d %d rle16 %u %d", w, h, static_cast<unsigned>(len), rot);
    send_blob(enc, len);
    heap_caps_free(enc);
    reply_ok("shot");
#endif
}

// ---------------------------------------------------------------------------------------
// tap: a touch injected through the panel's LVGL pointer device, in logical (rotated)
// coordinates as sentinelctl/sappctl use them.

struct InjectedTouch
{
    volatile bool active = false;
    int32_t native_x = 0;
    int32_t native_y = 0;
    int64_t release_at_us = 0;
};

InjectedTouch s_inject;
lv_indev_read_cb_t s_touch_read = nullptr;

void injecting_read_cb(lv_indev_t* indev, lv_indev_data_t* data)
{
    if (s_inject.active)
    {
        data->point.x = s_inject.native_x;
        data->point.y = s_inject.native_y;
        if (esp_timer_get_time() < s_inject.release_at_us)
        {
            data->state = LV_INDEV_STATE_PRESSED;
        }
        else
        {
            data->state = LV_INDEV_STATE_RELEASED;
            s_inject.active = false;
        }
        return;
    }
    if (s_touch_read != nullptr)
    {
        s_touch_read(indev, data);
    }
}

// Inverse of lv_display_rotate_point(): LVGL rotates raw pointer points itself.
void logical_to_native(lv_display_t* disp, int32_t lx, int32_t ly, int32_t* nx, int32_t* ny)
{
    const int32_t hor = lv_display_get_physical_horizontal_resolution(disp);
    const int32_t ver = lv_display_get_physical_vertical_resolution(disp);
    switch (lv_display_get_rotation(disp))
    {
    case LV_DISPLAY_ROTATION_90:
        *nx = ly;
        *ny = ver - 1 - lx;
        break;
    case LV_DISPLAY_ROTATION_180:
        *nx = hor - 1 - lx;
        *ny = ver - 1 - ly;
        break;
    case LV_DISPLAY_ROTATION_270:
        *nx = hor - 1 - ly;
        *ny = lx;
        break;
    default:
        *nx = lx;
        *ny = ly;
        break;
    }
}

void cmd_tap(int argc, char** argv)
{
    if (argc < 3)
    {
        reply_err("usage: tap <x> <y> [ms]");
        return;
    }
    lv_display_t* disp = lv_display_get_default();
    lv_indev_t* indev = bsp_display_get_input_dev();
    if (disp == nullptr || indev == nullptr)
    {
        reply_err("no display");
        return;
    }
    const int32_t lx = std::atoi(argv[1]);
    const int32_t ly = std::atoi(argv[2]);
    int ms = argc > 3 ? std::atoi(argv[3]) : 80;
    if (ms < 20)
    {
        ms = 20;
    }
    if (!bsp_display_lock(2000))
    {
        reply_err("display busy");
        return;
    }
    if (lx < 0 || ly < 0 || lx >= lv_display_get_horizontal_resolution(disp) ||
        ly >= lv_display_get_vertical_resolution(disp))
    {
        bsp_display_unlock();
        reply_err("point outside the screen");
        return;
    }
    if (lv_indev_get_read_cb(indev) != injecting_read_cb)
    {
        s_touch_read = lv_indev_get_read_cb(indev);
        lv_indev_set_read_cb(indev, injecting_read_cb);
    }
    logical_to_native(disp, lx, ly, &s_inject.native_x, &s_inject.native_y);
    s_inject.release_at_us = esp_timer_get_time() + static_cast<int64_t>(ms) * 1000;
    s_inject.active = true;
    bsp_display_unlock();
    for (int waited = 0; s_inject.active && waited < ms + 1000; waited += 10)
    {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    reply_ok("tap");
}

// ---------------------------------------------------------------------------------------

void cmd_info(int, char**)
{
    const esp_app_desc_t* d = esp_app_get_description();
    reply("app %s project %s version %s idf %s", kAppName, d->project_name, d->version, d->idf_ver);
    reply("under Sentinel: yes");
    reply("uptime_s %lld", static_cast<long long>(esp_timer_get_time() / 1000000));
    reply("heap internal %u psram %u",
          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    reply_ok("info");
}

void cmd_help(int, char**)
{
    reply("info               app, version and memory");
    reply("launcher           return to Sentinel (also: sentinel)");
    reply("restart            restart Trail Mate");
    reply("shot               screenshot of the panel (sappctl.py shot out.png)");
    reply("tap <x> <y> [ms]   inject a touch at a logical point");
    reply_ok("help");
}

void handle_line(char* line)
{
    char* argv[6] = {};
    int argc = 0;
    for (char* tok = std::strtok(line, " \t"); tok != nullptr && argc < 6; tok = std::strtok(nullptr, " \t"))
    {
        argv[argc++] = tok;
    }
    if (argc == 0)
    {
        return;
    }
    const char* cmd = argv[0];
    if (std::strcmp(cmd, "info") == 0)
    {
        cmd_info(argc, argv);
    }
    else if (std::strcmp(cmd, "launcher") == 0 || std::strcmp(cmd, "sentinel") == 0)
    {
        reply_ok("returning to Sentinel");
        vTaskDelay(pdMS_TO_TICKS(50));
        return_to_sentinel();
    }
    else if (std::strcmp(cmd, "restart") == 0)
    {
        reply_ok("restarting");
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_restart();
    }
    else if (std::strcmp(cmd, "shot") == 0)
    {
        cmd_shot(argc, argv);
    }
    else if (std::strcmp(cmd, "tap") == 0)
    {
        cmd_tap(argc, argv);
    }
    else if (std::strcmp(cmd, "help") == 0)
    {
        cmd_help(argc, argv);
    }
    else
    {
        reply_err("unknown command '%s' (try help)", cmd);
    }
}

// Reads USB-Serial-JTAG's receive FIFO directly. Nothing else in the Tab5 build reads it
// (logs only write), so no driver is installed and the console VFS keeps working as is.
void console_task(void*)
{
    std::size_t len = 0;
    uint8_t buf[64];
    for (;;)
    {
        int n = 0;
        if (usb_serial_jtag_ll_rxfifo_data_available())
        {
            n = usb_serial_jtag_ll_read_rxfifo(buf, sizeof(buf));
        }
        if (n <= 0)
        {
            vTaskDelay(pdMS_TO_TICKS(kConsolePollMs));
            continue;
        }
        for (int i = 0; i < n; ++i)
        {
            const char c = static_cast<char>(buf[i]);
            if (c == '\n' || c == '\r')
            {
                if (len > 0)
                {
                    s_line[len] = '\0';
                    handle_line(s_line);
                    len = 0;
                }
            }
            else if (len < kLineMax - 1)
            {
                s_line[len++] = c;
            }
        }
    }
}

} // namespace

bool enabled()
{
#if defined(TRAIL_MATE_SENTINEL)
    return true;
#else
    return false;
#endif
}

void return_to_sentinel()
{
    ESP_LOGW(kTag, "returning to Sentinel");
    (void)bsp_display_backlight_off();
    // A blank otadata makes Sentinel's bootloader start its launcher.
    const esp_partition_t* otadata =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
    if (otadata != nullptr)
    {
        const esp_err_t err = esp_partition_erase_range(otadata, 0, otadata->size);
        if (err != ESP_OK)
        {
            ESP_LOGE(kTag, "otadata erase failed: %s", esp_err_to_name(err));
        }
    }
    else
    {
        ESP_LOGE(kTag, "no otadata partition; restarting Trail Mate instead");
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_restart();
}

void on_board_initialized()
{
    if (!enabled())
    {
        return;
    }
    bsp_set_charge_en(true);
    ESP_LOGI(kTag, "battery charging enabled");
}

void start_console()
{
    if (!enabled() || s_console_task != nullptr)
    {
        return;
    }
    if (xTaskCreate(console_task, "sentinel_con", kConsoleStackBytes, nullptr, tskIDLE_PRIORITY + 2,
                    &s_console_task) != pdPASS)
    {
        s_console_task = nullptr;
        ESP_LOGE(kTag, "console task not started");
        return;
    }
    ESP_LOGI(kTag, "Sentinel console on USB-Serial-JTAG (try: help)");
}

} // namespace platform::esp::idf_common::sentinel
