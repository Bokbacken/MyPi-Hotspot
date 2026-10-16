#include "net.h"

#include <string.h>

#include "esp_log.h"
#include "esp_netif_net_stack.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "iperf.h"
#include "lwip/lwip_napt.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"

static const char *TAG = "net";

// ---------------------------------------------------------------------------
// NAPT / DHCP server helpers
// ---------------------------------------------------------------------------

typedef struct {
    esp_netif_t *nif;
    bool on;
} napt_ctx_t;

static esp_err_t napt_cb(void *ctx)
{
    napt_ctx_t *c = ctx;
    struct netif *n = esp_netif_get_netif_impl(c->nif);
    if (!n) return ESP_ERR_INVALID_STATE;
    // Returns 0 only if the interface is down.
    return ip_napt_enable_netif(n, c->on ? 1 : 0) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t net_napt(esp_netif_t *nif, bool on)
{
    if (!nif) return ESP_ERR_INVALID_ARG;
    napt_ctx_t c = {nif, on};
    esp_err_t err = esp_netif_tcpip_exec(napt_cb, &c);
    ESP_LOGI(TAG, "NAPT %s on %s: %s", on ? "enable" : "disable", esp_netif_get_desc(nif), esp_err_to_name(err));
    return err;
}

esp_err_t net_dhcps_setup(esp_netif_t *nif, uint8_t a, uint8_t b, uint8_t c, const char *dns)
{
    if (!nif) return ESP_ERR_INVALID_ARG;
    esp_netif_dhcps_stop(nif);  // may report "already stopped"
    esp_netif_ip_info_t ip = {0};
    esp_netif_set_ip4_addr(&ip.ip, a, b, c, 1);
    esp_netif_set_ip4_addr(&ip.gw, a, b, c, 1);
    esp_netif_set_ip4_addr(&ip.netmask, 255, 255, 255, 0);
    esp_err_t err = esp_netif_set_ip_info(nif, &ip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_ip_info(%s): %s", esp_netif_get_desc(nif), esp_err_to_name(err));
        return err;
    }
    uint8_t offer_dns = 1;
    esp_netif_dhcps_option(nif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer_dns, sizeof(offer_dns));
    esp_netif_dns_info_t d = {0};
    d.ip.type = ESP_IPADDR_TYPE_V4;
    d.ip.u_addr.ip4.addr = esp_ip4addr_aton(dns && dns[0] ? dns : "8.8.8.8");
    esp_netif_set_dns_info(nif, ESP_NETIF_DNS_MAIN, &d);
    err = esp_netif_dhcps_start(nif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
        ESP_LOGE(TAG, "dhcps_start(%s): %s", esp_netif_get_desc(nif), esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "%s: %d.%d.%d.1/24, DHCP server on, DNS offered %s", esp_netif_get_desc(nif), a, b, c,
             dns && dns[0] ? dns : "8.8.8.8");
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Traffic counters: the lwIP netif input / linkoutput hooks are wrapped per
// interface. Hooks are (re)installed from the sampler task inside the TCP/IP
// context, so interfaces created later (or re-initialised) are picked up.
// ---------------------------------------------------------------------------

typedef struct {
    esp_netif_t *nif;
    struct netif *lwip;
    netif_input_fn orig_input;
    netif_linkoutput_fn orig_linkoutput;
    uint64_t rx_bytes, tx_bytes;
    uint32_t rx_pkts, tx_pkts;
    uint64_t last_rx, last_tx;
    uint32_t rx_bps, tx_bps;
    bool manual;                // counted by net_stats_add() (no lwIP netif, e.g. bridged ETH)
} if_slot_t;

static if_slot_t s_if[NET_IF_COUNT];
static volatile net_tap_fn s_ap_tap;    // L2 bridge tap on the AP netif
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static const char *IF_NAMES[NET_IF_COUNT] = {"sta", "ap", "eth"};

const char *net_if_name(net_if_id_t id) { return id < NET_IF_COUNT ? IF_NAMES[id] : "?"; }

static if_slot_t *slot_for(struct netif *n)
{
    for (int i = 0; i < NET_IF_COUNT; i++) {
        if (s_if[i].lwip == n) return &s_if[i];
    }
    return NULL;
}

static err_t hook_input(struct pbuf *p, struct netif *inp)
{
    if_slot_t *s = slot_for(inp);
    if (!s || !s->orig_input) {
        pbuf_free(p);
        return ERR_OK;
    }
    portENTER_CRITICAL_SAFE(&s_mux);
    s->rx_bytes += p->tot_len;
    s->rx_pkts++;
    portEXIT_CRITICAL_SAFE(&s_mux);
    net_tap_fn tap = s_ap_tap;
    if (tap && s == &s_if[NET_IF_AP] && tap(p, inp, true)) {
        pbuf_free(p);   // consumed by the bridge (not addressed to this host)
        return ERR_OK;
    }
    return s->orig_input(p, inp);
}

static err_t hook_linkoutput(struct netif *n, struct pbuf *p)
{
    if_slot_t *s = slot_for(n);
    if (!s || !s->orig_linkoutput) return ERR_IF;
    portENTER_CRITICAL_SAFE(&s_mux);
    s->tx_bytes += p->tot_len;
    s->tx_pkts++;
    portEXIT_CRITICAL_SAFE(&s_mux);
    net_tap_fn tap = s_ap_tap;
    if (tap && s == &s_if[NET_IF_AP] && tap(p, n, false)) {
        return ERR_OK;  // sent over the bridge only (destination is on Ethernet)
    }
    return s->orig_linkoutput(n, p);
}

static esp_err_t install_hooks_cb(void *ctx)
{
    for (int i = 0; i < NET_IF_COUNT; i++) {
        if_slot_t *s = &s_if[i];
        if (!s->nif) continue;
        struct netif *n = esp_netif_get_netif_impl(s->nif);
        if (!n) continue;
        s->lwip = n;
        if (n->input && n->input != hook_input) {
            s->orig_input = n->input;
            n->input = hook_input;
        }
        if (n->linkoutput && n->linkoutput != hook_linkoutput) {
            s->orig_linkoutput = n->linkoutput;
            n->linkoutput = hook_linkoutput;
        }
    }
    return ESP_OK;
}

void net_stats_register(net_if_id_t id, esp_netif_t *nif)
{
    if (id >= NET_IF_COUNT) return;
    s_if[id].nif = nif;
    esp_netif_tcpip_exec(install_hooks_cb, NULL);
}

void net_set_ap_tap(net_tap_fn tap)
{
    s_ap_tap = tap;
}

esp_err_t net_ap_inject(const uint8_t *frame, size_t len)
{
    if_slot_t *s = &s_if[NET_IF_AP];
    struct netif *n = s->lwip;
    if (!n || !s->orig_input || len == 0 || len > 1600) return ESP_ERR_INVALID_STATE;
    struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_RAM);
    if (!p) return ESP_ERR_NO_MEM;
    pbuf_take(p, frame, (u16_t)len);
    // orig_input is tcpip_input (thread-safe); bypasses the tap so frames do not loop.
    if (s->orig_input(p, n) != ERR_OK) {
        pbuf_free(p);
        return ESP_FAIL;
    }
    return ESP_OK;
}

void net_stats_add(net_if_id_t id, uint32_t rx_bytes, uint32_t tx_bytes)
{
    if (id >= NET_IF_COUNT) return;
    portENTER_CRITICAL_SAFE(&s_mux);
    if_slot_t *s = &s_if[id];
    s->manual = true;
    if (rx_bytes) {
        s->rx_bytes += rx_bytes;
        s->rx_pkts++;
    }
    if (tx_bytes) {
        s->tx_bytes += tx_bytes;
        s->tx_pkts++;
    }
    portEXIT_CRITICAL_SAFE(&s_mux);
}

void net_stats_get(net_if_id_t id, net_if_stats_t *out)
{
    memset(out, 0, sizeof(*out));
    if (id >= NET_IF_COUNT) return;
    portENTER_CRITICAL(&s_mux);
    const if_slot_t *s = &s_if[id];
    out->present = (s->nif != NULL && s->lwip != NULL) || s->manual;
    out->rx_bytes = s->rx_bytes;
    out->tx_bytes = s->tx_bytes;
    out->rx_pkts = s->rx_pkts;
    out->tx_pkts = s->tx_pkts;
    out->rx_bps = s->rx_bps;
    out->tx_bps = s->tx_bps;
    portEXIT_CRITICAL(&s_mux);
}

// ---------------------------------------------------------------------------
// iperf server control
// ---------------------------------------------------------------------------

static volatile bool s_iperf_armed;
static volatile bool s_iperf_running;
static volatile bool s_iperf_instance_alive;
static volatile uint32_t s_iperf_left_s;
static iperf_id_t s_iperf_id = -1;
static float s_last_mbps, s_summary_mbps;
static uint64_t s_total_bytes;

// Overrides the component's weak default so results are visible in the web UI.
void iperf_report_output(const iperf_report_t *r)
{
    if (r->report_type == IPERF_REPORT_PERIOD) {
        uint32_t secs = r->traffic.end_sec - r->traffic.period_start_sec;
        s_last_mbps = secs ? (float)(r->traffic.period_bytes * 8.0 / 1e6 / secs) : 0;
        s_total_bytes = r->traffic.total_transfer_bytes;
        s_iperf_running = true;
    } else if (r->report_type == IPERF_REPORT_SUMMARY) {
        uint32_t secs = r->traffic.end_sec;
        s_summary_mbps = secs ? (float)(r->traffic.total_transfer_bytes * 8.0 / 1e6 / secs) : 0;
        s_total_bytes = r->traffic.total_transfer_bytes;
        ESP_LOGI(TAG, "iperf: %.2f Mbit/s average, %llu bytes in %lu s", s_summary_mbps,
                 (unsigned long long)s_total_bytes, (unsigned long)secs);
    } else if (r->report_type == IPERF_REPORT_CONNECT_INFO) {
        ESP_LOGI(TAG, "iperf: client connected");
    }
}

static void iperf_state_cb(iperf_id_t id, iperf_state_data_t *d, void *priv)
{
    if (d->state == IPERF_CLOSED) {
        s_iperf_running = false;
        s_iperf_instance_alive = false;
    } else if (d->state == IPERF_STARTED) {
        s_iperf_running = true;
    }
}

static void iperf_arm_instance(void)
{
    esp_ip_addr_t any = {.type = ESP_IPADDR_TYPE_V4};
    iperf_cfg_t cfg = IPERF_DEFAULT_CONFIG_SERVER(IPERF_FLAG_TCP, any);
    cfg.time = 3600;        // server runs until the client disconnects
    cfg.interval = 1;
    cfg.state_handler = iperf_state_cb;
    s_iperf_instance_alive = true;
    s_iperf_id = iperf_start_instance(&cfg);
    if (s_iperf_id < 0) {
        s_iperf_instance_alive = false;
        ESP_LOGW(TAG, "iperf_start_instance failed");
    } else {
        // The component logs an error every time accept() times out (5 s) while
        // nobody connects; our own log lines report the results instead.
        char tag[24];
        snprintf(tag, sizeof(tag), "iperf(id=%d)", s_iperf_id);
        esp_log_level_set(tag, ESP_LOG_NONE);
    }
}

esp_err_t net_iperf_start(uint32_t window_s)
{
    if (window_s < 10) window_s = 10;
    if (window_s > 3600) window_s = 3600;
    esp_log_level_set("iperf", ESP_LOG_WARN);
    s_iperf_left_s = window_s;
    if (!s_iperf_armed) {
        ESP_LOGI(TAG, "iperf TCP server armed on port 5001 for %lu s - run: iperf -c <ip> -t 10 -i 1",
                 (unsigned long)window_s);
    }
    s_iperf_armed = true;
    return ESP_OK;
}

void net_iperf_stop(void)
{
    if (s_iperf_armed) ESP_LOGI(TAG, "iperf server disarmed");
    s_iperf_armed = false;
    s_iperf_left_s = 0;
    if (s_iperf_instance_alive) iperf_stop_instance(IPERF_ALL_INSTANCES_ID);
}

void net_iperf_get(net_iperf_status_t *out)
{
    out->armed = s_iperf_armed;
    out->armed_left_s = s_iperf_left_s;
    out->running = s_iperf_running;
    out->last_mbps = s_last_mbps;
    out->summary_mbps = s_summary_mbps;
    out->total_bytes = s_total_bytes;
}

// ---------------------------------------------------------------------------
// 1 s sampler task: rates, hook maintenance, iperf re-arming
// ---------------------------------------------------------------------------

static void sampler_task(void *arg)
{
    TickType_t last = xTaskGetTickCount();
    while (true) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(1000));
        esp_netif_tcpip_exec(install_hooks_cb, NULL);
        portENTER_CRITICAL(&s_mux);
        for (int i = 0; i < NET_IF_COUNT; i++) {
            if_slot_t *s = &s_if[i];
            s->rx_bps = (uint32_t)(s->rx_bytes - s->last_rx);
            s->tx_bps = (uint32_t)(s->tx_bytes - s->last_tx);
            s->last_rx = s->rx_bytes;
            s->last_tx = s->tx_bytes;
        }
        portEXIT_CRITICAL(&s_mux);

        if (s_iperf_armed) {
            if (!s_iperf_running && s_iperf_left_s > 0) s_iperf_left_s--;
            if (s_iperf_left_s == 0) {
                net_iperf_stop();
            } else if (!s_iperf_instance_alive) {
                iperf_arm_instance();
            }
        }
    }
}

void net_stats_start(void)
{
    static bool started;
    if (started) return;
    started = true;
    xTaskCreate(sampler_task, "net_stats", 4096, NULL, 3, NULL);
}
