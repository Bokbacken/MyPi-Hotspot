#pragma once

// Emergency (rescue) mode: Ethernet-only access when Wi-Fi / the ESP32-C5 is
// unusable. Entered by a swipe down on the LCD within RESCUE_SWIPE_WINDOW_S of
// boot, or automatically when the C5 co-processor fails. The LCD then shows 5
// choices for the Ethernet port: DHCP client, or a static <AP subnet>.32/24 for
// each of the 4 AP subnets (AP_SUBNET_BASE). The Wi-Fi AP is stopped (avoids a
// subnet clash); the web UI stays reachable on the Ethernet IP. Runtime only:
// a reboot returns to normal operation.

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define RESCUE_SWIPE_WINDOW_S   10
#define RESCUE_HOST_OCTET       32      // static IP = a.b.c.32
#define RESCUE_OPT_DHCP         0       // options 1..4 = AP subnet 0..3, static
#define RESCUE_OPT_COUNT        5

typedef enum {
    RESCUE_REASON_NONE = 0,
    RESCUE_REASON_SWIPE,            // user swiped down on the LCD after boot
    RESCUE_REASON_C5_INIT_FAIL,     // esp_wifi_init()/Wi-Fi start failed
    RESCUE_REASON_C5_INIT_TIMEOUT,  // esp_wifi_init() did not return in time
    RESCUE_REASON_C5_TRANSPORT,     // ESP-Hosted reported an SDIO transport failure
    RESCUE_REASON_C5_NO_RESPONSE,   // periodic RPC health check failed repeatedly
} rescue_reason_t;

typedef struct {
    bool active;
    rescue_reason_t reason;
    int option;         // selected option (RESCUE_OPT_*), -1 = none yet
    bool busy;          // the selected option is being applied
    esp_err_t err;      // result of the last apply (ESP_OK if fine)
} rescue_state_t;

/** Enter rescue mode (idempotent, callable from any task, not from an ISR). */
void rescue_request(rescue_reason_t reason);
bool rescue_active(void);
/** Select an option (applied asynchronously by the rescue task). */
void rescue_select(int option);
void rescue_get(rescue_state_t *out);
const char *rescue_reason_text(rescue_reason_t r);
/** "DHCP" or "a.b.c.32" for the option. */
void rescue_option_label(int option, char *buf, size_t n);
/** Seconds left of the boot-time swipe window (0 = closed). */
int rescue_swipe_seconds_left(void);
