#include "eth.h"

#include <stdio.h>
#include <string.h>

#include <stdlib.h>

#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_private/wifi.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "net.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "eth";

#define NVS_NS      "mypi"
#define NVS_KEY     "ethmode"

static esp_eth_handle_t s_eth;
static esp_netif_t *s_netif;
static eth_mode_t s_mode = ETH_MODE_LAN;
static eth_mode_t s_saved_mode = ETH_MODE_LAN;
static volatile bool s_init, s_link, s_has_ip;
static char s_err[64];

const char *eth_mode_name(eth_mode_t m) { return m == ETH_MODE_WAN ? "wan" : "lan"; }
eth_mode_t eth_mode(void) { return s_mode; }
esp_netif_t *eth_netif(void) { return s_netif; }
bool eth_is_wan_uplink(void) { return s_init && s_mode == ETH_MODE_WAN && s_link && s_has_ip; }

static void load_mode(void)
{
    nvs_handle_t h;
    uint8_t v = ETH_MODE_LAN;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, NVS_KEY, &v);
        nvs_close(h);
    }
    s_mode = s_saved_mode = v == ETH_MODE_WAN ? ETH_MODE_WAN : ETH_MODE_LAN;
}

esp_err_t eth_set_mode(eth_mode_t mode)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, NVS_KEY, (uint8_t)mode);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        s_saved_mode = mode;
        ESP_LOGI(TAG, "Ethernet mode set to %s (applied after reboot)", eth_mode_name(mode));
    }
    return err;
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == ETH_EVENT) {
        switch (id) {
        case ETHERNET_EVENT_START:
            ESP_LOGI(TAG, "Ethernet started (%s mode)", eth_mode_name(s_mode));
            break;
        case ETHERNET_EVENT_CONNECTED: {
            eth_speed_t sp = ETH_SPEED_10M;
            eth_duplex_t dx = ETH_DUPLEX_HALF;
            esp_eth_ioctl(s_eth, ETH_CMD_G_SPEED, &sp);
            esp_eth_ioctl(s_eth, ETH_CMD_G_DUPLEX_MODE, &dx);
            s_link = true;
            ESP_LOGI(TAG, "Link up: %s Mbit/s %s duplex", sp == ETH_SPEED_100M ? "100" : "10",
                     dx == ETH_DUPLEX_FULL ? "full" : "half");
            break;
        }
        case ETHERNET_EVENT_DISCONNECTED:
            s_link = false;
            if (s_mode == ETH_MODE_WAN) s_has_ip = false;
            ESP_LOGW(TAG, "Link down");
            break;
        case ETHERNET_EVENT_STOP:
            ESP_LOGI(TAG, "Ethernet stopped");
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        s_has_ip = true;
        ESP_LOGI(TAG, "WAN got IP " IPSTR " (gw " IPSTR ") - Ethernet is now the uplink", IP2STR(&ev->ip_info.ip),
                 IP2STR(&ev->ip_info.gw));
        esp_netif_set_default_netif(s_netif);
    } else if (base == IP_EVENT && id == IP_EVENT_ETH_LOST_IP) {
        s_has_ip = false;
        ESP_LOGW(TAG, "WAN lost IP");
    }
}

// ---------------------------------------------------------------------------
// LAN mode: transparent L2 bridge Ethernet <-> Wi-Fi AP (like Waveshare 08_eth2ap).
// Ethernet has no netif/IP of its own; wired clients share the AP subnet and the
// AP netif (IP, DHCP server, NAT) serves both sides.
//   ETH rx  -> Wi-Fi clients (esp_wifi_internal_tx) and/or the AP netif (inject)
//   AP tap  -> frames from Wi-Fi clients / from the host towards Ethernet
// A small learning table (MAC -> side) avoids flooding unicast traffic.
// ---------------------------------------------------------------------------

#define FDB_SIZE    32
#define FDB_AGE_US  (300LL * 1000000)

typedef enum { SIDE_ETH = 1, SIDE_WIFI = 2 } side_t;
typedef struct {
    uint8_t mac[6];
    uint8_t side;
    int64_t seen;
} fdb_entry_t;

static fdb_entry_t s_fdb[FDB_SIZE];
static portMUX_TYPE s_fdb_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_bridge_on;
static uint8_t s_ap_mac[6];
static volatile uint32_t s_br_drop;

static inline bool is_mcast(const uint8_t *mac) { return mac[0] & 1; }

static void fdb_learn(const uint8_t *mac, side_t side)
{
    if (is_mcast(mac)) return;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL_SAFE(&s_fdb_mux);
    int slot = -1, oldest = 0;
    for (int i = 0; i < FDB_SIZE; i++) {
        if (s_fdb[i].side && memcmp(s_fdb[i].mac, mac, 6) == 0) {
            slot = i;
            break;
        }
        if (s_fdb[i].seen < s_fdb[oldest].seen) oldest = i;
    }
    if (slot < 0) {
        slot = oldest;  // empty entries have seen == 0, so they are picked first
        memcpy(s_fdb[slot].mac, mac, 6);
    }
    s_fdb[slot].side = side;
    s_fdb[slot].seen = now;
    portEXIT_CRITICAL_SAFE(&s_fdb_mux);
}

static side_t fdb_lookup(const uint8_t *mac)
{
    side_t r = 0;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL_SAFE(&s_fdb_mux);
    for (int i = 0; i < FDB_SIZE; i++) {
        if (s_fdb[i].side && memcmp(s_fdb[i].mac, mac, 6) == 0) {
            if (now - s_fdb[i].seen < FDB_AGE_US) r = s_fdb[i].side;
            break;
        }
    }
    portEXIT_CRITICAL_SAFE(&s_fdb_mux);
    return r;
}

static void eth_tx_pbuf(struct pbuf *p)
{
    if (!s_link || p->tot_len < 14) return;
    esp_err_t err;
    if (!p->next) {
        err = esp_eth_transmit(s_eth, p->payload, p->len);
    } else {
        uint8_t *buf = malloc(p->tot_len);
        if (!buf) {
            s_br_drop++;
            return;
        }
        pbuf_copy_partial(p, buf, p->tot_len, 0);
        err = esp_eth_transmit(s_eth, buf, p->tot_len);
        free(buf);
    }
    if (err == ESP_OK) net_stats_add(NET_IF_ETH, 0, p->tot_len);
    else s_br_drop++;
}

// Installed on the AP lwIP netif (see net.c). rx: frame from a Wi-Fi client;
// !rx: frame the host (AP netif) is sending. Returns true if consumed.
static bool ap_tap(struct pbuf *p, struct netif *n, bool rx)
{
    if (!s_bridge_on || p->len < 14) return false;
    const uint8_t *dst = p->payload;
    const uint8_t *src = dst + 6;
    if (rx) {
        fdb_learn(src, SIDE_WIFI);
        if (memcmp(dst, n->hwaddr, 6) == 0) return false;   // for the host only
        eth_tx_pbuf(p);
        return !is_mcast(dst);  // broadcast/multicast also goes to the host
    }
    if (is_mcast(dst)) {
        eth_tx_pbuf(p);
        return false;           // and to Wi-Fi
    }
    switch (fdb_lookup(dst)) {
    case SIDE_ETH:
        eth_tx_pbuf(p);
        return true;
    case SIDE_WIFI:
        return false;
    default:
        eth_tx_pbuf(p);         // unknown unicast: flood to both sides
        return false;
    }
}

// Ethernet input path in LAN mode (EMAC rx task). Owns `buf`.
static esp_err_t eth_bridge_input(esp_eth_handle_t h, uint8_t *buf, uint32_t len, void *priv)
{
    if (len >= 14) net_stats_add(NET_IF_ETH, len, 0);
    if (!s_bridge_on || len < 14) {
        free(buf);
        return ESP_OK;
    }
    const uint8_t *dst = buf;
    fdb_learn(buf + 6, SIDE_ETH);
    bool to_host = is_mcast(dst) || memcmp(dst, s_ap_mac, 6) == 0;
    bool to_wifi = is_mcast(dst) || (!to_host && fdb_lookup(dst) != SIDE_ETH);
    if (to_wifi) {
        // The Wi-Fi link is slower than 100 Mbit Ethernet: retry briefly when the
        // hosted transport queue is full, then drop (TCP backs off).
        int r = esp_wifi_internal_tx(WIFI_IF_AP, buf, (uint16_t)len);
        for (int i = 0; r != 0 && i < 3; i++) {
            vTaskDelay(1);
            r = esp_wifi_internal_tx(WIFI_IF_AP, buf, (uint16_t)len);
        }
        if (r != 0) s_br_drop++;
    }
    if (to_host && net_ap_inject(buf, len) != ESP_OK) s_br_drop++;
    free(buf);
    return ESP_OK;
}

void eth_bridge_start(void)
{
    if (!s_init || s_mode != ETH_MODE_LAN || s_bridge_on) return;
    if (esp_wifi_get_mac(WIFI_IF_AP, s_ap_mac) != ESP_OK) {
        ESP_LOGW(TAG, "Bridge: cannot read AP MAC");
        return;
    }
    net_set_ap_tap(ap_tap);
    s_bridge_on = true;
    ESP_LOGI(TAG, "L2 bridge Ethernet <-> Wi-Fi AP active (AP MAC " MACSTR ")", MAC2STR(s_ap_mac));
}

void eth_bridge_stop(void)
{
    if (!s_bridge_on) return;
    s_bridge_on = false;
    net_set_ap_tap(NULL);
    ESP_LOGI(TAG, "L2 bridge stopped");
}

bool eth_bridge_active(void) { return s_bridge_on; }

#define ETRY(expr)                                                                  \
    do {                                                                            \
        esp_err_t _e = (expr);                                                      \
        if (_e != ESP_OK) {                                                         \
            ESP_LOGE(TAG, "[FAIL] %s: %s", #expr, esp_err_to_name(_e));             \
            strlcpy(s_err, esp_err_to_name(_e), sizeof(s_err));                     \
            return _e;                                                              \
        }                                                                           \
    } while (0)

esp_err_t eth_init(void)
{
    load_mode();
    ESP_LOGI(TAG, "Initializing Ethernet (IP101 RMII, MDC=31 MDIO=52 RST=51, REF_CLK in GPIO50), mode %s",
             eth_mode_name(s_mode));

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();  // P4 defaults = NANO pinout
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_cfg, &mac_cfg);
    if (!mac) {
        snprintf(s_err, sizeof(s_err), "EMAC init failed");
        ESP_LOGE(TAG, "%s", s_err);
        return ESP_FAIL;
    }
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr = CONFIG_MYPI_ETH_PHY_ADDR;
    phy_cfg.reset_gpio_num = CONFIG_MYPI_ETH_PHY_RST_GPIO;
    // IP101 is IEEE 802.3 compliant; IDF v6 only ships the generic PHY driver.
    esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_cfg);
    if (!phy) {
        mac->del(mac);
        snprintf(s_err, sizeof(s_err), "PHY init failed");
        ESP_LOGE(TAG, "%s", s_err);
        return ESP_FAIL;
    }
    esp_eth_config_t cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_err_t err = esp_eth_driver_install(&cfg, &s_eth);
    if (err != ESP_OK) {
        snprintf(s_err, sizeof(s_err), "driver install: %s (PHY not found?)", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", s_err);
        phy->del(phy);
        mac->del(mac);
        return err;
    }

    ETRY(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));

    if (s_mode == ETH_MODE_LAN) {
        // L2 bridge to the Wi-Fi AP: no netif, raw frames go to eth_bridge_input().
        // Promiscuous so frames addressed to Wi-Fi clients / the AP MAC are received.
        bool promisc = true;
        ETRY(esp_eth_update_input_path(s_eth, eth_bridge_input, NULL));
        ETRY(esp_eth_ioctl(s_eth, ETH_CMD_S_PROMISCUOUS, &promisc));
        ETRY(esp_eth_start(s_eth));
        s_init = true;
        s_err[0] = 0;
        ESP_LOGI(TAG, "LAN mode: bridged to the Wi-Fi AP (shared subnet, waiting for AP start)");
        return ESP_OK;
    }

    esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_ETH();
    base.route_prio = 200;      // preferred over Wi-Fi STA (100) as uplink
    esp_netif_config_t ncfg = {.base = &base, .driver = NULL, .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH};
    s_netif = esp_netif_new(&ncfg);
    if (!s_netif) {
        snprintf(s_err, sizeof(s_err), "esp_netif_new failed");
        return ESP_FAIL;
    }
    esp_netif_set_hostname(s_netif, CONFIG_MYPI_HOSTNAME);
    ETRY(esp_netif_attach(s_netif, esp_eth_new_netif_glue(s_eth)));
    ETRY(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_event, NULL));
    ETRY(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP, on_event, NULL));
    net_stats_register(NET_IF_ETH, s_netif);
    ETRY(esp_eth_start(s_eth));
    s_init = true;
    s_err[0] = 0;
    return ESP_OK;
}

void eth_get_info(eth_info_t *o)
{
    memset(o, 0, sizeof(*o));
    o->initialized = s_init;
    o->mode = s_mode;
    o->saved_mode = s_saved_mode;
    o->link_up = s_link;
    o->has_ip = s_has_ip;
    strlcpy(o->error, s_err, sizeof(o->error));
    // LAN mode is a pure L2 bridge port: no IP/gateway of its own (device IP is on the AP netif).
    if (s_mode == ETH_MODE_WAN) {
        strcpy(o->ip, "0.0.0.0");
        strcpy(o->gw, "0.0.0.0");
    }
    if (!s_init) return;
    if (s_link) {
        eth_speed_t sp;
        eth_duplex_t dx;
        if (esp_eth_ioctl(s_eth, ETH_CMD_G_SPEED, &sp) == ESP_OK) o->speed_mbps = sp == ETH_SPEED_100M ? 100 : 10;
        if (esp_eth_ioctl(s_eth, ETH_CMD_G_DUPLEX_MODE, &dx) == ESP_OK) o->full_duplex = dx == ETH_DUPLEX_FULL;
    }
    uint8_t m[6];
    if (esp_eth_ioctl(s_eth, ETH_CMD_G_MAC_ADDR, m) == ESP_OK) {
        snprintf(o->mac, sizeof(o->mac), MACSTR, MAC2STR(m));
    }
    o->bridged = s_bridge_on;
    o->bridge_drops = s_br_drop;
    esp_netif_ip_info_t ip;
    if (s_mode == ETH_MODE_WAN && s_netif && esp_netif_get_ip_info(s_netif, &ip) == ESP_OK) {
        snprintf(o->ip, sizeof(o->ip), IPSTR, IP2STR(&ip.ip));
        snprintf(o->gw, sizeof(o->gw), IPSTR, IP2STR(&ip.gw));
    }
}
