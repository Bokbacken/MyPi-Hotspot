#pragma once

#include "esp_err.h"

/**
 * Start the HTTP server on port 80:
 *   GET  /            status page (auto-refresh via fetch)
 *   GET  /api/status  JSON status
 *   GET  /api/log     log ring buffer (text/plain)
 *   GET  /update      OTA upload page
 *   POST /update      raw app image (.bin) -> next OTA slot -> reboot
 */
esp_err_t web_start(void);
