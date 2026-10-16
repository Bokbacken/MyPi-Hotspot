#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// ============================================================================
// === VERIFY BEFORE FLASHING — adjust to your actual wiring ===
// CST816D capacitive touch controller (I2C) on the Waveshare 2" LCD.
// INT and RST are not connected: the controller is polled.
// ============================================================================
#define TOUCH_SDA           7       // I2C data
#define TOUCH_SCL           8       // I2C clock
#define TOUCH_I2C_ADDR      0x15    // CST816D 7-bit address
// ============================================================================

#define TOUCH_I2C_PORT      0
#define TOUCH_I2C_HZ        (400 * 1000)

/** Gesture IDs reported by the CST816D (register 0x01). */
typedef enum {
    TOUCH_GESTURE_NONE        = 0x00,
    TOUCH_GESTURE_SWIPE_UP    = 0x01,
    TOUCH_GESTURE_SWIPE_DOWN  = 0x02,
    TOUCH_GESTURE_SWIPE_LEFT  = 0x03,
    TOUCH_GESTURE_SWIPE_RIGHT = 0x04,
    TOUCH_GESTURE_SINGLE_TAP  = 0x05,
    TOUCH_GESTURE_DOUBLE_TAP  = 0x0B,
    TOUCH_GESTURE_LONG_PRESS  = 0x0C,
} touch_gesture_t;

typedef struct {
    bool pressed;           // true while a finger is on the panel
    uint16_t x;             // 0 .. LCD_H_RES-1
    uint16_t y;             // 0 .. LCD_V_RES-1
    uint8_t gesture;        // touch_gesture_t (controller-side gesture, informational)
} touch_point_t;

/** Initialize the I2C bus and probe the CST816D. */
esp_err_t touch_init(void);

/** Poll the controller once. Returns ESP_OK and fills *pt on success. */
esp_err_t touch_read(touch_point_t *pt);

/** Human-readable gesture name for logging. */
const char *touch_gesture_name(uint8_t gesture);
