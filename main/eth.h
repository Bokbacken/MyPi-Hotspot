#pragma once

// On-board Ethernet (internal EMAC + IP101 RMII PHY on the ESP32-P4-NANO).
// Mode is stored in NVS and applied at boot:
//   LAN (default): transparent L2 bridge to the Wi-Fi AP. Wired clients share the
//                  AP subnet (e.g. 192.168.4.0/24); the AP netif provides IP, DHCP and NAT.
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
    bool bridged;               // LAN mode: bridge to the AP is forwarding
    uint32_t bridge_drops;      // frames dropped by the bridge (queue full etc.)
    uint32_t bridge_inject_drops;   // subset: ETH -> host (lwIP) drops
    bool rescue;                // emergency mode active (standalone interface, not stored)
    bool rescue_dhcp;           // rescue: DHCP client (false = static IP)
} eth_info_t;

/** LAN mode: start/stop forwarding between Ethernet and the Wi-Fi AP (call on AP start/stop). */
void eth_bridge_start(void);
void eth_bridge_stop(void);
bool eth_bridge_active(void);

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

/**
 * Emergency (rescue) mode: give Ethernet its own IP stack independent of Wi-Fi /
 * ESP-Hosted. In LAN mode the L2 bridge is stopped and a netif is created at
 * runtime. dhcp = DHCP client, otherwise static a.b.c.d/24 with gateway a.b.c.1.
 * Can be called again to switch between DHCP and static. Not stored in NVS.
 */
esp_err_t eth_rescue_apply(bool dhcp, uint8_t a, uint8_t b, uint8_t c, uint8_t d);
bool eth_rescue_active(void);
