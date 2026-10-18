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
/** Emergency mode: take the AP interface down at once and stop the AP radio in the background. */
void wifi_rescue_stop_ap(void);
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
#define RADIO_BW_MAX RADIO_BW_40     // the C5 supports max 40 MHz; RADIO_BW_80 kept for NVS compat
enum { RADIO_BAND_AUTO = 0, RADIO_BAND_2G = 1, RADIO_BAND_5G = 2 };

// Protocol bitmask (same bits as WIFI_PROTOCOL_*); 0 = driver default
#define RADIO_PROTO_B   0x01
#define RADIO_PROTO_G   0x02
#define RADIO_PROTO_N   0x04
#define RADIO_PROTO_A   0x10
#define RADIO_PROTO_AC  0x20
#define RADIO_PROTO_AX  0x40
#define RADIO_PROTO_ALL 0x77
#define RADIO_PROTO_ALLOWED 0x77      // v0.9.3: 11ax selectable again (known C5 fw upload issue, ESP-IDF #19238)
#define RADIO_PROTO_DEFAULT 0x37      // used for mask 0: b/g/n + a/ac (no ax)

// v0.9.3: only band_mode and the shared protocol mask are settings. sta_proto == ap_proto always
// (one selector, migrated with opt_proto_merge); sta_bw/ap_bw/tx_power_dbm/ap_dtim are ignored
// (forced to auto/0 at load) and kept only for the NVS blob layout.
typedef struct {
    uint8_t sta_bw;                 // ignored since v0.9.3 (always auto: 2.4 GHz HT20, 5 GHz HT40)
    uint8_t ap_bw;                  // ignored since v0.9.3
    uint8_t sta_proto;              // RADIO_PROTO_* bitmask, 0 = default (RADIO_PROTO_DEFAULT)
    uint8_t ap_proto;               // == sta_proto since v0.9.3
    uint8_t band_mode;              // RADIO_BAND_*
    uint8_t tx_power_dbm;           // ignored since v0.9.3 (Optimize "tx_dbm" is the only TX setting)
    uint8_t ap_beacon_interval_tu;  // unused since v0.9.1 (Optimize "beacon_ms"); kept for NVS blob layout
    uint8_t ap_dtim;                // ignored since v0.9.3 (driver default DTIM)
    uint8_t _pad[8];
} wifi_radio_cfg_t;

typedef struct {
    wifi_radio_cfg_t active;    // settings used since boot
    wifi_radio_cfg_t saved;     // settings in NVS (applied after reboot)
    int sta_channel, ap_channel;
    int sta_bw_mhz, ap_bw_mhz;  // width configured on the C5 for the current band (0 = unknown)
    char sta_proto[16], ap_proto[16];
    uint8_t sta_proto_actual, ap_proto_actual;  // WIFI_PROTOCOL_* bits for the current band
    int tx_power_dbm;           // max TX power reported by the driver (0 = unknown)
    int up_second;              // upstream AP secondary channel: 0 none, 1 above, 2 below
    char up_phy[16];            // upstream AP PHY capabilities (from its beacon)
} wifi_radio_info_t;

const char *wifi_radio_bw_name(uint8_t v);
/** Parse "auto"/"20"/"40"/"80"; -1 if invalid. */
int wifi_radio_bw_parse(const char *s);
/** Save radio settings to NVS; they take effect after a reboot. */
esp_err_t wifi_radio_save(const wifi_radio_cfg_t *c);
void wifi_get_radio_info(wifi_radio_info_t *out);

// --- Co-processor (ESP32-C5 running ESP-Hosted slave firmware) ---
typedef struct {
    bool fw_valid;              // slave firmware version could be read
    uint32_t fw_major, fw_minor, fw_patch;
    bool cp_valid;              // chip id / target name could be read
    uint32_t chip_id;           // esp_chip_id_t of the co-processor (ESP32-C5 = 23)
    char target[16];            // e.g. "esp32c5"
    int host_major, host_minor, host_patch;     // ESP-Hosted host component version
    bool version_match;         // host and slave major.minor equal
    bool wifi_init_ok;          // esp_wifi_init() succeeded
    uint8_t sta_mac[6], ap_mac[6];
    bool mac_valid;
    int sdio_clk, sdio_cmd, sdio_d0, sdio_d1, sdio_d2, sdio_d3, reset_gpio;
    int sdio_freq_khz;
} wifi_coproc_info_t;

void wifi_get_coproc_info(wifi_coproc_info_t *out);
/** Re-read the slave firmware version / chip info (RPC). */
void wifi_coproc_refresh(void);
/**
 * Serialize co-processor RPCs (shared with scan/config/connect). Works before
 * Wi-Fi init too (returns true if no lock exists yet). Timeout in ms.
 */
bool wifi_rpc_lock(uint32_t timeout_ms);
void wifi_rpc_unlock(void);

/** Optimize tab (v0.9.0): re-apply the TX power cap (opt override, else radio setting) now. */
void wifi_apply_tx_power(void);
/** Optimize tab (v0.9.0): re-apply NAT on the running AP (profile NAT && opt master switch). */
void wifi_apply_napt(void);
/** Optimize tab (v0.9.1): re-apply the STA power save mode (esp_wifi_set_ps) now. */
void wifi_apply_ps(void);
