#pragma once

// Shared networking helpers: NAPT, DHCP server setup, per-interface traffic
// counters (/api/netstats) and the iperf throughput test server.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"
#include "esp_netif.h"

typedef enum { NET_IF_STA = 0, NET_IF_AP, NET_IF_ETH, NET_IF_COUNT } net_if_id_t;

typedef struct {
    bool present;
    uint64_t rx_bytes, tx_bytes;
    uint32_t rx_pkts, tx_pkts;
    float rx_mbps, tx_mbps;     // Mbit/s over the last 1 s sample
    float rx_mbps_peak, tx_mbps_peak;   // highest 1 s rate since boot / last reset (Mbit/s)
    uint32_t tx_drops;          // frames the driver refused on transmit (queue full / error)
} net_if_stats_t;

/** Clear the peak rates of all interfaces. */
void net_stats_reset_peaks(void);

// --- Traffic log: 1 sample/s ring buffer in PSRAM (allocated on first start) ---
#define NET_LOG_MAX_SAMPLES 3600

typedef struct {
    uint32_t ts;                        // seconds since the log was started
    time_t wall_time;                   // absolute sample time, or 0 until RTC/NTP time is valid
    float rx_mbps[NET_IF_COUNT];        // Mbit/s
    float tx_mbps[NET_IF_COUNT];
} net_log_sample_t;

/** Start (or restart, clearing old samples) the traffic log. */
esp_err_t net_log_start(void);
void net_log_stop(void);
bool net_log_is_running(void);
/** True once the log has been started at least once since boot. */
bool net_log_ever_started(void);
uint32_t net_log_sample_count(void);
/** Write callback for net_log_write_csv(); return false to abort. */
typedef bool (*net_log_write_fn)(const char *buf, size_t len, void *ctx);
/** Stream the log as CSV (header + one row per sample, oldest first). */
esp_err_t net_log_write_csv(net_log_write_fn fn, void *ctx);

/** Start the 1 s sampler task (hooks interfaces as they appear). */
void net_stats_start(void);
/** Register an esp_netif for counting under the given id. */
void net_stats_register(net_if_id_t id, esp_netif_t *nif);
void net_stats_get(net_if_id_t id, net_if_stats_t *out);
const char *net_if_name(net_if_id_t id);
/** Count traffic for an interface without an lwIP netif (bridged Ethernet in LAN mode). */
void net_stats_add(net_if_id_t id, uint32_t rx_bytes, uint32_t tx_bytes);

// --- L2 bridge support (Ethernet LAN <-> Wi-Fi AP) ---
struct pbuf;
struct netif;
/**
 * Tap on the AP lwIP netif. rx=true: frame received from a Wi-Fi client (before
 * lwIP); rx=false: frame lwIP is about to send to Wi-Fi. Return true to consume
 * the frame (rx: not passed to lwIP, tx: not sent over Wi-Fi).
 */
typedef bool (*net_tap_fn)(struct pbuf *p, struct netif *n, bool rx);
void net_set_ap_tap(net_tap_fn tap);
/** Hand an Ethernet frame to the AP lwIP netif as if it came from a Wi-Fi client. */
esp_err_t net_ap_inject(const uint8_t *frame, size_t len);

/**
 * Enable/disable NAPT on an (inbound) interface. Unlike esp_netif_napt_enable()
 * this does not disable NAPT on other interfaces, so AP and Ethernet-LAN can both
 * be NATed. The interface must be up.
 */
esp_err_t net_napt(esp_netif_t *nif, bool on);

/** (Re)configure a DHCP server interface: static ip/24, DNS offered to clients. */
esp_err_t net_dhcps_setup(esp_netif_t *nif, uint8_t a, uint8_t b, uint8_t c, const char *dns);

// --- iperf (TCP server, port 5001) ---
typedef struct {
    bool armed;                 // server is (re)listening
    uint32_t armed_left_s;      // seconds until auto-disarm
    bool running;               // a client is connected and sending
    float last_mbps;            // last 1 s interval
    float summary_mbps;         // average of the last finished test
    uint64_t total_bytes;
} net_iperf_status_t;

/** Arm the iperf TCP server for window_s seconds (re-listens after each test). */
esp_err_t net_iperf_start(uint32_t window_s);
void net_iperf_stop(void);
void net_iperf_get(net_iperf_status_t *out);
