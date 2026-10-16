#pragma once

// On-board Ethernet (internal EMAC + IP101 RMII PHY on the ESP32-P4-NANO).
// Mode is stored in NVS and applied at boot:
//   LAN (default): static 192.168.5.1/24 with DHCP server, NATed out via the Wi-Fi uplink
//   WAN:           DHCP client, becomes the default route (uplink) while it has an IP

#include <stdbool.h>

#include "esp_err.h"
#include "esp_netif.h"

typedef enum { ETH_MODE_LAN = 0, ETH_MODE_WAN = 1 } eth_mode_t;

typedef struct {
    bool initialized;
    eth_mode_t mode;            // active mode (since boot)
    eth_mode_t saved_mode;      // mode stored in NVS (applied after reboot)
    bool link_up;
    bool has_ip;
    int speed_mbps;             // 10 / 100, 0 if unknown
    bool full_duplex;
    char ip[16];
    char gw[16];
    char mac[18];
    char error[64];
} eth_info_t;

/** Initialize Ethernet (needs esp_netif_init + default event loop). Never aborts. */
esp_err_t eth_init(void);
void eth_get_info(eth_info_t *out);
eth_mode_t eth_mode(void);
esp_netif_t *eth_netif(void);
/** True in WAN mode while Ethernet has an IP (it is then the uplink). */
bool eth_is_wan_uplink(void);
/** Store the mode in NVS; takes effect after reboot. */
esp_err_t eth_set_mode(eth_mode_t mode);
const char *eth_mode_name(eth_mode_t m);
