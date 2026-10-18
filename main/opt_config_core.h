#pragma once

// Pure logic of the "Optimize" settings (v0.9.0, C5 radio fields v0.9.1, LwIP/NAT fields v0.9.2): defaults, validation, key/value load/save,
// reboot-required detection and the SDIO 50 MHz trial/fallback decision. No ESP-IDF
// dependencies, so the host test (tests/test_ui_primitives.c) runs it against a mock store;
// main/opt_config.c binds it to NVS.

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// NVS namespace and keys (uint8_t unless noted).
#define OPT_NVS_NS          "mypi_opt"
#define OPT_KEY_SDIO_MHZ    "sdio_mhz"      // 40 | 50
#define OPT_KEY_TX_DBM      "tx_dbm"        // 0 = radio setting / driver default, else 2..21
#define OPT_KEY_NAPT        "napt"          // 0 | 1 (master switch on top of the AP profile NAT flag)
#define OPT_KEY_LOG_LVL     "log_lvl"       // 0 error, 1 warn, 2 info, 3 debug
#define OPT_KEY_PS_MODE     "ps_mode"       // STA power save: 0 off, 1 min modem, 2 max modem
#define OPT_KEY_BEACON_MS   "beacon_ms"     // uint16_t: AP beacon interval 100 | 200 | 300 (ms ~ TU)
#define OPT_KEY_COUNTRY     "country"       // string: "SE" | "EU" | "US" | "JP"
#define OPT_KEY_LWIP_CORE   "lwip_core"     // tcpip task pinning: 0 auto (no pin), 1 core 0, 2 core 1
#define OPT_KEY_NAPT_MAX    "napt_max"      // NAPT table size code: 5 = 512, 10 = 1024, 20 = 2048 entries
#define OPT_KEY_NAPT_TCP_TO "napt_tcp_to"   // NAPT idle TCP timeout code: 2 = 2 h, 1 = 1 h, 0 = 30 min
#define OPT_KEY_TCP_KB_OLD  "tcp_kb"        // removed in v0.9.1 (32 KB build-time window); erased at boot
#define OPT_KEY_SDIO_TRIAL  "sdio_try"      // 1 = 50 MHz boot started but not yet confirmed
#define OPT_KEY_EMERGENCY   "emerg"         // 1 = emergency mode was entered after a C5 failure

#define OPT_DEFAULT_SDIO_MHZ    40
#define OPT_DEFAULT_TX_DBM      0
#define OPT_DEFAULT_NAPT        true
#define OPT_DEFAULT_LOG_LVL     2
#define OPT_DEFAULT_PS_MODE     0
#define OPT_DEFAULT_BEACON_MS   100
#define OPT_DEFAULT_COUNTRY     "SE"
#define OPT_DEFAULT_LWIP_CORE   0
#define OPT_DEFAULT_NAPT_MAX    5
#define OPT_DEFAULT_NAPT_TCP_TO 2

typedef struct {
    uint8_t sdio_freq_mhz;
    uint8_t wifi_tx_power_dbm;
    bool napt;
    uint8_t log_level;
    uint8_t ps_mode;            // 0..2 == WIFI_PS_NONE / WIFI_PS_MIN_MODEM / WIFI_PS_MAX_MODEM
    uint16_t ap_beacon_ms;      // 100 | 200 | 300
    char country[3];            // OPT_COUNTRIES[].cc
    uint8_t lwip_core;          // 0 auto, 1 core 0, 2 core 1
    uint8_t napt_max;           // 5 | 10 | 20 (see opt_napt_max_entries)
    uint8_t napt_tcp_to;        // 2 | 1 | 0 (see opt_napt_tcp_timeout_s)
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
    c->sdio_freq_mhz = OPT_DEFAULT_SDIO_MHZ;
    c->wifi_tx_power_dbm = OPT_DEFAULT_TX_DBM;
    c->napt = OPT_DEFAULT_NAPT;
    c->log_level = OPT_DEFAULT_LOG_LVL;
    c->ps_mode = OPT_DEFAULT_PS_MODE;
    c->ap_beacon_ms = OPT_DEFAULT_BEACON_MS;
    memcpy(c->country, OPT_DEFAULT_COUNTRY, sizeof(c->country));
    c->lwip_core = OPT_DEFAULT_LWIP_CORE;
    c->napt_max = OPT_DEFAULT_NAPT_MAX;
    c->napt_tcp_to = OPT_DEFAULT_NAPT_TCP_TO;
}

static inline bool opt_sdio_valid(int v) { return v == 40 || v == 50; }
static inline bool opt_tx_valid(int v) { return v == 0 || (v >= 2 && v <= 21); }
static inline bool opt_log_valid(int v) { return v >= 0 && v <= 3; }
static inline bool opt_ps_valid(int v) { return v >= 0 && v <= 2; }
static inline bool opt_beacon_valid(int v) { return v == 100 || v == 200 || v == 300; }
static inline bool opt_lwip_core_valid(int v) { return v >= 0 && v <= 2; }
static inline bool opt_napt_max_valid(int v) { return v == 5 || v == 10 || v == 20; }
static inline bool opt_napt_tcp_to_valid(int v) { return v >= 0 && v <= 2; }

/** NAPT table entries for a napt_max code (5/10/20 -> 512/1024/2048), default size for unknown codes. */
static inline uint16_t opt_napt_max_entries(uint8_t code)
{
    return code == 20 ? 2048 : code == 10 ? 1024 : 512;
}

/** NAPT idle TCP session timeout in seconds for a napt_tcp_to code (2 -> 2 h, 1 -> 1 h, 0 -> 30 min). */
static inline uint32_t opt_napt_tcp_timeout_s(uint8_t code)
{
    return code == 0 ? 1800 : code == 1 ? 3600 : 7200;
}

/** FreeRTOS core id for a lwip_core code: -1 = no pinning (tskNO_AFFINITY), else 0 or 1. */
static inline int opt_lwip_core_id(uint8_t code)
{
    return code == 1 ? 0 : code == 2 ? 1 : -1;
}

static inline bool opt_config_valid(const opt_config_t *c)
{
    return opt_sdio_valid(c->sdio_freq_mhz) && opt_tx_valid(c->wifi_tx_power_dbm) &&
           opt_log_valid(c->log_level) && opt_ps_valid(c->ps_mode) && opt_beacon_valid(c->ap_beacon_ms) &&
           memchr(c->country, 0, sizeof(c->country)) && opt_country_find(c->country) &&
           opt_lwip_core_valid(c->lwip_core) && opt_napt_max_valid(c->napt_max) && opt_napt_tcp_to_valid(c->napt_tcp_to);
}

/** Missing or invalid keys fall back to the defaults (field by field). */
static inline void opt_config_load_from(const opt_store_t *s, opt_config_t *c)
{
    opt_config_defaults(c);
    uint8_t v;
    uint16_t w;
    char cc[sizeof(c->country)];
    if (!s) return;
    if (s->get_u8(s->ctx, OPT_KEY_SDIO_MHZ, &v) == 0 && opt_sdio_valid(v)) c->sdio_freq_mhz = v;
    if (s->get_u8(s->ctx, OPT_KEY_TX_DBM, &v) == 0 && opt_tx_valid(v)) c->wifi_tx_power_dbm = v;
    if (s->get_u8(s->ctx, OPT_KEY_NAPT, &v) == 0 && v <= 1) c->napt = v != 0;
    if (s->get_u8(s->ctx, OPT_KEY_LOG_LVL, &v) == 0 && opt_log_valid(v)) c->log_level = v;
    if (s->get_u8(s->ctx, OPT_KEY_PS_MODE, &v) == 0 && opt_ps_valid(v)) c->ps_mode = v;
    if (s->get_u16(s->ctx, OPT_KEY_BEACON_MS, &w) == 0 && opt_beacon_valid(w)) c->ap_beacon_ms = w;
    if (s->get_str(s->ctx, OPT_KEY_COUNTRY, cc, sizeof(cc)) == 0 && opt_country_find(cc)) memcpy(c->country, cc, sizeof(cc));
    if (s->get_u8(s->ctx, OPT_KEY_LWIP_CORE, &v) == 0 && opt_lwip_core_valid(v)) c->lwip_core = v;
    if (s->get_u8(s->ctx, OPT_KEY_NAPT_MAX, &v) == 0 && opt_napt_max_valid(v)) c->napt_max = v;
    if (s->get_u8(s->ctx, OPT_KEY_NAPT_TCP_TO, &v) == 0 && opt_napt_tcp_to_valid(v)) c->napt_tcp_to = v;
}

/** Returns 0 on success, -1 for an invalid config (nothing written), else the backend error. */
static inline int opt_config_save_to(const opt_store_t *s, const opt_config_t *c)
{
    if (!s || !opt_config_valid(c)) return -1;
    int e;
    if ((e = s->set_u8(s->ctx, OPT_KEY_SDIO_MHZ, c->sdio_freq_mhz)) != 0) return e;
    if ((e = s->set_u8(s->ctx, OPT_KEY_TX_DBM, c->wifi_tx_power_dbm)) != 0) return e;
    if ((e = s->set_u8(s->ctx, OPT_KEY_NAPT, c->napt ? 1 : 0)) != 0) return e;
    if ((e = s->set_u8(s->ctx, OPT_KEY_LOG_LVL, c->log_level)) != 0) return e;
    if ((e = s->set_u8(s->ctx, OPT_KEY_PS_MODE, c->ps_mode)) != 0) return e;
    if ((e = s->set_u16(s->ctx, OPT_KEY_BEACON_MS, c->ap_beacon_ms)) != 0) return e;
    if ((e = s->set_str(s->ctx, OPT_KEY_COUNTRY, c->country)) != 0) return e;
    if ((e = s->set_u8(s->ctx, OPT_KEY_LWIP_CORE, c->lwip_core)) != 0) return e;
    if ((e = s->set_u8(s->ctx, OPT_KEY_NAPT_MAX, c->napt_max)) != 0) return e;
    if ((e = s->set_u8(s->ctx, OPT_KEY_NAPT_TCP_TO, c->napt_tcp_to)) != 0) return e;
    return s->commit(s->ctx);
}

/**
 * SDIO clock, AP beacon interval, country, LwIP core and NAPT table size are only read at boot; the
 * other fields (incl. the NAPT TCP timeout) apply at runtime.
 */
static inline bool opt_config_reboot_required(const opt_config_t *active, const opt_config_t *saved)
{
    return active->sdio_freq_mhz != saved->sdio_freq_mhz || active->ap_beacon_ms != saved->ap_beacon_ms ||
           strncmp(active->country, saved->country, sizeof(active->country)) != 0 ||
           active->lwip_core != saved->lwip_core || active->napt_max != saved->napt_max;
}

/**
 * SDIO clock for this boot. 50 MHz is a trial until the C5 came up (opt_sdio_confirm): if the
 * previous 50 MHz boot never confirmed (hang, crash, power cut during init) the trial flag is
 * still set and this boot falls back to 40 MHz. *fallback = the setting must be rewritten to 40.
 */
static inline uint8_t opt_sdio_boot_mhz(uint8_t configured, bool trial_pending, bool *fallback)
{
    *fallback = configured == 50 && trial_pending;
    return configured == 50 && !trial_pending ? 50 : 40;
}
