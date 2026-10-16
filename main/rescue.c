#include "rescue.h"

#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "eth.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "profiles.h"
#include "wifi.h"

static const char *TAG = "rescue";

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static rescue_state_t s_st = {.option = -1};
static int s_pending = -1;          // option waiting to be applied
static TaskHandle_t s_task;

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

void rescue_request(rescue_reason_t reason)
{
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
