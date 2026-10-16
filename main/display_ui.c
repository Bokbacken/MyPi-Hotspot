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
#define UI_TICK_MS      200     // OTA view refresh (5 Hz); the status screen redraws every UI_PERIOD_MS
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

/** "<label> v XXX.XX ^ XXX.XX Mbit/s" with the label white and values in val_color. */
static void draw_rate_row(int y, const char *label, float rx_mbps, float tx_mbps, uint16_t val_color)
{
    char v[16];
    char arrow[2] = {0, 0};
    int x = display_draw_str(0, y, label, COL_WHITE, COL_BLACK, 1);
    arrow[0] = DISPLAY_CH_DOWN;
    x = display_draw_str(x + CHAR_W, y, arrow, COL_WHITE, COL_BLACK, 1);
    snprintf(v, sizeof(v), "%6.2f", (double)rx_mbps);
    x = display_draw_str(x, y, v, val_color, COL_BLACK, 1);
    arrow[0] = DISPLAY_CH_UP;
    x = display_draw_str(x + CHAR_W, y, arrow, COL_WHITE, COL_BLACK, 1);
    snprintf(v, sizeof(v), "%6.2f", (double)tx_mbps);
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
    draw_rate_row(108, eth_up ? "ETH " : "STA ", ns.rx_mbps, ns.tx_mbps, COL_CYAN);
    draw_rate_row(124, "peak", ns.rx_mbps_peak, ns.tx_mbps_peak, COL_YELLOW);

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

// ---------------------------------------------------------------------------
// OTA progress view (full screen while a P4 or C5 update runs)
// ---------------------------------------------------------------------------

#define OTA_BAR_X0  8
#define OTA_BAR_X1  231
#define OTA_BAR_Y0  112
#define OTA_BAR_Y1  152
#define OTA_MSG_Y   64      // Phase text: up to 3 lines of 40 characters
#define OTA_PCT_Y   168
#define OTA_KB_Y    216

static struct {
    ota_target_t target;
    ota_phase_t phase;
    uint32_t err_seq;
    int bar_w;              // Filled bar width in px (-1 = redraw)
    int pct;
    uint32_t kb_done, kb_total;
} s_ota_drawn;

static uint16_t ota_phase_color(ota_phase_t p)
{
    switch (p) {
    case OTA_PHASE_FAILED:   return COL_RED;
    case OTA_PHASE_VERIFIED:
    case OTA_PHASE_DONE:     return COL_GREEN;
    case OTA_PHASE_VERIFYING: return COL_YELLOW;
    default:                 return COL_CYAN;
    }
}

/** Draw text centered horizontally, clearing the full-width band behind it. */
static void draw_centered(int y, const char *s, uint16_t fg, int scale)
{
    int w = (int)strlen(s) * CHAR_W * scale;
    if (w > LCD_H_RES) w = LCD_H_RES;
    display_fill_rect(0, y, LCD_H_RES, y + CHAR_H * scale, COL_BLACK);
    display_draw_str((LCD_H_RES - w) / 2, y, s, fg, COL_BLACK, scale);
}

/** Redraw only what changed since the last frame; each changed region is flushed on its own. */
static void draw_ota(bool full)
{
    diag_ota_t o;
    diag_get_ota(&o);
    char buf[48];

    display_lock();
    if (full || o.target != s_ota_drawn.target) {
        display_fill_screen(COL_BLACK);
        snprintf(buf, sizeof(buf), "Firmware update (%s)", o.target == OTA_TARGET_C5 ? "C5" : "P4");
        draw_centered(16, buf, COL_WHITE, 2);
        draw_centered(LCD_V_RES - 24, "Do not power off", COL_GRAY, 1);
        display_draw_rect_outline(OTA_BAR_X0 - 2, OTA_BAR_Y0 - 2, OTA_BAR_X1 + 2, OTA_BAR_Y1 + 2, COL_WHITE);
        display_flush();
        s_ota_drawn.target = o.target;
        s_ota_drawn.phase = (ota_phase_t)-1;
        s_ota_drawn.bar_w = -1;
        s_ota_drawn.pct = -1;
        s_ota_drawn.kb_done = s_ota_drawn.kb_total = UINT32_MAX;
    }

    uint16_t col = ota_phase_color(o.phase);
    if (o.phase != s_ota_drawn.phase || (o.phase == OTA_PHASE_FAILED && o.seq != s_ota_drawn.err_seq)) {
        display_fill_rect(0, OTA_MSG_Y, LCD_H_RES, OTA_MSG_Y + 3 * (CHAR_H + 2), COL_BLACK);
        switch (o.phase) {
        case OTA_PHASE_VERIFYING: draw_centered(OTA_MSG_Y, "Verifying image", col, 1); break;
        case OTA_PHASE_VERIFIED:  draw_centered(OTA_MSG_Y, "Upload complete - verified", col, 1); break;
        case OTA_PHASE_DONE:      draw_centered(OTA_MSG_Y, "Done - rebooting", col, 1); break;
        case OTA_PHASE_FAILED: {
            // "FAILED: <msg>" wrapped over up to 3 lines of 40 characters.
            char msg[3 * 40 + 1];
            snprintf(msg, sizeof(msg), "FAILED: %s", o.err);
            size_t len = strlen(msg);
            for (int line = 0; line < 3 && (size_t)line * 40 < len; line++) {
                char part[41];
                strlcpy(part, msg + line * 40, sizeof(part));
                display_draw_str(0, OTA_MSG_Y + line * (CHAR_H + 2), part, col, COL_BLACK, 1);
            }
            break;
        }
        default:                  draw_centered(OTA_MSG_Y, "Receiving + writing flash", col, 1); break;
        }
        display_flush();
        s_ota_drawn.bar_w = -1;     // Bar color follows the phase
        s_ota_drawn.pct = -1;
        s_ota_drawn.phase = o.phase;
        s_ota_drawn.err_seq = o.seq;
    }

    // Progress bar: grow incrementally, full redraw on phase change.
    const int bar_max = OTA_BAR_X1 - OTA_BAR_X0 + 1;
    int pct = o.pct < 0 ? 0 : (o.pct > 100 ? 100 : o.pct);
    int w = bar_max * pct / 100;
    if (w != s_ota_drawn.bar_w) {
        if (s_ota_drawn.bar_w < 0 || w < s_ota_drawn.bar_w) {
            display_fill_rect(OTA_BAR_X0, OTA_BAR_Y0, OTA_BAR_X1 + 1, OTA_BAR_Y1 + 1, COL_BLACK);
            s_ota_drawn.bar_w = 0;
        }
        if (w > s_ota_drawn.bar_w) {
            display_fill_rect(OTA_BAR_X0 + s_ota_drawn.bar_w, OTA_BAR_Y0, OTA_BAR_X0 + w, OTA_BAR_Y1 + 1, col);
        }
        display_flush();
        s_ota_drawn.bar_w = w;
    }

    if (pct != s_ota_drawn.pct) {
        snprintf(buf, sizeof(buf), "%d%%", pct);
        draw_centered(OTA_PCT_Y, buf, col, 4);
        display_flush();
        s_ota_drawn.pct = pct;
    }

    uint32_t kb_done = o.bytes_done / 1024, kb_total = o.bytes_total / 1024;
    if (kb_done != s_ota_drawn.kb_done || kb_total != s_ota_drawn.kb_total) {
        snprintf(buf, sizeof(buf), "%lu / %lu KB", (unsigned long)kb_done, (unsigned long)kb_total);
        draw_centered(OTA_KB_Y, buf, COL_WHITE, 2);
        display_flush();
        s_ota_drawn.kb_done = kb_done;
        s_ota_drawn.kb_total = kb_total;
    }
    display_unlock();
}

static void ui_task(void *arg)
{
    TickType_t last = xTaskGetTickCount();
    bool ota_shown = false;
    int ticks = 0;
    while (!s_stop) {
        if (diag_ota_ui_visible()) {
            draw_ota(!ota_shown);
            ota_shown = true;
        } else {
            if (ota_shown) {
                // Leaving the OTA view: wipe it, then redraw the status screen right away.
                display_lock();
                display_fill_screen(COL_BLACK);
                display_flush();
                display_unlock();
                ota_shown = false;
                ticks = 0;
            }
            if (ticks == 0) draw_status();
            if (++ticks >= UI_PERIOD_MS / UI_TICK_MS) ticks = 0;
        }
        vTaskDelayUntil(&last, pdMS_TO_TICKS(UI_TICK_MS));
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
