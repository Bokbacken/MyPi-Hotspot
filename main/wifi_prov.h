#pragma once

// v0.11.0 Wi-Fi QR import: decode a WIFI: QR code with the MyPi camera (on-device, quirc), review
// it against the current connection and a network scan, then save it as a STA profile only, or
// save it and connect with automatic rollback to the previous connection on failure.
//
// One job runs at a time in a short-lived worker task; the web UI polls wifi_prov_status_json().
// The decoded password stays on the device (status reports its length only), is never logged,
// and is wiped when the candidate is discarded, committed or expires.

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "esp_err.h"

/** Capture camera frames and look for a Wi-Fi QR code (replaces any pending candidate). */
esp_err_t wifi_prov_scan_camera(void);
/** Review WIFI: text from another trusted reader (same validation as a camera scan). */
esp_err_t wifi_prov_submit_text(const char *payload, size_t len);
/** Scan the available networks again for the pending candidate. */
esp_err_t wifi_prov_rescan(void);
/**
 * Save the pending candidate as a STA profile (slot -1 = automatic; overwrite confirms replacing
 * another network) and, if connect, switch to it with rollback to the previous connection.
 */
esp_err_t wifi_prov_commit(bool connect, int slot, bool overwrite);
/** Cancel a running connect attempt (the previous connection is restored), or discard a review. */
esp_err_t wifi_prov_cancel(void);
/** Forget the pending candidate and the last result. */
esp_err_t wifi_prov_discard(void);
/** Status for GET /api/wifiqr/status (caller deletes). Never contains the password. */
cJSON *wifi_prov_status_json(void);
