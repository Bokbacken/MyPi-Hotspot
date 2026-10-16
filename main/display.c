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
#include "freertos/task.h"
#include "esp_timer.h"
#include "diag.h"

#ifdef CONFIG_MYPI_LCD_INVERT_COLOR
#define LCD_INVERT true
#else
#define LCD_INVERT false
#endif
#ifdef CONFIG_MYPI_LCD_BGR_ORDER
#define LCD_BGR 1
#else
#define LCD_BGR 0
#endif

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

// Log and propagate a failing step; also record it for the web UI.
#define STEP(expr, what)                                                            \
    do {                                                                            \
        esp_err_t _e = (expr);                                                      \
        if (_e != ESP_OK) {                                                         \
            ESP_LOGE(TAG, "[FAIL] %s: %s (0x%x)", what, esp_err_to_name(_e), _e);   \
            diag_set_display_status("failed at '%s': %s", what, esp_err_to_name(_e)); \
            return _e;                                                              \
        }                                                                           \
        ESP_LOGI(TAG, "[ OK ] %s", what);                                           \
    } while (0)

static esp_lcd_panel_io_handle_t s_io;

static void set_backlight(int level, const char *why)
{
    gpio_set_level(PIN_LCD_BL, level);
    ESP_LOGI(TAG, "LCD_BL (GPIO%d) -> %d (%s)", PIN_LCD_BL, level, why);
}

// Try to read ST7789 ID/status registers over MISO. On the Waveshare module the
// LCD's SDO pin may not be wired to MISO (MISO serves the microSD slot), in which
// case the result is all 0x00 or 0xFF. In 4-wire SPI mode the ST7789 inserts one
// dummy clock before multi-byte read data, so the raw bytes are also shown
// shifted left by one bit.
static void read_panel_ids(void)
{
    uint8_t id[4] = {0}, st[5] = {0}, pm = 0;
    esp_err_t e1 = esp_lcd_panel_io_rx_param(s_io, 0x04, id, sizeof(id));   // RDDID
    esp_err_t e2 = esp_lcd_panel_io_rx_param(s_io, 0x09, st, sizeof(st));   // RDDST
    esp_err_t e3 = esp_lcd_panel_io_rx_param(s_io, 0x0A, &pm, 1);           // RDDPM
    if (e1 != ESP_OK || e2 != ESP_OK || e3 != ESP_OK) {
        ESP_LOGW(TAG, "Panel register read failed: RDDID=%s RDDST=%s RDDPM=%s",
                 esp_err_to_name(e1), esp_err_to_name(e2), esp_err_to_name(e3));
        diag_set_lcd_id("read failed (%s)", esp_err_to_name(e1));
        return;
    }
    uint32_t raw = ((uint32_t)id[0] << 24) | ((uint32_t)id[1] << 16) | ((uint32_t)id[2] << 8) | id[3];
    uint32_t shifted = (raw << 1) >> 8;   // Drop dummy bit, keep 24 ID bits
    bool all_same = (raw == 0) || (raw == 0xFFFFFFFF);
    ESP_LOGI(TAG, "RDDID raw=%02X %02X %02X %02X (dummy-bit corrected ID=0x%06lX)",
             id[0], id[1], id[2], id[3], (unsigned long)shifted);
    ESP_LOGI(TAG, "RDDST raw=%02X %02X %02X %02X %02X, RDDPM=0x%02X",
             st[0], st[1], st[2], st[3], st[4], pm);
    if (all_same) {
        ESP_LOGW(TAG, "Panel ID reads all 0x%02X - LCD SDO probably not connected to MISO "
                      "(normal for this module, not proof of a fault)", id[0]);
    }
    diag_set_lcd_id("RDDID %02X%02X%02X%02X (id 0x%06lX) RDDPM 0x%02X%s",
                    id[0], id[1], id[2], id[3], (unsigned long)shifted, pm,
                    all_same ? " - SDO likely not wired" : "");
}

esp_err_t display_init(void)
{
    diag_set_display_status("initializing");
    diag_set_spi_clock(LCD_PIXEL_CLOCK_HZ);
    ESP_LOGI(TAG, "Pins: SCLK=%d MOSI=%d MISO=%d SD_CS=%d LCD_CS=%d LCD_DC=%d LCD_RESET=%d LCD_BL=%d",
             PIN_SCLK, PIN_MOSI, PIN_MISO, PIN_SD_CS, PIN_LCD_CS, PIN_LCD_DC, PIN_LCD_RESET, PIN_LCD_BL);
    ESP_LOGI(TAG, "SPI clock %d MHz, mode 0, invert=%d, order=%s, %dx%d",
             CONFIG_MYPI_LCD_SPI_CLOCK_MHZ, LCD_INVERT, LCD_BGR ? "BGR" : "RGB", LCD_H_RES, LCD_V_RES);

    // Backlight off during init to avoid showing garbage.
    gpio_config_t bl_cfg = {
        .pin_bit_mask = 1ULL << PIN_LCD_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    STEP(gpio_config(&bl_cfg), "LCD_BL gpio_config");
    set_backlight(0, "off during init");

    // Keep the module's microSD card deselected so it never drives the shared SPI bus.
    if (PIN_SD_CS >= 0) {
        gpio_config_t sd_cfg = {
            .pin_bit_mask = 1ULL << PIN_SD_CS,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
        };
        STEP(gpio_config(&sd_cfg), "SD_CS gpio_config (held high)");
        gpio_set_level(PIN_SD_CS, 1);
    }

    s_trans_done = xSemaphoreCreateBinary();
    STEP(s_trans_done ? ESP_OK : ESP_ERR_NO_MEM, "create flush semaphore");

    s_fb = heap_caps_malloc(LCD_H_RES * LCD_V_RES * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    STEP(s_fb ? ESP_OK : ESP_ERR_NO_MEM, "framebuffer alloc (PSRAM)");

    const size_t band_bytes = LCD_H_RES * FLUSH_BAND_ROWS * sizeof(uint16_t);
    s_dma_buf = heap_caps_malloc(band_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    STEP(s_dma_buf ? ESP_OK : ESP_ERR_NO_MEM, "DMA bounce buffer alloc (internal)");

    spi_bus_config_t bus_cfg = {
        .sclk_io_num = PIN_SCLK,
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,    // microSD data out; also used for the panel ID read attempt
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = band_bytes,
    };
    STEP(spi_bus_initialize(LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO), "spi_bus_initialize (SPI2)");

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
    STEP(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &s_io), "esp_lcd_new_panel_io_spi");

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RESET,   // -1 falls back to software reset
        .rgb_ele_order = LCD_BGR ? LCD_RGB_ELEMENT_ORDER_BGR : LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    STEP(esp_lcd_new_panel_st7789(s_io, &panel_cfg, &s_panel), "esp_lcd_new_panel_st7789");

    STEP(esp_lcd_panel_reset(s_panel), "panel reset (LCD_RESET pulse)");
    STEP(esp_lcd_panel_init(s_panel), "panel init (SLPOUT, COLMOD, MADCTL)");
    read_panel_ids();
    STEP(esp_lcd_panel_invert_color(s_panel, LCD_INVERT), LCD_INVERT ? "invert_color(true)" : "invert_color(false)");
    STEP(esp_lcd_panel_disp_on_off(s_panel, true), "disp_on (DISPON)");

    display_fill_screen(COLOR_BLACK);
    STEP(display_flush(), "first full-screen flush (black)");
    set_backlight(1, "on after first flush");

    ESP_LOGI(TAG, "ST7789 %dx%d ready, framebuffer %u bytes in PSRAM",
             LCD_H_RES, LCD_V_RES, (unsigned)(LCD_H_RES * LCD_V_RES * sizeof(uint16_t)));
    diag_set_display_status("ok (SPI writes complete; cannot confirm the panel shows them)");
    return ESP_OK;
}

void display_fill_rect(int x0, int y0, int x1, int y1, uint16_t color)
{
    if (!s_fb) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > LCD_H_RES) x1 = LCD_H_RES;
    if (y1 > LCD_V_RES) y1 = LCD_V_RES;
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            s_fb[y * LCD_H_RES + x] = color;
        }
    }
    mark_dirty(x0, y0, x1 - 1, y1 - 1);
}

void display_test_pattern(void)
{
    static const struct { uint16_t color; const char *name; } fills[] = {
        { COLOR_RED, "red" }, { COLOR_GREEN, "green" }, { COLOR_BLUE, "blue" },
        { COLOR_WHITE, "white" }, { COLOR_BLACK, "black" },
    };
    ESP_LOGI(TAG, "Test pattern start");
    for (size_t i = 0; i < sizeof(fills) / sizeof(fills[0]); i++) {
        display_fill_screen(fills[i].color);
        int64_t t0 = esp_timer_get_time();
        esp_err_t err = display_flush();
        ESP_LOGI(TAG, "Test pattern: full-screen %-5s -> %s (%lld us)", fills[i].name,
                 esp_err_to_name(err), (long long)(esp_timer_get_time() - t0));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    static const uint16_t bars[] = {
        COLOR_WHITE, COLOR_YELLOW, COLOR_CYAN, COLOR_GREEN,
        COLOR_MAGENTA, COLOR_RED, COLOR_BLUE, COLOR_BLACK,
    };
    const int n = sizeof(bars) / sizeof(bars[0]);
    for (int i = 0; i < n; i++) {
        display_fill_rect(i * LCD_H_RES / n, 0, (i + 1) * LCD_H_RES / n, LCD_V_RES, bars[i]);
    }
    esp_err_t err = display_flush();
    ESP_LOGI(TAG, "Test pattern: color bars (white yellow cyan green magenta red blue black) -> %s",
             esp_err_to_name(err));
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_LOGI(TAG, "Test pattern done");
}

void display_fill_screen(uint16_t color)
{
    if (!s_fb) return;
    for (int i = 0; i < LCD_H_RES * LCD_V_RES; i++) {
        s_fb[i] = color;
    }
    mark_dirty(0, 0, LCD_H_RES - 1, LCD_V_RES - 1);
}

void display_draw_pixel(int x, int y, uint16_t color)
{
    if (!s_fb) return;
    if (x < 0 || y < 0 || x >= LCD_H_RES || y >= LCD_V_RES) return;
    s_fb[y * LCD_H_RES + x] = color;
    mark_dirty(x, y, x, y);
}

void display_draw_circle(int cx, int cy, int radius, uint16_t color)
{
    if (!s_fb) return;
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
    if (!s_panel || !s_fb) {
        return ESP_ERR_INVALID_STATE;
    }
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
            ESP_LOGE(TAG, "draw_bitmap failed at rows %d-%d: %s", y, y + rows - 1, esp_err_to_name(err));
            diag_frame_flushed(false);
            return err;
        }
        // Wait for the DMA transfer before reusing the bounce buffer.
        if (xSemaphoreTake(s_trans_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGE(TAG, "SPI color transfer timeout (>1 s) at rows %d-%d - DMA done callback never fired",
                     y, y + rows - 1);
            diag_frame_flushed(false);
            return ESP_ERR_TIMEOUT;
        }
    }

    s_dirty_x0 = LCD_H_RES;
    s_dirty_y0 = LCD_V_RES;
    s_dirty_x1 = -1;
    s_dirty_y1 = -1;
    diag_frame_flushed(true);
    return ESP_OK;
}
