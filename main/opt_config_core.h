#pragma once

// Pure logic of the "Optimize" settings: defaults, validation, key/value load/save, reboot-required
// detection, the SDIO trial/fallback decision and the last-STA-channel helpers. No ESP-IDF
// dependencies, so the host test (tests/test_ui_primitives.c) runs it against a mock store;
// main/opt_config.c binds it to NVS.
//
// v0.9.3: the Optimize page was trimmed back. Only TX power, STA power save, AP beacon interval and
// country remain here (band + protocol live in the radio blob, wifi.c). SDIO clock, log level, NAPT
// switch, LwIP core, NAPT table size and NAPT TCP timeout run on fixed defaults; their old NVS keys
// are ignored and erased at boot (OPT_OBSOLETE_KEYS).

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// NVS namespace and keys (uint8_t unless noted).
#define OPT_NVS_NS          "mypi_opt"
#define OPT_KEY_SDIO_MHZ    "sdio_mhz"      // 40 | 50 - no UI since v0.9.3, stored value ignored (40 MHz)
#define OPT_KEY_TX_DBM      "tx_dbm"        // 0 = driver default, else 8..21
#define OPT_KEY_PS_MODE     "ps_mode"       // STA power save: 0 off, 1 min modem, 2 max modem
#define OPT_KEY_BEACON_MS   "beacon_ms"     // uint16_t: AP beacon interval 100 | 200 | 300 (ms ~ TU)
#define OPT_KEY_COUNTRY     "country"       // string: "SE" | "EU" | "US" | "JP"
#define OPT_KEY_SDIO_TRIAL  "sdio_try"      // 1 = 50 MHz boot started but not yet confirmed
#define OPT_KEY_EMERGENCY   "emerg"         // 1 = emergency mode was entered after a C5 failure
#define OPT_KEY_STA_LAST_CH "sta_last_ch"   // v0.9.3: last channel the STA connected on (1-14, 5 GHz 36-177)

// Keys of removed settings, erased once at boot so stale values can never be applied.
static const char *const OPT_OBSOLETE_KEYS[] = {
    "tcp_kb",       // TCP window (v0.9.0, removed v0.9.1)
    "napt",         // NAPT master switch (removed v0.9.3: always on, per AP profile NAT flag)
    "log_lvl",      // global log level (removed v0.9.3: info)
    "lwip_core",    // tcpip core pinning (removed v0.9.3: auto)
    "napt_max",     // NAPT table size (removed v0.9.3: 512)
    "napt_tcp_to",  // NAPT TCP timeout (removed v0.9.3: 2 h)
};
#define OPT_OBSOLETE_KEY_COUNT ((int)(sizeof(OPT_OBSOLETE_KEYS) / sizeof(OPT_OBSOLETE_KEYS[0])))

#define OPT_DEFAULT_SDIO_MHZ    40
#define OPT_DEFAULT_TX_DBM      0
#define OPT_DEFAULT_PS_MODE     0
#define OPT_DEFAULT_BEACON_MS   100
#define OPT_DEFAULT_COUNTRY     "SE"
// Fixed values of the removed settings.
#define OPT_FIXED_NAPT_ENTRIES  512
#define OPT_FIXED_NAPT_TCP_S    7200
#define OPT_FIXED_LOG_LVL       2       // info

typedef struct {
    uint8_t wifi_tx_power_dbm;  // 0 = driver default, 8..21
    uint8_t ps_mode;            // 0..2 == WIFI_PS_NONE / WIFI_PS_MIN_MODEM / WIFI_PS_MAX_MODEM
    uint16_t ap_beacon_ms;      // 100 | 200 | 300
    char country[3];            // OPT_COUNTRIES[].cc
} opt_config_t;

/** Regulatory domain per country code (2.4 GHz channel range, max TX power in dBm). */
typedef struct {
    const char *cc;
    const char *name;
    uint8_t schan, nchan;
    int8_t max_tx_power;
} opt_country_t;

static const opt_country_t OPT_COUNTRIES[] = {
    {"SE", "Sweden", 1, 13, 20},
    {"EU", "Europe", 1, 13, 20},
    {"US", "United States", 1, 11, 30},
    {"JP", "Japan", 1, 14, 23},
};
#define OPT_COUNTRY_COUNT ((int)(sizeof(OPT_COUNTRIES) / sizeof(OPT_COUNTRIES[0])))

/** Entry for a country code, NULL if unknown. */
static inline const opt_country_t *opt_country_find(const char *cc)
{
    for (int i = 0; cc && i < OPT_COUNTRY_COUNT; i++) {
        if (strcmp(OPT_COUNTRIES[i].cc, cc) == 0) return &OPT_COUNTRIES[i];
    }
    return NULL;
}

/** Key/value backend: get returns 0 if the key exists, set/commit return 0 on success. */
typedef struct {
    int (*get_u8)(void *ctx, const char *key, uint8_t *out);
    int (*set_u8)(void *ctx, const char *key, uint8_t v);
    int (*get_u16)(void *ctx, const char *key, uint16_t *out);
    int (*set_u16)(void *ctx, const char *key, uint16_t v);
    /** get_str: out has room for len bytes incl. NUL. */
    int (*get_str)(void *ctx, const char *key, char *out, unsigned len);
    int (*set_str)(void *ctx, const char *key, const char *v);
    int (*commit)(void *ctx);
    void *ctx;
} opt_store_t;

static inline void opt_config_defaults(opt_config_t *c)
{
    c->wifi_tx_power_dbm = OPT_DEFAULT_TX_DBM;
    c->ps_mode = OPT_DEFAULT_PS_MODE;
    c->ap_beacon_ms = OPT_DEFAULT_BEACON_MS;
    memcpy(c->country, OPT_DEFAULT_COUNTRY, sizeof(c->country));
}

static inline bool opt_tx_valid(int v) { return v == 0 || (v >= 2 && v <= 21); }
static inline bool opt_ps_valid(int v) { return v >= 0 && v <= 2; }
static inline bool opt_beacon_valid(int v) { return v == 100 || v == 200 || v == 300; }

static inline bool opt_config_valid(const opt_config_t *c)
{
    return opt_tx_valid(c->wifi_tx_power_dbm) && opt_ps_valid(c->ps_mode) && opt_beacon_valid(c->ap_beacon_ms) &&
           memchr(c->country, 0, sizeof(c->country)) && opt_country_find(c->country);
}

/** Missing or invalid keys fall back to the defaults (field by field); obsolete keys are never read. */
static inline void opt_config_load_from(const opt_store_t *s, opt_config_t *c)
{
    opt_config_defaults(c);
    uint8_t v;
    uint16_t w;
    char cc[sizeof(c->country)];
    if (!s) return;
    if (s->get_u8(s->ctx, OPT_KEY_TX_DBM, &v) == 0 && opt_tx_valid(v)) c->wifi_tx_power_dbm = v;
    if (s->get_u8(s->ctx, OPT_KEY_PS_MODE, &v) == 0 && opt_ps_valid(v)) c->ps_mode = v;
    if (s->get_u16(s->ctx, OPT_KEY_BEACON_MS, &w) == 0 && opt_beacon_valid(w)) c->ap_beacon_ms = w;
    if (s->get_str(s->ctx, OPT_KEY_COUNTRY, cc, sizeof(cc)) == 0 && opt_country_find(cc)) memcpy(c->country, cc, sizeof(cc));
}

/** Returns 0 on success, -1 for an invalid config (nothing written), else the backend error. */
static inline int opt_config_save_to(const opt_store_t *s, const opt_config_t *c)
{
    if (!s || !opt_config_valid(c)) return -1;
    int e;
    if ((e = s->set_u8(s->ctx, OPT_KEY_TX_DBM, c->wifi_tx_power_dbm)) != 0) return e;
    if ((e = s->set_u8(s->ctx, OPT_KEY_PS_MODE, c->ps_mode)) != 0) return e;
    if ((e = s->set_u16(s->ctx, OPT_KEY_BEACON_MS, c->ap_beacon_ms)) != 0) return e;
    if ((e = s->set_str(s->ctx, OPT_KEY_COUNTRY, c->country)) != 0) return e;
    return s->commit(s->ctx);
}

/** AP beacon interval and country are only read at boot; TX power and power save apply at runtime. */
static inline bool opt_config_reboot_required(const opt_config_t *active, const opt_config_t *saved)
{
    return active->ap_beacon_ms != saved->ap_beacon_ms ||
           strncmp(active->country, saved->country, sizeof(active->country)) != 0;
}

/**
 * SDIO clock for this boot. 50 MHz is a trial until the C5 came up (opt_sdio_confirm): if the
 * previous 50 MHz boot never confirmed (hang, crash, power cut during init) the trial flag is
 * still set and this boot falls back to 40 MHz. *fallback = the setting must be rewritten to 40.
 * Since v0.9.3 the configured clock is always 40 (no UI), so this only keeps the mechanism.
 */
static inline uint8_t opt_sdio_boot_mhz(uint8_t configured, bool trial_pending, bool *fallback)
{
    *fallback = configured == 50 && trial_pending;
    return configured == 50 && !trial_pending ? 50 : 40;
}

// --- Last STA channel (v0.9.3) ---
// Band codes match RADIO_BAND_* in wifi.h: 0 auto (both), 1 = 2.4 GHz, 2 = 5 GHz.

/** Valid STA channel: 1-14 or a 20 MHz 5 GHz channel (36-64, 100-144, 149-177, step 4). */
static inline bool opt_sta_ch_valid(int ch)
{
    if (ch >= 1 && ch <= 14) return true;
    if (ch >= 36 && ch <= 64) return (ch - 36) % 4 == 0;
    if (ch >= 100 && ch <= 144) return (ch - 100) % 4 == 0;
    if (ch >= 149 && ch <= 177) return (ch - 149) % 4 == 0;
    return false;
}

/** 1 = 2.4 GHz, 2 = 5 GHz, 0 = invalid channel. */
static inline int opt_ch_band(int ch)
{
    return !opt_sta_ch_valid(ch) ? 0 : ch <= 14 ? 1 : 2;
}

/** The channel is valid, allowed by the band mode and (2.4 GHz) inside the country's channel range. */
static inline bool opt_ch_usable(int ch, int band_mode, int nchan_2g)
{
    int b = opt_ch_band(ch);
    if (b == 0 || (band_mode != 0 && band_mode != b)) return false;
    return b != 1 || nchan_2g <= 0 || ch <= nchan_2g;
}

/** Store the new channel only if it is valid and differs from the stored one (flash wear). */
static inline bool opt_sta_ch_should_store(int stored, int ch)
{
    return opt_sta_ch_valid(ch) && ch != stored;
}

/**
 * Channel for the soft-AP when it is (re)configured: the current STA channel wins (single radio,
 * AP must follow), then the AP's current channel, then the persisted last STA channel if it fits the
 * band mode / country, else 1 (2.4 GHz) or 36 (5 GHz-only band mode).
 */
static inline int opt_ap_channel_pick(int sta_ch, int ap_ch, int persisted, int band_mode, int nchan_2g)
{
    if (sta_ch > 0) return sta_ch;
    if (ap_ch > 0) return ap_ch;
    if (opt_ch_usable(persisted, band_mode, nchan_2g)) return persisted;
    return band_mode == 2 ? 36 : 1;
}

// --- Shared STA/AP protocol (v0.9.3) ---
// Bits == WIFI_PROTOCOL_*: b 0x01, g 0x02, n 0x04, a 0x10, ac 0x20, ax 0x40; 0 = default (b/g/n/a/ac).
#define OPT_PROTO_ALL 0x77

/** Migration of the old separate STA/AP masks: the STA value wins, else the AP value. */
static inline uint8_t opt_proto_merge(uint8_t sta_proto, uint8_t ap_proto)
{
    uint8_t s = sta_proto & OPT_PROTO_ALL, a = ap_proto & OPT_PROTO_ALL;
    return s ? s : a;
}
