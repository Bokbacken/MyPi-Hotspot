#pragma once

#include <stdint.h>
#include "esp_err.h"

// ============================================================================
// === VERIFY BEFORE FLASHING — adjust to your actual wiring ===
// Waveshare 2" Capacitive Touch LCD (ST7789, 240x320, SPI)
// on Waveshare ESP32-P4-NANO.
// ============================================================================
// SPI pins are routed through the GPIO matrix, so any free header GPIO works.
// Avoid 34-38 (strapping; 35 = boot mode), 24/25 (USB-JTAG), 6 (tied to Wi-Fi co-processor IO).
// MISO is not used: the ST7789 module is write-only.
#define LCD_SPI_SCLK        23      // SPI clock (SCL/CLK on the LCD)
#define LCD_SPI_MOSI        5       // SPI data  (SDA/DIN on the LCD)
#define LCD_CS              20      // Chip select
#define LCD_DC              21      // Data/command select
#define LCD_BL              22      // Backlight (driven high = on)
#define LCD_RST             (-1)    // Reset not connected -> software reset
// ============================================================================

#define LCD_SPI_HOST        SPI2_HOST
#define LCD_PIXEL_CLOCK_HZ  (40 * 1000 * 1000)
#define LCD_H_RES           240
#define LCD_V_RES           320

// RGB565 helpers (native-endian; byte swapping is handled during flush)
#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define COLOR_BLACK     RGB565(0, 0, 0)
#define COLOR_WHITE     RGB565(255, 255, 255)
#define COLOR_NAVY      RGB565(0, 0, 128)

/** Initialize SPI bus, ST7789 panel, backlight and the PSRAM framebuffer. */
esp_err_t display_init(void);

/** Fill the whole framebuffer with one color (marks the whole screen dirty). */
void display_fill_screen(uint16_t color);

/** Set a single pixel in the framebuffer (out-of-range coordinates are ignored). */
void display_draw_pixel(int x, int y, uint16_t color);

/** Draw a filled circle centered at (cx, cy). */
void display_draw_circle(int cx, int cy, int radius, uint16_t color);

/** Draw a line with the given thickness (1 = single pixel). */
void display_draw_line(int x0, int y0, int x1, int y1, int thickness, uint16_t color);

/** Push the changed (dirty) region of the framebuffer to the panel. */
esp_err_t display_flush(void);
