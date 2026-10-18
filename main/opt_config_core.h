#pragma once

// Pure logic of the "Optimize" settings (v0.9.0): defaults, validation, key/value load/save,
// reboot-required detection and the SDIO 50 MHz trial/fallback decision. No ESP-IDF
// dependencies, so the host test (tests/test_ui_primitives.c) runs it against a mock store;
// main/opt_config.c binds it to NVS.

#include <stdbool.h>
#include <stdint.h>

// NVS namespace and keys (all uint8_t).
#define OPT_NVS_NS          "mypi_opt"
#define OPT_KEY_SDIO_MHZ    "sdio_mhz"      // 40 | 50
#define OPT_KEY_TX_DBM      "tx_dbm"        // 0 = radio setting / driver default, else 2..21
#define OPT_KEY_TCP_KB      "tcp_kb"        // 32 | 64 | 128
#define OPT_KEY_NAPT        "napt"          // 0 | 1 (master switch on top of the AP profile NAT flag)
#define OPT_KEY_LOG_LVL     "log_lvl"       // 0 error, 1 warn, 2 info, 3 debug
#define OPT_KEY_SDIO_TRIAL  "sdio_try"      // 1 = 50 MHz boot started but not yet confirmed
#define OPT_KEY_EMERGENCY   "emerg"         // 1 = emergency mode was entered after a C5 failure

#define OPT_DEFAULT_SDIO_MHZ    40
#define OPT_DEFAULT_TX_DBM      0
#define OPT_DEFAULT_TCP_KB      64
#define OPT_DEFAULT_NAPT        true
#define OPT_DEFAULT_LOG_LVL     2

typedef struct {
    uint8_t sdio_freq_mhz;
    uint8_t wifi_tx_power_dbm;
    uint8_t tcp_window_kb;
    bool napt;
    uint8_t log_level;
} opt_config_t;

/** Key/value backend: get returns 0 if the key exists, set/commit return 0 on success. */
typedef struct {
    int (*get_u8)(void *ctx, const char *key, uint8_t *out);
    int (*set_u8)(void *ctx, const char *key, uint8_t v);
    int (*commit)(void *ctx);
    void *ctx;
} opt_store_t;

static inline void opt_config_defaults(opt_config_t *c)
{
    c->sdio_freq_mhz = OPT_DEFAULT_SDIO_MHZ;
    c->wifi_tx_power_dbm = OPT_DEFAULT_TX_DBM;
    c->tcp_window_kb = OPT_DEFAULT_TCP_KB;
    c->napt = OPT_DEFAULT_NAPT;
    c->log_level = OPT_DEFAULT_LOG_LVL;
}

static inline bool opt_sdio_valid(int v) { return v == 40 || v == 50; }
static inline bool opt_tx_valid(int v) { return v == 0 || (v >= 2 && v <= 21); }
static inline bool opt_tcp_valid(int v) { return v == 32 || v == 64 || v == 128; }
static inline bool opt_log_valid(int v) { return v >= 0 && v <= 3; }

static inline bool opt_config_valid(const opt_config_t *c)
{
    return opt_sdio_valid(c->sdio_freq_mhz) && opt_tx_valid(c->wifi_tx_power_dbm) &&
           opt_tcp_valid(c->tcp_window_kb) && opt_log_valid(c->log_level);
}

/** Missing or invalid keys fall back to the defaults (field by field). */
static inline void opt_config_load_from(const opt_store_t *s, opt_config_t *c)
{
    opt_config_defaults(c);
    uint8_t v;
    if (!s) return;
    if (s->get_u8(s->ctx, OPT_KEY_SDIO_MHZ, &v) == 0 && opt_sdio_valid(v)) c->sdio_freq_mhz = v;
    if (s->get_u8(s->ctx, OPT_KEY_TX_DBM, &v) == 0 && opt_tx_valid(v)) c->wifi_tx_power_dbm = v;
    if (s->get_u8(s->ctx, OPT_KEY_TCP_KB, &v) == 0 && opt_tcp_valid(v)) c->tcp_window_kb = v;
    if (s->get_u8(s->ctx, OPT_KEY_NAPT, &v) == 0 && v <= 1) c->napt = v != 0;
    if (s->get_u8(s->ctx, OPT_KEY_LOG_LVL, &v) == 0 && opt_log_valid(v)) c->log_level = v;
}

/** Returns 0 on success, -1 for an invalid config (nothing written), else the backend error. */
static inline int opt_config_save_to(const opt_store_t *s, const opt_config_t *c)
{
    if (!s || !opt_config_valid(c)) return -1;
    int e;
    if ((e = s->set_u8(s->ctx, OPT_KEY_SDIO_MHZ, c->sdio_freq_mhz)) != 0) return e;
    if ((e = s->set_u8(s->ctx, OPT_KEY_TX_DBM, c->wifi_tx_power_dbm)) != 0) return e;
    if ((e = s->set_u8(s->ctx, OPT_KEY_TCP_KB, c->tcp_window_kb)) != 0) return e;
    if ((e = s->set_u8(s->ctx, OPT_KEY_NAPT, c->napt ? 1 : 0)) != 0) return e;
    if ((e = s->set_u8(s->ctx, OPT_KEY_LOG_LVL, c->log_level)) != 0) return e;
    return s->commit(s->ctx);
}

/** SDIO clock and TCP window are only read at boot; the other fields apply at runtime. */
static inline bool opt_config_reboot_required(const opt_config_t *active, const opt_config_t *saved)
{
    return active->sdio_freq_mhz != saved->sdio_freq_mhz || active->tcp_window_kb != saved->tcp_window_kb;
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
