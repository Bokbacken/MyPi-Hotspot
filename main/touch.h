#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "sdkconfig.h"
#include "pins.h"   // All GPIO assignments (VERIFY BEFORE FLASHING)

// CST816D capacitive touch controller (I2C) on the Waveshare 2" LCD.
// TP_RST is pulsed at init. When idle the controller is read only after a TP_INT
// falling edge (the CST816 sleeps and stops answering on I2C); while a finger is
// down it is polled every TOUCH_ACTIVE_POLL_MS.
#define TOUCH_I2C_ADDR      0x15    // CST816D 7-bit address

#define TOUCH_I2C_PORT      0
#define TOUCH_I2C_HZ        (CONFIG_MYPI_TOUCH_I2C_KHZ * 1000)
#define TOUCH_ACTIVE_POLL_MS 10

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

/**
 * Wait until the controller should be read.
 * Returns true when a read is due (TP_INT edge, finger down, or polling mode);
 * false when idle_timeout_ms passed without a TP_INT edge.
 */
bool touch_wait(bool finger_down, int idle_timeout_ms);

/** Read the controller once. Returns ESP_OK and fills *pt on success.
 *  Failures are counted, logged rate-limited and trigger bus recovery. */
esp_err_t touch_read(touch_point_t *pt);

/** Number of consecutive failed reads (0 = healthy). */
uint32_t touch_consecutive_errors(void);

/** Human-readable gesture name for logging. */
const char *touch_gesture_name(uint8_t gesture);
