// MyPi-Hotspot — hello world for display + touch.
//
// Behavior:
//   * Boot: fill the screen navy blue.
//   * Tap (finger down + up with < TAP_MAX_DRIFT_PX movement): draw a filled
//     circle in a random color at the touch point.
//   * Drag: draw a black line following the finger.
//   * All touch events are logged to the UART console (115200 baud).
//
// Diagnostics: boot banner + heartbeat on serial, log ring buffer, Wi-Fi STA,
// web status page and OTA updates (see README.md). Wi-Fi runs in its own task
// and never blocks the display/touch loop.

#include <stdlib.h>

#include "diag.h"
#include "display.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "logbuf.h"
#include "nvs_flash.h"
#include "wifi.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "touch.h"

static const char *TAG = "main";

#define POLL_INTERVAL_MS    10
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
    ESP_LOGI(TAG, "[%s] nvs_flash_init: %s", err == ESP_OK ? " OK " : "FAIL", esp_err_to_name(err));

    // Wi-Fi + web server + mDNS start in the background (needs NVS).
    wifi_start_background();
    diag_start_heartbeat();

    bool display_ok = false;
    err = display_init();
    if (err == ESP_OK) {
        display_ok = true;
#ifdef CONFIG_MYPI_LCD_TEST_PATTERN
        display_test_pattern();
#endif
        display_fill_screen(COLOR_NAVY);
        err = display_flush();
        ESP_LOGI(TAG, "Screen filled navy blue: %s", esp_err_to_name(err));
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
    int read_errors = 0;

    while (true) {
        touch_point_t pt;
        if (touch_read(&pt) != ESP_OK) {
            // Occasional NACKs are normal; only log periodically.
            if (++read_errors % 100 == 1) {
                ESP_LOGW(TAG, "Touch read failed (%d errors so far)", read_errors);
            }
            vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
            continue;
        }

        if (pt.pressed && !was_pressed) {
            // Finger down
            start_x = last_x = pt.x;
            start_y = last_y = pt.y;
            dragging = false;
            ESP_LOGI(TAG, "DOWN  x=%3d y=%3d", pt.x, pt.y);
            diag_touch_event("down %d,%d", pt.x, pt.y);
        } else if (pt.pressed && was_pressed) {
            // Finger held: check whether this has become a drag
            if (!dragging && (abs(pt.x - start_x) > TAP_MAX_DRIFT_PX || abs(pt.y - start_y) > TAP_MAX_DRIFT_PX)) {
                dragging = true;
                ESP_LOGI(TAG, "DRAG  start x=%3d y=%3d", start_x, start_y);
                display_draw_line(start_x, start_y, pt.x, pt.y, LINE_THICKNESS_PX, COLOR_BLACK);
                last_x = pt.x;
                last_y = pt.y;
            } else if (dragging && (pt.x != last_x || pt.y != last_y)) {
                display_draw_line(last_x, last_y, pt.x, pt.y, LINE_THICKNESS_PX, COLOR_BLACK);
                ESP_LOGI(TAG, "MOVE  x=%3d y=%3d", pt.x, pt.y);
                last_x = pt.x;
                last_y = pt.y;
            }
        } else if (!pt.pressed && was_pressed) {
            // Finger up
            if (dragging) {
                ESP_LOGI(TAG, "UP    drag end x=%3d y=%3d", last_x, last_y);
                diag_touch_event("drag %d,%d -> %d,%d", start_x, start_y, last_x, last_y);
            } else {
                uint16_t color = random_color();
                display_draw_circle(start_x, start_y, DOT_RADIUS_PX, color);
                ESP_LOGI(TAG, "TAP   x=%3d y=%3d color=0x%04X", start_x, start_y, color);
                diag_touch_event("tap %d,%d color 0x%04X", start_x, start_y, color);
            }
            if (pt.gesture != TOUCH_GESTURE_NONE) {
                ESP_LOGI(TAG, "      controller gesture: %s (0x%02X)", touch_gesture_name(pt.gesture), pt.gesture);
            }
            dragging = false;
        }
        was_pressed = pt.pressed;

        if (display_ok) {
            display_flush();
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}
