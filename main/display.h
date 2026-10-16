#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "pins.h"   // All GPIO assignments (VERIFY BEFORE FLASHING)
#include "sdkconfig.h"

// Waveshare 2" Capacitive Touch LCD (ST7789, 240x320, SPI)
#define LCD_SPI_HOST        SPI2_HOST
#define LCD_PIXEL_CLOCK_HZ  (CONFIG_MYPI_LCD_SPI_CLOCK_MHZ * 1000 * 1000)   // Kconfig, default 10 MHz
#define LCD_H_RES           240
#define LCD_V_RES           320

// RGB565 helpers (native-endian; byte swapping is handled during flush)
#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define COLOR_BLACK     RGB565(0, 0, 0)
#define COLOR_WHITE     RGB565(255, 255, 255)
#define COLOR_NAVY      RGB565(0, 0, 128)
#define COLOR_RED       RGB565(255, 0, 0)
#define COLOR_GREEN     RGB565(0, 255, 0)
#define COLOR_BLUE      RGB565(0, 0, 255)
#define COLOR_YELLOW    RGB565(255, 255, 0)
#define COLOR_CYAN      RGB565(0, 255, 255)
#define COLOR_MAGENTA   RGB565(255, 0, 255)

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

/** Fill a rectangle (inclusive of x0/y0, exclusive of x1/y1). */
void display_fill_rect(int x0, int y0, int x1, int y1, uint16_t color);

/** Cycle full-screen R/G/B/white/black (~1 s each) then show color bars (~2 s). */
void display_test_pattern(void);

/** Push the changed (dirty) region of the framebuffer to the panel. */
esp_err_t display_flush(void);
