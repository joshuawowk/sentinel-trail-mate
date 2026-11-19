#include "sentinel_wifi.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bsp/m5stack_tab5.h"
#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_hosted_event.h"
#include "esp_hosted_host_fw_ver.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char* TAG = "sentinel_wifi";

/* The C6's supply needs a moment before it answers on SDIO (as sapp_net waits). */
#define POWER_SETTLE_MS 300

static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;
static volatile bool s_ready;
static volatile bool s_dead; /* the link failed; esp-hosted's retry counter will not recover without a restart */
static char s_c6[24];

static void lock_init(void)
{
    /* Created on first use; the first caller is the Wi-Fi runtime, long after start-up. */
    static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    portENTER_CRITICAL(&mux);
    if (!s_lock)
    {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }
    portEXIT_CRITICAL(&mux);
}

static void on_link_failure(void* arg, esp_event_base_t base, int32_t id, void* data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;
    /* CONFIG_ESP_HOSTED_TRANSPORT_RESTART_ON_FAILURE is off: mark it dead, never restart the Tab5. */
    s_dead = true;
    ESP_LOGW(TAG, "the ESP32-C6 stopped answering on SDIO");
}

bool sentinel_wifi_link_up(void)
{
    if (s_ready)
    {
        return true;
    }
    lock_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_ready)
    {
        xSemaphoreGive(s_lock);
        return true;
    }
    if (s_dead)
    {
        /* esp_hosted_connect_to_slave()'s internal retry counter is static and only resets on
         * success, so a second attempt after a real failure cannot recover; a Tab5 reset is
         * needed. Do not keep resetting the C6 under the launcher's shared firmware. */
        xSemaphoreGive(s_lock);
        return false;
    }
    esp_err_t e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "default event loop: %s", esp_err_to_name(e));
    }
    static bool handler_registered;
    if (!handler_registered)
    {
        handler_registered = esp_event_handler_instance_register(ESP_HOSTED_EVENT, ESP_HOSTED_EVENT_TRANSPORT_FAILURE,
                                                                 on_link_failure, NULL, NULL) == ESP_OK;
    }
    /* The launcher leaves the C6 powered, but make sure (idempotent) and let it settle, as
     * sapp_net does. This only drives WLAN_PWR_EN; it does not touch the C6's flash. */
    bsp_set_wifi_power_enable(true);
    vTaskDelay(pdMS_TO_TICKS(POWER_SETTLE_MS));

    ESP_LOGI(TAG, "connecting to the ESP32-C6 (esp-hosted %d.%d.%d host, shared with Sentinel)",
             ESP_HOSTED_VERSION_MAJOR_1, ESP_HOSTED_VERSION_MINOR_1, ESP_HOSTED_VERSION_PATCH_1);
    /* Resets the C6 (GPIO 15) and brings its SDIO link up; its flash is not touched. The return
     * value is not a reliable liveness signal: esp_hosted_connect_to_slave() swallows a transport
     * failure (ESP_ERROR_CHECK_WITHOUT_ABORT) and returns ESP_OK. Confirm the link with a real
     * RPC (the firmware-version read) and the transport-failure event instead. */
    (void)esp_hosted_connect_to_slave();
    esp_hosted_coprocessor_fwver_t fw = {0};
    const esp_err_t ver = esp_hosted_get_coprocessor_fwversion(&fw);
    if (s_dead || ver != ESP_OK)
    {
        s_dead = true;
        ESP_LOGE(TAG, "the ESP32-C6 did not answer (fwver %s); Wi-Fi stays off", esp_err_to_name(ver));
        xSemaphoreGive(s_lock);
        return false;
    }
    snprintf(s_c6, sizeof(s_c6), "%lu.%lu.%lu", (unsigned long)fw.major1, (unsigned long)fw.minor1,
             (unsigned long)fw.patch1);
    if ((int)fw.major1 != ESP_HOSTED_VERSION_MAJOR_1 || (int)fw.minor1 != ESP_HOSTED_VERSION_MINOR_1)
    {
        ESP_LOGW(TAG, "ESP32-C6 firmware %s does not match this host's esp-hosted %d.%d", s_c6,
                 ESP_HOSTED_VERSION_MAJOR_1, ESP_HOSTED_VERSION_MINOR_1);
    }
    else
    {
        ESP_LOGI(TAG, "ESP32-C6 firmware %s", s_c6);
    }
    s_ready = true;
    xSemaphoreGive(s_lock);
    return true;
}

bool sentinel_wifi_link_ready(void)
{
    return s_ready;
}

void sentinel_wifi_c6_version(char* out, size_t len)
{
    if (out && len)
    {
        strlcpy(out, s_c6, len);
    }
}

void sentinel_wifi_stop_for_launch(void)
{
    if (!s_ready)
    {
        return;
    }
    /* Both fail harmlessly (not initialised, not started) when Wi-Fi was never switched on. The
     * C6's supply is left on for the launcher; its bootloader resets the C6 when it starts. */
    esp_err_t d = esp_wifi_disconnect();
    esp_err_t s = esp_wifi_stop();
    ESP_LOGI(TAG, "Wi-Fi stopped for the launcher (disconnect %s, stop %s)", esp_err_to_name(d), esp_err_to_name(s));
}
