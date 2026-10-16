#include "wifi.h"

#include <string.h>

#include "diag.h"
#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "mdns.h"
#include "sdkconfig.h"
#include "web.h"

static const char *TAG = "wifi";

#define BIT_GOT_IP          BIT0
#define BIT_DISCONNECTED    BIT1
#define BIT_LOG_RSSI        BIT2

static EventGroupHandle_t s_events;
static esp_netif_t *s_netif;

static const char *disconnect_reason_name(uint8_t r)
{
    switch (r) {
    case WIFI_REASON_AUTH_EXPIRE:            return "auth expired";
    case WIFI_REASON_AUTH_FAIL:              return "auth failed (wrong password?)";
    case WIFI_REASON_ASSOC_FAIL:             return "association failed";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:      return "4-way handshake timeout (wrong password?)";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "4-way handshake timeout (wrong password?)";
    case WIFI_REASON_NO_AP_FOUND:            return "AP not found (SSID wrong / out of range / 5 GHz-only?)";
    case WIFI_REASON_BEACON_TIMEOUT:         return "beacon timeout (signal lost)";
    case WIFI_REASON_CONNECTION_FAIL:        return "connection failed";
    case WIFI_REASON_ASSOC_LEAVE:            return "left (assoc leave)";
    default:                                 return "see esp_wifi_types.h";
    }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "STA started, connecting to \"%s\"", CONFIG_MYPI_WIFI_SSID);
        diag_set_wifi(WIFI_STATE_CONNECTING, NULL, 0);
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        wifi_event_sta_connected_t *ev = data;
        ESP_LOGI(TAG, "Associated with \"%.*s\" on channel %d", ev->ssid_len, (const char *)ev->ssid, ev->channel);
        diag_set_wifi(WIFI_STATE_CONNECTED, NULL, 0);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *ev = data;
        ESP_LOGW(TAG, "Disconnected: reason %d (%s), rssi %d", ev->reason, disconnect_reason_name(ev->reason), ev->rssi);
        diag_set_wifi(WIFI_STATE_DISCONNECTED, "0.0.0.0", 0);
        xEventGroupClearBits(s_events, BIT_GOT_IP);
        xEventGroupSetBits(s_events, BIT_DISCONNECTED);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        char ip[16];
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ev->ip_info.ip));
        // RSSI is queried from wifi_task (an RPC to the co-processor; avoid it in the event loop).
        ESP_LOGI(TAG, "Got IP %s (gw " IPSTR ", mask " IPSTR ")", ip,
                 IP2STR(&ev->ip_info.gw), IP2STR(&ev->ip_info.netmask));
        ESP_LOGI(TAG, "Web UI: http://%s/  or  http://%s.local/", ip, CONFIG_MYPI_HOSTNAME);
        diag_set_wifi(WIFI_STATE_GOT_IP, ip, 0);
        xEventGroupSetBits(s_events, BIT_GOT_IP | BIT_LOG_RSSI);
    }
}

static void log_hosted_info(void)
{
    ESP_LOGI(TAG, "ESP-Hosted host component v%d.%d.%d, transport SDIO (CLK=%d CMD=%d D0=%d D1=%d D2=%d D3=%d, slave reset GPIO%d)",
             ESP_HOSTED_VERSION_MAJOR_1, ESP_HOSTED_VERSION_MINOR_1, ESP_HOSTED_VERSION_PATCH_1,
             CONFIG_ESP_HOSTED_SDIO_PIN_CLK, CONFIG_ESP_HOSTED_SDIO_PIN_CMD, CONFIG_ESP_HOSTED_SDIO_PIN_D0,
             CONFIG_ESP_HOSTED_SDIO_PIN_D1, CONFIG_ESP_HOSTED_SDIO_PIN_D2, CONFIG_ESP_HOSTED_SDIO_PIN_D3,
             CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE);

    esp_hosted_coprocessor_fwver_t ver = {0};
    char target[16] = "?";
    uint32_t chip_id = 0;
    int r1 = esp_hosted_get_coprocessor_fwversion(&ver);
    int r2 = esp_hosted_get_cp_info(&chip_id, target, sizeof(target));
    if (r1 == ESP_OK) {
        ESP_LOGI(TAG, "Co-processor (%s) ESP-Hosted slave firmware v%lu.%lu.%lu",
                 r2 == ESP_OK ? target : "?", (unsigned long)ver.major1, (unsigned long)ver.minor1,
                 (unsigned long)ver.patch1);
        diag_set_hosted_info("host v%d.%d.%d, slave %s v%lu.%lu.%lu",
                             ESP_HOSTED_VERSION_MAJOR_1, ESP_HOSTED_VERSION_MINOR_1, ESP_HOSTED_VERSION_PATCH_1,
                             r2 == ESP_OK ? target : "?", (unsigned long)ver.major1,
                             (unsigned long)ver.minor1, (unsigned long)ver.patch1);
        if (ver.major1 != ESP_HOSTED_VERSION_MAJOR_1 || ver.minor1 != ESP_HOSTED_VERSION_MINOR_1) {
            ESP_LOGW(TAG, "Host/slave ESP-Hosted version mismatch - Wi-Fi may misbehave; consider updating the C5 firmware");
        }
    } else {
        ESP_LOGW(TAG, "Could not read co-processor firmware version (err %d)", r1);
        diag_set_hosted_info("host v%d.%d.%d, slave version unknown (err %d)",
                             ESP_HOSTED_VERSION_MAJOR_1, ESP_HOSTED_VERSION_MINOR_1, ESP_HOSTED_VERSION_PATCH_1, r1);
    }
}

static void start_mdns(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns_init: %s", esp_err_to_name(err));
        return;
    }
    mdns_hostname_set(CONFIG_MYPI_HOSTNAME);
    mdns_instance_name_set("MyPi-Hotspot");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS: %s.local", CONFIG_MYPI_HOSTNAME);
}

#define TRY(expr)                                                                   \
    do {                                                                            \
        esp_err_t _e = (expr);                                                      \
        if (_e != ESP_OK) {                                                         \
            ESP_LOGE(TAG, "[FAIL] %s: %s", #expr, esp_err_to_name(_e));             \
            diag_set_wifi(WIFI_STATE_INIT_FAILED, NULL, 0);                         \
            vTaskDelete(NULL);                                                      \
            return;                                                                 \
        }                                                                           \
        ESP_LOGD(TAG, "[ OK ] %s", #expr);                                          \
    } while (0)

static void wifi_task(void *arg)
{
    diag_set_wifi(WIFI_STATE_INIT, NULL, 0);
    diag_set_wifi_ssid(CONFIG_MYPI_WIFI_SSID);
    s_events = xEventGroupCreate();

    TRY(esp_netif_init());
    TRY(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_netif, CONFIG_MYPI_HOSTNAME);

    ESP_LOGI(TAG, "Initializing Wi-Fi via ESP-Hosted (ESP32-C5 over SDIO); this may take a few seconds");
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    TRY(esp_wifi_init(&cfg));
    log_hosted_info();

    TRY(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    TRY(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));

    wifi_config_t wcfg = {0};
    strlcpy((char *)wcfg.sta.ssid, CONFIG_MYPI_WIFI_SSID, sizeof(wcfg.sta.ssid));
    strlcpy((char *)wcfg.sta.password, CONFIG_MYPI_WIFI_PASSWORD, sizeof(wcfg.sta.password));
    wcfg.sta.threshold.authmode = strlen(CONFIG_MYPI_WIFI_PASSWORD) ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    wcfg.sta.pmf_cfg.capable = true;
    wcfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    TRY(esp_wifi_set_mode(WIFI_MODE_STA));
    TRY(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
    TRY(esp_wifi_start());

    web_start();
    start_mdns();

    // Reconnect loop with exponential back-off; also refresh RSSI periodically.
    uint32_t backoff_s = 1;
    while (true) {
        EventBits_t bits = xEventGroupWaitBits(s_events, BIT_DISCONNECTED | BIT_LOG_RSSI, pdFALSE, pdFALSE,
                                               pdMS_TO_TICKS(5000));
        xEventGroupClearBits(s_events, BIT_DISCONNECTED | BIT_LOG_RSSI);
        if (bits & BIT_DISCONNECTED) {
            ESP_LOGI(TAG, "Reconnecting in %lu s", (unsigned long)backoff_s);
            vTaskDelay(pdMS_TO_TICKS(backoff_s * 1000));
            diag_wifi_retry();
            diag_set_wifi(WIFI_STATE_CONNECTING, NULL, 0);
            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
                xEventGroupSetBits(s_events, BIT_DISCONNECTED);
            }
            backoff_s = backoff_s * 2 > CONFIG_MYPI_WIFI_BACKOFF_MAX_S ? CONFIG_MYPI_WIFI_BACKOFF_MAX_S : backoff_s * 2;
        } else if (xEventGroupGetBits(s_events) & BIT_GOT_IP) {
            backoff_s = 1;
            wifi_ap_record_t ap = {0};
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                diag_status_t st;
                diag_get(&st);
                diag_set_wifi(WIFI_STATE_GOT_IP, st.ip, ap.rssi);
                if (bits & BIT_LOG_RSSI) {
                    ESP_LOGI(TAG, "Connected to \"%s\" ch %d, RSSI %d dBm", (const char *)ap.ssid,
                             ap.primary, ap.rssi);
                }
            }
        }
    }
}

void wifi_start_background(void)
{
    if (xTaskCreate(wifi_task, "wifi_mgr", 6144, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Wi-Fi task");
        diag_set_wifi(WIFI_STATE_INIT_FAILED, NULL, 0);
    }
}
