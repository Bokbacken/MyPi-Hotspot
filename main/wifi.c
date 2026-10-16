#include "wifi.h"

#include <stdlib.h>
#include <string.h>

#include "diag.h"
#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_ap_get_sta_list.h"
#include "eth.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"
#include "net.h"
#include "profiles.h"
#include "sdkconfig.h"
#include "web.h"

static const char *TAG = "wifi";

#define BIT_GOT_IP          BIT0
#define BIT_DISCONNECTED    BIT1
#define BIT_LOG_RSSI        BIT2
#define BIT_CONNECT_REQ     BIT3

#define LOCK_TIMEOUT_MS     10000
#define SCAN_MAX            32
#define AP_DNS              "8.8.8.8"

static EventGroupHandle_t s_events;
static SemaphoreHandle_t s_lock;        // serializes Wi-Fi RPCs (scan/config/connect)
static esp_netif_t *s_sta;
static esp_netif_t *s_ap;

// Station state
static volatile bool s_manual_off;
static volatile int s_pinned_slot = -1;     // -1 = automatic profile selection
static volatile int s_cur_slot = -1;
static volatile bool s_ignore_disc;         // disconnect requested by us
static volatile uint8_t s_sta_channel;
static sta_profile_t s_cur;                 // profile currently in use
static int s_rr;                            // round robin index for blind attempts

// AP state
static bool s_apsta_ok = true;
static volatile bool s_ap_running;
static int s_ap_slot = -1;
static ap_profile_t s_ap_cur;
static volatile int s_ap_clients;
static uint8_t s_ap_channel;
static void apply_bw20(wifi_interface_t ifx);

static bool lock(void) { return xSemaphoreTakeRecursive(s_lock, pdMS_TO_TICKS(LOCK_TIMEOUT_MS)) == pdTRUE; }
static void unlock(void) { xSemaphoreGiveRecursive(s_lock); }

static const char *disconnect_reason_name(uint8_t r)
{
    switch (r) {
    case WIFI_REASON_AUTH_EXPIRE:            return "auth expired";
    case WIFI_REASON_AUTH_FAIL:              return "auth failed (wrong password?)";
    case WIFI_REASON_ASSOC_FAIL:             return "association failed";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:      return "4-way handshake timeout (wrong password?)";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "4-way handshake timeout (wrong password?)";
    case WIFI_REASON_NO_AP_FOUND:            return "AP not found (SSID wrong / out of range / 5 GHz-only?)";
    case WIFI_REASON_BEACON_TIMEOUT:         return "beacon timeout (signal lost)";
    case WIFI_REASON_CONNECTION_FAIL:        return "connection failed";
    case WIFI_REASON_ASSOC_LEAVE:            return "left (assoc leave)";
    default:                                 return "see esp_wifi_types.h";
    }
}

static void apply_static_ip(void)
{
    esp_netif_dhcpc_stop(s_sta);
    esp_netif_ip_info_t ip = {0};
    ip.ip.addr = esp_ip4addr_aton(s_cur.ip);
    ip.netmask.addr = esp_ip4addr_aton(s_cur.mask);
    ip.gw.addr = esp_ip4addr_aton(s_cur.gw);
    esp_err_t err = esp_netif_set_ip_info(s_sta, &ip);
    if (s_cur.dns[0]) {
        esp_netif_dns_info_t d = {0};
        d.ip.type = ESP_IPADDR_TYPE_V4;
        d.ip.u_addr.ip4.addr = esp_ip4addr_aton(s_cur.dns);
        esp_netif_set_dns_info(s_sta, ESP_NETIF_DNS_MAIN, &d);
    }
    ESP_LOGI(TAG, "Static IP %s gw %s mask %s dns %s: %s", s_cur.ip, s_cur.gw, s_cur.mask,
             s_cur.dns[0] ? s_cur.dns : "-", esp_err_to_name(err));
    if (err == ESP_OK) {
        diag_set_wifi(WIFI_STATE_GOT_IP, s_cur.ip, 0);
        xEventGroupSetBits(s_events, BIT_GOT_IP | BIT_LOG_RSSI);
    }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "STA started");
            xEventGroupSetBits(s_events, BIT_CONNECT_REQ);
            break;
        case WIFI_EVENT_STA_CONNECTED: {
            wifi_event_sta_connected_t *ev = data;
            s_sta_channel = ev->channel;
            ESP_LOGI(TAG, "Associated with \"%.*s\" on channel %d", ev->ssid_len, (const char *)ev->ssid, ev->channel);
            diag_set_wifi(WIFI_STATE_CONNECTED, NULL, 0);
            if (s_cur.valid && !s_cur.dhcp) apply_static_ip();
            break;
        }
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *ev = data;
            ESP_LOGW(TAG, "Disconnected: reason %d (%s), rssi %d", ev->reason, disconnect_reason_name(ev->reason), ev->rssi);
            s_sta_channel = 0;
            diag_set_wifi(WIFI_STATE_DISCONNECTED, "0.0.0.0", 0);
            xEventGroupClearBits(s_events, BIT_GOT_IP);
            if (s_ignore_disc) {
                s_ignore_disc = false;
            } else {
                xEventGroupSetBits(s_events, BIT_DISCONNECTED);
            }
            break;
        }
        case WIFI_EVENT_AP_START:
            ESP_LOGI(TAG, "AP \"%s\" started", s_ap_cur.ssid);
            s_ap_clients = 0;
            if (s_ap_cur.nat) net_napt(s_ap, true);
            eth_bridge_start();     // LAN mode: wired clients join the AP subnet
            break;
        case WIFI_EVENT_AP_STOP:
            ESP_LOGI(TAG, "AP stopped");
            s_ap_clients = 0;
            eth_bridge_stop();
            break;
        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *ev = data;
            s_ap_clients++;
            ESP_LOGI(TAG, "AP client " MACSTR " joined (aid %d), %d connected", MAC2STR(ev->mac), ev->aid, s_ap_clients);
            break;
        }
        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t *ev = data;
            if (s_ap_clients > 0) s_ap_clients--;
            ESP_LOGI(TAG, "AP client " MACSTR " left (reason %d), %d connected", MAC2STR(ev->mac), ev->reason, s_ap_clients);
            break;
        }
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        char ip[16];
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ev->ip_info.ip));
        // RSSI is queried from wifi_task (an RPC to the co-processor; avoid it in the event loop).
        ESP_LOGI(TAG, "Got IP %s (gw " IPSTR ", mask " IPSTR ")", ip,
                 IP2STR(&ev->ip_info.gw), IP2STR(&ev->ip_info.netmask));
        ESP_LOGI(TAG, "Web UI: http://%s/  or  http://%s.local/", ip, CONFIG_MYPI_HOSTNAME);
        diag_set_wifi(WIFI_STATE_GOT_IP, ip, 0);
        xEventGroupSetBits(s_events, BIT_GOT_IP | BIT_LOG_RSSI);
    } else if (base == IP_EVENT && id == IP_EVENT_ASSIGNED_IP_TO_CLIENT) {
        ip_event_assigned_ip_to_client_t *ev = data;
        ESP_LOGI(TAG, "DHCP lease on %s: " MACSTR " -> " IPSTR, esp_netif_get_desc(ev->esp_netif), MAC2STR(ev->mac), IP2STR(&ev->ip));
    }
}

static void log_hosted_info(void)
{
    ESP_LOGI(TAG, "ESP-Hosted host component v%d.%d.%d, transport SDIO (CLK=%d CMD=%d D0=%d D1=%d D2=%d D3=%d, slave reset GPIO%d)",
             ESP_HOSTED_VERSION_MAJOR_1, ESP_HOSTED_VERSION_MINOR_1, ESP_HOSTED_VERSION_PATCH_1,
             CONFIG_ESP_HOSTED_SDIO_PIN_CLK, CONFIG_ESP_HOSTED_SDIO_PIN_CMD, CONFIG_ESP_HOSTED_SDIO_PIN_D0,
             CONFIG_ESP_HOSTED_SDIO_PIN_D1, CONFIG_ESP_HOSTED_SDIO_PIN_D2, CONFIG_ESP_HOSTED_SDIO_PIN_D3,
             CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE);

    esp_hosted_coprocessor_fwver_t ver = {0};
    char target[16] = "?";
    uint32_t chip_id = 0;
    int r1 = esp_hosted_get_coprocessor_fwversion(&ver);
    int r2 = esp_hosted_get_cp_info(&chip_id, target, sizeof(target));
    if (r1 == ESP_OK) {
        ESP_LOGI(TAG, "Co-processor (%s) ESP-Hosted slave firmware v%lu.%lu.%lu",
                 r2 == ESP_OK ? target : "?", (unsigned long)ver.major1, (unsigned long)ver.minor1,
                 (unsigned long)ver.patch1);
        diag_set_hosted_info("host v%d.%d.%d, slave %s v%lu.%lu.%lu",
                             ESP_HOSTED_VERSION_MAJOR_1, ESP_HOSTED_VERSION_MINOR_1, ESP_HOSTED_VERSION_PATCH_1,
                             r2 == ESP_OK ? target : "?", (unsigned long)ver.major1,
                             (unsigned long)ver.minor1, (unsigned long)ver.patch1);
        if (ver.major1 != ESP_HOSTED_VERSION_MAJOR_1 || ver.minor1 != ESP_HOSTED_VERSION_MINOR_1) {
            ESP_LOGW(TAG, "Host/slave ESP-Hosted version mismatch - Wi-Fi may misbehave; consider updating the C5 firmware");
        }
    } else {
        ESP_LOGW(TAG, "Could not read co-processor firmware version (err %d)", r1);
        diag_set_hosted_info("host v%d.%d.%d, slave version unknown (err %d)",
                             ESP_HOSTED_VERSION_MAJOR_1, ESP_HOSTED_VERSION_MINOR_1, ESP_HOSTED_VERSION_PATCH_1, r1);
    }
}

static void start_mdns(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns_init: %s", esp_err_to_name(err));
        return;
    }
    mdns_hostname_set(CONFIG_MYPI_HOSTNAME);
    mdns_instance_name_set("MyPi-Hotspot");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS: %s.local", CONFIG_MYPI_HOSTNAME);
}

// ---------------------------------------------------------------------------
// Station: scanning and profile selection
// ---------------------------------------------------------------------------

/** Blocking scan; caller holds the lock. Returns number of records or <0. */
static int scan_locked(wifi_ap_record_t *recs, uint16_t max)
{
    wifi_scan_config_t sc = {.show_hidden = false};
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan: %s", esp_err_to_name(err));
        return -1;
    }
    uint16_t n = max;
    err = esp_wifi_scan_get_ap_records(&n, recs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan records: %s", esp_err_to_name(err));
        return -1;
    }
    return n;
}

static void kconfig_profile(sta_profile_t *p)
{
    memset(p, 0, sizeof(*p));
    p->valid = strlen(CONFIG_MYPI_WIFI_SSID) > 0;
    strlcpy(p->ssid, CONFIG_MYPI_WIFI_SSID, sizeof(p->ssid));
    strlcpy(p->pass, CONFIG_MYPI_WIFI_PASSWORD, sizeof(p->pass));
    p->dhcp = true;
}

/** Choose the profile to connect to; caller holds the lock. */
static bool pick_profile(int forced, sta_profile_t *out, int *slot)
{
    if (forced >= 0) {
        *slot = forced;
        return sta_profile_get(forced, out) && out->valid;
    }
    if (sta_profile_count() == 0) {
        *slot = -1;
        kconfig_profile(out);
        return out->valid;
    }
    wifi_ap_record_t *recs = calloc(SCAN_MAX, sizeof(wifi_ap_record_t));
    int best = -1, best_rssi = -128;
    if (recs) {
        int n = scan_locked(recs, SCAN_MAX);
        for (int i = 0; i < n; i++) {
            int s = sta_profile_find((const char *)recs[i].ssid);
            if (s >= 0 && recs[i].rssi > best_rssi) {
                best = s;
                best_rssi = recs[i].rssi;
            }
        }
        free(recs);
    }
    if (best >= 0) {
        ESP_LOGI(TAG, "Auto-select: profile %d (RSSI %d dBm)", best, best_rssi);
        *slot = best;
        return sta_profile_get(best, out);
    }
    // Nothing visible (or hidden SSIDs): try the saved profiles in turn.
    for (int i = 0; i < STA_PROFILE_MAX; i++) {
        int idx = (s_rr + i) % STA_PROFILE_MAX;
        if (sta_profile_get(idx, out) && out->valid) {
            s_rr = idx + 1;
            *slot = idx;
            ESP_LOGI(TAG, "No saved network visible; trying profile %d \"%s\"", idx, out->ssid);
            return true;
        }
    }
    return false;
}

static void connect_now(int forced)
{
    if (!lock()) {
        xEventGroupSetBits(s_events, BIT_DISCONNECTED);
        return;
    }
    sta_profile_t p;
    int slot = -1;
    if (!pick_profile(forced, &p, &slot)) {
        ESP_LOGW(TAG, "No STA profile to connect to (add one in the web UI)");
        unlock();
        xEventGroupSetBits(s_events, BIT_DISCONNECTED);
        return;
    }
    wifi_config_t w = {0};
    strlcpy((char *)w.sta.ssid, p.ssid, sizeof(w.sta.ssid));
    strlcpy((char *)w.sta.password, p.pass, sizeof(w.sta.password));
    w.sta.threshold.authmode = p.pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    w.sta.pmf_cfg.capable = true;
    w.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    s_cur = p;
    s_cur_slot = slot;
    if (p.dhcp) {
        esp_netif_dhcpc_start(s_sta);   // "already started" is fine
    } else {
        esp_netif_dhcpc_stop(s_sta);
    }
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &w);
    if (err == ESP_OK) {
        diag_set_wifi_ssid(p.ssid);
        diag_set_wifi(WIFI_STATE_CONNECTING, NULL, 0);
        ESP_LOGI(TAG, "Connecting to \"%s\" (%s, %s)", p.ssid, slot >= 0 ? "profile" : "Kconfig default",
                 p.dhcp ? "DHCP" : "static IP");
        err = esp_wifi_connect();
    }
    unlock();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "connect: %s", esp_err_to_name(err));
        xEventGroupSetBits(s_events, BIT_DISCONNECTED);
    }
}

int wifi_scan(wifi_scan_entry_t *out, int max)
{
    if (!s_lock || !lock()) return -2;
    wifi_ap_record_t *recs = calloc(SCAN_MAX, sizeof(wifi_ap_record_t));
    int n = recs ? scan_locked(recs, SCAN_MAX) : -1;
    unlock();
    int cnt = 0;
    for (int i = 0; i < n && cnt < max; i++) {
        if (!recs[i].ssid[0]) continue;
        strlcpy(out[cnt].ssid, (const char *)recs[i].ssid, sizeof(out[cnt].ssid));
        out[cnt].rssi = recs[i].rssi;
        out[cnt].channel = recs[i].primary;
        out[cnt].auth = recs[i].authmode;
        out[cnt].profile = sta_profile_find(out[cnt].ssid);
        cnt++;
    }
    free(recs);
    return n < 0 ? n : cnt;
}

esp_err_t wifi_connect_profile(int slot)
{
    if (!s_events) return ESP_ERR_INVALID_STATE;
    if (slot >= 0) {
        sta_profile_t p;
        if (!sta_profile_get(slot, &p) || !p.valid) return ESP_ERR_NOT_FOUND;
    }
    s_manual_off = false;
    s_pinned_slot = slot;
    if (xEventGroupGetBits(s_events) & BIT_GOT_IP) {
        s_ignore_disc = true;
        esp_wifi_disconnect();
    }
    xEventGroupSetBits(s_events, BIT_CONNECT_REQ);
    return ESP_OK;
}

esp_err_t wifi_disconnect(void)
{
    if (!s_events) return ESP_ERR_INVALID_STATE;
    s_manual_off = true;
    ESP_LOGI(TAG, "STA disconnect requested (stays off until connect)");
    return esp_wifi_disconnect();
}

void wifi_get_sta_extra(wifi_sta_extra_t *out)
{
    out->sta_profile = s_cur_slot;
    out->sta_manual_off = s_manual_off;
    if (eth_is_wan_uplink()) {
        strcpy(out->uplink, "eth");
    } else if (s_events && (xEventGroupGetBits(s_events) & BIT_GOT_IP)) {
        strcpy(out->uplink, "sta");
    } else {
        strcpy(out->uplink, "none");
    }
}

// ---------------------------------------------------------------------------
// Access point
// ---------------------------------------------------------------------------

static void default_ap_profile(ap_profile_t *p)
{
    memset(p, 0, sizeof(*p));
    p->valid = true;
    strlcpy(p->ssid, CONFIG_MYPI_AP_SSID, sizeof(p->ssid));
    strlcpy(p->pass, CONFIG_MYPI_AP_PASSWORD, sizeof(p->pass));
    p->subnet = 0;
    p->nat = true;
}

/** Configure (and, if Wi-Fi is started, (re)start) the AP; caller holds the lock and mode is APSTA. */
static esp_err_t ap_configure(int slot)
{
    ap_profile_t p;
    if (slot >= 0) {
        if (!ap_profile_get(slot, &p) || !p.valid) return ESP_ERR_NOT_FOUND;
    } else {
        default_ap_profile(&p);
    }
    if (strlen(p.pass) > 0 && strlen(p.pass) < 8) {
        ESP_LOGE(TAG, "AP password must be 8-63 characters (or empty for open)");
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ap_running && s_ap_cur.nat && !p.nat) net_napt(s_ap, false);
    const uint8_t *sn = AP_SUBNET_BASE[p.subnet % AP_SUBNET_COUNT];
    net_dhcps_setup(s_ap, sn[0], sn[1], sn[2], AP_DNS);

    wifi_config_t c = {0};
    strlcpy((char *)c.ap.ssid, p.ssid, sizeof(c.ap.ssid));
    c.ap.ssid_len = strlen(p.ssid);
    strlcpy((char *)c.ap.password, p.pass, sizeof(c.ap.password));
    c.ap.authmode = p.pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    c.ap.max_connection = CONFIG_MYPI_AP_MAX_CONN;
    c.ap.channel = s_sta_channel ? s_sta_channel : (s_ap_channel ? s_ap_channel : 1);
    c.ap.pmf_cfg.required = false;
    s_ap_cur = p;   // read by the AP_START handler (NAT)
    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &c);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AP set_config: %s", esp_err_to_name(err));
        return err;
    }
    s_ap_slot = slot;
    s_ap_channel = c.ap.channel;
    s_ap_running = true;
    // If the AP was already up the AP_START event may not fire again.
    net_napt(s_ap, p.nat);
    eth_bridge_start();
    ESP_LOGI(TAG, "AP \"%s\" (%s) on %d.%d.%d.1/24, channel %d, NAT %s, max %d clients", p.ssid,
             p.pass[0] ? "WPA2" : "open", sn[0], sn[1], sn[2], c.ap.channel, p.nat ? "on" : "off",
             CONFIG_MYPI_AP_MAX_CONN);
    return ESP_OK;
}

esp_err_t wifi_ap_start(int slot)
{
    if (!s_lock || !lock()) return ESP_ERR_TIMEOUT;
    esp_err_t err = ESP_OK;
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    if (mode != WIFI_MODE_APSTA) {
        err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "APSTA mode not available (%s) - staying STA-only", esp_err_to_name(err));
            s_apsta_ok = false;
        } else {
            s_apsta_ok = true;
        }
    }
    if (err == ESP_OK && mode != WIFI_MODE_APSTA) apply_bw20(WIFI_IF_AP);
    if (err == ESP_OK) err = ap_configure(slot);
    unlock();
    return err;
}

esp_err_t wifi_ap_stop(void)
{
    if (!s_lock || !lock()) return ESP_ERR_TIMEOUT;
    if (s_ap_running) net_napt(s_ap, false);
    eth_bridge_stop();
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) {
        s_ap_running = false;
        s_ap_clients = 0;
        ESP_LOGI(TAG, "AP stopped (STA only)");
    } else {
        ESP_LOGE(TAG, "AP stop: %s", esp_err_to_name(err));
    }
    unlock();
    return err;
}

void wifi_get_ap_info(wifi_ap_info_t *o)
{
    memset(o, 0, sizeof(*o));
    o->apsta_supported = s_apsta_ok;
    o->running = s_ap_running;
    o->slot = s_ap_slot;
    o->max_clients = CONFIG_MYPI_AP_MAX_CONN;
    strcpy(o->ip, "0.0.0.0");
    if (!s_ap_running) return;
    strlcpy(o->ssid, s_ap_cur.ssid, sizeof(o->ssid));
    o->open = !s_ap_cur.pass[0];
    o->nat = s_ap_cur.nat;
    o->channel = s_ap_channel;
    o->sta_channel = s_sta_channel;
    o->clients = s_ap_clients;
    const uint8_t *sn = AP_SUBNET_BASE[s_ap_cur.subnet % AP_SUBNET_COUNT];
    snprintf(o->ip, sizeof(o->ip), "%d.%d.%d.1", sn[0], sn[1], sn[2]);
}

int wifi_ap_clients(wifi_client_t *out, int max)
{
    if (!s_ap_running || !s_lock || !lock()) return 0;
    wifi_sta_list_t *list = calloc(1, sizeof(*list));
    wifi_sta_mac_ip_list_t *ipl = calloc(1, sizeof(*ipl));
    int n = 0;
    if (list && ipl && esp_wifi_ap_get_sta_list(list) == ESP_OK) {
        bool have_ip = esp_wifi_ap_get_sta_list_with_ip(list, ipl) == ESP_OK;
        for (int i = 0; i < list->num && n < max; i++, n++) {
            memcpy(out[n].mac, list->sta[i].mac, 6);
            out[n].rssi = list->sta[i].rssi;
            strcpy(out[n].ip, "-");
            if (have_ip && ipl->sta[i].ip.addr) {
                snprintf(out[n].ip, sizeof(out[n].ip), IPSTR, IP2STR(&ipl->sta[i].ip));
            }
        }
        s_ap_clients = list->num;
    }
    unlock();
    free(list);
    free(ipl);
    return n;
}

// Force HT20 on an interface. If esp_wifi_set_bandwidth() is rejected (band
// mode "auto"), fall back to esp_wifi_set_bandwidths() for both bands.
static void apply_bw20(wifi_interface_t ifx)
{
#if CONFIG_MYPI_WIFI_BW_20
    const char *name = ifx == WIFI_IF_AP ? "AP" : "STA";
    esp_err_t err = esp_wifi_set_bandwidth(ifx, WIFI_BW20);
    ESP_LOGI(TAG, "esp_wifi_set_bandwidth(%s, HT20): %s", name, esp_err_to_name(err));
    if (err != ESP_OK) {
        wifi_bandwidths_t bw = {.ghz_2g = WIFI_BW20, .ghz_5g = WIFI_BW20};
        err = esp_wifi_set_bandwidths(ifx, &bw);
        ESP_LOGI(TAG, "esp_wifi_set_bandwidths(%s, 2.4G HT20 / 5G HT20): %s", name, esp_err_to_name(err));
    }
    if (err != ESP_OK) ESP_LOGW(TAG, "Could not force HT20 on %s - radio keeps its default bandwidth", name);
#endif
}

static inline const char *band_of(int ch) { return ch > 14 ? "5 GHz" : "2.4 GHz"; }

// Read the channel the AP actually uses from the co-processor. The C5 has one
// radio: in APSTA the AP always runs on the STA's channel.
static void refresh_ap_channel(void)
{
    if (!s_ap_running) return;
    wifi_config_t c = {0};
    if (esp_wifi_get_config(WIFI_IF_AP, &c) == ESP_OK && c.ap.channel) {
        if (c.ap.channel != s_ap_channel) {
            ESP_LOGI(TAG, "AP channel %d (%s), STA channel %d (%s)", c.ap.channel, band_of(c.ap.channel),
                     s_sta_channel, s_sta_channel ? band_of(s_sta_channel) : "-");
        }
        s_ap_channel = c.ap.channel;
    }
}

// ---------------------------------------------------------------------------
// Task
// ---------------------------------------------------------------------------

#define TRY(expr)                                                                   \
    do {                                                                            \
        esp_err_t _e = (expr);                                                      \
        if (_e != ESP_OK) {                                                         \
            ESP_LOGE(TAG, "[FAIL] %s: %s", #expr, esp_err_to_name(_e));             \
            diag_set_wifi(WIFI_STATE_INIT_FAILED, NULL, 0);                         \
            vTaskDelete(NULL);                                                      \
            return;                                                                 \
        }                                                                           \
        ESP_LOGD(TAG, "[ OK ] %s", #expr);                                          \
    } while (0)

static void wifi_task(void *arg)
{
    diag_set_wifi(WIFI_STATE_INIT, NULL, 0);
    s_events = xEventGroupCreate();
    s_lock = xSemaphoreCreateRecursiveMutex();

    TRY(esp_netif_init());
    TRY(esp_event_loop_create_default());
    profiles_load();
    net_stats_start();

    // Ethernet, web server and mDNS come up first so the device is reachable over
    // Ethernet even if the Wi-Fi co-processor fails.
#if CONFIG_MYPI_ETH_ENABLE
    if (eth_init() != ESP_OK) {
        ESP_LOGW(TAG, "Ethernet unavailable - continuing without it");
    }
#endif
    web_start();
    start_mdns();

    s_sta = esp_netif_create_default_wifi_sta();
    s_ap = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_sta, CONFIG_MYPI_HOSTNAME);
    net_stats_register(NET_IF_STA, s_sta);
    net_stats_register(NET_IF_AP, s_ap);

    ESP_LOGI(TAG, "Initializing Wi-Fi via ESP-Hosted (ESP32-C5 over SDIO); this may take a few seconds");
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    TRY(esp_wifi_init(&cfg));
    log_hosted_info();

    TRY(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    TRY(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));
    TRY(esp_event_handler_register(IP_EVENT, IP_EVENT_ASSIGNED_IP_TO_CLIENT, on_event, NULL));

    int boot_ap = ap_profile_boot_slot();
    bool want_ap = boot_ap >= 0 || CONFIG_MYPI_AP_AUTOSTART;
    if (want_ap) {
        esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_set_mode(APSTA): %s - falling back to STA only", esp_err_to_name(err));
            s_apsta_ok = false;
            TRY(esp_wifi_set_mode(WIFI_MODE_STA));
        } else if (ap_configure(boot_ap) != ESP_OK && boot_ap >= 0) {
            ESP_LOGW(TAG, "AP profile %d invalid, using the default AP", boot_ap);
            ap_configure(-1);
        }
    } else {
        TRY(esp_wifi_set_mode(WIFI_MODE_STA));
    }
    // Channel width must be set before the radio starts.
    apply_bw20(WIFI_IF_STA);
    if (s_apsta_ok && want_ap) apply_bw20(WIFI_IF_AP);
    TRY(esp_wifi_start());   // STA_START triggers the first (profile) connect

    // Main loop: connect requests, reconnect with exponential back-off, RSSI refresh.
    uint32_t backoff_s = 1;
    while (true) {
        EventBits_t bits = xEventGroupWaitBits(s_events, BIT_DISCONNECTED | BIT_LOG_RSSI | BIT_CONNECT_REQ,
                                               pdFALSE, pdFALSE, pdMS_TO_TICKS(5000));
        xEventGroupClearBits(s_events, bits & (BIT_DISCONNECTED | BIT_LOG_RSSI | BIT_CONNECT_REQ));
        if (bits & BIT_CONNECT_REQ) {
            backoff_s = 1;
            if (!s_manual_off) connect_now(s_pinned_slot);
        } else if (bits & BIT_DISCONNECTED) {
            if (s_manual_off) {
                diag_set_wifi(WIFI_STATE_DISCONNECTED, "0.0.0.0", 0);
                continue;
            }
            ESP_LOGI(TAG, "Reconnecting in %lu s", (unsigned long)backoff_s);
            if (xEventGroupWaitBits(s_events, BIT_CONNECT_REQ, pdFALSE, pdFALSE,
                                    pdMS_TO_TICKS(backoff_s * 1000)) & BIT_CONNECT_REQ) {
                continue;   // a manual connect request overrides the back-off
            }
            if (s_manual_off) continue;
            diag_wifi_retry();
            connect_now(s_pinned_slot);
            backoff_s = backoff_s * 2 > CONFIG_MYPI_WIFI_BACKOFF_MAX_S ? CONFIG_MYPI_WIFI_BACKOFF_MAX_S : backoff_s * 2;
        } else if (xEventGroupGetBits(s_events) & BIT_GOT_IP) {
            backoff_s = 1;
            wifi_ap_record_t ap = {0};
            if (lock()) {
                esp_err_t err = esp_wifi_sta_get_ap_info(&ap);
                unlock();
                if (err == ESP_OK) {
                    diag_status_t st;
                    diag_get(&st);
                    diag_set_wifi(WIFI_STATE_GOT_IP, st.ip, ap.rssi);
                    s_sta_channel = ap.primary;
                    if (bits & BIT_LOG_RSSI) {
                        ESP_LOGI(TAG, "Connected to \"%s\" ch %d, RSSI %d dBm", (const char *)ap.ssid,
                                 ap.primary, ap.rssi);
                    }
                }
            }
        }
        if (s_ap_running && lock()) {
            refresh_ap_channel();
            unlock();
        }
    }
}

void wifi_start_background(void)
{
    if (xTaskCreate(wifi_task, "wifi_mgr", 8192, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Wi-Fi task");
        diag_set_wifi(WIFI_STATE_INIT_FAILED, NULL, 0);
    }
}
