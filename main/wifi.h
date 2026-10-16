#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * Start Wi-Fi (ESP32-C5 co-processor via esp_wifi_remote/ESP-Hosted) in APSTA
 * mode, Ethernet, the web server and mDNS in a background task. Never blocks the
 * caller, and a Wi-Fi failure never affects the display/touch loop.
 */
void wifi_start_background(void);

// --- Station (uplink) ---
typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t channel;
    int auth;           // wifi_auth_mode_t
    int profile;        // matching saved STA profile slot, or -1
} wifi_scan_entry_t;

/** Blocking scan (2-4 s). Returns the number of entries, or a negative value on error. */
int wifi_scan(wifi_scan_entry_t *out, int max);
/** Connect to a saved profile (slot), or -1 = automatic (best RSSI). */
esp_err_t wifi_connect_profile(int slot);
/** Disconnect and stay disconnected until wifi_connect_profile() is called. */
esp_err_t wifi_disconnect(void);

// --- Access point ---
typedef struct {
    bool apsta_supported;   // false if APSTA mode was rejected (STA-only fallback)
    bool running;
    int slot;               // AP profile slot, -1 = Kconfig default AP
    char ssid[33];
    bool open;
    char ip[16];
    bool nat;
    int channel;            // channel the AP actually uses (read from the C5)
    int sta_channel;        // STA uplink channel, 0 if not connected
    int clients;
    int max_clients;
} wifi_ap_info_t;

typedef struct {
    uint8_t mac[6];
    char ip[16];
    int rssi;
} wifi_client_t;

/** Start the AP with a saved profile (slot) or -1 = Kconfig default. Replaces a running AP. */
esp_err_t wifi_ap_start(int slot);
esp_err_t wifi_ap_stop(void);
void wifi_get_ap_info(wifi_ap_info_t *out);
/** Connected AP stations (RPC to the co-processor). Returns count. */
int wifi_ap_clients(wifi_client_t *out, int max);

// --- Summary ---
typedef struct {
    int sta_profile;        // slot in use, -1 = none/Kconfig fallback
    bool sta_manual_off;
    char uplink[8];         // "eth", "sta" or "none"
} wifi_sta_extra_t;

void wifi_get_sta_extra(wifi_sta_extra_t *out);

// --- Radio settings (stored in NVS, applied at boot) ---
enum { RADIO_BW_AUTO = 0, RADIO_BW_20 = 1, RADIO_BW_40 = 2, RADIO_BW_80 = 3 };    // == WIFI_BW20/40/80
enum { RADIO_PROTO_DEFAULT = 0, RADIO_PROTO_N = 1, RADIO_PROTO_AC = 2, RADIO_PROTO_AX = 3 };

typedef struct {
    uint8_t sta_bw;         // RADIO_BW_* (auto = driver default / negotiated)
    uint8_t ap_bw;          // RADIO_BW_20..80
    uint8_t sta_proto;      // RADIO_PROTO_*
    uint8_t ap_proto;       // RADIO_PROTO_*
} wifi_radio_cfg_t;

typedef struct {
    wifi_radio_cfg_t active;    // settings used since boot
    wifi_radio_cfg_t saved;     // settings in NVS (applied after reboot)
    int sta_channel, ap_channel;
    int sta_bw_mhz, ap_bw_mhz;  // width configured on the C5 for the current band (0 = unknown)
    char sta_proto[16], ap_proto[16];
    int up_second;              // upstream AP secondary channel: 0 none, 1 above, 2 below
    char up_phy[16];            // upstream AP PHY capabilities (from its beacon)
} wifi_radio_info_t;

const char *wifi_radio_bw_name(uint8_t v);
const char *wifi_radio_proto_name(uint8_t v);
/** Parse "auto"/"20"/"40"/"80" resp. "default"/"n"/"ac"/"ax"; -1 if invalid. */
int wifi_radio_bw_parse(const char *s);
int wifi_radio_proto_parse(const char *s);
/** Save radio settings to NVS; they take effect after a reboot. */
esp_err_t wifi_radio_save(const wifi_radio_cfg_t *c);
void wifi_get_radio_info(wifi_radio_info_t *out);
