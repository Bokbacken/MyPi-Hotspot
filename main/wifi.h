#pragma once

/**
 * Start Wi-Fi station mode (ESP32-C5 co-processor via esp_wifi_remote/ESP-Hosted),
 * the web server and mDNS in a background task. Never blocks the caller, and a
 * Wi-Fi failure never affects the display/touch loop.
 */
void wifi_start_background(void);
