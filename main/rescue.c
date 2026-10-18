#include "rescue.h"

#include <stdint.h>
#include <stdio.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "eth.h"
#include "logstore.h"
#include "opt_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "profiles.h"
#include "wifi.h"

static const char *TAG = "rescue";

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static rescue_state_t s_st = {.option = -1};
static int s_pending = -1;          // option waiting to be applied
static TaskHandle_t s_task;

// Automatic C5 recovery (v0.8.6): the first C5 failure restarts the P4 (which also hard-resets
// the C5 via its reset GPIO) instead of entering emergency mode. Only if the C5 fails again
// before Wi-Fi came up properly is emergency mode entered. The counter lives in no-init RAM
// so it survives esp_restart(); a power-on reset clears it.
#define RETRY_MAGIC     0x52455452u     // "RETR"
#define RETRY_MAX       1
typedef struct {
    uint32_t magic;
    uint32_t count;     // automatic restarts since Wi-Fi last came up
    uint32_t reason;    // reason of the last automatic restart
} retry_t;
static __NOINIT_ATTR retry_t s_retry;
static bool s_retry_checked;
static bool s_retry_pending;

static void retry_check(void)
{
    if (s_retry_checked) return;
    s_retry_checked = true;
    esp_reset_reason_t rr = esp_reset_reason();
    if (s_retry.magic != RETRY_MAGIC || rr == ESP_RST_POWERON || rr == ESP_RST_BROWNOUT || rr == ESP_RST_UNKNOWN ||
        s_retry.count > RETRY_MAX) {
        s_retry.magic = RETRY_MAGIC;
        s_retry.count = 0;
        s_retry.reason = RESCUE_REASON_NONE;
    }
}

int rescue_auto_restarts(rescue_reason_t *last_reason)
{
    retry_check();
    if (last_reason) *last_reason = (rescue_reason_t)s_retry.reason;
    return (int)s_retry.count;
}

void rescue_c5_ok(void)
{
    retry_check();
    if (s_retry.count) {
        ESP_LOGI(TAG, "C5 recovered after %lu automatic restart(s) (last cause: %s)", (unsigned long)s_retry.count,
                 rescue_reason_text((rescue_reason_t)s_retry.reason));
    }
    s_retry.count = 0;
}

static void retry_task(void *arg)
{
    rescue_reason_t reason = (rescue_reason_t)(uintptr_t)arg;
    char msg[96];
    snprintf(msg, sizeof(msg), "automatic restart %lu/%d to recover the C5 (%s)", (unsigned long)s_retry.count,
             RETRY_MAX, rescue_reason_text(reason));
    ESP_LOGE(TAG, "=== %s ===", msg);
    vTaskDelay(pdMS_TO_TICKS(500));     // let the log line reach the RAM ring
    logstore_save_now(msg);             // Always keep the evidence (the RAM log is lost on restart)
    logstore_before_restart(msg);
    esp_restart();
}

const char *rescue_reason_text(rescue_reason_t r)
{
    switch (r) {
    case RESCUE_REASON_SWIPE:           return "manual (swipe)";
    case RESCUE_REASON_C5_INIT_FAIL:    return "C5 Wi-Fi init failed";
    case RESCUE_REASON_C5_INIT_TIMEOUT: return "C5 init timeout";
    case RESCUE_REASON_C5_TRANSPORT:    return "C5 SDIO link failed";
    case RESCUE_REASON_C5_NO_RESPONSE:  return "C5 not responding";
    default:                            return "-";
    }
}

void rescue_option_label(int option, char *buf, size_t n)
{
    if (option == RESCUE_OPT_DHCP) {
        snprintf(buf, n, "DHCP");
    } else if (option > 0 && option <= AP_SUBNET_COUNT) {
        const uint8_t *b = AP_SUBNET_BASE[option - 1];
        snprintf(buf, n, "%u.%u.%u.%u", b[0], b[1], b[2], RESCUE_HOST_OCTET);
    } else {
        snprintf(buf, n, "?");
    }
}

int rescue_swipe_seconds_left(void)
{
    int64_t left_us = (int64_t)RESCUE_SWIPE_WINDOW_S * 1000000 - esp_timer_get_time();
    return left_us > 0 ? (int)((left_us + 999999) / 1000000) : 0;
}

bool rescue_active(void) { return s_st.active; }

void rescue_get(rescue_state_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_st;
    portEXIT_CRITICAL(&s_mux);
}

static void apply(int option)
{
    esp_err_t err;
    if (option == RESCUE_OPT_DHCP) {
        err = eth_rescue_apply(true, 0, 0, 0, 0);
    } else {
        const uint8_t *b = AP_SUBNET_BASE[option - 1];
        err = eth_rescue_apply(false, b[0], b[1], b[2], RESCUE_HOST_OCTET);
    }
    char label[20];
    rescue_option_label(option, label, sizeof(label));
    ESP_LOGW(TAG, "Ethernet rescue option %s: %s", label, esp_err_to_name(err));
    portENTER_CRITICAL(&s_mux);
    s_st.err = err;
    s_st.busy = s_pending >= 0;
    portEXIT_CRITICAL(&s_mux);
}

static void rescue_task(void *arg)
{
    // Always keep a log of why emergency mode was entered (the store may still be mounting at boot).
    char msg[96];
    snprintf(msg, sizeof(msg), "emergency mode: %s (after %lu automatic restart(s))", rescue_reason_text(s_st.reason),
             (unsigned long)s_retry.count);
    // v0.9.0: remember in NVS that emergency mode was entered because of the C5 (shown on the
    // Optimize page; cleared automatically after the next successful C5 init, or manually).
    if (s_st.reason != RESCUE_REASON_SWIPE) opt_emergency_set(true);
    for (int i = 0; i < 30 && logstore_save_now(msg) == ESP_ERR_INVALID_STATE; i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    // Free the AP subnets first (the static options reuse them); radio off in the background.
    wifi_rescue_stop_ap();
    while (true) {
        portENTER_CRITICAL(&s_mux);
        int opt = s_pending;
        s_pending = -1;
        portEXIT_CRITICAL(&s_mux);
        if (opt >= 0) {
            apply(opt);
            continue;   // a newer selection may have arrived meanwhile
        }
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
}

void rescue_select(int option)
{
    if (option < 0 || option >= RESCUE_OPT_COUNT || !s_st.active) return;
    portENTER_CRITICAL(&s_mux);
    s_st.option = option;
    s_st.busy = true;
    s_pending = option;
    portEXIT_CRITICAL(&s_mux);
    if (s_task) xTaskNotifyGive(s_task);
}

void rescue_c5_init_ok(void)
{
    // v0.9.0: the C5 came up (ESP-Hosted transport + esp_wifi_init OK) - a stale emergency flag
    // from an earlier boot is cleared automatically.
    if (!opt_emergency_flag()) return;
    if (opt_emergency_set(false) == ESP_OK) {
        ESP_LOGI(TAG, "Emergency flag cleared after successful C5 init");
    } else {
        ESP_LOGW(TAG, "Could not clear the emergency flag in NVS");
    }
}

// SDIO 50 MHz auto-fallback (v0.9.0): 40 MHz is stored in NVS by opt_sdio_fallback_to_40()
// and the P4 restarts (resetting the C5). Does not use the automatic-restart budget.
static void sdio_fallback_task(void *arg)
{
    rescue_reason_t reason = (rescue_reason_t)(uintptr_t)arg;
    char msg[96];
    opt_sdio_fallback_to_40();
    snprintf(msg, sizeof(msg), "SDIO 50 MHz auto-fallback to 40 MHz (%s) - restarting", rescue_reason_text(reason));
    ESP_LOGE(TAG, "=== %s ===", msg);
    vTaskDelay(pdMS_TO_TICKS(500));
    logstore_save_now(msg);
    logstore_before_restart(msg);
    esp_restart();
}

void rescue_request(rescue_reason_t reason)
{
    retry_check();
    if (reason != RESCUE_REASON_SWIPE && !s_st.active) {
        bool restart = false;
        portENTER_CRITICAL(&s_mux);
        if (s_retry_pending) {
            portEXIT_CRITICAL(&s_mux);
            return;     // restart already on its way
        }
        bool sdio50 = opt_sdio_active_mhz() == 50;
        if (sdio50) s_retry_pending = true;
        portEXIT_CRITICAL(&s_mux);
        if (sdio50) {
            if (xTaskCreate(sdio_fallback_task, "sdio_fb", 4096, (void *)(uintptr_t)reason, 5, NULL) == pdPASS) return;
            s_retry_pending = false;
            ESP_LOGE(TAG, "Failed to create SDIO fallback task");
        }
        portENTER_CRITICAL(&s_mux);
        if (s_retry.count < RETRY_MAX) {
            s_retry.count++;
            s_retry.reason = reason;
            s_retry_pending = restart = true;
        }
        portEXIT_CRITICAL(&s_mux);
        if (restart) {
            if (xTaskCreate(retry_task, "c5_retry", 4096, (void *)(uintptr_t)reason, 5, NULL) == pdPASS) return;
            s_retry_pending = false;
            ESP_LOGE(TAG, "Failed to create retry task - entering emergency mode");
        }
    }
    bool first = false;
    portENTER_CRITICAL(&s_mux);
    if (!s_st.active) {
        first = true;
        s_st.active = true;
        s_st.reason = reason;
        s_st.option = RESCUE_OPT_DHCP;  // preselected so a router-connected port gets an IP at once
        s_st.busy = true;
        s_st.err = ESP_OK;
        s_pending = RESCUE_OPT_DHCP;
    }
    portEXIT_CRITICAL(&s_mux);
    if (!first) return;
    ESP_LOGE(TAG, "=== EMERGENCY MODE (%s): Ethernet-only access, Wi-Fi AP stopped ===", rescue_reason_text(reason));
    if (xTaskCreate(rescue_task, "rescue", 4096, NULL, 3, &s_task) != pdPASS) {
        s_task = NULL;
        ESP_LOGE(TAG, "Failed to create rescue task");
    }
}
