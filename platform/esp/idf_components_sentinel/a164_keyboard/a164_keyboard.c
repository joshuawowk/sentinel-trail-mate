/*
 * M5Stack Tab5 Keyboard (SKU A164) -- raw I2C driver. See a164_keyboard.h.
 *
 *   0x00 INT_CFG  0x01 INT_STATUS  0x02 EVENT_NUM (write 0: clear FIFO)  0x03 RGB brightness
 *   0x10 MODE (0 normal, 1 HID, 2 char)  0x11 RGB mode
 *   0x30 HID event: modifier, keycode (keycode 0 = released, FF FF = FIFO empty)
 *   0xFE firmware version  0xFF I2C address (writing it moves the keyboard - never write)
 *
 * Reading a register the keyboard does not map gets no defined answer, so only these are
 * read. The mode lives in the keyboard's RAM and is 0 after power-up (and after a replug);
 * writing the mode it already has does not clear its FIFO. Sentinel's launcher hands the
 * keyboard over in mode 0 with an empty FIFO before it starts a firmware.
 */
#include "a164_keyboard.h"

#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char* TAG = "a164_kbd";

#define KB_ADDR 0x6D
#define KB_SDA 0
#define KB_SCL 1
#define KB_HZ 100000
#define KB_TIMEOUT_MS 30

#define REG_INT_CFG 0x00
#define REG_SYS 0x00 /* INT_CFG, INT_STATUS, EVENT_NUM, brightness */
#define REG_EVENTS 0x02
#define REG_MODE 0x10
#define REG_HID 0x30
#define REG_VERSION 0xFE
#define REG_ADDRESS 0xFF

#define MODE_HID 1

#define POLL_MS 15
#define PROBE_MS 500
#define QUEUE_DEPTH 32
#define HISTORY 16

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static SemaphoreHandle_t s_lock; /* status/history between the poll task and readers */
static QueueHandle_t s_queue;
static volatile bool s_present;
static a164_kbd_status_t s_status = {.i2c_port = -1};
static struct
{
    a164_kbd_event_t ev;
    int64_t us;
} s_hist[HISTORY];
static unsigned s_nhist;

static esp_err_t rd(uint8_t reg, uint8_t* out, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, out, n, KB_TIMEOUT_MS);
}

static esp_err_t wr(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = {reg, val};
    return i2c_master_transmit(s_dev, b, sizeof(b), KB_TIMEOUT_MS);
}

static void note(const a164_kbd_event_t* ev, bool injected)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_hist[s_nhist % HISTORY].ev = *ev;
    s_hist[s_nhist % HISTORY].us = esp_timer_get_time();
    s_nhist++;
    if (injected)
    {
        s_status.injected++;
    }
    else
    {
        s_status.events++;
    }
    xSemaphoreGive(s_lock);
}

static void enqueue(const a164_kbd_event_t* ev, bool injected)
{
    if (xQueueSend(s_queue, ev, injected ? pdMS_TO_TICKS(200) : 0) != pdTRUE)
    {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.dropped++;
        xSemaphoreGive(s_lock);
        return;
    }
    note(ev, injected);
}

/* Is a keyboard there, and does it take HID mode? The probe keeps an absent keyboard
 * from logging an I2C NACK error every half second. */
static bool connect(void)
{
    uint8_t version = 0, addr = 0, mode = 0xFF, found = 0xFF;
    if (i2c_master_probe(s_bus, KB_ADDR, KB_TIMEOUT_MS) != ESP_OK || rd(REG_VERSION, &version, 1) != ESP_OK ||
        rd(REG_ADDRESS, &addr, 1) != ESP_OK || addr != KB_ADDR)
    {
        return false;
    }
    (void)rd(REG_MODE, &found, 1);
    if (wr(REG_INT_CFG, 0) != ESP_OK || /* polled: no interrupt pin */
        wr(REG_MODE, MODE_HID) != ESP_OK || rd(REG_MODE, &mode, 1) != ESP_OK || mode != MODE_HID ||
        wr(REG_EVENTS, 0) != ESP_OK) /* drop anything queued before now */
    {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.version = version;
    s_status.mode_found = found;
    s_status.connects++;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "A164 keyboard connected (firmware 0x%02X, found in mode %u)", version, found);
    return true;
}

static void kb_task(void* arg)
{
    (void)arg;
    int failures = 0;
    unsigned polls = 0;
    for (;;)
    {
        if (!s_present)
        {
            if (connect())
            {
                failures = 0;
                s_present = true;
            }
            vTaskDelay(pdMS_TO_TICKS(s_present ? POLL_MS : PROBE_MS));
            continue;
        }
        uint8_t sys[4] = {0};
        bool ok = rd(REG_SYS, sys, sizeof(sys)) == ESP_OK;
        if (ok && ++polls % 32 == 0)
        {
            /* A keyboard that was replugged or reset still answers, but in normal mode. */
            uint8_t mode = 0;
            ok = rd(REG_MODE, &mode, 1) == ESP_OK && mode == MODE_HID;
        }
        for (unsigned i = 0; ok && i < sys[2] && i < 16; i++)
        {
            uint8_t hid[2];
            if (rd(REG_HID, hid, sizeof(hid)) != ESP_OK)
            {
                ok = false;
                break;
            }
            if (hid[0] == 0xFF && hid[1] == 0xFF)
            {
                break; /* FIFO empty */
            }
            a164_kbd_event_t ev = {.modifier = hid[0], .keycode = hid[1]};
            enqueue(&ev, false);
        }
        failures = ok ? 0 : failures + 1;
        if (failures >= 3)
        {
            s_present = false;
            /* Whatever was held is released: the adapter sees a release, not a stuck key. */
            a164_kbd_event_t up = {0, 0};
            (void)xQueueSend(s_queue, &up, 0);
            ESP_LOGW(TAG, "A164 keyboard disconnected; probing again");
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

esp_err_t a164_kbd_start(void)
{
    if (s_bus)
    {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(QUEUE_DEPTH, sizeof(a164_kbd_event_t));
    if (!s_lock || !s_queue)
    {
        return ESP_ERR_NO_MEM;
    }
    /* Controller 0 is the Tab5's system bus (GPIO31/32); 1 is the board's external bus,
     * which Trail Mate never brings up. Take 1 like Sentinel's launcher does, else any. */
    const int ports[] = {1, -1};
    esp_err_t err = ESP_FAIL;
    for (size_t i = 0; i < sizeof(ports) / sizeof(ports[0]) && !s_bus; i++)
    {
        i2c_master_bus_config_t cfg = {
            .i2c_port = ports[i],
            .sda_io_num = KB_SDA,
            .scl_io_num = KB_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        err = i2c_new_master_bus(&cfg, &s_bus);
        if (err == ESP_OK)
        {
            s_status.i2c_port = ports[i];
        }
        else
        {
            s_bus = NULL;
        }
    }
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "keyboard I2C bus: %s", esp_err_to_name(err));
        return err;
    }
    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = KB_ADDR,
        .scl_speed_hz = KB_HZ,
    };
    err = i2c_master_bus_add_device(s_bus, &dev, &s_dev);
    if (err == ESP_OK && xTaskCreate(kb_task, "a164_kbd", 3072, NULL, 4, NULL) != pdPASS)
    {
        err = ESP_ERR_NO_MEM;
    }
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "keyboard start: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "A164 keyboard driver started on I2C port %d (SDA %d, SCL %d)", s_status.i2c_port, KB_SDA,
             KB_SCL);
    return ESP_OK;
}

bool a164_kbd_present(void)
{
    return s_present;
}

bool a164_kbd_get(a164_kbd_event_t* ev)
{
    return s_queue && xQueueReceive(s_queue, ev, 0) == pdTRUE;
}

bool a164_kbd_inject(uint8_t keycode, uint8_t modifier)
{
    if (!s_queue || keycode == 0)
    {
        return false;
    }
    a164_kbd_event_t down = {.modifier = modifier, .keycode = keycode};
    a164_kbd_event_t up = {.modifier = modifier, .keycode = 0};
    enqueue(&down, true);
    enqueue(&up, true);
    return true;
}

bool a164_kbd_inject_edge(uint8_t keycode, uint8_t modifier)
{
    if (!s_queue)
    {
        return false;
    }
    a164_kbd_event_t ev = {.modifier = modifier, .keycode = keycode};
    enqueue(&ev, true);
    return true;
}

void a164_kbd_get_status(a164_kbd_status_t* out)
{
    if (!out)
    {
        return;
    }
    if (!s_lock)
    {
        memset(out, 0, sizeof(*out));
        out->i2c_port = -1;
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_lock);
    out->present = s_present;
}

int a164_kbd_history(a164_kbd_event_t* out, uint32_t* age_ms, int max)
{
    if (!s_lock || !out || !age_ms)
    {
        return 0;
    }
    int n = 0;
    const int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (unsigned i = s_nhist; i > 0 && n < max && s_nhist - i < HISTORY; i--, n++)
    {
        out[n] = s_hist[(i - 1) % HISTORY].ev;
        age_ms[n] = (uint32_t)((now - s_hist[(i - 1) % HISTORY].us) / 1000);
    }
    xSemaphoreGive(s_lock);
    return n;
}
