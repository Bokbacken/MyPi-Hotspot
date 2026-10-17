#include "ui_lvgl_port.h"

#include "display.h"
#include "esp_timer.h"
#include "mypi_ui/lvgl_view.h"

static lv_display_t *s_display;
static mypi_ui_lvgl_view_t s_view;
_Alignas(LV_DRAW_BUF_ALIGN) static uint16_t s_pixels[LCD_H_RES * 20];

static uint32_t tick_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    uint32_t stride = lv_draw_buf_width_to_stride(lv_area_get_width(area), LV_COLOR_FORMAT_RGB565);
    for (int y = area->y1; y <= area->y2; y++) {
        const uint16_t *src = (const uint16_t *)(pixels + (y - area->y1) * stride);
        for (int x = area->x1; x <= area->x2; x++) {
            display_draw_pixel(x, y + MYPI_LVGL_STATUS_Y, *src++);
        }
    }
    // The existing display_flush() owns SPI/DMA lifetime and RGB565 wire byte order.
    lv_display_flush_ready(display);
}

bool ui_lvgl_port_init(void)
{
    if (s_display) return true;
    lv_init();
    lv_tick_set_cb(tick_ms);
    s_display = lv_display_create(LCD_H_RES, MYPI_LVGL_STATUS_HEIGHT);
    if (!s_display) {
        lv_deinit();
        return false;
    }
    lv_display_set_color_format(s_display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_display, s_pixels, NULL, sizeof(s_pixels), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_display, flush);
    mypi_ui_lvgl_view_init(&s_view, lv_display_get_screen_active(s_display));
    return true;
}

bool ui_lvgl_port_render(const mypi_ui_status_t *state)
{
    if (!s_display || !mypi_ui_lvgl_view_update(&s_view, state)) return false;
    // Legacy screens may have overwritten the viewport since the previous frame.
    lv_obj_invalidate(lv_display_get_screen_active(s_display));
    lv_refr_now(s_display);
    return true;
}

void ui_lvgl_port_deinit(void)
{
    if (!s_display) return;
    lv_display_delete(s_display);
    s_display = NULL;
    s_view.root = NULL;
    lv_deinit();
}
