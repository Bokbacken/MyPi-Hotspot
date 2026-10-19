#include "time_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

static const char *TAG = "time";

#define TIME_NVS_NS         "mypi"
#define TIME_NVS_KEY        "tz"
#define TIME_VALID_EPOCH    1577836800
#define TIMEZONE_DEFAULT    1

typedef struct {
    const char *name;
    const char *posix;
} timezone_t;

static const timezone_t s_timezones[TIME_ZONE_COUNT] = {
    {"UTC", "UTC0"},
    {"Sweden (CET)", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"United Kingdom", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Eastern Europe (EET)", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"US Eastern", "EST5EDT,M3.2.0,M11.1.0"},
    {"US Pacific", "PST8PDT,M3.2.0,M11.1.0"},
    {"Japan", "JST-9"},
};

static uint8_t s_timezone = TIMEZONE_DEFAULT;
static bool s_ntp_synced;
static bool s_sntp_started;
static SemaphoreHandle_t s_mutex;

const char *time_service_timezone_name(uint8_t timezone)
{
    return timezone < TIME_ZONE_COUNT ? s_timezones[timezone].name : s_timezones[TIMEZONE_DEFAULT].name;
}

static esp_err_t apply_timezone(uint8_t timezone)
{
    if (timezone >= TIME_ZONE_COUNT) return ESP_ERR_INVALID_ARG;
    if (setenv("TZ", s_timezones[timezone].posix, 1) != 0) return ESP_FAIL;
    tzset();
    s_timezone = timezone;
    return ESP_OK;
}

void time_service_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    uint8_t timezone = TIMEZONE_DEFAULT;
    nvs_handle_t handle;
    if (nvs_open(TIME_NVS_NS, NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_u8(handle, TIME_NVS_KEY, &timezone);
        nvs_close(handle);
    }
    if (timezone >= TIME_ZONE_COUNT) timezone = TIMEZONE_DEFAULT;
    apply_timezone(timezone);

    struct timeval now;
    gettimeofday(&now, NULL);
    if (now.tv_sec >= TIME_VALID_EPOCH) {
        ESP_LOGI(TAG, "RTC time available at boot: %lld", (long long)now.tv_sec);
    } else {
        ESP_LOGW(TAG, "RTC time is not set; waiting for network time");
    }
    ESP_LOGI(TAG, "Timezone: %s", time_service_timezone_name(s_timezone));
}

static void on_time_sync(struct timeval *tv)
{
    if (tv) settimeofday(tv, NULL);
    if (s_mutex && xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        s_ntp_synced = true;
        xSemaphoreGive(s_mutex);
    }
    ESP_LOGI(TAG, "NTP synchronized; system RTC updated");
}

void time_service_network_connected(void)
{
    if (!s_sntp_started) {
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
        esp_sntp_set_time_sync_notification_cb(on_time_sync);
        esp_sntp_init();
        s_sntp_started = true;
        ESP_LOGI(TAG, "NTP synchronization started");
    } else if (esp_sntp_enabled()) {
        esp_sntp_restart();
    }
}

esp_err_t time_service_set_timezone(uint8_t timezone)
{
    if (timezone >= TIME_ZONE_COUNT) return ESP_ERR_INVALID_ARG;
    if (s_mutex && xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return ESP_FAIL;
    uint8_t previous = s_timezone;
    esp_err_t err = apply_timezone(timezone);
    if (s_mutex) {
        xSemaphoreGive(s_mutex);
    }
    if (err != ESP_OK) return err;

    nvs_handle_t handle;
    err = nvs_open(TIME_NVS_NS, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, TIME_NVS_KEY, timezone);
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    if (err != ESP_OK) {
        if (s_mutex && xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
            apply_timezone(previous);
            xSemaphoreGive(s_mutex);
        } else if (!s_mutex) {
            apply_timezone(previous);
        }
        return err;
    }
    ESP_LOGI(TAG, "Timezone changed to %s", time_service_timezone_name(timezone));
    return ESP_OK;
}

bool time_service_format(char *dst, size_t dst_size, const char *format)
{
    if (!dst || dst_size == 0 || !format) return false;
    dst[0] = '\0';
    if (s_mutex && xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return false;
    struct timeval now;
    gettimeofday(&now, NULL);
    if (now.tv_sec < TIME_VALID_EPOCH) {
        if (s_mutex) xSemaphoreGive(s_mutex);
        return false;
    }
    struct tm local;
    bool formatted = localtime_r(&now.tv_sec, &local) &&
                     strftime(dst, dst_size, format, &local) != 0;
    if (s_mutex) xSemaphoreGive(s_mutex);
    return formatted;
}

time_t time_service_now(void)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    return now.tv_sec >= TIME_VALID_EPOCH ? now.tv_sec : 0;
}

void time_service_get_status(time_service_status_t *status)
{
    if (!status) return;
    status->valid = time_service_format(status->clock, sizeof(status->clock), "%Y-%m-%d %H:%M:%S");
    if (s_mutex && xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        status->ntp_synced = s_ntp_synced;
        status->timezone = s_timezone;
        xSemaphoreGive(s_mutex);
    } else {
        status->ntp_synced = false;
        status->timezone = TIMEZONE_DEFAULT;
    }
    if (!status->valid) snprintf(status->clock, sizeof(status->clock), "Time not set");
}
