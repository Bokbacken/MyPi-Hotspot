#include "display.h"

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "display";

// Rows per SPI transfer. The full frame lives in PSRAM; each band is copied
// (and byte-swapped) into a small internal DMA-capable buffer before sending.
#define FLUSH_BAND_ROWS 40

static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_fb;          // Full framebuffer, LCD_H_RES * LCD_V_RES, in PSRAM
static uint16_t *s_dma_buf;     // Bounce buffer in internal DMA memory
static SemaphoreHandle_t s_trans_done;

// Dirty rectangle (inclusive). s_dirty_x0 > s_dirty_x1 means "nothing dirty".
static int s_dirty_x0 = LCD_H_RES, s_dirty_y0 = LCD_V_RES, s_dirty_x1 = -1, s_dirty_y1 = -1;

static void mark_dirty(int x0, int y0, int x1, int y1)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= LCD_H_RES) x1 = LCD_H_RES - 1;
    if (y1 >= LCD_V_RES) y1 = LCD_V_RES - 1;
    if (x0 > x1 || y0 > y1) return;
    if (x0 < s_dirty_x0) s_dirty_x0 = x0;
    if (y0 < s_dirty_y0) s_dirty_y0 = y0;
    if (x1 > s_dirty_x1) s_dirty_x1 = x1;
    if (y1 > s_dirty_y1) s_dirty_y1 = y1;
}

static bool on_color_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_trans_done, &woken);
    return woken == pdTRUE;
}

esp_err_t display_init(void)
{
    ESP_LOGI(TAG, "Pins: SCLK=%d MOSI=%d MISO=%d SD_CS=%d LCD_CS=%d LCD_DC=%d LCD_RESET=%d LCD_BL=%d",
             PIN_SCLK, PIN_MOSI, PIN_MISO, PIN_SD_CS, PIN_LCD_CS, PIN_LCD_DC, PIN_LCD_RESET, PIN_LCD_BL);

    // Backlight off during init to avoid showing garbage.
    gpio_config_t bl_cfg = {
        .pin_bit_mask = 1ULL << PIN_LCD_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&bl_cfg), TAG, "backlight gpio");
    gpio_set_level(PIN_LCD_BL, 0);

    // Keep the module's microSD card deselected so it never drives the shared SPI bus.
    if (PIN_SD_CS >= 0) {
        gpio_config_t sd_cfg = {
            .pin_bit_mask = 1ULL << PIN_SD_CS,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&sd_cfg), TAG, "sd_cs gpio");
        gpio_set_level(PIN_SD_CS, 1);
    }

    s_trans_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_trans_done, ESP_ERR_NO_MEM, TAG, "semaphore");

    s_fb = heap_caps_malloc(LCD_H_RES * LCD_V_RES * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s_fb, ESP_ERR_NO_MEM, TAG, "framebuffer alloc in PSRAM failed");

    const size_t band_bytes = LCD_H_RES * FLUSH_BAND_ROWS * sizeof(uint16_t);
    s_dma_buf = heap_caps_malloc(band_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_RETURN_ON_FALSE(s_dma_buf, ESP_ERR_NO_MEM, TAG, "DMA buffer alloc failed");

    spi_bus_config_t bus_cfg = {
        .sclk_io_num = PIN_SCLK,
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,    // Only used by the microSD slot
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = band_bytes,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO), TAG, "spi bus");

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = PIN_LCD_DC,
        .cs_gpio_num = PIN_LCD_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 4,
        .on_color_trans_done = on_color_trans_done,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &io), TAG, "panel io");

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RESET,   // -1 falls back to software reset
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io, &panel_cfg, &s_panel), TAG, "st7789");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init");
    // The Waveshare 2" ST7789 module needs color inversion for correct colors.
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), TAG, "invert");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "disp on");

    display_fill_screen(COLOR_BLACK);
    ESP_RETURN_ON_ERROR(display_flush(), TAG, "first flush");
    gpio_set_level(PIN_LCD_BL, 1);

    ESP_LOGI(TAG, "ST7789 %dx%d ready, framebuffer %u bytes in PSRAM",
             LCD_H_RES, LCD_V_RES, (unsigned)(LCD_H_RES * LCD_V_RES * sizeof(uint16_t)));
    return ESP_OK;
}

void display_fill_screen(uint16_t color)
{
    for (int i = 0; i < LCD_H_RES * LCD_V_RES; i++) {
        s_fb[i] = color;
    }
    mark_dirty(0, 0, LCD_H_RES - 1, LCD_V_RES - 1);
}

void display_draw_pixel(int x, int y, uint16_t color)
{
    if (x < 0 || y < 0 || x >= LCD_H_RES || y >= LCD_V_RES) return;
    s_fb[y * LCD_H_RES + x] = color;
    mark_dirty(x, y, x, y);
}

void display_draw_circle(int cx, int cy, int radius, uint16_t color)
{
    const int r2 = radius * radius;
    for (int dy = -radius; dy <= radius; dy++) {
        int y = cy + dy;
        if (y < 0 || y >= LCD_V_RES) continue;
        for (int dx = -radius; dx <= radius; dx++) {
            int x = cx + dx;
            if (x < 0 || x >= LCD_H_RES) continue;
            if (dx * dx + dy * dy <= r2) {
                s_fb[y * LCD_H_RES + x] = color;
            }
        }
    }
    mark_dirty(cx - radius, cy - radius, cx + radius, cy + radius);
}

void display_draw_line(int x0, int y0, int x1, int y1, int thickness, uint16_t color)
{
    // Bresenham; thick lines are drawn by stamping small filled circles.
    const int r = thickness > 1 ? thickness / 2 : 0;
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (true) {
        if (r == 0) {
            display_draw_pixel(x0, y0, color);
        } else {
            display_draw_circle(x0, y0, r, color);
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

esp_err_t display_flush(void)
{
    if (s_dirty_x0 > s_dirty_x1 || s_dirty_y0 > s_dirty_y1) {
        return ESP_OK;  // Nothing changed
    }
    const int x0 = s_dirty_x0, x1 = s_dirty_x1, y0 = s_dirty_y0, y1 = s_dirty_y1;
    const int w = x1 - x0 + 1;
    const int band_rows = (LCD_H_RES * FLUSH_BAND_ROWS) / w;  // Rows that fit in the bounce buffer

    for (int y = y0; y <= y1; y += band_rows) {
        int rows = y1 - y + 1;
        if (rows > band_rows) rows = band_rows;

        // Copy region into the DMA buffer, swapping to big-endian (ST7789 wire order).
        uint16_t *dst = s_dma_buf;
        for (int row = 0; row < rows; row++) {
            const uint16_t *src = &s_fb[(y + row) * LCD_H_RES + x0];
            for (int i = 0; i < w; i++) {
                uint16_t c = src[i];
                *dst++ = (uint16_t)((c >> 8) | (c << 8));
            }
        }

        // x_end / y_end are exclusive in esp_lcd.
        esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, x0, y, x1 + 1, y + rows, s_dma_buf);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "draw_bitmap failed: %s", esp_err_to_name(err));
            return err;
        }
        // Wait for the DMA transfer before reusing the bounce buffer.
        if (xSemaphoreTake(s_trans_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGE(TAG, "SPI transfer timeout");
            return ESP_ERR_TIMEOUT;
        }
    }

    s_dirty_x0 = LCD_H_RES;
    s_dirty_y0 = LCD_V_RES;
    s_dirty_x1 = -1;
    s_dirty_y1 = -1;
    return ESP_OK;
}
