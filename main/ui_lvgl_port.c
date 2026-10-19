#include "ui_lvgl_port.h"

#include <string.h>

#include "display.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "mypi_kbd.h"
#include "mypi_ui/lvgl_view.h"
// lv_image_cache_drop(): not in lvgl.h (LVGL 9.4 exposes it via lvgl_private.h).
#include "src/misc/cache/lv_cache.h"

#if LV_TXT_ENC != LV_TXT_ENC_UTF8 || !LV_USE_LABEL
#error "MyPi LVGL UI requires UTF-8 text encoding and the label widget"
#endif

LV_FONT_DECLARE(mypi_font_14);

static lv_display_t *s_display;
static lv_indev_t *s_indev;
static mypi_ui_lvgl_view_t s_view;
static bool s_render_active;
// Camera preview (v0.10.0): an image + overlay label in the same status viewport, shown instead of
// the status view while the camera page is open. The pixels stay owned by the caller.
static lv_obj_t *s_cam_root;
static lv_obj_t *s_cam_img;
static lv_obj_t *s_cam_label;
static lv_image_dsc_t s_cam_dsc;
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

static void camera_view_create(lv_obj_t *screen)
{
    s_cam_root = lv_obj_create(screen);
    lv_obj_remove_style_all(s_cam_root);
    lv_obj_set_size(s_cam_root, LCD_H_RES, MYPI_LVGL_STATUS_HEIGHT);
    lv_obj_set_style_bg_color(s_cam_root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_cam_root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_cam_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_cam_root, LV_OBJ_FLAG_HIDDEN);
    s_cam_img = lv_image_create(s_cam_root);
    lv_obj_center(s_cam_img);
    lv_obj_add_flag(s_cam_img, LV_OBJ_FLAG_HIDDEN);
    s_cam_label = lv_label_create(s_cam_root);
    lv_obj_set_style_text_font(s_cam_label, &mypi_font_14, 0);
    lv_obj_set_width(s_cam_label, LCD_H_RES - 16);
    lv_obj_set_style_text_align(s_cam_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_cam_label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_bg_color(s_cam_label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_cam_label, LV_OPA_70, 0);
    lv_obj_center(s_cam_label);
    lv_obj_add_flag(s_cam_label, LV_OBJ_FLAG_HIDDEN);
}

static void camera_view_show(bool on)
{
    if (!s_cam_root || !s_view.root) return;
    if (on) {
        lv_obj_add_flag(s_view.root, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_cam_root, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_cam_root, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_view.root, LV_OBJ_FLAG_HIDDEN);
    }
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
    camera_view_create(lv_display_get_screen_active(s_display));

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
    if (!s_display) return false;
    camera_view_show(false);
    if (!mypi_ui_lvgl_view_update(&s_view, state)) return false;
    // Legacy screens may have overwritten the viewport since the previous frame.
    lv_obj_invalidate(lv_display_get_screen_active(s_display));
    s_render_active = true;
    lv_refr_now(s_display);
    s_render_active = false;
    return true;
}

bool ui_lvgl_port_render_camera(const uint16_t *pixels, int width, int height, const char *overlay,
                                uint32_t overlay_rgb)
{
    if (!s_display || !s_cam_root) return false;
    camera_view_show(true);
    if (pixels && width > 0 && height > 0 && width <= LCD_H_RES && height <= MYPI_LVGL_STATUS_HEIGHT) {
        lv_image_cache_drop(&s_cam_dsc);    // Same buffer, new frame
        memset(&s_cam_dsc, 0, sizeof(s_cam_dsc));
        s_cam_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        s_cam_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        s_cam_dsc.header.w = (uint32_t)width;
        s_cam_dsc.header.h = (uint32_t)height;
        s_cam_dsc.header.stride = (uint32_t)width * 2;
        s_cam_dsc.data_size = (uint32_t)width * (uint32_t)height * 2;
        s_cam_dsc.data = (const uint8_t *)pixels;
        lv_image_set_src(s_cam_img, &s_cam_dsc);
        lv_obj_center(s_cam_img);
        lv_obj_remove_flag(s_cam_img, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_cam_img, LV_OBJ_FLAG_HIDDEN);
    }
    if (overlay && overlay[0]) {
        lv_label_set_text(s_cam_label, overlay);
        lv_obj_set_style_text_color(s_cam_label, lv_color_hex(overlay_rgb), 0);
        lv_obj_remove_flag(s_cam_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_cam_label, LV_OBJ_FLAG_HIDDEN);
    }
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
    s_cam_root = s_cam_img = s_cam_label = NULL;
    lv_deinit();
}
