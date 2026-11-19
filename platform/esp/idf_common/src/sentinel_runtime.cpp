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
#include "platform/esp/idf_common/screen_touch.h"
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
char s_out[kBlobChunk / 3 * 4 + 24];

// ---------------------------------------------------------------------------------------
// Replies. Every reply line is written with one fwrite on stdout so log lines from other
// tasks (which lock the same FILE) cannot split it, and starts with a newline: the logger
// may write one log line in several pieces, and a reply landing between them must still
// begin a line of its own for the host to recognise it.

void write_line(const char* prefix, const char* fmt, va_list args)
{
    int n = std::snprintf(s_out, sizeof(s_out), "\n%s", prefix);
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
        std::memcpy(s_out, "\n@data ", 7);
        o = 7;
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
    // Native panel coordinates at the start and end of the stroke (equal for a tap).
    int32_t x0 = 0;
    int32_t y0 = 0;
    int32_t x1 = 0;
    int32_t y1 = 0;
    int64_t start_us = 0;
    int64_t move_us = 0; // 0 for a tap
    int64_t release_at_us = 0;
    uint32_t pressed_reads = 0;
};

// A press must reach LVGL before its release. The console waits for the display lock while
// LVGL renders (a full 1280x720 frame takes tens of ms), so a short tap could otherwise be
// read only once, already released, and click nothing.
constexpr uint32_t kMinPressedReads = 3;

InjectedTouch s_inject;
lv_indev_read_cb_t s_touch_read = nullptr;

void injecting_read_cb(lv_indev_t* indev, lv_indev_data_t* data)
{
    if (s_inject.active)
    {
        const int64_t now = esp_timer_get_time();
        const int64_t t = now - s_inject.start_us;
        if (s_inject.move_us > 0 && t < s_inject.move_us)
        {
            data->point.x = s_inject.x0 + static_cast<int32_t>((s_inject.x1 - s_inject.x0) * t / s_inject.move_us);
            data->point.y = s_inject.y0 + static_cast<int32_t>((s_inject.y1 - s_inject.y0) * t / s_inject.move_us);
        }
        else
        {
            data->point.x = s_inject.x1;
            data->point.y = s_inject.y1;
        }
        if (now < s_inject.release_at_us || s_inject.pressed_reads < kMinPressedReads)
        {
            data->state = LV_INDEV_STATE_PRESSED;
            ++s_inject.pressed_reads;
        }
        else
        {
            data->state = LV_INDEV_STATE_RELEASED;
            s_inject.active = false;
        }
        // A tap on a sleeping screen wakes it, as a finger does, instead of reaching the UI.
        (void)screen_touch::filter(indev, data);
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

bool on_screen(lv_display_t* disp, int32_t x, int32_t y)
{
    return x >= 0 && y >= 0 && x < lv_display_get_horizontal_resolution(disp) &&
           y < lv_display_get_vertical_resolution(disp);
}

// A stroke from (x0,y0) to (x1,y1) in logical coordinates: moving for move_ms, then held
// for hold_ms. A tap is a stroke that does not move.
void inject_stroke(const char* what, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int move_ms, int hold_ms)
{
    lv_display_t* disp = lv_display_get_default();
    lv_indev_t* indev = bsp_display_get_input_dev();
    if (disp == nullptr || indev == nullptr)
    {
        reply_err("no display");
        return;
    }
    if (!bsp_display_lock(2000))
    {
        reply_err("display busy");
        return;
    }
    if (!on_screen(disp, x0, y0) || !on_screen(disp, x1, y1))
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
    logical_to_native(disp, x0, y0, &s_inject.x0, &s_inject.y0);
    logical_to_native(disp, x1, y1, &s_inject.x1, &s_inject.y1);
    s_inject.start_us = esp_timer_get_time();
    s_inject.move_us = static_cast<int64_t>(move_ms) * 1000;
    s_inject.release_at_us = s_inject.start_us + static_cast<int64_t>(move_ms + hold_ms) * 1000;
    s_inject.pressed_reads = 0;
    s_inject.active = true;
    bsp_display_unlock();
    // The Tab5's touch indev runs in LV_INDEV_MODE_EVENT (esp_lvgl_port reads it only when
    // the touch controller raises its interrupt), so drive the reads while the stroke lasts.
    const bool event_driven = lv_indev_get_mode(indev) == LV_INDEV_MODE_EVENT;
    for (int waited = 0; s_inject.active && waited < move_ms + hold_ms + 3000; waited += 10)
    {
        if (event_driven && bsp_display_lock(500))
        {
            lv_indev_read(indev);
            bsp_display_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_inject.active)
    {
        s_inject.active = false;
        reply_err("%s was not read by the UI", what);
        return;
    }
    reply_ok("%s", what);
}

void cmd_tap(int argc, char** argv)
{
    if (argc < 3)
    {
        reply_err("usage: tap <x> <y> [ms]");
        return;
    }
    const int32_t x = std::atoi(argv[1]);
    const int32_t y = std::atoi(argv[2]);
    const int ms = argc > 3 ? std::atoi(argv[3]) : 80;
    inject_stroke("tap", x, y, x, y, 0, ms < 20 ? 20 : ms);
}

void cmd_drag(int argc, char** argv)
{
    if (argc < 5)
    {
        reply_err("usage: drag <x0> <y0> <x1> <y1> [ms]");
        return;
    }
    const int ms = argc > 5 ? std::atoi(argv[5]) : 300;
    inject_stroke("drag", std::atoi(argv[1]), std::atoi(argv[2]), std::atoi(argv[3]), std::atoi(argv[4]),
                  ms < 40 ? 40 : ms, 40);
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
    reply("drag <x0> <y0> <x1> <y1> [ms]   inject a stroke (a swipe up resumes the saver)");
    reply_ok("help");
}

void handle_line(char* line)
{
    char* argv[8] = {};
    int argc = 0;
    for (char* tok = std::strtok(line, " \t"); tok != nullptr && argc < 8; tok = std::strtok(nullptr, " \t"))
    {
        argv[argc++] = tok;
    }
    if (argc == 0)
    {
        return;
    }
    const char* cmd = argv[0];
    ESP_LOGI(kTag, "console: %s (%d args)", cmd, argc - 1);
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
    else if (std::strcmp(cmd, "drag") == 0)
    {
        cmd_drag(argc, argv);
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
    // Above the app's background workers, so a busy app cannot starve "launcher".
    if (xTaskCreate(console_task, "sentinel_con", kConsoleStackBytes, nullptr, tskIDLE_PRIORITY + 5,
                    &s_console_task) != pdPASS)
    {
        s_console_task = nullptr;
        ESP_LOGE(kTag, "console task not started");
        return;
    }
    ESP_LOGI(kTag, "Sentinel console on USB-Serial-JTAG (try: help)");
}

} // namespace platform::esp::idf_common::sentinel
