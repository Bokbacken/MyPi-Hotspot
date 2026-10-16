#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "pins.h"   // All GPIO assignments (VERIFY BEFORE FLASHING)

// Waveshare 2" Capacitive Touch LCD (ST7789, 240x320, SPI)
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
