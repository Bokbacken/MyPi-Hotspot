// MyPi-Hotspot — hello world for display + touch.
//
// Behavior:
//   * Boot: fill the screen navy blue, then start the LCD status screen
//     (display_ui.c). While the status screen runs, touch drawing is disabled
//     (events are still recorded); the drawing below is the fallback.
//   * Tap (finger down + up with < TAP_MAX_DRIFT_PX movement): draw a filled
//     circle in a random color at the touch point.
//   * Drag: draw a black line following the finger.
//   * Touch events are kept in the web UI history; with debug mode on they are
//     also logged to the UART console (115200 baud).
//
// Diagnostics: boot banner + heartbeat on serial, log ring buffer, Wi-Fi STA,
// web status page and OTA updates (see README.md). Wi-Fi runs in its own task
// and never blocks the display/touch loop.

#include <stdlib.h>

#include "diag.h"
#include "display.h"
#include "display_ui.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "logbuf.h"
#include "nvs_flash.h"
#include "pintest.h"
#include "wifi.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "touch.h"

static const char *TAG = "main";

#define IDLE_WAIT_MS        1000
#define TAP_MAX_DRIFT_PX    10
#define DOT_RADIUS_PX       8
#define LINE_THICKNESS_PX   3

static uint16_t random_color(void)
{
    uint32_t r = esp_random();
    return RGB565((r >> 16) & 0xFF, (r >> 8) & 0xFF, r & 0xFF);
}

void app_main(void)
{
    // Mirror logs into RAM first so the web UI shows the whole boot log.
    if (logbuf_init(CONFIG_MYPI_LOG_BUFFER_SIZE) != ESP_OK) {
        ESP_LOGW(TAG, "Log ring buffer allocation failed; web log disabled");
    }
    diag_print_boot_banner();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase (%s), erasing", esp_err_to_name(err));
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
    }
    diag_debug_load();   // Needs NVS; decides whether the boot-time checks below run

    // Wiring check must run before the SPI/I2C drivers claim the module pins.
    if (diag_debug_enabled()) {
        pintest_boot_check();
    }

    // Wi-Fi + web server + mDNS start in the background (needs NVS).
    wifi_start_background();
    diag_start_heartbeat();

    bool display_ok = false;
    bool status_ui = false;         // Status screen owns the panel; touch drawing is disabled
    err = display_init();
    if (err == ESP_OK) {
        display_ok = true;
        if (diag_debug_enabled()) {
            display_test_pattern();
        }
        display_fill_screen(COLOR_NAVY);
        err = display_flush();
        ESP_LOGD(TAG, "Screen filled navy blue: %s", esp_err_to_name(err));
        status_ui = display_ui_init() == ESP_OK;
    } else {
        ESP_LOGE(TAG, "Display init failed (%s) - continuing without display", esp_err_to_name(err));
    }

    bool touch_ok = touch_init() == ESP_OK;
    if (!touch_ok) {
        ESP_LOGE(TAG, "Touch init failed - touch disabled");
    }

    // Rollback protection: a freshly OTA-updated image is confirmed once it got this far.
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        err = esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "New OTA image marked valid: %s", esp_err_to_name(err));
        diag_set_ota_status("new image on %s marked valid", running->label);
    }

    if (!touch_ok) {
        vTaskDelay(portMAX_DELAY);   // Nothing more to do here; Wi-Fi/web keep running
    }

    bool was_pressed = false;
    bool dragging = false;
    int start_x = 0, start_y = 0;   // Where the finger went down
    int last_x = 0, last_y = 0;     // Last position seen while pressed
    while (true) {
        // Idle: block until TP_INT fires (or 1 s passes). Finger down: poll every 10 ms.
        if (!touch_wait(was_pressed, IDLE_WAIT_MS)) {
            continue;   // No touch activity; don't poke a sleeping controller
        }
        touch_point_t pt;
        if (touch_read(&pt) != ESP_OK) {
            // touch.c counts, rate-limits logging and recovers the bus.
            // Back off so a dead bus doesn't spin the CPU.
            uint32_t n = touch_consecutive_errors();
            vTaskDelay(pdMS_TO_TICKS(n > 20 ? 500 : 50));
            if (was_pressed && n >= 3) {
                was_pressed = false;   // Treat as finger lifted; don't draw a stale tap
                dragging = false;
            }
            continue;
        }

        if (pt.pressed && !was_pressed) {
            // Finger down
            start_x = last_x = pt.x;
            start_y = last_y = pt.y;
            dragging = false;
            ESP_LOGD(TAG, "DOWN  x=%3d y=%3d", pt.x, pt.y);
            diag_touch_event("down %d,%d", pt.x, pt.y);
        } else if (pt.pressed && was_pressed) {
            // Finger held: check whether this has become a drag
            if (!dragging && (abs(pt.x - start_x) > TAP_MAX_DRIFT_PX || abs(pt.y - start_y) > TAP_MAX_DRIFT_PX)) {
                dragging = true;
                ESP_LOGD(TAG, "DRAG  start x=%3d y=%3d", start_x, start_y);
                if (!status_ui) display_draw_line(start_x, start_y, pt.x, pt.y, LINE_THICKNESS_PX, COLOR_BLACK);
                last_x = pt.x;
                last_y = pt.y;
            } else if (dragging && (pt.x != last_x || pt.y != last_y)) {
                if (!status_ui) display_draw_line(last_x, last_y, pt.x, pt.y, LINE_THICKNESS_PX, COLOR_BLACK);
                ESP_LOGD(TAG, "MOVE  x=%3d y=%3d", pt.x, pt.y);
                last_x = pt.x;
                last_y = pt.y;
            }
        } else if (!pt.pressed && was_pressed) {
            // Finger up
            if (dragging) {
                ESP_LOGD(TAG, "UP    drag end x=%3d y=%3d", last_x, last_y);
                diag_touch_event("drag %d,%d -> %d,%d", start_x, start_y, last_x, last_y);
            } else {
                uint16_t color = random_color();
                if (!status_ui) display_draw_circle(start_x, start_y, DOT_RADIUS_PX, color);
                ESP_LOGD(TAG, "TAP   x=%3d y=%3d color=0x%04X", start_x, start_y, color);
                diag_touch_event("tap %d,%d color 0x%04X", start_x, start_y, color);
            }
            if (pt.gesture != TOUCH_GESTURE_NONE) {
                ESP_LOGD(TAG, "      controller gesture: %s (0x%02X)", touch_gesture_name(pt.gesture), pt.gesture);
            }
            dragging = false;
        }
        was_pressed = pt.pressed;

        if (display_ok && !status_ui) {
            display_flush();
        }
    }
}
