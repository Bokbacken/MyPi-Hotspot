#include "opt_config.h"

#include <string.h>

#include "diag.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sys.h"
#include "lwip_tune.h"
#include "nvs.h"
#include "wifi.h"

static const char *TAG = "opt";

static opt_config_t s_active;       // in use since boot (boot-only fields never change at runtime)
static opt_config_t s_saved;        // stored in NVS
static bool s_loaded;
static uint8_t s_sdio_mhz = OPT_DEFAULT_SDIO_MHZ;
static bool s_sdio_fell_back;
static bool s_sdio_trial;
static bool s_emergency;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

// --- LwIP / NAT tuning (v0.9.2), see lwip_tune.h ---
// Read by lwIP's ip4_napt.c through IP_NAPT_MAX / IP_NAPT_TIMEOUT_MS_TCP (force-included header).
uint16_t mypi_napt_max_entries = 512;
volatile uint32_t mypi_napt_tcp_timeout_ms = 7200u * 1000u;
static int s_lwip_core_id = -1;     // -1 = no pinning

static void apply_lwip_tune(const opt_config_t *c, bool boot)
{
    if (boot) {
        mypi_napt_max_entries = opt_napt_max_entries(c->napt_max);  // table allocated on the first NAPT enable
        s_lwip_core_id = opt_lwip_core_id(c->lwip_core);            // used when esp_netif_init() creates tcpip
    }
    mypi_napt_tcp_timeout_ms = opt_napt_tcp_timeout_s(c->napt_tcp_to) * 1000u;
}

// The tcpip task affinity is a Kconfig constant (CONFIG_LWIP_TCPIP_TASK_AFFINITY) inside
// sys_thread_new(); the linker wraps that call (-Wl,--wrap=sys_thread_new, main/CMakeLists.txt) so
// the NVS setting can pin the task when esp_netif_init() creates it. Other lwIP threads are unchanged.
sys_thread_t __real_sys_thread_new(const char *name, lwip_thread_fn thread, void *arg, int stacksize, int prio);
sys_thread_t __wrap_sys_thread_new(const char *name, lwip_thread_fn thread, void *arg, int stacksize, int prio);

sys_thread_t __wrap_sys_thread_new(const char *name, lwip_thread_fn thread, void *arg, int stacksize, int prio)
{
    if (s_lwip_core_id < 0 || s_lwip_core_id >= CONFIG_FREERTOS_NUMBER_OF_CORES || !name ||
        strcmp(name, TCPIP_THREAD_NAME) != 0) {
        return __real_sys_thread_new(name, thread, arg, stacksize, prio);
    }
    TaskHandle_t t = NULL;
    if (xTaskCreatePinnedToCore(thread, name, stacksize, arg, prio, &t, s_lwip_core_id) != pdPASS) return NULL;
    ESP_LOGI("opt", "LwIP tcpip task pinned to core %d", s_lwip_core_id);
    return (sys_thread_t)t;
}

// --- NVS binding of opt_store_t ---
static int nvs_get_cb(void *ctx, const char *key, uint8_t *out)
{
    return nvs_get_u8(*(nvs_handle_t *)ctx, key, out) == ESP_OK ? 0 : -1;
}

static int nvs_set_cb(void *ctx, const char *key, uint8_t v)
{
    return nvs_set_u8(*(nvs_handle_t *)ctx, key, v) == ESP_OK ? 0 : -1;
}

static int nvs_get16_cb(void *ctx, const char *key, uint16_t *out)
{
    return nvs_get_u16(*(nvs_handle_t *)ctx, key, out) == ESP_OK ? 0 : -1;
}

static int nvs_set16_cb(void *ctx, const char *key, uint16_t v)
{
    return nvs_set_u16(*(nvs_handle_t *)ctx, key, v) == ESP_OK ? 0 : -1;
}

static int nvs_getstr_cb(void *ctx, const char *key, char *out, unsigned len)
{
    size_t n = len;
    return nvs_get_str(*(nvs_handle_t *)ctx, key, out, &n) == ESP_OK ? 0 : -1;
}

static int nvs_setstr_cb(void *ctx, const char *key, const char *v)
{
    return nvs_set_str(*(nvs_handle_t *)ctx, key, v) == ESP_OK ? 0 : -1;
}

#define NVS_STORE(h) {nvs_get_cb, nvs_set_cb, nvs_get16_cb, nvs_set16_cb, nvs_getstr_cb, nvs_setstr_cb, nvs_commit_cb, &(h)}

static int nvs_commit_cb(void *ctx)
{
    return nvs_commit(*(nvs_handle_t *)ctx) == ESP_OK ? 0 : -1;
}

static esp_err_t set_flag(const char *key, uint8_t v)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(OPT_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, key, v);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_log_level_t opt_log_esp_level(void)
{
    static const esp_log_level_t map[] = {ESP_LOG_ERROR, ESP_LOG_WARN, ESP_LOG_INFO, ESP_LOG_DEBUG};
    uint8_t l = s_loaded ? s_active.log_level : OPT_DEFAULT_LOG_LVL;
    return map[l <= 3 ? l : OPT_DEFAULT_LOG_LVL];
}

static void apply_log_level(void)
{
    esp_log_level_set("*", opt_log_esp_level());
    diag_log_levels_refresh();      // app tags: debug mode wins, else this level
}

void opt_config_load(void)
{
    opt_config_t c;
    uint8_t trial = 0, emerg = 0;
    nvs_handle_t h;
    if (nvs_open(OPT_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        opt_store_t st = NVS_STORE(h);
        opt_config_load_from(&st, &c);
        nvs_get_u8(h, OPT_KEY_SDIO_TRIAL, &trial);
        nvs_get_u8(h, OPT_KEY_EMERGENCY, &emerg);
        nvs_close(h);
    } else {
        opt_config_defaults(&c);    // namespace not created yet = all defaults
    }
    bool fallback;
    uint8_t mhz = opt_sdio_boot_mhz(c.sdio_freq_mhz, trial != 0, &fallback);
    if (fallback) {
        // The previous 50 MHz boot never confirmed the C5 (hang, crash or power loss during init).
        ESP_LOGW(TAG, "SDIO 50 MHz was not confirmed on the last boot - falling back to 40 MHz");
        c.sdio_freq_mhz = 40;
        set_flag(OPT_KEY_SDIO_MHZ, 40);
        set_flag(OPT_KEY_SDIO_TRIAL, 0);
        s_sdio_fell_back = true;
    }
    // v0.9.1: the TCP window setting is gone (32 KB build-time); drop the stale key once.
    if (nvs_open(OPT_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_erase_key(h, OPT_KEY_TCP_KB_OLD) == ESP_OK) {
            nvs_commit(h);
            ESP_LOGI(TAG, "Removed obsolete NVS key \"%s\"", OPT_KEY_TCP_KB_OLD);
        }
        nvs_close(h);
    }
    s_sdio_mhz = mhz;
    apply_lwip_tune(&c, true);
    s_active = s_saved = c;
    s_emergency = emerg != 0;
    s_loaded = true;
    apply_log_level();
    ESP_LOGI(TAG, "Optimize: SDIO %d MHz, TX %s%d dBm, NAPT %s, log level %d, PS %d, beacon %d ms, country %s, "
             "emergency flag %s", c.sdio_freq_mhz, c.wifi_tx_power_dbm ? "" : "radio setting/", c.wifi_tx_power_dbm,
             c.napt ? "on" : "off", c.log_level, c.ps_mode, c.ap_beacon_ms, c.country, s_emergency ? "SET" : "clear");
    ESP_LOGI(TAG, "LwIP/NAT: tcpip core %s, NAPT table %u entries, NAPT TCP timeout %lu s",
             c.lwip_core == 0 ? "auto" : c.lwip_core == 1 ? "0" : "1", (unsigned)mypi_napt_max_entries,
             (unsigned long)(mypi_napt_tcp_timeout_ms / 1000u));
}

void opt_config_get_active(opt_config_t *out)
{
    portENTER_CRITICAL(&s_mux);
    if (s_loaded) *out = s_active; else opt_config_defaults(out);
    portEXIT_CRITICAL(&s_mux);
}

void opt_config_get_saved(opt_config_t *out)
{
    portENTER_CRITICAL(&s_mux);
    if (s_loaded) *out = s_saved; else opt_config_defaults(out);
    portEXIT_CRITICAL(&s_mux);
}

bool opt_config_reboot_pending(void)
{
    opt_config_t a, s;
    opt_config_get_active(&a);
    opt_config_get_saved(&s);
    return opt_config_reboot_required(&a, &s);
}

esp_err_t opt_config_save(const opt_config_t *c, bool *reboot_required)
{
    if (!opt_config_valid(c)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(OPT_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    opt_store_t st = NVS_STORE(h);
    int r = opt_config_save_to(&st, c);
    if (r == 0 && c->sdio_freq_mhz != 50) nvs_set_u8(h, OPT_KEY_SDIO_TRIAL, 0);   // no pending 50 MHz trial
    if (r == 0) nvs_commit(h);
    nvs_close(h);
    if (r != 0) return ESP_FAIL;

    opt_config_t old;
    portENTER_CRITICAL(&s_mux);
    old = s_active;
    s_saved = *c;
    // Runtime fields take effect now; SDIO clock, beacon interval and country stay until the next boot.
    s_active.wifi_tx_power_dbm = c->wifi_tx_power_dbm;
    s_active.napt = c->napt;
    s_active.log_level = c->log_level;
    s_active.ps_mode = c->ps_mode;
    s_active.napt_tcp_to = c->napt_tcp_to;
    portEXIT_CRITICAL(&s_mux);
    apply_lwip_tune(c, false);     // NAPT TCP timeout only; core and table size stay until reboot

    if (old.log_level != c->log_level) apply_log_level();
    if (old.napt != c->napt) wifi_apply_napt();
    if (old.wifi_tx_power_dbm != c->wifi_tx_power_dbm) wifi_apply_tx_power();
    if (old.ps_mode != c->ps_mode) wifi_apply_ps();
    bool rb = opt_config_reboot_pending();
    if (reboot_required) *reboot_required = rb;
    ESP_LOGI(TAG, "Optimize settings saved: SDIO %d MHz, TX %d dBm, NAPT %s, log %d, PS %d, beacon %d ms, country %s, "
             "lwip core %d, napt max %d, napt tcp to %d%s", c->sdio_freq_mhz, c->wifi_tx_power_dbm, c->napt ? "on" : "off",
             c->log_level, c->ps_mode, c->ap_beacon_ms, c->country, c->lwip_core, c->napt_max, c->napt_tcp_to,
             rb ? " (reboot required)" : "");
    return ESP_OK;
}

bool opt_napt_enabled(void)
{
    return s_loaded ? s_active.napt : OPT_DEFAULT_NAPT;
}

uint8_t opt_tx_power_dbm(void)
{
    return s_loaded ? s_active.wifi_tx_power_dbm : OPT_DEFAULT_TX_DBM;
}

uint8_t opt_ps_mode(void)
{
    return s_loaded ? s_active.ps_mode : OPT_DEFAULT_PS_MODE;
}

uint16_t opt_ap_beacon_ms(void)
{
    return s_loaded ? s_active.ap_beacon_ms : OPT_DEFAULT_BEACON_MS;
}

uint8_t opt_lwip_core(void)
{
    return s_loaded ? s_active.lwip_core : OPT_DEFAULT_LWIP_CORE;
}

uint8_t opt_napt_max(void)
{
    return s_loaded ? s_active.napt_max : OPT_DEFAULT_NAPT_MAX;
}

const opt_country_t *opt_country(void)
{
    const opt_country_t *c = s_loaded ? opt_country_find(s_active.country) : NULL;
    return c ? c : opt_country_find(OPT_DEFAULT_COUNTRY);
}

// --- SDIO clock trial / fallback ---
uint32_t opt_sdio_begin(void)
{
    if (s_sdio_mhz == 50) {
        // Marked before the transport starts: if this boot hangs or crashes the next one uses 40 MHz.
        s_sdio_trial = set_flag(OPT_KEY_SDIO_TRIAL, 1) == ESP_OK;
        ESP_LOGI(TAG, "SDIO clock 50 MHz (trial until the C5 is up)");
    }
    return (uint32_t)s_sdio_mhz * 1000;
}

int opt_sdio_active_mhz(void) { return s_sdio_mhz; }
bool opt_sdio_fell_back(void) { return s_sdio_fell_back; }

void opt_sdio_confirm(void)
{
    if (!s_sdio_trial) return;
    s_sdio_trial = false;
    set_flag(OPT_KEY_SDIO_TRIAL, 0);
    ESP_LOGI(TAG, "SDIO 50 MHz confirmed (C5 up)");
}

bool opt_sdio_fallback_to_40(void)
{
    if (s_sdio_mhz != 50) return false;
    ESP_LOGW(TAG, "C5 failure at SDIO 50 MHz - storing 40 MHz (auto-fallback)");
    set_flag(OPT_KEY_SDIO_MHZ, 40);
    set_flag(OPT_KEY_SDIO_TRIAL, 0);
    portENTER_CRITICAL(&s_mux);
    s_saved.sdio_freq_mhz = 40;
    portEXIT_CRITICAL(&s_mux);
    s_sdio_trial = false;
    s_sdio_fell_back = true;
    return true;
}

// --- Emergency flag ---
bool opt_emergency_flag(void) { return s_emergency; }

esp_err_t opt_emergency_set(bool on)
{
    esp_err_t err = set_flag(OPT_KEY_EMERGENCY, on ? 1 : 0);
    if (err == ESP_OK) s_emergency = on;
    return err;
}
