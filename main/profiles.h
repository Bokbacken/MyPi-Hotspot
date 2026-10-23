#pragma once

// Wi-Fi station (uplink) and access point profiles stored in NVS.
// Same concept as MyPi-Companion: 10 STA slots, 4 AP slots.

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define STA_PROFILE_MAX     10
#define AP_PROFILE_MAX      4
#define AP_SUBNET_COUNT     4

// v0.11.0 STA security (minimum accepted by the station). AUTO = legacy profiles: WPA/WPA2 PSK
// minimum when a password is set, otherwise open. Values match WQ_SEC_* in wifi_qr_core.h.
#define STA_SEC_AUTO        0
#define STA_SEC_OPEN        1
#define STA_SEC_WPA         2   // WPA/WPA2 personal (accept WPA-PSK and better)
#define STA_SEC_WPA2        3   // WPA2 personal or better
#define STA_SEC_WPA3        4   // WPA3-SAE only

typedef struct {
    bool valid;
    char ssid[33];
    char pass[65];      // 8-63 char passphrase or 64 hex-digit PSK; empty = open
    uint8_t security;   // STA_SEC_* (v0.11.0, NVS key s<n>sec; missing = AUTO)
    bool dhcp;          // false = static IP below
    char ip[16];
    char mask[16];
    char gw[16];
    char dns[16];
} sta_profile_t;

typedef struct {
    bool valid;
    char ssid[33];
    char pass[65];      // empty = open network, otherwise >= 8 chars (WPA2)
    uint8_t subnet;     // index into ap_subnet_base (0-3)
    bool nat;           // share the uplink (NAPT) with AP clients
    bool on_boot;       // start this profile at boot (only one slot)
} ap_profile_t;

/** First three octets of each AP subnet; the AP is always .1/24. */
extern const uint8_t AP_SUBNET_BASE[AP_SUBNET_COUNT][3];

/** Load both profile tables from NVS (call after nvs_flash_init). */
void profiles_load(void);

/** Copies of a slot (returns false if the index is out of range). */
bool sta_profile_get(int slot, sta_profile_t *out);
bool ap_profile_get(int slot, ap_profile_t *out);

/** Save a slot; validates fields. */
esp_err_t sta_profile_save(int slot, const sta_profile_t *p);
esp_err_t ap_profile_save(int slot, const ap_profile_t *p);   // clears on_boot on other slots if set

esp_err_t sta_profile_delete(int slot);
esp_err_t ap_profile_delete(int slot);

int sta_profile_count(void);
/** Slot of the saved STA profile matching ssid, or -1. */
int sta_profile_find(const char *ssid);
/** Slot of the AP profile flagged on_boot, or -1. */
int ap_profile_boot_slot(void);
