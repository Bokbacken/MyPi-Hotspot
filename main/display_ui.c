#include "display_ui.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "diag.h"
#include "display.h"
#include "display_font.h"
#include "esp_log.h"
#include "eth.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.h"
#include "wifi.h"

static const char *TAG = "display_ui";

#define UI_PERIOD_MS    2000
#define UI_AREA_BOTTOM  160     // Rows 0..159 are owned (and cleared) by the status screen
#define CHAR_W          6
#define CHAR_H          8

static TaskHandle_t s_task;
static volatile bool s_stop;

// ---------------------------------------------------------------------------
// Drawing primitives
// ---------------------------------------------------------------------------

void display_draw_char(int x, int y, char ch, uint16_t fg, uint16_t bg, int scale)
{
    if (scale < 1) scale = 1;
    const uint8_t *glyph;
    if (ch == DISPLAY_CH_UP) {
        glyph = g_font_arrow_up;
    } else if (ch == DISPLAY_CH_DOWN) {
        glyph = g_font_arrow_down;
    } else {
        if (ch < 0x20 || ch > 0x7E) ch = '?';
        glyph = g_font5x7[ch - 0x20];
    }
    // 6x8 cell: 5 glyph columns + 1 spacing column, 7 rows + 1 spacing row.
    for (int col = 0; col < CHAR_W; col++) {
        uint8_t bits = col < 5 ? glyph[col] : 0;
        for (int row = 0; row < CHAR_H; row++) {
            uint16_t c = (bits >> row) & 1 ? fg : bg;
            if (scale == 1) {
                display_draw_pixel(x + col, y + row, c);
            } else {
                display_fill_rect(x + col * scale, y + row * scale, x + (col + 1) * scale, y + (row + 1) * scale, c);
            }
        }
    }
}

int display_draw_str(int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale)
{
    if (scale < 1) scale = 1;
    for (; s && *s; s++) {
        if (x >= LCD_H_RES) break;
        display_draw_char(x, y, *s, fg, bg, scale);
        x += CHAR_W * scale;
    }
    return x;
}

void display_draw_rect_outline(int x0, int y0, int x1, int y1, uint16_t color)
{
    display_fill_rect(x0, y0, x1 + 1, y0 + 1, color);   // top
    display_fill_rect(x0, y1, x1 + 1, y1 + 1, color);   // bottom
    display_fill_rect(x0, y0, x0 + 1, y1 + 1, color);   // left
    display_fill_rect(x1, y0, x1 + 1, y1 + 1, color);   // right
}

void display_draw_wifi_icon(int cx, int cy, uint16_t color)
{
    // Three 90-degree arcs opening upwards (45..135 degrees), 2 px thick.
    static const int radii[] = {4, 7, 10};
    for (int i = 0; i < 3; i++) {
        for (int t = 0; t < 2; t++) {
            float r = (float)(radii[i] - t);
            for (int a = 45; a <= 135; a += 3) {
                float rad = (float)a * (float)M_PI / 180.0f;
                display_draw_pixel(cx + (int)lroundf(r * cosf(rad)), cy - (int)lroundf(r * sinf(rad)), color);
            }
        }
    }
    display_draw_circle(cx, cy, 1, color);
}

// ---------------------------------------------------------------------------
// Status screen
// ---------------------------------------------------------------------------

static float mbit(uint32_t bytes_per_s) { return (float)bytes_per_s * 8.0f / 1e6f; }

/** "<label> v XX.X ^ XX.X Mbit/s" with the label white and values in val_color. */
static void draw_rate_row(int y, const char *label, uint32_t rx, uint32_t tx, uint16_t val_color)
{
    char v[16];
    char arrow[2] = {0, 0};
    int x = display_draw_str(0, y, label, COL_WHITE, COL_BLACK, 1);
    arrow[0] = DISPLAY_CH_DOWN;
    x = display_draw_str(x + CHAR_W, y, arrow, COL_WHITE, COL_BLACK, 1);
    snprintf(v, sizeof(v), "%5.1f", mbit(rx));
    x = display_draw_str(x, y, v, val_color, COL_BLACK, 1);
    arrow[0] = DISPLAY_CH_UP;
    x = display_draw_str(x + CHAR_W, y, arrow, COL_WHITE, COL_BLACK, 1);
    snprintf(v, sizeof(v), "%5.1f", mbit(tx));
    x = display_draw_str(x, y, v, val_color, COL_BLACK, 1);
    display_draw_str(x + CHAR_W, y, "Mbit/s", COL_WHITE, COL_BLACK, 1);
}

static void draw_status(void)
{
    static diag_status_t d;     // Large (touch history); keep it off the task stack
    wifi_ap_info_t ap;
    wifi_sta_extra_t ex;
    eth_info_t eth;
    diag_get(&d);
    wifi_get_ap_info(&ap);
    wifi_get_sta_extra(&ex);
    eth_get_info(&eth);
    char buf[48];

    display_lock();
    display_fill_rect(0, 0, LCD_H_RES, UI_AREA_BOTTOM, COL_BLACK);

    // Row 0: title + Wi-Fi icon when the AP is running.
    display_draw_str(0, 0, "MyPi-Hotspot", COL_WHITE, COL_BLACK, 1);
    if (ap.running) display_draw_wifi_icon(LCD_H_RES - 14, 14, COL_GREEN);

    // Row 1: uplink IP (large).
    bool sta_ip = d.wifi_state == WIFI_STATE_GOT_IP && d.ip[0] && strcmp(d.ip, "0.0.0.0") != 0;
    if (sta_ip) {
        display_draw_str(0, 24, d.ip, COL_GREEN, COL_BLACK, 2);
    } else if (eth.mode == ETH_MODE_WAN && eth.has_ip) {
        display_draw_str(0, 24, eth.ip, COL_CYAN, COL_BLACK, 2);
    } else {
        display_draw_str(0, 24, "No uplink", COL_RED, COL_BLACK, 2);
    }

    // Row 2: uplink SSID + channel.
    if (d.ssid[0] && d.wifi_state >= WIFI_STATE_CONNECTING) {
        if (ap.sta_channel) {
            snprintf(buf, sizeof(buf), "%.28s ch%d", d.ssid, ap.sta_channel);
        } else {
            snprintf(buf, sizeof(buf), "%.32s", d.ssid);
        }
        display_draw_str(0, 44, buf, sta_ip ? COL_WHITE : COL_GRAY, COL_BLACK, 1);
    } else {
        display_draw_str(0, 44, ex.sta_manual_off ? "STA: disconnected" : "STA: not connected", COL_GRAY,
                         COL_BLACK, 1);
    }

    // Rows 3-4: access point.
    if (ap.running) {
        snprintf(buf, sizeof(buf), "AP: %d client%s  ch%d", ap.clients, ap.clients == 1 ? "" : "s", ap.channel);
        display_draw_str(0, 60, buf, COL_GREEN, COL_BLACK, 1);
        snprintf(buf, sizeof(buf), "%.32s", ap.ssid);
        display_draw_str(0, 76, buf, COL_WHITE, COL_BLACK, 1);
    } else {
        display_draw_str(0, 60, "AP: 0 clients", COL_GRAY, COL_BLACK, 1);
        display_draw_str(0, 76, "AP off", COL_GRAY, COL_BLACK, 1);
    }

    // Row 5: Ethernet.
    if (!eth.initialized) {
        display_draw_str(0, 92, "ETH: --", COL_GRAY, COL_BLACK, 1);
    } else if (!eth.link_up) {
        display_draw_str(0, 92, "ETH: down", COL_RED, COL_BLACK, 1);
    } else if (eth.mode == ETH_MODE_LAN) {
        display_draw_str(0, 92, "ETH: LAN UP", COL_GREEN, COL_BLACK, 1);
    } else {
        display_draw_str(0, 92, eth.has_ip ? "ETH: WAN UP" : "ETH: WAN (no IP)", COL_CYAN, COL_BLACK, 1);
    }

    // Rows 6-7: uplink throughput (STA, or Ethernet when it is the WAN uplink).
    bool eth_up = strcmp(ex.uplink, "eth") == 0;
    net_if_stats_t ns;
    net_stats_get(eth_up ? NET_IF_ETH : NET_IF_STA, &ns);
    draw_rate_row(108, eth_up ? "ETH " : "STA ", ns.rx_bps, ns.tx_bps, COL_CYAN);
    draw_rate_row(124, "peak", ns.rx_bps_peak, ns.tx_bps_peak, COL_YELLOW);

    // Row 8: traffic log (only once it has been used).
    if (net_log_ever_started()) {
        if (net_log_is_running()) {
            snprintf(buf, sizeof(buf), "[LOG] %lu samples", (unsigned long)net_log_sample_count());
            display_draw_str(0, 140, buf, COL_ORANGE, COL_BLACK, 1);
        } else {
            snprintf(buf, sizeof(buf), "[LOG] stopped (%lu)", (unsigned long)net_log_sample_count());
            display_draw_str(0, 140, buf, COL_GRAY, COL_BLACK, 1);
        }
    }

    display_flush();
    display_unlock();
}

static void ui_task(void *arg)
{
    TickType_t last = xTaskGetTickCount();
    while (!s_stop) {
        draw_status();
        vTaskDelayUntil(&last, pdMS_TO_TICKS(UI_PERIOD_MS));
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t display_ui_init(void)
{
    if (s_task) return ESP_OK;
    s_stop = false;
    display_lock();
    display_fill_screen(COL_BLACK);
    display_unlock();
    if (xTaskCreate(ui_task, "display_ui", 4096, NULL, 2, &s_task) != pdPASS) {
        s_task = NULL;
        ESP_LOGE(TAG, "failed to create status screen task");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "status screen started (refresh %d ms)", UI_PERIOD_MS);
    return ESP_OK;
}

void display_ui_stop(void)
{
    if (!s_task) return;
    s_stop = true;  // The task exits after its current frame
    ESP_LOGI(TAG, "status screen stopping");
}
