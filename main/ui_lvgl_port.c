#include "ui_lvgl_port.h"

#include "display.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "mypi_ui/lvgl_view.h"

#if LV_TXT_ENC != LV_TXT_ENC_UTF8 || !LV_USE_LABEL
#error "MyPi LVGL UI requires UTF-8 text encoding and the label widget"
#endif

LV_FONT_DECLARE(mypi_font_14);

static lv_display_t *s_display;
static lv_indev_t *s_indev;
static mypi_ui_lvgl_view_t s_view;
static bool s_render_active;
_Alignas(LV_DRAW_BUF_ALIGN) static uint16_t s_pixels[LCD_H_RES * 20];

// Latest touch sample, written by the touch owner task and read by the LVGL indev on the UI task.
static portMUX_TYPE s_touch_mux = portMUX_INITIALIZER_UNLOCKED;
static struct {
    bool pressed;
    int x;
    int y;
} s_touch;

static uint32_t tick_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    // Only ui_lvgl_port_render() (display_lock held, home view active) may write pixels;
    // a refresh triggered from lv_timer_handler() must not draw over menus/OTA/rescue.
    if (s_render_active) {
        uint32_t stride = lv_draw_buf_width_to_stride(lv_area_get_width(area), LV_COLOR_FORMAT_RGB565);
        for (int y = area->y1; y <= area->y2; y++) {
            const uint16_t *src = (const uint16_t *)(pixels + (y - area->y1) * stride);
            for (int x = area->x1; x <= area->x2; x++) {
                display_draw_pixel(x, y + MYPI_LVGL_STATUS_Y, *src++);
            }
        }
    }
    // The existing display_flush() owns SPI/DMA lifetime and RGB565 wire byte order.
    lv_display_flush_ready(display);
}

void ui_lvgl_port_touch_feed(bool pressed, int x, int y)
{
    portENTER_CRITICAL(&s_touch_mux);
    s_touch.pressed = pressed;
    if (pressed) {
        s_touch.x = x;
        s_touch.y = y;
    }
    portEXIT_CRITICAL(&s_touch_mux);
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    portENTER_CRITICAL(&s_touch_mux);
    bool pressed = s_touch.pressed;
    int x = s_touch.x;
    int y = s_touch.y - MYPI_LVGL_STATUS_Y;
    portEXIT_CRITICAL(&s_touch_mux);
    // Points outside the viewport (header, menu button) are reported as released.
    bool inside = x >= 0 && x < LCD_H_RES && y >= 0 && y < MYPI_LVGL_STATUS_HEIGHT;
    data->point.x = x < 0 ? 0 : (x >= LCD_H_RES ? LCD_H_RES - 1 : x);
    data->point.y = y < 0 ? 0 : (y >= MYPI_LVGL_STATUS_HEIGHT ? MYPI_LVGL_STATUS_HEIGHT - 1 : y);
    data->state = pressed && inside ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
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
    // Status labels inherit the MyPi font (ASCII + å ä ö Å Ä Ö μ π).
    lv_obj_set_style_text_font(s_view.root, &mypi_font_14, 0);

    s_indev = lv_indev_create();
    if (s_indev) {
        lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(s_indev, touch_read_cb);
        lv_indev_set_display(s_indev, s_display);
    }
    return true;
}

bool ui_lvgl_port_render(const mypi_ui_status_t *state)
{
    if (!s_display || !mypi_ui_lvgl_view_update(&s_view, state)) return false;
    // Legacy screens may have overwritten the viewport since the previous frame.
    lv_obj_invalidate(lv_display_get_screen_active(s_display));
    s_render_active = true;
    lv_refr_now(s_display);
    s_render_active = false;
    return true;
}

void ui_lvgl_port_poll(void)
{
    if (s_display) lv_timer_handler();
}

void ui_lvgl_port_deinit(void)
{
    if (!s_display) return;
    if (s_indev) {
        lv_indev_delete(s_indev);
        s_indev = NULL;
    }
    lv_display_delete(s_display);
    s_display = NULL;
    s_view.root = NULL;
    lv_deinit();
}
