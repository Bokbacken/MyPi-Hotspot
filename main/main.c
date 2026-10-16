// MyPi-Hotspot — hello world for display + touch.
//
// Behavior:
//   * Boot: fill the screen navy blue.
//   * Tap (finger down + up with < TAP_MAX_DRIFT_PX movement): draw a filled
//     circle in a random color at the touch point.
//   * Drag: draw a black line following the finger.
//   * All touch events are logged to the UART console (115200 baud).

#include <stdlib.h>

#include "display.h"
#include "esp_log.h"
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
    ESP_LOGI(TAG, "MyPi-Hotspot hello world starting");

    ESP_ERROR_CHECK(display_init());
    display_fill_screen(COLOR_NAVY);
    ESP_ERROR_CHECK(display_flush());
    ESP_LOGI(TAG, "Screen filled navy blue");

    if (touch_init() != ESP_OK) {
        ESP_LOGE(TAG, "Touch init failed - display still works, touch disabled");
        vTaskDelay(portMAX_DELAY);
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
            } else {
                uint16_t color = random_color();
                display_draw_circle(start_x, start_y, DOT_RADIUS_PX, color);
                ESP_LOGI(TAG, "TAP   x=%3d y=%3d color=0x%04X", start_x, start_y, color);
            }
            if (pt.gesture != TOUCH_GESTURE_NONE) {
                ESP_LOGI(TAG, "      controller gesture: %s (0x%02X)", touch_gesture_name(pt.gesture), pt.gesture);
            }
            dragging = false;
        }
        was_pressed = pt.pressed;

        display_flush();
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}
