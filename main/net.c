#include "net.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif_net_stack.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
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
    float rx_mbps, tx_mbps;             // Mbit/s over the last 1 s sample
    float rx_mbps_peak, tx_mbps_peak;
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
    out->rx_mbps = s->rx_mbps;
    out->tx_mbps = s->tx_mbps;
    out->rx_mbps_peak = s->rx_mbps_peak;
    out->tx_mbps_peak = s->tx_mbps_peak;
    portEXIT_CRITICAL(&s_mux);
}

void net_stats_reset_peaks(void)
{
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < NET_IF_COUNT; i++) {
        s_if[i].rx_mbps_peak = 0.0f;
        s_if[i].tx_mbps_peak = 0.0f;
    }
    portEXIT_CRITICAL(&s_mux);
}

// ---------------------------------------------------------------------------
// Traffic log (ring buffer in PSRAM, one sample per second)
// ---------------------------------------------------------------------------

static net_log_sample_t *s_log;         // NET_LOG_MAX_SAMPLES entries, allocated lazily
static SemaphoreHandle_t s_log_mtx;
static uint32_t s_log_head;             // index of the oldest sample
static uint32_t s_log_count;
static TickType_t s_log_t0;             // tick count when the log was started
static volatile bool s_log_running;
static volatile bool s_log_ever;

static bool log_lock_to(TickType_t to)
{
    if (!s_log_mtx) {
        static portMUX_TYPE init_mux = portMUX_INITIALIZER_UNLOCKED;
        SemaphoreHandle_t m = xSemaphoreCreateMutex();
        if (!m) return false;
        portENTER_CRITICAL(&init_mux);
        if (!s_log_mtx) {
            s_log_mtx = m;
            m = NULL;
        }
        portEXIT_CRITICAL(&init_mux);
        if (m) vSemaphoreDelete(m);
    }
    return xSemaphoreTake(s_log_mtx, to) == pdTRUE;
}

static bool log_lock(void) { return log_lock_to(portMAX_DELAY); }

static void log_unlock(void) { xSemaphoreGive(s_log_mtx); }

esp_err_t net_log_start(void)
{
    if (!log_lock()) return ESP_ERR_NO_MEM;
    if (!s_log) {
        s_log = heap_caps_malloc(sizeof(net_log_sample_t) * NET_LOG_MAX_SAMPLES, MALLOC_CAP_SPIRAM);
        if (!s_log) {
            log_unlock();
            ESP_LOGE(TAG, "traffic log: PSRAM allocation failed");
            return ESP_ERR_NO_MEM;
        }
    }
    s_log_head = 0;
    s_log_count = 0;
    s_log_t0 = xTaskGetTickCount();
    s_log_running = true;
    s_log_ever = true;
    log_unlock();
    ESP_LOGI(TAG, "traffic log started (max %d samples)", NET_LOG_MAX_SAMPLES);
    return ESP_OK;
}

void net_log_stop(void)
{
    if (!log_lock()) return;
    bool was = s_log_running;
    s_log_running = false;
    uint32_t n = s_log_count;
    log_unlock();
    if (was) ESP_LOGI(TAG, "traffic log stopped, %lu samples", (unsigned long)n);
}

bool net_log_is_running(void) { return s_log_running; }
bool net_log_ever_started(void) { return s_log_ever; }

uint32_t net_log_sample_count(void)
{
    if (!log_lock()) return 0;
    uint32_t n = s_log_count;
    log_unlock();
    return n;
}

static void log_add(const float *rx, const float *tx)
{
    // Never stall the sampler: if a CSV download holds the lock, drop this sample.
    if (!s_log_running || !log_lock_to(pdMS_TO_TICKS(20))) return;
    if (s_log_running && s_log) {
        uint32_t idx = (s_log_head + s_log_count) % NET_LOG_MAX_SAMPLES;
        if (s_log_count == NET_LOG_MAX_SAMPLES) {
            s_log_head = (s_log_head + 1) % NET_LOG_MAX_SAMPLES;  // overwrite the oldest
        } else {
            s_log_count++;
        }
        net_log_sample_t *e = &s_log[idx];
        e->ts = (uint32_t)((uint64_t)(xTaskGetTickCount() - s_log_t0) * portTICK_PERIOD_MS / 1000);
        memcpy(e->rx_mbps, rx, sizeof(e->rx_mbps));
        memcpy(e->tx_mbps, tx, sizeof(e->tx_mbps));
    }
    log_unlock();
}

esp_err_t net_log_write_csv(net_log_write_fn fn, void *ctx)
{
    if (!fn) return ESP_ERR_INVALID_ARG;
    static const char header[] = "ts,sta_rx_mbps,sta_tx_mbps,ap_rx_mbps,ap_tx_mbps,eth_rx_mbps,eth_tx_mbps\n";
    if (!fn(header, sizeof(header) - 1, ctx)) return ESP_FAIL;
    if (!log_lock()) return ESP_ERR_NO_MEM;
    // Rows are batched into a chunk buffer; the lock is held while streaming so
    // the sampler cannot overwrite entries mid-read (it only misses samples).
    char buf[1024];
    size_t used = 0;
    esp_err_t err = ESP_OK;
    for (uint32_t i = 0; i < s_log_count && s_log; i++) {
        const net_log_sample_t *e = &s_log[(s_log_head + i) % NET_LOG_MAX_SAMPLES];
        char row[96];
        int n = snprintf(row, sizeof(row), "%lu,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f\n", (unsigned long)e->ts,
                         (double)e->rx_mbps[NET_IF_STA], (double)e->tx_mbps[NET_IF_STA],
                         (double)e->rx_mbps[NET_IF_AP], (double)e->tx_mbps[NET_IF_AP],
                         (double)e->rx_mbps[NET_IF_ETH], (double)e->tx_mbps[NET_IF_ETH]);
        if (n <= 0) continue;
        if (used + (size_t)n > sizeof(buf)) {
            if (!fn(buf, used, ctx)) {
                err = ESP_FAIL;
                break;
            }
            used = 0;
        }
        memcpy(buf + used, row, (size_t)n);
        used += (size_t)n;
    }
    if (err == ESP_OK && used && !fn(buf, used, ctx)) err = ESP_FAIL;
    log_unlock();
    return err;
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
        float rx[NET_IF_COUNT], tx[NET_IF_COUNT];
        portENTER_CRITICAL(&s_mux);
        for (int i = 0; i < NET_IF_COUNT; i++) {
            if_slot_t *s = &s_if[i];
            s->rx_mbps = (float)(s->rx_bytes - s->last_rx) * 8.0f / 1e6f;
            s->tx_mbps = (float)(s->tx_bytes - s->last_tx) * 8.0f / 1e6f;
            s->last_rx = s->rx_bytes;
            s->last_tx = s->tx_bytes;
            if (s->rx_mbps > s->rx_mbps_peak) s->rx_mbps_peak = s->rx_mbps;
            if (s->tx_mbps > s->tx_mbps_peak) s->tx_mbps_peak = s->tx_mbps;
            rx[i] = s->rx_mbps;
            tx[i] = s->tx_mbps;
        }
        portEXIT_CRITICAL(&s_mux);
        log_add(rx, tx);

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
