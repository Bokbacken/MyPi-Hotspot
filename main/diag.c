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
#include "nvs.h"
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

static diag_ota_t s_ota;

void diag_ota_begin(ota_target_t target, uint32_t bytes_total)
{
    taskENTER_CRITICAL(&s_lock);
    s_ota = (diag_ota_t){ .active = true, .target = target, .phase = OTA_PHASE_WRITING,
                          .bytes_total = bytes_total, .seq = s_ota.seq + 1 };
    taskEXIT_CRITICAL(&s_lock);
}

void diag_ota_progress(uint32_t bytes_done)
{
    taskENTER_CRITICAL(&s_lock);
    s_ota.bytes_done = bytes_done;
    s_ota.pct = s_ota.bytes_total ? (int)((uint64_t)bytes_done * 100 / s_ota.bytes_total) : 0;
    s_ota.seq++;
    taskEXIT_CRITICAL(&s_lock);
}

void diag_ota_phase(ota_phase_t phase)
{
    taskENTER_CRITICAL(&s_lock);
    s_ota.phase = phase;
    if (phase >= OTA_PHASE_VERIFIED) s_ota.pct = 100;
    s_ota.seq++;
    taskEXIT_CRITICAL(&s_lock);
}

void diag_ota_fail(const char *fmt, ...)
{
    char tmp[sizeof(s_ota.err)];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_lock);
    strlcpy(s_ota.err, tmp, sizeof(s_ota.err));
    s_ota.phase = OTA_PHASE_FAILED;
    s_ota.active = false;
    s_ota.failed_at_us = now ? now : 1;
    s_ota.seq++;
    taskEXIT_CRITICAL(&s_lock);
}

void diag_get_ota(diag_ota_t *out)
{
    taskENTER_CRITICAL(&s_lock);
    *out = s_ota;
    taskEXIT_CRITICAL(&s_lock);
}

bool diag_ota_ui_visible(void)
{
    diag_ota_t o;
    diag_get_ota(&o);
    if (o.active) return true;
    return o.phase == OTA_PHASE_FAILED && o.failed_at_us &&
           esp_timer_get_time() - o.failed_at_us < (int64_t)DIAG_OTA_FAIL_SHOW_MS * 1000;
}

const char *diag_ota_phase_name(ota_phase_t p)
{
    switch (p) {
    case OTA_PHASE_WRITING:   return "writing";
    case OTA_PHASE_VERIFYING: return "verifying";
    case OTA_PHASE_VERIFIED:  return "verified";
    case OTA_PHASE_DONE:      return "done";
    case OTA_PHASE_FAILED:    return "failed";
    default:                  return "idle";
    }
}

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

// ---------------------------------------------------------------------------
// Debug mode
// ---------------------------------------------------------------------------

#define DEBUG_HEARTBEAT_S   5
#define NVS_NAMESPACE       "mypi"
#define NVS_KEY_DEBUG       "debug"

static volatile bool s_debug;

// App log tags switched between INFO and DEBUG. (main/CMakeLists.txt compiles this
// component with LOG_LOCAL_LEVEL=DEBUG so the ESP_LOGD calls exist in the binary.)
static const char *const DEBUG_TAGS[] = { "main", "display", "touch", "pintest", "wifi", "web", "diag", "net", "eth" };

static void apply_debug_levels(void)
{
    for (size_t i = 0; i < sizeof(DEBUG_TAGS) / sizeof(DEBUG_TAGS[0]); i++) {
        esp_log_level_set(DEBUG_TAGS[i], s_debug ? ESP_LOG_DEBUG : ESP_LOG_INFO);
    }
}

void diag_debug_load(void)
{
#ifdef CONFIG_MYPI_DEBUG_MODE_DEFAULT
    bool on = true;
#else
    bool on = false;
#endif
    const char *source = "Kconfig default";
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, NVS_KEY_DEBUG, &v) == ESP_OK) {
            on = v != 0;
            source = "saved setting";
        }
        nvs_close(h);
    }
    s_debug = on;
    apply_debug_levels();
    ESP_LOGI(TAG, "Debug mode %s (%s) - toggle on the web status page", on ? "ON" : "off", source);
}

bool diag_debug_enabled(void)
{
    return s_debug;
}

esp_err_t diag_set_debug(bool on)
{
    s_debug = on;
    apply_debug_levels();
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, NVS_KEY_DEBUG, on ? 1 : 0);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "Debug mode %s (saved: %s)", on ? "ON" : "off", esp_err_to_name(err));
    return err;
}

static void heartbeat_task(void *arg)
{
    diag_status_t st;
    int64_t last = esp_timer_get_time();
    while (true) {
        // Check once per second so a debug-mode toggle takes effect quickly.
        vTaskDelay(pdMS_TO_TICKS(1000));
        int64_t interval_us = (int64_t)(s_debug ? DEBUG_HEARTBEAT_S : CONFIG_MYPI_HEARTBEAT_INTERVAL_S) * 1000000;
        if (esp_timer_get_time() - last < interval_us) {
            continue;
        }
        last = esp_timer_get_time();
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
