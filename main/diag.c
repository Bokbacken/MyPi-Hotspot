#include "diag.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "diag";

static diag_status_t s_st = {
    .display_status = "not started",
    .lcd_id = "not read",
    .touch_status = "not started",
    .hosted_info = "unknown",
    .ota_status = "idle",
    .ip = "0.0.0.0",
};
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

#define SET_STR_FIELD(field)                                \
    do {                                                    \
        char tmp[sizeof(s_st.field)];                       \
        va_list ap;                                         \
        va_start(ap, fmt);                                  \
        vsnprintf(tmp, sizeof(tmp), fmt, ap);               \
        va_end(ap);                                         \
        taskENTER_CRITICAL(&s_lock);                        \
        memcpy(s_st.field, tmp, sizeof(tmp));               \
        taskEXIT_CRITICAL(&s_lock);                         \
    } while (0)

void diag_set_display_status(const char *fmt, ...) { SET_STR_FIELD(display_status); }
void diag_set_lcd_id(const char *fmt, ...)         { SET_STR_FIELD(lcd_id); }
void diag_set_touch_status(const char *fmt, ...)   { SET_STR_FIELD(touch_status); }
void diag_set_hosted_info(const char *fmt, ...)    { SET_STR_FIELD(hosted_info); }
void diag_set_ota_status(const char *fmt, ...)     { SET_STR_FIELD(ota_status); }

void diag_set_spi_clock(uint32_t hz)
{
    s_st.spi_clock_hz = hz;
}

void diag_frame_flushed(bool ok)
{
    taskENTER_CRITICAL(&s_lock);
    if (ok) s_st.frames_flushed++;
    else s_st.flush_errors++;
    taskEXIT_CRITICAL(&s_lock);
}

void diag_touch_event(const char *fmt, ...)
{
    char tmp[48];
    uint32_t ms = (uint32_t)(esp_timer_get_time() / 1000);
    int n = snprintf(tmp, sizeof(tmp), "%lu.%03lu ", (unsigned long)(ms / 1000), (unsigned long)(ms % 1000));
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp + n, sizeof(tmp) - n, fmt, ap);
    va_end(ap);
    taskENTER_CRITICAL(&s_lock);
    s_st.touch_events++;
    memcpy(s_st.touch_history[s_st.touch_history_head], tmp, sizeof(tmp));
    s_st.touch_history_head = (s_st.touch_history_head + 1) % DIAG_TOUCH_HISTORY;
    taskEXIT_CRITICAL(&s_lock);
}

void diag_set_wifi(wifi_state_t state, const char *ip, int rssi)
{
    taskENTER_CRITICAL(&s_lock);
    s_st.wifi_state = state;
    if (ip) {
        strncpy(s_st.ip, ip, sizeof(s_st.ip) - 1);
        s_st.ip[sizeof(s_st.ip) - 1] = '\0';
    }
    s_st.rssi = rssi;
    taskEXIT_CRITICAL(&s_lock);
}

void diag_set_wifi_ssid(const char *ssid)
{
    taskENTER_CRITICAL(&s_lock);
    strncpy(s_st.ssid, ssid, sizeof(s_st.ssid) - 1);
    taskEXIT_CRITICAL(&s_lock);
}

void diag_wifi_retry(void)
{
    taskENTER_CRITICAL(&s_lock);
    s_st.wifi_retries++;
    taskEXIT_CRITICAL(&s_lock);
}

void diag_get(diag_status_t *out)
{
    taskENTER_CRITICAL(&s_lock);
    memcpy(out, &s_st, sizeof(*out));
    taskEXIT_CRITICAL(&s_lock);
}

const char *diag_wifi_state_name(wifi_state_t s)
{
    switch (s) {
    case WIFI_STATE_OFF:          return "off";
    case WIFI_STATE_INIT:         return "initializing";
    case WIFI_STATE_INIT_FAILED:  return "init failed";
    case WIFI_STATE_CONNECTING:   return "connecting";
    case WIFI_STATE_CONNECTED:    return "associated (waiting for DHCP)";
    case WIFI_STATE_GOT_IP:       return "connected";
    case WIFI_STATE_DISCONNECTED: return "disconnected";
    default:                      return "?";
    }
}

const char *diag_reset_reason_name(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external pin";
    case ESP_RST_SW:        return "software (esp_restart)";
    case ESP_RST_PANIC:     return "panic / exception";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "task watchdog";
    case ESP_RST_WDT:       return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep-sleep wake";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "SDIO";
    case ESP_RST_USB:       return "USB peripheral";
    case ESP_RST_JTAG:      return "JTAG";
    case ESP_RST_EFUSE:     return "eFuse error";
    case ESP_RST_PWR_GLITCH: return "power glitch";
    case ESP_RST_CPU_LOCKUP: return "CPU lockup";
    default:                return "unknown";
    }
}

void diag_print_boot_banner(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash_size = 0;
    esp_err_t ferr = esp_flash_get_size(NULL, &flash_size);
    const esp_partition_t *running = esp_ota_get_running_partition();

    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, " MyPi-Hotspot firmware %s", app->version);
    ESP_LOGI(TAG, " Built %s %s with ESP-IDF %s", app->date, app->time, esp_get_idf_version());
    ESP_LOGI(TAG, " Chip: %s, %d core(s), revision v%d.%d", CONFIG_IDF_TARGET, chip.cores,
             chip.revision / 100, chip.revision % 100);
    ESP_LOGI(TAG, " Reset reason: %s (%d)", diag_reset_reason_name(), (int)esp_reset_reason());
    if (ferr == ESP_OK) {
        ESP_LOGI(TAG, " Flash: %lu MB detected (image configured for %s)",
                 (unsigned long)(flash_size / (1024 * 1024)), CONFIG_ESPTOOLPY_FLASHSIZE);
    } else {
        ESP_LOGW(TAG, " Flash size read failed: %s", esp_err_to_name(ferr));
    }
    ESP_LOGI(TAG, " PSRAM: %u KB total, %u KB free",
             (unsigned)(esp_psram_get_size() / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    ESP_LOGI(TAG, " Internal heap: %u KB free (min ever %u KB)",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024));
    if (running) {
        ESP_LOGI(TAG, " Running partition: %s @ 0x%06lx (%lu KB)", running->label,
                 (unsigned long)running->address, (unsigned long)(running->size / 1024));
    }
    ESP_LOGI(TAG, "==================================================");
}

static void heartbeat_task(void *arg)
{
    diag_status_t st;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_MYPI_HEARTBEAT_INTERVAL_S * 1000));
        diag_get(&st);
        uint64_t up = esp_timer_get_time() / 1000000ULL;
        ESP_LOGI(TAG, "HB up=%lluh%02llum%02llus heap=%uK(min %uK) psram=%uK wifi=%s ip=%s rssi=%d touch=%lu frames=%lu flush_err=%lu",
                 up / 3600, (up / 60) % 60, up % 60,
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                 diag_wifi_state_name(st.wifi_state), st.ip, st.rssi,
                 (unsigned long)st.touch_events, (unsigned long)st.frames_flushed,
                 (unsigned long)st.flush_errors);
    }
}

void diag_start_heartbeat(void)
{
    if (xTaskCreate(heartbeat_task, "heartbeat", 4096, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start heartbeat task");
    }
}
