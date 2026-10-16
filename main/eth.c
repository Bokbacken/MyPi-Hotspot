#include "eth.h"

#include <stdio.h>
#include <string.h>

#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
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
            if (s_mode == ETH_MODE_LAN) {
                net_dhcps_setup(s_netif, 192, 168, 5, "8.8.8.8");
                net_napt(s_netif, true);
            }
            break;
        case ETHERNET_EVENT_CONNECTED: {
            eth_speed_t sp = ETH_SPEED_10M;
            eth_duplex_t dx = ETH_DUPLEX_HALF;
            esp_eth_ioctl(s_eth, ETH_CMD_G_SPEED, &sp);
            esp_eth_ioctl(s_eth, ETH_CMD_G_DUPLEX_MODE, &dx);
            s_link = true;
            ESP_LOGI(TAG, "Link up: %s Mbit/s %s duplex", sp == ETH_SPEED_100M ? "100" : "10",
                     dx == ETH_DUPLEX_FULL ? "full" : "half");
            if (s_mode == ETH_MODE_LAN) {
                s_has_ip = true;
                net_napt(s_netif, true);
            }
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

    esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_ETH();
    static esp_netif_ip_info_t lan_ip;
    if (s_mode == ETH_MODE_LAN) {
        esp_netif_set_ip4_addr(&lan_ip.ip, 192, 168, 5, 1);
        esp_netif_set_ip4_addr(&lan_ip.gw, 192, 168, 5, 1);
        esp_netif_set_ip4_addr(&lan_ip.netmask, 255, 255, 255, 0);
        base.flags = (esp_netif_flags_t)(ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP);
        base.ip_info = &lan_ip;
        base.get_ip_event = 0;
        base.lost_ip_event = 0;
        base.route_prio = 5;        // never the default route
    } else {
        base.route_prio = 200;      // preferred over Wi-Fi STA (100) as uplink
    }
    esp_netif_config_t ncfg = {.base = &base, .driver = NULL, .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH};
    s_netif = esp_netif_new(&ncfg);
    if (!s_netif) {
        snprintf(s_err, sizeof(s_err), "esp_netif_new failed");
        return ESP_FAIL;
    }
    esp_netif_set_hostname(s_netif, CONFIG_MYPI_HOSTNAME);
    ETRY(esp_netif_attach(s_netif, esp_eth_new_netif_glue(s_eth)));
    ETRY(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
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
    strcpy(o->ip, "0.0.0.0");
    strcpy(o->gw, "0.0.0.0");
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
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(s_netif, &ip) == ESP_OK) {
        snprintf(o->ip, sizeof(o->ip), IPSTR, IP2STR(&ip.ip));
        snprintf(o->gw, sizeof(o->gw), IPSTR, IP2STR(&ip.gw));
    }
}
