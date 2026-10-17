#include "display_ui.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diag.h"
#include "display.h"
#include "display_font.h"
#include "header_font.h"
#include "char_wheel.h"
#include "esp_log.h"
#include "eth.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.h"
#include "profiles.h"
#include "rescue.h"
#include "wifi.h"

static const char *TAG = "display_ui";

#define UI_PERIOD_MS    2000
#define UI_TICK_MS      100     // Wheel interaction and OTA view refresh (10 Hz)
#define UI_AREA_BOTTOM  LCD_V_RES
#define CHAR_W          6
#define CHAR_H          8
#define HEADER_H        18
#define DIVIDER_H       6

static TaskHandle_t s_task;
static volatile bool s_stop;
static volatile bool s_wheel_active;
static bool s_wheel_busy;
static bool s_password_visible;
static bool s_input_password;
static char_wheel_t s_wheel;
static char_wheel_layout_t s_wheel_layout;
static char s_input_ssid[33];
static char s_input_pass[64];
static char s_wheel_message[32];
static char s_status_notice[32];

static void draw_centered(int y, const char *s, uint16_t fg, int scale);

static const char *const s_wheel_ranges[] = {
    "abcdefghijklmnopqrstuvwxyz",
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ",
    "0123456789",
    " !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~",
    "\\€£¥§±×÷°©®™",
    "åäöæøÅÄÖÆØß",
    "éèêëáàâãíìîïóòôõúùûüçñÉÈÊËÁÀÂÃÍÌÎÏÓÒÔÕÚÙÛÜÇÑ",
};
static const char *const s_wheel_tabs[] = {"a-z", "A-Z", "0-9", "Sp1", "Sp2", "Int1", "Int2"};

typedef struct {
    int slot;
    sta_profile_t profile;
} wheel_save_t;

static size_t utf8_char_size(const unsigned char *p)
{
    if (p[0] < 0x80) return 1;
    if ((p[0] & 0xE0) == 0xC0 && p[1] && (p[1] & 0xC0) == 0x80) return 2;
    if ((p[0] & 0xF0) == 0xE0 && p[1] && p[2] && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) return 3;
    if ((p[0] & 0xF8) == 0xF0 && p[1] && p[2] && p[3] &&
        (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) return 4;
    return 1;
}

static int utf8_count(const char *s)
{
    int count = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p += utf8_char_size(p)) count++;
    return count;
}

static int wheel_count(int range)
{
    if (range < 0 || range >= (int)(sizeof(s_wheel_ranges) / sizeof(s_wheel_ranges[0]))) return 0;
    return utf8_count(s_wheel_ranges[range]);
}

static void wheel_counts(int counts[CHAR_WHEEL_MAX_RANGES])
{
    for (int i = 0; i < (int)(sizeof(s_wheel_ranges) / sizeof(s_wheel_ranges[0])); i++) {
        counts[i] = wheel_count(i);
    }
}

static size_t wheel_char(int range, int index, char out[5])
{
    const unsigned char *p = (const unsigned char *)s_wheel_ranges[range];
    while (index-- > 0) p += utf8_char_size(p);
    size_t n = utf8_char_size(p);
    memcpy(out, p, n);
    out[n] = 0;
    return n;
}

static uint32_t utf8_codepoint(const unsigned char *p)
{
    if (p[0] < 0x80) return p[0];
    if ((p[0] & 0xE0) == 0xC0) return ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
    if ((p[0] & 0xF0) == 0xE0) return ((uint32_t)(p[0] & 0x0F) << 12) |
                                       ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    return ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
           ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
}

static void make_preview(const char *src, char *dst, size_t size, bool mask)
{
    size_t used = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p && used + 1 < size;) {
        size_t n = utf8_char_size(p);
        if (mask) {
            dst[used++] = '*';
        } else {
            if (used + n >= size) break;
            memcpy(dst + used, p, n);
            used += n;
        }
        p += n;
    }
    dst[used] = 0;
}

static void draw_serial_divider(void)
{
    static const char text[] = "Hotspot";
    const int cells = (sizeof(text) - 1) * 10;
    int previous_y = HEADER_H;  // Serial idle is high.
    for (int cell = 0; cell < cells; cell++) {
        int bit = cell % 10;
        bool high = bit == 9 || (bit > 0 && ((text[cell / 10] >> (bit - 1)) & 1));
        int x0 = cell * LCD_H_RES / cells;
        int x1 = (cell + 1) * LCD_H_RES / cells;
        int y = HEADER_H + (high ? 0 : DIVIDER_H - 1);
        display_fill_rect(x0, y, x1, y + 1, COL_ORANGE);
        if (y != previous_y) {
            display_fill_rect(x0, HEADER_H, x0 + 1, HEADER_H + DIVIDER_H, COL_ORANGE);
        }
        previous_y = y;
    }
}

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

static void display_draw_utf8_char(int x, int y, const unsigned char *s,
                                   uint16_t fg, uint16_t bg, int scale)
{
    size_t n = utf8_char_size(s);
    uint32_t cp = n == 1 ? s[0] : utf8_codepoint(s);
    char base = '?';
    enum {
        MARK_NONE, MARK_DIAERESIS, MARK_RING, MARK_ACUTE, MARK_GRAVE,
        MARK_CIRCUMFLEX, MARK_TILDE, MARK_CEDILLA
    } mark = MARK_NONE;
    switch (cp) {
    case 0x00E6: base = 'a'; break;
    case 0x00C6: base = 'A'; break;
    case 0x00F8: base = 'o'; break;
    case 0x00D8: base = 'O'; break;
    case 0x20AC: base = 'E'; break;
    case 0x00A3: base = 'L'; break;
    case 0x00A5: base = 'Y'; break;
    case 0x00A7: base = 'S'; break;
    case 0x00B1: base = '+'; break;
    case 0x00D7: base = 'x'; break;
    case 0x00F7: base = '/'; break;
    case 0x00B0: base = 'o'; break;
    case 0x00A9: base = 'C'; break;
    case 0x00AE: base = 'R'; break;
    case 0x2122: base = 'T'; break;
    case 0x00E5: base = 'a'; mark = MARK_RING; break;
    case 0x00C5: base = 'A'; mark = MARK_RING; break;
    case 0x00E4: base = 'a'; mark = MARK_DIAERESIS; break;
    case 0x00C4: base = 'A'; mark = MARK_DIAERESIS; break;
    case 0x00F6: base = 'o'; mark = MARK_DIAERESIS; break;
    case 0x00D6: base = 'O'; mark = MARK_DIAERESIS; break;
    case 0x00E9: base = 'e'; mark = MARK_ACUTE; break;
    case 0x00C9: base = 'E'; mark = MARK_ACUTE; break;
    case 0x00E8: base = 'e'; mark = MARK_GRAVE; break;
    case 0x00C8: base = 'E'; mark = MARK_GRAVE; break;
    case 0x00EA: base = 'e'; mark = MARK_CIRCUMFLEX; break;
    case 0x00CA: base = 'E'; mark = MARK_CIRCUMFLEX; break;
    case 0x00EB: base = 'e'; mark = MARK_DIAERESIS; break;
    case 0x00CB: base = 'E'; mark = MARK_DIAERESIS; break;
    case 0x00E1: base = 'a'; mark = MARK_ACUTE; break;
    case 0x00C1: base = 'A'; mark = MARK_ACUTE; break;
    case 0x00E0: base = 'a'; mark = MARK_GRAVE; break;
    case 0x00C0: base = 'A'; mark = MARK_GRAVE; break;
    case 0x00E2: base = 'a'; mark = MARK_CIRCUMFLEX; break;
    case 0x00C2: base = 'A'; mark = MARK_CIRCUMFLEX; break;
    case 0x00E3: base = 'a'; mark = MARK_TILDE; break;
    case 0x00C3: base = 'A'; mark = MARK_TILDE; break;
    case 0x00ED: base = 'i'; mark = MARK_ACUTE; break;
    case 0x00CD: base = 'I'; mark = MARK_ACUTE; break;
    case 0x00EC: base = 'i'; mark = MARK_GRAVE; break;
    case 0x00CC: base = 'I'; mark = MARK_GRAVE; break;
    case 0x00EE: base = 'i'; mark = MARK_CIRCUMFLEX; break;
    case 0x00CE: base = 'I'; mark = MARK_CIRCUMFLEX; break;
    case 0x00EF: base = 'i'; mark = MARK_DIAERESIS; break;
    case 0x00CF: base = 'I'; mark = MARK_DIAERESIS; break;
    case 0x00F3: base = 'o'; mark = MARK_ACUTE; break;
    case 0x00D3: base = 'O'; mark = MARK_ACUTE; break;
    case 0x00F2: base = 'o'; mark = MARK_GRAVE; break;
    case 0x00D2: base = 'O'; mark = MARK_GRAVE; break;
    case 0x00F4: base = 'o'; mark = MARK_CIRCUMFLEX; break;
    case 0x00D4: base = 'O'; mark = MARK_CIRCUMFLEX; break;
    case 0x00F5: base = 'o'; mark = MARK_TILDE; break;
    case 0x00D5: base = 'O'; mark = MARK_TILDE; break;
    case 0x00FA: base = 'u'; mark = MARK_ACUTE; break;
    case 0x00DA: base = 'U'; mark = MARK_ACUTE; break;
    case 0x00F9: base = 'u'; mark = MARK_GRAVE; break;
    case 0x00D9: base = 'U'; mark = MARK_GRAVE; break;
    case 0x00FB: base = 'u'; mark = MARK_CIRCUMFLEX; break;
    case 0x00DB: base = 'U'; mark = MARK_CIRCUMFLEX; break;
    case 0x00E7: base = 'c'; mark = MARK_CEDILLA; break;
    case 0x00C7: base = 'C'; mark = MARK_CEDILLA; break;
    case 0x00FC: base = 'u'; mark = MARK_DIAERESIS; break;
    case 0x00DC: base = 'U'; mark = MARK_DIAERESIS; break;
    case 0x00F1: base = 'n'; mark = MARK_TILDE; break;
    case 0x00D1: base = 'N'; mark = MARK_TILDE; break;
    case 0x00DF: base = 's'; break;
    default:
        if (n == 1) base = (char)cp;
        break;
    }
    if (scale < 1) scale = 1;
    if (n == 1 || cp == 0x00DF) {
        display_draw_char(x, y, base, fg, bg, scale);
        return;
    }

    display_fill_rect(x, y, x + CHAR_W * scale, y + CHAR_H * scale, bg);
    display_draw_char(x, y + scale, base, fg, bg, scale);
    if (mark == MARK_DIAERESIS) {
        display_fill_rect(x + scale, y, x + 2 * scale, y + scale, fg);
        display_fill_rect(x + 3 * scale, y, x + 4 * scale, y + scale, fg);
    } else if (mark == MARK_RING) {
        display_fill_rect(x + scale, y + scale, x + 2 * scale, y + 2 * scale, fg);
        display_fill_rect(x + 3 * scale, y + scale, x + 4 * scale, y + 2 * scale, fg);
        display_fill_rect(x + 2 * scale, y, x + 3 * scale, y + scale, fg);
    } else if (mark == MARK_ACUTE) {
        display_fill_rect(x + 3 * scale, y, x + 4 * scale, y + scale, fg);
        display_fill_rect(x + 2 * scale, y + scale, x + 3 * scale, y + 2 * scale, fg);
    } else if (mark == MARK_GRAVE) {
        display_fill_rect(x + 2 * scale, y, x + 3 * scale, y + scale, fg);
        display_fill_rect(x + 3 * scale, y + scale, x + 4 * scale, y + 2 * scale, fg);
    } else if (mark == MARK_CIRCUMFLEX) {
        display_fill_rect(x + scale, y + scale, x + 2 * scale, y + 2 * scale, fg);
        display_fill_rect(x + 2 * scale, y, x + 3 * scale, y + scale, fg);
        display_fill_rect(x + 3 * scale, y + scale, x + 4 * scale, y + 2 * scale, fg);
    } else if (mark == MARK_TILDE) {
        display_fill_rect(x + scale, y + scale, x + 2 * scale, y + 2 * scale, fg);
        display_fill_rect(x + 2 * scale, y, x + 3 * scale, y + scale, fg);
        display_fill_rect(x + 3 * scale, y + scale, x + 4 * scale, y + 2 * scale, fg);
    } else if (mark == MARK_CEDILLA) {
        display_fill_rect(x + 2 * scale, y + 6 * scale, x + 3 * scale, y + 7 * scale, fg);
        display_fill_rect(x + 3 * scale, y + 7 * scale, x + 4 * scale, y + 8 * scale, fg);
    }
}

int display_draw_str(int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale)
{
    if (scale < 1) scale = 1;
    for (const unsigned char *p = (const unsigned char *)s; p && *p;) {
        if (x >= LCD_H_RES) break;
        size_t n = utf8_char_size(p);
        display_draw_utf8_char(x, y, p, fg, bg, scale);
        x += CHAR_W * scale;
        p += n;
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

static uint16_t wifi_signal_color(const diag_status_t *status)
{
    if ((status->wifi_state != WIFI_STATE_CONNECTED && status->wifi_state != WIFI_STATE_GOT_IP) ||
        status->rssi >= 0) return COL_GRAY;
    if (status->rssi < -80) return RGB565(128, 0, 0);
    if (status->rssi < -70) return COL_RED;
    if (status->rssi < -60) return COL_ORANGE;
    if (status->rssi < -40) return COL_YELLOW;
    return COL_GREEN;
}

static void draw_brand(const diag_status_t *status)
{
    display_fill_rect(0, 0, LCD_H_RES, HEADER_H, COL_BLACK);
    for (int y = 0; y < HEADER_TITLE_H; y++) {
        for (int x = 0; x < HEADER_TITLE_W; x++) {
            uint8_t value = s_header_title[y * HEADER_TITLE_W + x];
            display_draw_pixel(4 + x, (HEADER_H - HEADER_TITLE_H) / 2 + y,
                               RGB565(value, value, value));
        }
    }
    display_draw_wifi_icon(LCD_H_RES - 14, HEADER_H - 4,
                           wifi_signal_color(status));
    draw_serial_divider();
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

    draw_brand(&d);

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

    // Row 9: boot-time hint for emergency mode.
    int left = rescue_swipe_seconds_left();
    if (left > 0) {
        snprintf(buf, sizeof(buf), "Swipe down: emergency mode (%ds)", left);
        display_draw_str(0, 150, buf, COL_ORANGE, COL_BLACK, 1);
    }

    display_draw_rect_outline(8, 208, LCD_H_RES - 9, 260, COL_ORANGE);
    display_draw_str(54, 226, "Wi-Fi setup", COL_WHITE, COL_BLACK, 2);
    display_draw_str(20, 272, "Tap here to enter credentials", COL_GRAY, COL_BLACK, 1);
    if (s_status_notice[0]) display_draw_str(8, 296, s_status_notice, COL_CYAN, COL_BLACK, 1);

    display_flush();
    display_unlock();
}

static void draw_wheel(void)
{
    char preview[64], ch[5];
    static diag_status_t status;
    diag_get(&status);
    const int range_count = (int)(sizeof(s_wheel_ranges) / sizeof(s_wheel_ranges[0]));
    display_lock();
    const char *value = s_input_password ? s_input_pass : s_input_ssid;
    make_preview(value, preview, sizeof(preview), s_input_password && !s_password_visible);
    int count = wheel_count(s_wheel.range);
    display_fill_screen(COL_BLACK);
    draw_brand(&status);
    display_draw_str(0, HEADER_H + DIVIDER_H, "Wi-Fi setup", COL_CYAN, COL_BLACK, 1);

    for (int i = 0; i < 2; i++) {
        char_wheel_rect_t r = s_wheel_layout.fields[i];
        uint16_t fill = (i == (s_input_password ? 1 : 0)) ? COL_ORANGE : COL_BLACK;
        display_fill_rect(r.x, r.y, r.x + r.w, r.y + r.h, fill);
        display_draw_rect_outline(r.x, r.y, r.x + r.w - 1, r.y + r.h - 1, COL_WHITE);
        const char *title = i == 0 ? "SSID" : "Password";
        display_draw_str(r.x + r.w / 2 - (int)strlen(title) * CHAR_W / 2,
                         r.y + (r.h - CHAR_H) / 2, title,
                         fill == COL_ORANGE ? COL_BLACK : COL_WHITE, fill, 1);
    }

    char_wheel_rect_t preview_box = s_wheel_layout.preview;
    display_fill_rect(preview_box.x, preview_box.y, preview_box.x + preview_box.w,
                      preview_box.y + preview_box.h, COL_BLACK);
    display_draw_str(preview_box.x + 3, preview_box.y + (preview_box.h - CHAR_H) / 2,
                     preview[0] ? preview : "(empty)", COL_WHITE, COL_BLACK, 1);
    for (int i = 0; i < range_count; i++) {
        char_wheel_rect_t tab = s_wheel_layout.tabs[i];
        uint16_t fill = i == s_wheel.range ? COL_ORANGE : COL_GRAY;
        display_fill_rect(tab.x + 1, tab.y, tab.x + tab.w - 1, tab.y + tab.h, fill);
        display_draw_rect_outline(tab.x + 1, tab.y, tab.x + tab.w - 2, tab.y + tab.h - 1, fill);
        int text_x = tab.x + (tab.w - (int)strlen(s_wheel_tabs[i]) * CHAR_W) / 2;
        display_draw_str(text_x, tab.y + (tab.h - CHAR_H) / 2, s_wheel_tabs[i],
                         i == s_wheel.range ? COL_BLACK : COL_WHITE, fill, 1);
    }

    int center_y = s_wheel_layout.wheel_y + s_wheel_layout.wheel_h / 2;
    int center_x = s_wheel_layout.wheel_x + s_wheel_layout.wheel_w / 2;
    display_draw_rect_outline(s_wheel_layout.wheel_x, s_wheel_layout.wheel_y,
                              s_wheel_layout.wheel_x + s_wheel_layout.wheel_w - 1,
                              s_wheel_layout.wheel_y + s_wheel_layout.wheel_h - 1, COL_GRAY);
    for (int slot = 0; slot < CHAR_WHEEL_VISIBLE_ITEMS; slot++) {
        int y = center_y + (slot - CHAR_WHEEL_VISIBLE_ITEMS / 2) * s_wheel_layout.item_h
                - s_wheel_layout.item_h / 2 - (int)s_wheel.visual_shift_px;
        if (y + s_wheel_layout.item_h <= s_wheel_layout.wheel_y ||
            y >= s_wheel_layout.wheel_y + s_wheel_layout.wheel_h) continue;
        int index = (s_wheel.selected + slot - CHAR_WHEEL_VISIBLE_ITEMS / 2 + count) % count;
        wheel_char(s_wheel.range, index, ch);
        bool selected = slot == CHAR_WHEEL_VISIBLE_ITEMS / 2;
        uint16_t fill = selected ? COL_CYAN : COL_BLACK;
        display_fill_rect(s_wheel_layout.wheel_x + 1, y,
                          s_wheel_layout.wheel_x + s_wheel_layout.wheel_w - 1,
                          y + s_wheel_layout.item_h, fill);
        if (selected) {
            display_draw_rect_outline(s_wheel_layout.wheel_x + 1, y,
                                      s_wheel_layout.wheel_x + s_wheel_layout.wheel_w - 2,
                                      y + s_wheel_layout.item_h - 1, COL_CYAN);
        }
        int scale = selected ? s_wheel_layout.item_h / CHAR_H : 1;
        if (scale < 1) scale = 1;
        if (scale > 3) scale = 3;
        if (ch[0] == ' ') {
            display_draw_str(center_x - 5 * CHAR_W * scale / 2,
                             y + (s_wheel_layout.item_h - CHAR_H * scale) / 2,
                             "SPACE", selected ? COL_BLACK : COL_WHITE, fill, scale);
        } else {
            display_draw_utf8_char(center_x - CHAR_W * scale / 2,
                                   y + (s_wheel_layout.item_h - CHAR_H * scale) / 2,
                                   (const unsigned char *)ch, selected ? COL_BLACK : COL_WHITE, fill, scale);
        }
    }
    display_draw_str(8, LCD_V_RES * 60 / 100, "Swipe up/down: scroll", COL_GRAY, COL_BLACK, 1);
    display_draw_str(8, LCD_V_RES * 63 / 100, "Swipe left/right: range", COL_GRAY, COL_BLACK, 1);
    if (s_input_password) {
        char_wheel_rect_t r = s_wheel_layout.visibility;
        display_draw_rect_outline(r.x, r.y, r.x + r.w - 1, r.y + r.h - 1, COL_GRAY);
        const char *visibility = s_password_visible ? "Hide password" : "Show password";
        display_draw_str(r.x + (r.w - (int)strlen(visibility) * CHAR_W) / 2,
                         r.y + (r.h - CHAR_H) / 2, visibility,
                         s_password_visible ? COL_YELLOW : COL_WHITE, COL_BLACK, 1);
    } else {
        display_draw_str(8, s_wheel_layout.visibility.y + 4, "UTF-8 input", COL_GRAY, COL_BLACK, 1);
    }
    const char *buttons[] = {"BACK", "DEL", "NEXT", "OK"};
    for (int i = 0; i < 4; i++) {
        char_wheel_rect_t r = s_wheel_layout.actions[i];
        uint16_t fill = i == 3 ? COL_GREEN : COL_GRAY;
        display_fill_rect(r.x + 2, r.y, r.x + r.w - 2, r.y + r.h, fill);
        display_draw_rect_outline(r.x + 2, r.y, r.x + r.w - 3, r.y + r.h - 1, fill);
        display_draw_str(r.x + (r.w - (int)strlen(buttons[i]) * CHAR_W) / 2,
                         r.y + (r.h - CHAR_H) / 2, buttons[i], COL_BLACK, fill, 1);
    }
    char_wheel_rect_t message = s_wheel_layout.message;
    if (s_wheel_busy) display_draw_str(message.x, message.y, "Saving profile and connecting...", COL_CYAN, COL_BLACK, 1);
    else if (s_wheel_message[0]) display_draw_str(message.x, message.y, s_wheel_message, COL_YELLOW, COL_BLACK, 1);
    display_flush();
    display_unlock();
}

static void wheel_save_task(void *arg)
{
    wheel_save_t *save = arg;
    esp_err_t save_err = sta_profile_save(save->slot, &save->profile);
    esp_err_t connect_err = save_err == ESP_OK ? wifi_connect_profile(save->slot) : save_err;

    display_lock();
    s_wheel_busy = false;
    if (save_err == ESP_OK) {
        memset(s_input_ssid, 0, sizeof(s_input_ssid));
        memset(s_input_pass, 0, sizeof(s_input_pass));
        s_wheel_active = false;
        strlcpy(s_status_notice, connect_err == ESP_OK ? "Profile saved; connecting" :
                "Profile saved; connection failed", sizeof(s_status_notice));
    } else {
        strlcpy(s_wheel_message, "Could not save profile", sizeof(s_wheel_message));
    }
    display_unlock();
    memset(save, 0, sizeof(*save));
    free(save);
    vTaskDelete(NULL);
}

static void wheel_submit(void)
{
    size_t ssid_len = strlen(s_input_ssid);
    size_t pass_len = strlen(s_input_pass);
    if (!ssid_len) {
        strlcpy(s_wheel_message, "SSID cannot be empty", sizeof(s_wheel_message));
        return;
    }
    if (pass_len && (pass_len < 8 || pass_len > 63)) {
        strlcpy(s_wheel_message, "Password: 8-63 bytes or empty", sizeof(s_wheel_message));
        return;
    }
    int slot = -1;
    for (int i = 0; i < STA_PROFILE_MAX; i++) {
        sta_profile_t existing;
        if (sta_profile_get(i, &existing) && !existing.valid) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        strlcpy(s_wheel_message, "No free Wi-Fi profile slot", sizeof(s_wheel_message));
        return;
    }

    wheel_save_t *save = calloc(1, sizeof(*save));
    if (!save) {
        strlcpy(s_wheel_message, "Not enough memory", sizeof(s_wheel_message));
        return;
    }
    save->slot = slot;
    save->profile.valid = true;
    save->profile.dhcp = true;
    strlcpy(save->profile.ssid, s_input_ssid, sizeof(save->profile.ssid));
    strlcpy(save->profile.pass, s_input_pass, sizeof(save->profile.pass));
    s_wheel_busy = true;
    s_wheel_message[0] = 0;
    if (xTaskCreate(wheel_save_task, "wifi_profile_save", 3072, save, 4, NULL) != pdPASS) {
        s_wheel_busy = false;
        memset(save, 0, sizeof(*save));
        free(save);
        strlcpy(s_wheel_message, "Could not start Wi-Fi task", sizeof(s_wheel_message));
    }
}

static void wheel_append(void)
{
    char ch[5];
    size_t n = wheel_char(s_wheel.range, s_wheel.selected, ch);
    char *value = s_input_password ? s_input_pass : s_input_ssid;
    size_t cap = s_input_password ? sizeof(s_input_pass) : sizeof(s_input_ssid);
    size_t used = strlen(value);
    if (used + n >= cap) {
        strlcpy(s_wheel_message, s_input_password ? "Password is full" : "SSID is full", sizeof(s_wheel_message));
        return;
    }
    memcpy(value + used, ch, n + 1);
    char_wheel_select_offset(&s_wheel, 1, wheel_count(s_wheel.range));
    s_wheel_message[0] = 0;
}

static void wheel_delete(void)
{
    char *value = s_input_password ? s_input_pass : s_input_ssid;
    size_t n = strlen(value);
    if (!n) return;
    n--;
    while (n && (((unsigned char)value[n] & 0xC0) == 0x80)) n--;
    value[n] = 0;
    s_wheel_message[0] = 0;
}

bool display_ui_touch(int x0, int y0, int x1, int y1, bool dragged)
{
    if (!s_task) return false;
    bool handled = false;
    display_lock();
    if (s_wheel_active) {
        bool swiped = char_wheel_gesture_end(&s_wheel);
        handled = true;
        if (!s_wheel_busy && !swiped && !dragged) {
            char_wheel_zone_t zone = char_wheel_hit_test(&s_wheel_layout, x1, y1);
            switch (zone.type) {
            case CHAR_WHEEL_ZONE_FIELD:
                s_input_password = zone.index == 1;
                s_wheel_message[0] = 0;
                break;
            case CHAR_WHEEL_ZONE_TAB:
                char_wheel_set_range(&s_wheel, zone.index, wheel_count(zone.index));
                s_wheel_message[0] = 0;
                break;
            case CHAR_WHEEL_ZONE_ITEMS:
                char_wheel_select_offset(&s_wheel, zone.index - CHAR_WHEEL_VISIBLE_ITEMS / 2,
                                         wheel_count(s_wheel.range));
                break;
            case CHAR_WHEEL_ZONE_VISIBILITY:
                if (s_input_password) s_password_visible = !s_password_visible;
                break;
            case CHAR_WHEEL_ZONE_ACTION:
                if (zone.index == 0) {
                    memset(s_input_ssid, 0, sizeof(s_input_ssid));
                    memset(s_input_pass, 0, sizeof(s_input_pass));
                    s_wheel_active = false;
                    strlcpy(s_status_notice, "Wi-Fi setup cancelled", sizeof(s_status_notice));
                } else if (zone.index == 1) {
                    wheel_delete();
                } else if (zone.index == 2) {
                    wheel_append();
                } else if (!s_input_password) {
                    if (s_input_ssid[0]) {
                        s_input_password = true;
                        s_password_visible = false;
                        s_wheel_message[0] = 0;
                    } else {
                        strlcpy(s_wheel_message, "SSID cannot be empty", sizeof(s_wheel_message));
                    }
                } else {
                    wheel_submit();
                }
                break;
            default:
                break;
            }
        }
    } else {
        if (!dragged && y0 >= LCD_V_RES / 2) {
            memset(s_input_ssid, 0, sizeof(s_input_ssid));
            memset(s_input_pass, 0, sizeof(s_input_pass));
            s_input_password = false;
            s_password_visible = false;
            char_wheel_init(&s_wheel, (int)(sizeof(s_wheel_ranges) / sizeof(s_wheel_ranges[0])));
            char_wheel_layout_init(&s_wheel_layout, LCD_H_RES, LCD_V_RES, s_wheel.range_count);
            s_wheel_message[0] = 0;
            s_status_notice[0] = 0;
            s_wheel_active = true;
            handled = true;
        }
    }
    display_unlock();
    return handled;
}

bool display_ui_touch_begin(int x, int y)
{
    if (!s_task || !s_wheel_active) return false;
    display_lock();
    char_wheel_gesture_begin(&s_wheel, x, y);
    display_unlock();
    return true;
}

void display_ui_touch_move(int x, int y)
{
    if (!s_task) return;
    int counts[CHAR_WHEEL_MAX_RANGES] = {0};
    wheel_counts(counts);
    display_lock();
    if (s_wheel_active && !s_wheel_busy &&
        char_wheel_gesture_move(&s_wheel, x, y, &s_wheel_layout, counts)) {
        s_wheel_message[0] = 0;
    }
    display_unlock();
}

// ---------------------------------------------------------------------------
// Emergency (rescue) mode: Ethernet-only, 5 touch buttons
// ---------------------------------------------------------------------------

#define RB_X0   8
#define RB_X1   231
#define RB_Y0   86
#define RB_H    38
#define RB_GAP  6

int display_ui_rescue_hit(int x, int y)
{
    if (x < RB_X0 || x > RB_X1 || y < RB_Y0) return -1;
    int i = (y - RB_Y0) / (RB_H + RB_GAP);
    int off = (y - RB_Y0) % (RB_H + RB_GAP);
    return i < RESCUE_OPT_COUNT && off < RB_H ? i : -1;
}

/** Cheap signature of everything shown, so the screen is only redrawn on change. */
static uint32_t rescue_signature(const rescue_state_t *r, const eth_info_t *e)
{
    uint32_t h = (uint32_t)r->option * 31u + (r->busy ? 7u : 0u) + (uint32_t)r->err * 131u;
    h = h * 33u + (e->link_up ? 1u : 0u) + (e->has_ip ? 2u : 0u) + (uint32_t)e->speed_mbps;
    for (const char *p = e->ip; *p; p++) h = h * 33u + (uint8_t)*p;
    return h;
}

static void draw_rescue(bool force)
{
    static uint32_t s_sig;
    rescue_state_t r;
    eth_info_t e;
    rescue_get(&r);
    eth_get_info(&e);
    uint32_t sig = rescue_signature(&r, &e);
    if (!force && sig == s_sig) return;
    s_sig = sig;
    char buf[48];

    display_lock();
    display_fill_screen(COL_BLACK);
    draw_centered(2, "EMERGENCY MODE", COL_RED, 2);
    snprintf(buf, sizeof(buf), "Reason: %s", rescue_reason_text(r.reason));
    draw_centered(22, buf, COL_GRAY, 1);

    if (!e.initialized) {
        snprintf(buf, sizeof(buf), "Ethernet error: %.24s", e.error[0] ? e.error : "not initialized");
        draw_centered(34, buf, COL_RED, 1);
    } else if (!e.link_up) {
        draw_centered(34, "Ethernet: no link (check cable)", COL_RED, 1);
    } else {
        snprintf(buf, sizeof(buf), "Ethernet: link up %d Mbit/s%s", e.speed_mbps, r.busy ? "  applying..." : "");
        draw_centered(34, buf, COL_GREEN, 1);
    }

    // Current address: large, plus the web UI hint.
    bool ip_ok = e.rescue && e.has_ip && e.ip[0] && strcmp(e.ip, "0.0.0.0") != 0;
    if (r.err != ESP_OK) {
        snprintf(buf, sizeof(buf), "Failed: %.30s", esp_err_to_name(r.err));
        draw_centered(50, buf, COL_RED, 1);
    } else if (ip_ok) {
        draw_centered(48, e.ip, COL_CYAN, 2);
        snprintf(buf, sizeof(buf), "Web UI: http://%s/", e.ip);
        draw_centered(70, buf, COL_WHITE, 1);
    } else {
        draw_centered(48, r.option == RESCUE_OPT_DHCP ? "DHCP: waiting..." : "No IP yet", COL_YELLOW, 2);
        draw_centered(70, "Tap an option below", COL_GRAY, 1);
    }

    for (int i = 0; i < RESCUE_OPT_COUNT; i++) {
        int y0 = RB_Y0 + i * (RB_H + RB_GAP), y1 = y0 + RB_H - 1;
        bool sel = i == r.option;
        char label[20];
        rescue_option_label(i, label, sizeof(label));
        uint16_t fill = sel ? (i == RESCUE_OPT_DHCP ? COL_CYAN : COL_GREEN) : COL_BLACK;
        display_fill_rect(RB_X0, y0, RB_X1 + 1, y1 + 1, fill);
        display_draw_rect_outline(RB_X0, y0, RB_X1, y1, sel ? fill : COL_WHITE);
        int w = (int)strlen(label) * CHAR_W * 2;
        display_draw_str((LCD_H_RES - w) / 2, y0 + (RB_H - CHAR_H * 2) / 2, label, sel ? COL_BLACK : COL_WHITE, fill, 2);
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
    bool rescue_shown = false;
    int ticks = 0;
    while (!s_stop) {
        if (diag_ota_ui_visible()) {
            draw_ota(!ota_shown);
            ota_shown = true;
            rescue_shown = false;
        } else if (rescue_active()) {
            draw_rescue(!rescue_shown || ota_shown);    // redraws only on change
            rescue_shown = true;
            ota_shown = false;
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
            if (s_wheel_active) {
                draw_wheel();
                ticks = 0;
            } else {
                if (ticks == 0) draw_status();
                if (++ticks >= UI_PERIOD_MS / UI_TICK_MS) ticks = 0;
            }
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
