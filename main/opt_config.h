#pragma once

// "Optimize" settings (v0.9.0), NVS-backed (namespace OPT_NVS_NS, see opt_config_core.h):
//   sdio_freq_mhz     40/50 MHz SDIO clock to the C5 (boot only; 50 MHz falls back to 40 automatically)
//   wifi_tx_power_dbm max TX power, 0 = keep the radio setting (runtime)
//   napt              NAT master switch for the AP (runtime)
//   log_level         0 error .. 3 debug for all tags (runtime)
//   ps_mode           STA power save 0 off / 1 min / 2 max modem, esp_wifi_set_ps() (runtime)
//   ap_beacon_ms      AP beacon interval 100/200/300 (boot: wifi_config_t.ap.beacon_interval)
//   country           "SE"/"EU"/"US"/"JP", esp_wifi_set_country() before esp_wifi_start() (boot)
//   lwip_core         tcpip task pinning 0 auto / 1 core 0 / 2 core 1 (boot: sys_thread_new wrapper)
//   napt_max          NAPT table 5/10/20 = 512/1024/2048 entries (boot: table allocated on first NAPT enable)
//   napt_tcp_to       NAPT idle TCP timeout 2 = 2 h / 1 = 1 h / 0 = 30 min (runtime, read by the NAPT GC)
// The TCP window is no longer a setting (v0.9.1): fixed 32 KB in sdkconfig.defaults.
// Plus the emergency flag (set when emergency mode was entered after a C5 failure).

#include <stdbool.h>

#include "esp_err.h"
#include "esp_log.h"
#include "opt_config_core.h"

/** Read the settings from NVS (needs nvs_flash_init) and apply the log level. Call once at boot. */
void opt_config_load(void);
/** Settings in use since boot / settings stored in NVS. */
void opt_config_get_active(opt_config_t *out);
void opt_config_get_saved(opt_config_t *out);
/**
 * Validate, store and apply the runtime fields (TX power, NAPT, log level, STA power save).
 * *reboot_required = a boot-only field differs from the active value.
 */
esp_err_t opt_config_save(const opt_config_t *c, bool *reboot_required);
bool opt_config_reboot_pending(void);
/** NAT master switch (active value). */
bool opt_napt_enabled(void);
/** TX power override in dBm (active value), 0 = use the radio setting. */
uint8_t opt_tx_power_dbm(void);
/** STA power save mode 0..2 (== wifi_ps_type_t), active value. */
uint8_t opt_ps_mode(void);
/** AP beacon interval in ms/TU (100/200/300), value read at boot. */
uint16_t opt_ap_beacon_ms(void);
/** Regulatory domain read at boot (never NULL). */
const opt_country_t *opt_country(void);
/** LwIP core / NAPT table size read at boot (codes as stored in NVS). */
uint8_t opt_lwip_core(void);
uint8_t opt_napt_max(void);
/** Global log level mapped to esp_log_level_t (active value). */
esp_log_level_t opt_log_esp_level(void);

// --- SDIO clock trial / fallback (wifi.c, rescue.c) ---
/** Clock for this boot in kHz; marks a 50 MHz boot as a trial in NVS. */
uint32_t opt_sdio_begin(void);
/** SDIO clock used this boot (MHz). */
int opt_sdio_active_mhz(void);
/** True if this boot fell back from 50 to 40 MHz (or did so at runtime). */
bool opt_sdio_fell_back(void);
/** The C5 came up: a 50 MHz trial is confirmed (flag cleared). */
void opt_sdio_confirm(void);
/**
 * A C5 failure happened (init failure/timeout, SDIO transport failure, no response). If this boot
 * runs the SDIO bus at 50 MHz, store 40 MHz in NVS and return true (the caller restarts);
 * otherwise return false ("better safe than sorry": 40 MHz is the known-good clock).
 */
bool opt_sdio_fallback_to_40(void);

// --- Emergency flag ---
bool opt_emergency_flag(void);
esp_err_t opt_emergency_set(bool on);
