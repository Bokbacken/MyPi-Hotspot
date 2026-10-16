#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// Shared runtime status, written by the display/touch/Wi-Fi modules and read by
// the serial heartbeat and the web UI.

#define DIAG_TOUCH_HISTORY  10
#define DIAG_STR_LEN        96

typedef enum {
    WIFI_STATE_OFF = 0,
    WIFI_STATE_INIT,
    WIFI_STATE_INIT_FAILED,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CONNECTED,       // Associated, waiting for DHCP
    WIFI_STATE_GOT_IP,
    WIFI_STATE_DISCONNECTED,
} wifi_state_t;

typedef struct {
    // Display
    char display_status[DIAG_STR_LEN];
    uint32_t spi_clock_hz;
    char lcd_id[DIAG_STR_LEN];          // RDDID / RDDST read result
    uint32_t frames_flushed;
    uint32_t flush_errors;
    // Touch
    char touch_status[DIAG_STR_LEN];
    uint32_t touch_events;
    char touch_history[DIAG_TOUCH_HISTORY][48];
    int touch_history_head;
    // Wi-Fi
    wifi_state_t wifi_state;
    char ip[16];
    int rssi;
    char ssid[33];
    uint32_t wifi_retries;
    char hosted_info[DIAG_STR_LEN];
    // OTA
    char ota_status[DIAG_STR_LEN];
} diag_status_t;

/** Print the boot banner (firmware, chip, reset reason, flash, memory). */
void diag_print_boot_banner(void);

/** Start the periodic serial heartbeat task. */
void diag_start_heartbeat(void);

/**
 * Debug mode: DEBUG-level app logs, 5 s heartbeat and the boot-time checks
 * (wiring check, panel ID read, I2C scan, test pattern). Stored in NVS.
 * diag_debug_load() must run after nvs_flash_init().
 */
void diag_debug_load(void);
bool diag_debug_enabled(void);
esp_err_t diag_set_debug(bool on);

/** Thread-safe copy of the current status. */
void diag_get(diag_status_t *out);

const char *diag_wifi_state_name(wifi_state_t s);
const char *diag_reset_reason_name(void);

// Setters (thread-safe)
void diag_set_display_status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void diag_set_lcd_id(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void diag_set_touch_status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void diag_set_hosted_info(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void diag_set_ota_status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void diag_set_spi_clock(uint32_t hz);
void diag_frame_flushed(bool ok);
void diag_touch_event(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void diag_set_wifi(wifi_state_t state, const char *ip, int rssi);
void diag_set_wifi_ssid(const char *ssid);
void diag_wifi_retry(void);
