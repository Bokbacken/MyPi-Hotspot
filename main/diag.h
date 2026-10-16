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

// OTA progress shown on the LCD and reported in /api/status ("ota" object).
typedef enum {
    OTA_TARGET_P4 = 0,
    OTA_TARGET_C5,
} ota_target_t;

typedef enum {
    OTA_PHASE_IDLE = 0,
    OTA_PHASE_WRITING,      // Receiving + writing flash
    OTA_PHASE_VERIFYING,    // Image check (esp_ota_end / slave OTA end)
    OTA_PHASE_VERIFIED,     // Upload complete, image verified
    OTA_PHASE_DONE,         // Rebooting
    OTA_PHASE_FAILED,
} ota_phase_t;

#define DIAG_OTA_FAIL_SHOW_MS 10000     // How long the LCD keeps showing a failure

typedef struct {
    bool active;            // Transfer/verify in progress or done-and-rebooting
    ota_target_t target;
    ota_phase_t phase;
    int pct;
    uint32_t bytes_done;
    uint32_t bytes_total;
    char err[64];
    int64_t failed_at_us;   // esp_timer time of the failure (0 = none)
    uint32_t seq;           // Bumped on every change
} diag_ota_t;

void diag_ota_begin(ota_target_t target, uint32_t bytes_total);
void diag_ota_progress(uint32_t bytes_done);
void diag_ota_phase(ota_phase_t phase);
void diag_ota_fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void diag_get_ota(diag_ota_t *out);
/** True while the LCD should show the OTA view (active, or failed less than DIAG_OTA_FAIL_SHOW_MS ago). */
bool diag_ota_ui_visible(void);
const char *diag_ota_phase_name(ota_phase_t p);

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
