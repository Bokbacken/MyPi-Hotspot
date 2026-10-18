#include "ui_lvgl_port.h"

#include "display.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "mypi_kbd.h"
#include "mypi_ui/lvgl_view.h"

#if LV_TXT_ENC != LV_TXT_ENC_UTF8 || !LV_USE_LABEL
#error "MyPi LVGL UI requires UTF-8 text encoding and the label widget"
#endif

LV_FONT_DECLARE(mypi_font_14);

static lv_display_t *s_display;
static lv_indev_t *s_indev;
static mypi_ui_lvgl_view_t s_view;
static bool s_render_active;
// Partial draw buffer (LVGL flush buffer) in INTERNAL RAM (v0.9.0, RAM strategy: time-critical): LVGL
// renders here, flush() copies into the PSRAM framebuffer, which display.c pushes over SPI via
// internal DMA bounce buffers. 9.6 KB.
_Alignas(LV_DRAW_BUF_ALIGN) static uint16_t s_pixels[LCD_H_RES * 20];

// Full-screen keyboard host (v0.8.7): a second LVGL display covering the whole panel, created on
// first use. Keeping it separate leaves the status viewport (and its y offset) untouched.
static lv_display_t *s_kbd_display;
static lv_obj_t *s_kbd;
static bool s_kbd_active;
static bool s_kbd_close_req;
static bool s_kbd_drawing;
static bool s_kbd_dirty;
static bool s_in_kbd_tick;
// Keyboard flush buffer: internal RAM as well (v0.9.0), 9.6 KB.
_Alignas(LV_DRAW_BUF_ALIGN) static uint16_t s_kbd_pixels[LCD_H_RES * 20];

// Pointer mapping of the display the indev is attached to (UI task only).
static int s_indev_y0 = MYPI_LVGL_STATUS_Y;
static int s_indev_h = MYPI_LVGL_STATUS_HEIGHT;

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
    int y = s_touch.y - s_indev_y0;
    portEXIT_CRITICAL(&s_touch_mux);
    // Points outside the viewport (header, menu button) are reported as released.
    bool inside = x >= 0 && x < LCD_H_RES && y >= 0 && y < s_indev_h;
    data->point.x = x < 0 ? 0 : (x >= LCD_H_RES ? LCD_H_RES - 1 : x);
    data->point.y = y < 0 ? 0 : (y >= s_indev_h ? s_indev_h - 1 : y);
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
    // While the keyboard is open only ui_lvgl_port_kbd_tick() may run LVGL, so its
    // invalidations are always rendered instead of being dropped here.
    if (s_display && !s_kbd_active) lv_timer_handler();
}

static void kbd_flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    if (s_kbd_drawing) {
        uint32_t stride = lv_draw_buf_width_to_stride(lv_area_get_width(area), LV_COLOR_FORMAT_RGB565);
        for (int y = area->y1; y <= area->y2; y++) {
            const uint16_t *src = (const uint16_t *)(pixels + (y - area->y1) * stride);
            for (int x = area->x1; x <= area->x2; x++) display_draw_pixel(x, y, *src++);
        }
        s_kbd_dirty = true;
    }
    lv_display_flush_ready(display);
}

static void kbd_deleted(lv_event_t *e)
{
    (void)e;
    s_kbd = NULL;
}

static void indev_attach(lv_display_t *display, int y0, int height)
{
    if (!s_indev) return;
    lv_indev_reset(s_indev, NULL);
    lv_indev_set_display(s_indev, display);
    s_indev_y0 = y0;
    s_indev_h = height;
}

static void kbd_finish_close(void)
{
    if (s_kbd) mypi_kbd_delete(s_kbd);     // Async; executed by the next lv_timer_handler()
    s_kbd_active = false;
    s_kbd_close_req = false;
    indev_attach(s_display, MYPI_LVGL_STATUS_Y, MYPI_LVGL_STATUS_HEIGHT);
}

bool ui_lvgl_port_kbd_open(const char *label, const char *initial, size_t max_len, bool mask,
                           mypi_kbd_cb_t cb, void *user)
{
    if (!s_display) return false;
    if (s_kbd_active) kbd_finish_close();
    if (!s_kbd_display) {
        s_kbd_display = lv_display_create(LCD_H_RES, LCD_V_RES);
        lv_display_set_default(s_display);
        if (!s_kbd_display) return false;
        lv_display_set_color_format(s_kbd_display, LV_COLOR_FORMAT_RGB565);
        lv_display_set_buffers(s_kbd_display, s_kbd_pixels, NULL, sizeof(s_kbd_pixels),
                               LV_DISPLAY_RENDER_MODE_PARTIAL);
        lv_display_set_flush_cb(s_kbd_display, kbd_flush);
        lv_obj_t *screen = lv_display_get_screen_active(s_kbd_display);
        lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    }
    lv_obj_t *screen = lv_display_get_screen_active(s_kbd_display);
    s_kbd = mypi_kbd_create(screen, 0, 0, LCD_H_RES, LCD_V_RES, MYPI_KBD_STYLE_WHEEL, label, initial,
                            max_len, mask, cb, user);
    if (!s_kbd) return false;
    lv_obj_add_event_cb(s_kbd, kbd_deleted, LV_EVENT_DELETE, NULL);
    indev_attach(s_kbd_display, 0, LCD_V_RES);
    lv_obj_invalidate(screen);
    s_kbd_active = true;
    s_kbd_close_req = false;
    return true;
}

void ui_lvgl_port_kbd_close(void)
{
    if (!s_kbd_active) return;
    if (s_in_kbd_tick) {
        // Called from a keyboard callback: the indev is mid-event, switch after the handler.
        if (s_kbd) mypi_kbd_delete(s_kbd);
        s_kbd_close_req = true;
    } else {
        kbd_finish_close();
    }
}

bool ui_lvgl_port_kbd_active(void)
{
    return s_kbd_active;
}

void ui_lvgl_port_kbd_set_hint(const char *message)
{
    if (s_kbd) mypi_kbd_set_hint(s_kbd, message);
}

void ui_lvgl_port_kbd_tick(void)
{
    if (!s_kbd_active) return;
    display_lock();
    s_in_kbd_tick = true;
    s_kbd_drawing = true;
    lv_timer_handler();     // Indev read -> keyboard events -> invalidations
    if (s_kbd && !s_kbd_close_req) lv_refr_now(s_kbd_display);
    s_kbd_drawing = false;
    s_in_kbd_tick = false;
    if (s_kbd_dirty) display_flush();
    s_kbd_dirty = false;
    display_unlock();
    // BACK deletes the keyboard itself (s_kbd becomes NULL); OK closes via ui_lvgl_port_kbd_close().
    if (s_kbd_close_req || !s_kbd) kbd_finish_close();
}

void ui_lvgl_port_deinit(void)
{
    if (!s_display) return;
    if (s_indev) {
        lv_indev_delete(s_indev);
        s_indev = NULL;
    }
    if (s_kbd_display) {
        lv_display_delete(s_kbd_display);
        s_kbd_display = NULL;
        s_kbd = NULL;
        s_kbd_active = false;
    }
    lv_display_delete(s_display);
    s_display = NULL;
    s_view.root = NULL;
    lv_deinit();
}
