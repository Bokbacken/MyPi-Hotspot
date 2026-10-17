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
#include "profiles.h"
#include "rescue.h"
#include "ui_input.h"
#include "ui_layout.h"
#include "ui_serial.h"
#include "wifi.h"

static const char *TAG = "display_ui";

#define UI_PERIOD_MS    2000
#define UI_TICK_MS      200     // OTA view refresh (5 Hz); the status screen redraws every UI_PERIOD_MS
#define CHAR_W          6
#define CHAR_H          8
#define UART_BIT_W      10
#define UART_HIGH_Y     3
#define UART_LOW_Y      11
#define UART_X          ((LCD_H_RES - MYPI_UART_BITS_PER_BYTE * UART_BIT_W) / 2)
#define SCAN_MAX        8
#define CLIENT_MAX      8
#define WHEEL_ASCII_COUNT 95
#define WHEEL_LATIN_COUNT (0x250 - 0xA0)
#define WHEEL_COUNT     (WHEEL_ASCII_COUNT + WHEEL_LATIN_COUNT)
#define MENU_BUTTON_Y0  280
#define MENU_BUTTON_Y1  315

typedef enum {
    UI_HOME,
    UI_MENU,
    UI_WIFI_SCAN,
    UI_PASSWORD,
    UI_AP,
    UI_AP_STOP_CONFIRM,
    UI_CLIENTS,
    UI_TRAFFIC,
    UI_ETHERNET,
    UI_SYSTEM,
} ui_view_t;

typedef enum {
    UI_JOB_NONE,
    UI_JOB_SCAN,
    UI_JOB_CONNECT,
    UI_JOB_AP_START,
    UI_JOB_AP_STOP,
    UI_JOB_CLIENTS,
} ui_job_type_t;

typedef struct {
    ui_view_t view;
    bool busy;
    char message[64];
    wifi_scan_entry_t networks[SCAN_MAX];
    int network_count;
    char password_ssid[33];
    char password[64];
    int wheel_index;
    wifi_client_t clients[CLIENT_MAX];
    int client_count;
} ui_state_t;

typedef struct {
    ui_job_type_t type;
    int slot;
    char ssid[33];
    char password[64];
} ui_job_t;

static TaskHandle_t s_task;
static TaskHandle_t s_worker_task;
static volatile bool s_stop;
static portMUX_TYPE s_ui_mux = portMUX_INITIALIZER_UNLOCKED;
static ui_state_t s_ui;
static ui_job_t s_job;
static bool s_job_pending;
static bool s_redraw_requested = true;

static void draw_centered(int y, const char *s, uint16_t fg, int scale);

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

static void ui_set_message(const char *message, bool busy)
{
    portENTER_CRITICAL(&s_ui_mux);
    s_ui.busy = busy;
    snprintf(s_ui.message, sizeof(s_ui.message), "%s", message ? message : "");
    s_redraw_requested = true;
    portEXIT_CRITICAL(&s_ui_mux);
}

static bool ui_queue_job(ui_job_type_t type, int slot, const char *ssid, const char *password)
{
    bool queued = false;
    portENTER_CRITICAL(&s_ui_mux);
    if (!s_job_pending && !s_ui.busy) {
        memset(&s_job, 0, sizeof(s_job));
        s_job.type = type;
        s_job.slot = slot;
        if (ssid) strlcpy(s_job.ssid, ssid, sizeof(s_job.ssid));
        if (password) strlcpy(s_job.password, password, sizeof(s_job.password));
        s_job_pending = true;
        s_ui.busy = true;
        s_ui.message[0] = '\0';
        s_redraw_requested = true;
        queued = true;
    } else {
        snprintf(s_ui.message, sizeof(s_ui.message), "Another action is still running");
        s_redraw_requested = true;
    }
    portEXIT_CRITICAL(&s_ui_mux);
    return queued;
}

static void ui_worker(void *arg)
{
    (void)arg;
    for (;;) {
        ui_job_t job = {0};
        portENTER_CRITICAL(&s_ui_mux);
        if (s_job_pending) {
            job = s_job;
            memset(&s_job, 0, sizeof(s_job));
            s_job_pending = false;
        }
        portEXIT_CRITICAL(&s_ui_mux);
        if (job.type == UI_JOB_NONE) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        char message[64] = "Done";
        esp_err_t err = ESP_OK;
        switch (job.type) {
        case UI_JOB_SCAN: {
            wifi_scan_entry_t entries[SCAN_MAX] = {0};
            int count = wifi_scan(entries, SCAN_MAX);
            portENTER_CRITICAL(&s_ui_mux);
            s_ui.network_count = count < 0 ? 0 : count;
            if (s_ui.network_count > SCAN_MAX) s_ui.network_count = SCAN_MAX;
            if (s_ui.network_count) {
                memcpy(s_ui.networks, entries, s_ui.network_count * sizeof(entries[0]));
            }
            portEXIT_CRITICAL(&s_ui_mux);
            if (count < 0) strlcpy(message, "Scan failed", sizeof(message));
            else if (count == 0) strlcpy(message, "No networks found", sizeof(message));
            else snprintf(message, sizeof(message), "%d networks found", count);
            break;
        }
        case UI_JOB_CONNECT:
            if (job.slot < 0) {
                sta_profile_t profile = {0};
                int slot;
                for (slot = 0; slot < STA_PROFILE_MAX; slot++) {
                    if (!sta_profile_get(slot, &profile) || !profile.valid) break;
                }
                if (slot == STA_PROFILE_MAX) {
                    err = ESP_ERR_NO_MEM;
                } else {
                    memset(&profile, 0, sizeof(profile));
                    profile.valid = true;
                    profile.dhcp = true;
                    strlcpy(profile.ssid, job.ssid, sizeof(profile.ssid));
                    strlcpy(profile.pass, job.password, sizeof(profile.pass));
                    err = sta_profile_save(slot, &profile);
                    if (err == ESP_OK) err = wifi_connect_profile(slot);
                }
            } else {
                err = wifi_connect_profile(job.slot);
            }
            snprintf(message, sizeof(message), err == ESP_OK ? "Saved; connection requested" : "Save/connect failed");
            memset(job.password, 0, sizeof(job.password));
            portENTER_CRITICAL(&s_ui_mux);
            memset(s_ui.password, 0, sizeof(s_ui.password));
            portEXIT_CRITICAL(&s_ui_mux);
            break;
        case UI_JOB_AP_START:
            err = wifi_ap_start(job.slot);
            snprintf(message, sizeof(message), err == ESP_OK ? "AP start requested" : "AP start failed");
            break;
        case UI_JOB_AP_STOP:
            err = wifi_ap_stop();
            snprintf(message, sizeof(message), err == ESP_OK ? "AP stop requested" : "AP stop failed");
            break;
        case UI_JOB_CLIENTS: {
            wifi_client_t clients[CLIENT_MAX] = {0};
            int count = wifi_ap_clients(clients, CLIENT_MAX);
            portENTER_CRITICAL(&s_ui_mux);
            s_ui.client_count = count < 0 ? 0 : count;
            if (s_ui.client_count > CLIENT_MAX) s_ui.client_count = CLIENT_MAX;
            if (s_ui.client_count) memcpy(s_ui.clients, clients, s_ui.client_count * sizeof(clients[0]));
            portEXIT_CRITICAL(&s_ui_mux);
            if (count < 0) strlcpy(message, "Client list unavailable", sizeof(message));
            else snprintf(message, sizeof(message), "%d connected clients", count);
            break;
        }
        default:
            break;
        }
        if (err != ESP_OK && job.type != UI_JOB_SCAN && job.type != UI_JOB_CLIENTS) {
            strlcpy(message, esp_err_to_name(err), sizeof(message));
        }
        ui_set_message(message, false);
        memset(&job, 0, sizeof(job));
    }
}

static void ui_snapshot(ui_state_t *out)
{
    portENTER_CRITICAL(&s_ui_mux);
    *out = s_ui;
    portEXIT_CRITICAL(&s_ui_mux);
}

static void draw_uart_divider(int x, int y, int bit_width, uint16_t color)
{
    uint8_t bits[MYPI_UART_BITS_PER_BYTE];
    mypi_uart_frame('H', bits);
    for (size_t i = 0; i < MYPI_UART_BITS_PER_BYTE; i++) {
        int x0 = mypi_uart_x(x, i, bit_width);
        int x1 = mypi_uart_x(x, i + 1, bit_width);
        int level_y = y + (bits[i] ? 0 : 8);
        display_draw_line(x0, level_y, x1, level_y, 1, color);
        if (i && bits[i] != bits[i - 1]) {
            int previous_y = y + (bits[i - 1] ? 0 : 8);
            display_draw_line(x0, previous_y, x0, level_y, 1, color);
        }
    }
}

static void draw_button(int x0, int y0, int x1, int y1, const char *label, uint16_t color)
{
    display_fill_rect(x0, y0, x1 + 1, y1 + 1, MYPI_UI_BG);
    display_draw_rect_outline(x0, y0, x1, y1, color);
    int width = (int)strlen(label) * CHAR_W;
    display_draw_str(x0 + (x1 - x0 + 1 - width) / 2,
                     y0 + (y1 - y0 + 1 - CHAR_H) / 2, label, color, MYPI_UI_BG, 1);
}

static void draw_page_title(const char *title)
{
    display_draw_str(8, 8, title, MYPI_UI_TEXT, MYPI_UI_BG, 2);
    display_draw_rect_outline(8, 28, LCD_H_RES - 9, 28, MYPI_UI_ORANGE);
}

static uint32_t wheel_codepoint(int index)
{
    return index < WHEEL_ASCII_COUNT ? (uint32_t)(0x20 + index) :
           (uint32_t)(0xA0 + index - WHEEL_ASCII_COUNT);
}

static void draw_home(const ui_state_t *ui)
{
    diag_status_t d;
    wifi_ap_info_t ap;
    wifi_sta_extra_t ex;
    eth_info_t eth;
    net_if_stats_t stats;
    diag_get(&d);
    wifi_get_ap_info(&ap);
    wifi_get_sta_extra(&ex);
    eth_get_info(&eth);

    display_lock();
    display_fill_screen(MYPI_UI_BG);
    draw_uart_divider(UART_X, UART_HIGH_Y, UART_BIT_W, MYPI_UI_ORANGE);
    display_draw_str((LCD_H_RES - 7 * CHAR_W) / 2, 17, "Hotspot", MYPI_UI_TEXT, MYPI_UI_BG, 1);
    if (ap.running) display_draw_wifi_icon(LCD_H_RES - 15, 15, MYPI_UI_GREEN);

    bool sta_ip = d.wifi_state == WIFI_STATE_GOT_IP && d.ip[0] && strcmp(d.ip, "0.0.0.0") != 0;
    const char *uplink_ip = sta_ip ? d.ip :
                            (eth.mode == ETH_MODE_WAN && eth.has_ip ? eth.ip : "No uplink");
    display_draw_str(0, 34, uplink_ip, sta_ip ? MYPI_UI_GREEN :
                     (eth.mode == ETH_MODE_WAN && eth.has_ip ? MYPI_UI_CYAN : MYPI_UI_RED), MYPI_UI_BG, 2);
    if (d.ssid[0] && d.wifi_state >= WIFI_STATE_CONNECTING) {
        char line[40];
        snprintf(line, sizeof(line), "%.28s ch%d", d.ssid, ap.sta_channel);
        display_draw_str(0, 53, line, MYPI_UI_TEXT, MYPI_UI_BG, 1);
    } else {
        display_draw_str(0, 53, ex.sta_manual_off ? "STA: disconnected" : "STA: not connected",
                         MYPI_UI_MUTED, MYPI_UI_BG, 1);
    }

    char line[48];
    if (ap.running) {
        snprintf(line, sizeof(line), "AP: %d clients ch%d", ap.clients, ap.channel);
        display_draw_str(0, 69, line, MYPI_UI_GREEN, MYPI_UI_BG, 1);
        snprintf(line, sizeof(line), "%.34s", ap.ssid);
        display_draw_str(0, 85, line, MYPI_UI_TEXT, MYPI_UI_BG, 1);
    } else {
        display_draw_str(0, 69, "AP: stopped", MYPI_UI_MUTED, MYPI_UI_BG, 1);
    }

    if (!eth.initialized) display_draw_str(0, 101, "ETH: --", MYPI_UI_MUTED, MYPI_UI_BG, 1);
    else if (!eth.link_up) display_draw_str(0, 101, "ETH: down", MYPI_UI_RED, MYPI_UI_BG, 1);
    else display_draw_str(0, 101, eth.mode == ETH_MODE_LAN ? "ETH: LAN link up" :
                          (eth.has_ip ? "ETH: WAN uplink" : "ETH: WAN no IP"),
                          eth.mode == ETH_MODE_LAN ? MYPI_UI_GREEN : MYPI_UI_CYAN, MYPI_UI_BG, 1);

    net_stats_get(strcmp(ex.uplink, "eth") == 0 ? NET_IF_ETH : NET_IF_STA, &stats);
    char rate[32];
    snprintf(rate, sizeof(rate), "RX %5.2f TX %5.2f Mb/s", (double)stats.rx_mbps, (double)stats.tx_mbps);
    display_draw_str(0, 117, rate, MYPI_UI_CYAN, MYPI_UI_BG, 1);
    if (net_log_ever_started()) {
        snprintf(line, sizeof(line), "Traffic log: %lu samples", (unsigned long)net_log_sample_count());
        display_draw_str(0, 133, line, net_log_is_running() ? MYPI_UI_ORANGE : MYPI_UI_MUTED, MYPI_UI_BG, 1);
    }
    int left = rescue_swipe_seconds_left();
    if (left > 0) {
        snprintf(line, sizeof(line), "Swipe down for rescue (%ds)", left);
        display_draw_str(0, 149, line, MYPI_UI_ORANGE, MYPI_UI_BG, 1);
    }

    const char *management_ip = ap.running ? ap.ip :
                                (eth.mode == ETH_MODE_WAN && eth.has_ip ? eth.ip : "");
    display_draw_str(0, 177, "WEB MANAGEMENT ADDRESS", MYPI_UI_MUTED, MYPI_UI_BG, 1);
    display_draw_str(0, 190, management_ip[0] ? management_ip : "Use active network address",
                     MYPI_UI_ORANGE, MYPI_UI_BG, management_ip[0] ? 2 : 1);

    draw_uart_divider(UART_X, 236, UART_BIT_W, MYPI_UI_ORANGE);
    display_draw_str((LCD_H_RES - 7 * CHAR_W) / 2, 249, "Hotspot", MYPI_UI_TEXT, MYPI_UI_BG, 1);
    draw_button(8, MENU_BUTTON_Y0, 231, MENU_BUTTON_Y1, "OPEN MENU", MYPI_UI_ORANGE);
    display_flush();
    display_unlock();
    (void)ui;
}

static void draw_menu(void)
{
    static const char *items[] = {"Wi-Fi uplink", "Access point", "Connected clients",
                                  "Traffic", "Ethernet", "System / display"};
    display_lock();
    display_fill_screen(MYPI_UI_BG);
    draw_page_title("MyPi menu");
    for (int i = 0; i < (int)(sizeof(items) / sizeof(items[0])); i++) {
        int y = MYPI_UI_MENU_Y0 + i * (MYPI_UI_MENU_ROW_H + MYPI_UI_MENU_GAP);
        display_fill_rect(MYPI_UI_MENU_X0, y, MYPI_UI_MENU_X1 + 1, y + MYPI_UI_MENU_ROW_H, MYPI_UI_BG);
        display_draw_rect_outline(MYPI_UI_MENU_X0, y, MYPI_UI_MENU_X1, y + MYPI_UI_MENU_ROW_H - 1, MYPI_UI_MUTED);
        display_draw_str(MYPI_UI_MENU_X0 + 10, y + 13, items[i], MYPI_UI_TEXT, MYPI_UI_BG, 1);
    }
    draw_button(8, MENU_BUTTON_Y0, 231, MENU_BUTTON_Y1, "HOME", MYPI_UI_ORANGE);
    display_flush();
    display_unlock();
}

static void draw_wifi_scan(const ui_state_t *ui)
{
    display_lock();
    display_fill_screen(MYPI_UI_BG);
    draw_page_title("Choose Wi-Fi");
    if (ui->busy) display_draw_str(8, 38, "Scanning nearby networks...", MYPI_UI_YELLOW, MYPI_UI_BG, 1);
    for (int i = 0; i < ui->network_count; i++) {
        int y = 52 + i * 27;
        char line[38];
        snprintf(line, sizeof(line), "%d %-.25s %ddBm%s", i + 1, ui->networks[i].ssid,
                 ui->networks[i].rssi, ui->networks[i].profile >= 0 ? " *" : "");
        display_draw_rect_outline(8, y, 231, y + 24, MYPI_UI_MUTED);
        display_draw_str(13, y + 8, line, MYPI_UI_TEXT, MYPI_UI_BG, 1);
    }
    if (ui->network_count == 0 && !ui->busy && ui->message[0]) {
        display_draw_str(8, 64, ui->message, MYPI_UI_MUTED, MYPI_UI_BG, 1);
    }
    if (ui->message[0] && !ui->busy) display_draw_str(8, 272, ui->message, MYPI_UI_ORANGE, MYPI_UI_BG, 1);
    draw_button(8, MENU_BUTTON_Y0, 231, MENU_BUTTON_Y1, "BACK", MYPI_UI_ORANGE);
    display_flush();
    display_unlock();
}

static void draw_password(const ui_state_t *ui)
{
    uint32_t cp = wheel_codepoint(ui->wheel_index);
    char label[24];
    if (cp < 0x7F) snprintf(label, sizeof(label), "Character: %c", (char)cp);
    else snprintf(label, sizeof(label), "Character U+%04lX", (unsigned long)cp);
    display_lock();
    display_fill_screen(MYPI_UI_BG);
    draw_page_title("Wi-Fi password");
    display_draw_str(8, 42, ui->password_ssid, MYPI_UI_CYAN, MYPI_UI_BG, 1);
    display_draw_str(8, 62, "Password (hidden):", MYPI_UI_MUTED, MYPI_UI_BG, 1);
    int stars = (int)strlen(ui->password);
    char mask[32];
    int count = stars > 24 ? 24 : stars;
    memset(mask, '*', count);
    mask[count] = '\0';
    display_draw_str(8, 76, mask, MYPI_UI_TEXT, MYPI_UI_BG, 1);
    display_draw_str(8, 104, label, MYPI_UI_TEXT, MYPI_UI_BG, 2);
    if (ui->message[0]) display_draw_str(8, 133, ui->message, MYPI_UI_ORANGE, MYPI_UI_BG, 1);
    draw_button(8, 150, 59, 184, "<", MYPI_UI_MUTED);
    draw_button(64, 150, 115, 184, ">", MYPI_UI_MUTED);
    draw_button(120, 150, 173, 184, "ADD", MYPI_UI_GREEN);
    draw_button(178, 150, 231, 184, "DEL", MYPI_UI_RED);
    draw_button(8, 207, 231, 238, "SAVE + CONNECT", MYPI_UI_ORANGE);
    draw_button(8, 245, 231, 276, "OPEN NETWORK", MYPI_UI_CYAN);
    draw_button(8, MENU_BUTTON_Y0, 231, MENU_BUTTON_Y1, "CANCEL", MYPI_UI_MUTED);
    display_flush();
    display_unlock();
}

static void draw_ap(const ui_state_t *ui)
{
    wifi_ap_info_t ap;
    wifi_get_ap_info(&ap);
    display_lock();
    display_fill_screen(MYPI_UI_BG);
    draw_page_title("Access point");
    if (ap.running) {
        display_draw_str(8, 42, ap.ssid, MYPI_UI_GREEN, MYPI_UI_BG, 1);
        char line[40];
        snprintf(line, sizeof(line), "%s  %d clients  ch%d", ap.ip, ap.clients, ap.channel);
        display_draw_str(8, 58, line, MYPI_UI_TEXT, MYPI_UI_BG, 1);
    } else {
        display_draw_str(8, 42, "AP is stopped", MYPI_UI_MUTED, MYPI_UI_BG, 1);
    }
    draw_button(8, 78, 231, 112, "START DEFAULT AP", MYPI_UI_GREEN);
    for (int i = 0; i < AP_PROFILE_MAX; i++) {
        char label[32];
        ap_profile_t profile = {0};
        if (ap_profile_get(i, &profile) && profile.valid) snprintf(label, sizeof(label), "Start: %.25s", profile.ssid);
        else snprintf(label, sizeof(label), "AP profile %d (empty)", i);
        draw_button(8, 92 + i * 34, 231, 120 + i * 34, label, profile.valid ? MYPI_UI_TEXT : MYPI_UI_MUTED);
    }
    draw_button(8, 232, 231, 264, "STOP AP...", MYPI_UI_RED);
    if (ui->message[0]) display_draw_str(8, 264, ui->message, MYPI_UI_ORANGE, MYPI_UI_BG, 1);
    draw_button(8, MENU_BUTTON_Y0, 231, MENU_BUTTON_Y1, "BACK", MYPI_UI_ORANGE);
    display_flush();
    display_unlock();
}

static void draw_ap_stop_confirm(const ui_state_t *ui)
{
    display_lock();
    display_fill_screen(MYPI_UI_BG);
    draw_page_title("Stop access point?");
    display_draw_str(8, 62, "This disconnects AP clients.", MYPI_UI_TEXT, MYPI_UI_BG, 1);
    display_draw_str(8, 80, "Navigating away does not stop it.", MYPI_UI_MUTED, MYPI_UI_BG, 1);
    if (ui->message[0]) display_draw_str(8, 122, ui->message, MYPI_UI_ORANGE, MYPI_UI_BG, 1);
    draw_button(8, 170, 231, 220, "STOP AP", MYPI_UI_RED);
    draw_button(8, 232, 231, 273, "KEEP RUNNING", MYPI_UI_GREEN);
    display_flush();
    display_unlock();
}

static void draw_clients(const ui_state_t *ui)
{
    display_lock();
    display_fill_screen(MYPI_UI_BG);
    draw_page_title("Connected clients");
    if (ui->busy) display_draw_str(8, 40, "Reading AP client list...", MYPI_UI_MUTED, MYPI_UI_BG, 1);
    if (ui->client_count == 0 && !ui->busy) display_draw_str(8, 52, "No clients (or AP is off)", MYPI_UI_MUTED, MYPI_UI_BG, 1);
    for (int i = 0; i < ui->client_count; i++) {
        char mac[18], line[40];
        snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                 ui->clients[i].mac[0], ui->clients[i].mac[1], ui->clients[i].mac[2],
                 ui->clients[i].mac[3], ui->clients[i].mac[4], ui->clients[i].mac[5]);
        snprintf(line, sizeof(line), "%.17s %.15s", mac, ui->clients[i].ip);
        display_draw_str(8, 48 + i * 24, line, MYPI_UI_TEXT, MYPI_UI_BG, 1);
    }
    if (ui->message[0] && !ui->busy) display_draw_str(8, 260, ui->message, MYPI_UI_ORANGE, MYPI_UI_BG, 1);
    draw_button(8, MENU_BUTTON_Y0, 231, MENU_BUTTON_Y1, "BACK", MYPI_UI_ORANGE);
    display_flush();
    display_unlock();
}

static void draw_traffic(void)
{
    net_if_stats_t stats[NET_IF_COUNT];
    char names[NET_IF_COUNT][8];
    for (int i = 0; i < NET_IF_COUNT; i++) {
        net_stats_get((net_if_id_t)i, &stats[i]);
        strlcpy(names[i], net_if_name((net_if_id_t)i), sizeof(names[i]));
    }
    display_lock();
    display_fill_screen(MYPI_UI_BG);
    draw_page_title("Traffic");
    for (int i = 0; i < NET_IF_COUNT; i++) {
        char line[40];
        snprintf(line, sizeof(line), "%.3s RX %.2f TX %.2f", names[i],
                 (double)stats[i].rx_mbps, (double)stats[i].tx_mbps);
        display_draw_str(8, 58 + i * 28, line, MYPI_UI_CYAN, MYPI_UI_BG, 1);
    }
    draw_button(8, MENU_BUTTON_Y0, 231, MENU_BUTTON_Y1, "BACK", MYPI_UI_ORANGE);
    display_flush();
    display_unlock();
}

static void draw_ethernet(void)
{
    eth_info_t eth;
    eth_get_info(&eth);
    display_lock();
    display_fill_screen(MYPI_UI_BG);
    draw_page_title("Ethernet");
    display_draw_str(8, 52, eth.initialized ? (eth.link_up ? "Link up" : "Link down") : "Not initialized",
                     eth.link_up ? MYPI_UI_GREEN : MYPI_UI_RED, MYPI_UI_BG, 1);
    display_draw_str(8, 72, eth.mode == ETH_MODE_LAN ? "Mode: LAN bridge" : "Mode: WAN uplink",
                     MYPI_UI_TEXT, MYPI_UI_BG, 1);
    display_draw_str(8, 92, eth.has_ip ? eth.ip : "No Ethernet IP", MYPI_UI_CYAN, MYPI_UI_BG, 1);
    display_draw_str(8, 116, "Change mode on the web UI.", MYPI_UI_MUTED, MYPI_UI_BG, 1);
    draw_button(8, MENU_BUTTON_Y0, 231, MENU_BUTTON_Y1, "BACK", MYPI_UI_ORANGE);
    display_flush();
    display_unlock();
}

static void draw_system(void)
{
    display_lock();
    display_fill_screen(MYPI_UI_BG);
    draw_page_title("System / display");
    display_draw_str(8, 52, display_get_rotated() ? "Display: 180 degrees" : "Display: normal",
                     MYPI_UI_CYAN, MYPI_UI_BG, 1);
    display_draw_str(8, 78, "Rotation and advanced settings", MYPI_UI_MUTED, MYPI_UI_BG, 1);
    display_draw_str(8, 94, "are managed from the web UI.", MYPI_UI_MUTED, MYPI_UI_BG, 1);
    display_draw_str(8, 130, "Emergency Ethernet rescue:", MYPI_UI_TEXT, MYPI_UI_BG, 1);
    display_draw_str(8, 146, "swipe down during the first 10s.", MYPI_UI_ORANGE, MYPI_UI_BG, 1);
    draw_button(8, MENU_BUTTON_Y0, 231, MENU_BUTTON_Y1, "BACK", MYPI_UI_ORANGE);
    display_flush();
    display_unlock();
}

void display_ui_handle_tap(int x, int y)
{
    ui_state_t ui;
    ui_snapshot(&ui);
    switch (ui.view) {
    case UI_HOME:
        if (mypi_ui_button_hit(x, y, 8, MENU_BUTTON_Y0, 231, MENU_BUTTON_Y1)) ui.view = UI_MENU;
        break;
    case UI_MENU: {
        if (mypi_ui_button_hit(x, y, 8, MENU_BUTTON_Y0, 231, MENU_BUTTON_Y1)) {
            ui.view = UI_HOME;
            break;
        }
        int row = mypi_ui_row_hit(y, MYPI_UI_MENU_Y0, MYPI_UI_MENU_ROW_H, MYPI_UI_MENU_GAP, 6);
        if (row < 0 || x < MYPI_UI_MENU_X0 || x > MYPI_UI_MENU_X1) break;
        if (row == 0) {
            ui.view = UI_WIFI_SCAN;
            ui.network_count = 0;
            ui_queue_job(UI_JOB_SCAN, -1, NULL, NULL);
        } else if (row == 1) {
            ui.view = UI_AP;
        } else if (row == 2) {
            ui.view = UI_CLIENTS;
            ui.client_count = 0;
            ui_queue_job(UI_JOB_CLIENTS, -1, NULL, NULL);
        } else if (row == 3) ui.view = UI_TRAFFIC;
        else if (row == 4) ui.view = UI_ETHERNET;
        else ui.view = UI_SYSTEM;
        break;
    }
    case UI_WIFI_SCAN:
        if (mypi_ui_button_hit(x, y, 8, MENU_BUTTON_Y0, 231, MENU_BUTTON_Y1)) {
            ui.view = UI_MENU;
        } else if (!ui.busy && x >= 8 && x <= 231) {
            int row = mypi_ui_row_hit(y, 52, 25, 2, ui.network_count);
            if (row >= 0) {
                wifi_scan_entry_t selected = ui.networks[row];
                if (selected.profile >= 0) {
                    ui_queue_job(UI_JOB_CONNECT, selected.profile, NULL, NULL);
                } else {
                    portENTER_CRITICAL(&s_ui_mux);
                    memset(s_ui.password, 0, sizeof(s_ui.password));
                    strlcpy(s_ui.password_ssid, selected.ssid, sizeof(s_ui.password_ssid));
                    s_ui.wheel_index = 1;
                    s_ui.message[0] = '\0';
                    portEXIT_CRITICAL(&s_ui_mux);
                    ui.view = UI_PASSWORD;
                }
            }
        }
        break;
    case UI_PASSWORD:
        if (mypi_ui_button_hit(x, y, 8, MENU_BUTTON_Y0, 231, MENU_BUTTON_Y1)) {
            memset(ui.password, 0, sizeof(ui.password));
            ui.view = UI_WIFI_SCAN;
        } else if (mypi_ui_button_hit(x, y, 8, 150, 59, 184)) {
            ui.wheel_index = (ui.wheel_index + WHEEL_COUNT - 1) % WHEEL_COUNT;
        } else if (mypi_ui_button_hit(x, y, 64, 150, 115, 184)) {
            ui.wheel_index = (ui.wheel_index + 1) % WHEEL_COUNT;
        } else if (mypi_ui_button_hit(x, y, 120, 150, 173, 184)) {
            if (!mypi_utf8_append(ui.password, sizeof(ui.password), wheel_codepoint(ui.wheel_index))) {
                strlcpy(ui.message, "Password is at the 63-byte limit", sizeof(ui.message));
            } else {
                ui.message[0] = '\0';
            }
        } else if (mypi_ui_button_hit(x, y, 178, 150, 231, 184)) {
            mypi_utf8_backspace(ui.password);
            ui.message[0] = '\0';
        } else if (mypi_ui_button_hit(x, y, 8, 207, 231, 238) ||
                   mypi_ui_button_hit(x, y, 8, 245, 231, 276)) {
            bool open = y >= 245;
            if (open) ui.password[0] = '\0';
            if (!mypi_wifi_pass_length_valid(ui.password)) {
                strlcpy(ui.message, "Use 8-63 UTF-8 bytes, or Open network", sizeof(ui.message));
            } else {
                ui_queue_job(UI_JOB_CONNECT, -1, ui.password_ssid, ui.password);
                memset(ui.password, 0, sizeof(ui.password));
                ui.view = UI_WIFI_SCAN;
            }
        }
        portENTER_CRITICAL(&s_ui_mux);
        strlcpy(s_ui.password, ui.password, sizeof(s_ui.password));
        s_ui.wheel_index = ui.wheel_index;
        strlcpy(s_ui.message, ui.message, sizeof(s_ui.message));
        portEXIT_CRITICAL(&s_ui_mux);
        break;
    case UI_AP:
        if (mypi_ui_button_hit(x, y, 8, MENU_BUTTON_Y0, 231, MENU_BUTTON_Y1)) ui.view = UI_MENU;
        else if (mypi_ui_button_hit(x, y, 8, 78, 231, 112)) ui_queue_job(UI_JOB_AP_START, -1, NULL, NULL);
        else if (mypi_ui_button_hit(x, y, 8, 92, 231, 226)) {
            int slot = (y - 92) / 34;
            ap_profile_t profile;
            if (slot >= 0 && slot < AP_PROFILE_MAX && ap_profile_get(slot, &profile) && profile.valid) {
                ui_queue_job(UI_JOB_AP_START, slot, NULL, NULL);
            }
        } else if (mypi_ui_button_hit(x, y, 8, 232, 231, 264)) ui.view = UI_AP_STOP_CONFIRM;
        break;
    case UI_AP_STOP_CONFIRM:
        if (mypi_ui_button_hit(x, y, 8, 170, 231, 220)) {
            ui_queue_job(UI_JOB_AP_STOP, -1, NULL, NULL);
            ui.view = UI_AP;
        } else if (mypi_ui_button_hit(x, y, 8, 232, 231, 273)) ui.view = UI_AP;
        break;
    case UI_CLIENTS:
    case UI_TRAFFIC:
    case UI_ETHERNET:
    case UI_SYSTEM:
        if (mypi_ui_button_hit(x, y, 8, MENU_BUTTON_Y0, 231, MENU_BUTTON_Y1)) ui.view = UI_MENU;
        break;
    }
    portENTER_CRITICAL(&s_ui_mux);
    s_ui.view = ui.view;
    s_redraw_requested = true;
    portEXIT_CRITICAL(&s_ui_mux);
}

// ---------------------------------------------------------------------------
// Status screen
// ---------------------------------------------------------------------------

static void draw_status(void)
{
    ui_state_t ui;
    ui_snapshot(&ui);
    draw_home(&ui);
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
            bool redraw;
            portENTER_CRITICAL(&s_ui_mux);
            redraw = s_redraw_requested;
            s_redraw_requested = false;
            portEXIT_CRITICAL(&s_ui_mux);
            if (ticks == 0 || redraw) {
                ui_state_t ui;
                ui_snapshot(&ui);
                switch (ui.view) {
                case UI_HOME: draw_status(); break;
                case UI_MENU: draw_menu(); break;
                case UI_WIFI_SCAN: draw_wifi_scan(&ui); break;
                case UI_PASSWORD: draw_password(&ui); break;
                case UI_AP: draw_ap(&ui); break;
                case UI_AP_STOP_CONFIRM: draw_ap_stop_confirm(&ui); break;
                case UI_CLIENTS: draw_clients(&ui); break;
                case UI_TRAFFIC: draw_traffic(); break;
                case UI_ETHERNET: draw_ethernet(); break;
                case UI_SYSTEM: draw_system(); break;
                }
            }
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
    memset(&s_ui, 0, sizeof(s_ui));
    s_ui.view = UI_HOME;
    s_redraw_requested = true;
    display_lock();
    display_fill_screen(COL_BLACK);
    display_unlock();
    if (xTaskCreate(ui_worker, "display_ui_worker", 4096, NULL, 2, &s_worker_task) != pdPASS) {
        s_worker_task = NULL;
        ESP_LOGE(TAG, "failed to create local UI worker");
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(ui_task, "display_ui", 4096, NULL, 2, &s_task) != pdPASS) {
        s_task = NULL;
        vTaskDelete(s_worker_task);
        s_worker_task = NULL;
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
