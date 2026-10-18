#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "mypi_kbd.h"
#include "mypi_ui/lvgl_view.h"

static unsigned flushes;

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    assert(area->x1 >= 0 && area->y1 >= 0);
    assert(pixels != NULL);
    flushes++;
    lv_display_flush_ready(display);
}

static void test_view(int width, int height)
{
    static uint16_t pixels[480 * 20];
    lv_display_t *display = lv_display_create(width, height);
    assert(display);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, pixels, NULL, sizeof(pixels), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    mypi_ui_lvgl_view_t view = {0};
    mypi_ui_lvgl_view_init(&view, lv_display_get_screen_active(display));

    // Preserve UTF-8 bytes, including combining characters and glyphs absent from the font.
    const char text[] = "MyPi Åäö μ/π é 漢字 🙂 100%";
    const mypi_ui_status_line_t lines[] = {{text, 0xffffff}, {"Connected", 0x00ff00}};
    mypi_ui_status_t state = {.lines = lines, .count = 2};
    assert(mypi_ui_lvgl_view_update(&view, &state));
    assert(strcmp(lv_label_get_text(view.labels[0]), text) == 0);
    assert(strcmp(lv_label_get_text(view.labels[1]), "Connected") == 0);
    lv_obj_update_layout(view.root);
    assert(lv_obj_get_width(view.labels[0]) == width);
    assert(lv_obj_get_y(view.labels[1]) == height / 2);

    unsigned previous_flushes = flushes;
    lv_refr_now(display);
    assert(flushes > previous_flushes);

    // LVGL must own a copy, not retain a pointer into a backend stack snapshot.
    char temporary[] = "Nätverk";
    const mypi_ui_status_line_t borrowed = {temporary, 0xffffff};
    state = (mypi_ui_status_t){.lines = &borrowed, .count = 1};
    assert(mypi_ui_lvgl_view_update(&view, &state));
    temporary[0] = 'X';
    assert(strcmp(lv_label_get_text(view.labels[0]), "Nätverk") == 0);
    assert(lv_obj_has_flag(view.labels[1], LV_OBJ_FLAG_HIDDEN));

    assert(!mypi_ui_lvgl_view_update(NULL, &state));
    assert(!mypi_ui_lvgl_view_update(&view, NULL));
    state.count = MYPI_UI_STATUS_LINES + 1;
    assert(!mypi_ui_lvgl_view_update(&view, &state));
    state = (mypi_ui_status_t){.lines = NULL, .count = 1};
    assert(!mypi_ui_lvgl_view_update(&view, &state));
    const mypi_ui_status_line_t invalid[] = {{"Changed", 0xffffff}, {NULL, 0}};
    state = (mypi_ui_status_t){.lines = invalid, .count = 2};
    assert(!mypi_ui_lvgl_view_update(&view, &state));
    assert(strcmp(lv_label_get_text(view.labels[0]), "Nätverk") == 0);

    mypi_ui_status_line_t full[MYPI_UI_STATUS_LINES];
    for (size_t i = 0; i < MYPI_UI_STATUS_LINES; i++) full[i] = lines[0];
    state = (mypi_ui_status_t){.lines = full, .count = MYPI_UI_STATUS_LINES};
    assert(mypi_ui_lvgl_view_update(&view, &state));
    assert(!lv_obj_has_flag(view.labels[MYPI_UI_STATUS_LINES - 1], LV_OBJ_FLAG_HIDDEN));

    state = (mypi_ui_status_t){0};
    assert(mypi_ui_lvgl_view_update(&view, &state));
    for (size_t i = 0; i < MYPI_UI_STATUS_LINES; i++) {
        assert(lv_obj_has_flag(view.labels[i], LV_OBJ_FLAG_HIDDEN));
    }
    lv_display_delete(display);
}

LV_FONT_DECLARE(mypi_font_14);

// The committed MyPi status font must cover Swedish letters and the μ/π identity glyphs.
static void test_font_coverage(void)
{
    static const uint32_t required[] = {'A', 'z', '0', '/', 0xB5, 0xC5, 0xC4, 0xD6, 0xE5, 0xE4, 0xF6,
                                        0x3BC, 0x3C0};
    lv_font_glyph_dsc_t dsc;
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
        assert(lv_font_get_glyph_dsc(&mypi_font_14, &dsc, required[i], 0));
        assert(dsc.adv_w > 0);
    }
    assert(!lv_font_get_glyph_dsc(&mypi_font_14, &dsc, 0x4E2D, 0));   // CJK not in the subset
    assert(lv_font_get_line_height(&mypi_font_14) <= 200 / 10);       // 10 status rows fit
}

static char kbd_last[128];
static int kbd_calls, kbd_confirmed, kbd_deleted;

static void kbd_cb(const char *text, bool confirmed, void *user)
{
    assert(user == &kbd_calls);
    snprintf(kbd_last, sizeof(kbd_last), "%s", text);
    kbd_calls++;
    kbd_confirmed = confirmed;
}

static void kbd_delete_cb(lv_event_t *e)
{
    (void)e;
    kbd_deleted++;
}

static void click(lv_obj_t *kbd, int button)    // 0 BACK, 1 DEL, 2 NEXT, 3 OK (last 4 children)
{
    lv_obj_send_event(lv_obj_get_child(kbd, button - 4), LV_EVENT_CLICKED, NULL);
}

// The keyboard module scales to any placement/size and drives the callback via its 4 buttons.
static void test_kbd(int width, int height, int x, int y, int w, int h)
{
    static uint16_t pixels[480 * 20];
    lv_display_t *display = lv_display_create(width, height);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, pixels, NULL, sizeof(pixels), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_obj_t *screen = lv_display_get_screen_active(display);

    kbd_calls = kbd_deleted = 0;
    lv_obj_t *kbd = mypi_kbd_create(screen, x, y, w, h, (mypi_kbd_style_t)7 /* unknown -> WHEEL */,
                                    "Password: Nätverk", "ab\xC3\xA5", 6, true, kbd_cb, &kbd_calls);
    assert(kbd);
    lv_obj_add_event_cb(kbd, kbd_delete_cb, LV_EVENT_DELETE, NULL);
    lv_obj_update_layout(kbd);
    assert(lv_obj_get_x(kbd) == x && lv_obj_get_y(kbd) == y);
    assert(lv_obj_get_width(kbd) == w && lv_obj_get_height(kbd) == h);
    // Every child stays inside the requested area.
    for (uint32_t i = 0; i < lv_obj_get_child_count(kbd); i++) {
        lv_obj_t *c = lv_obj_get_child(kbd, (int32_t)i);
        assert(lv_obj_get_x(c) >= 0 && lv_obj_get_y(c) >= 0);
        assert(lv_obj_get_x(c) + lv_obj_get_width(c) <= w && lv_obj_get_y(c) + lv_obj_get_height(c) <= h);
    }
    assert(strcmp(mypi_kbd_get_text(kbd), "ab\xC3\xA5") == 0);
    unsigned previous_flushes = flushes;
    lv_refr_now(display);
    assert(flushes > previous_flushes);

    click(kbd, 2);                                   // NEXT: 'a' (first char of "az")
    assert(strcmp(mypi_kbd_get_text(kbd), "ab\xC3\xA5" "a") == 0);
    click(kbd, 2);                                   // 6 bytes = max_len
    click(kbd, 2);                                   // Would exceed max_len 6 -> rejected
    assert(strlen(mypi_kbd_get_text(kbd)) == 6);
    click(kbd, 1);
    click(kbd, 1);
    click(kbd, 1);                                   // DEL removes 'a', 'a', then the 2-byte 'å'
    assert(strcmp(mypi_kbd_get_text(kbd), "ab") == 0);
    click(kbd, 3);                                   // OK: callback, keyboard stays open
    assert(kbd_calls == 1 && kbd_confirmed && strcmp(kbd_last, "ab") == 0);
    mypi_kbd_set_hint(kbd, "Too short");
    lv_refr_now(display);
    click(kbd, 0);                                   // BACK: cancel callback, then self-delete
    assert(kbd_calls == 2 && !kbd_confirmed);
    assert(mypi_kbd_get_text(kbd) == NULL);          // Closing: text already wiped
    mypi_kbd_delete(kbd);                            // Idempotent
    for (int i = 0; i < 5 && !kbd_deleted; i++) {
        lv_tick_inc(40);
        lv_timer_handler();
    }
    assert(kbd_deleted == 1);
    assert(mypi_kbd_create(screen, 0, 0, 20, 20, MYPI_KBD_STYLE_WHEEL, "", "", 8, false, NULL, NULL) == NULL);
    lv_display_delete(display);
}

static lv_point_t touch_point;
static bool touch_pressed;

static void touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point = touch_point;
    data->state = touch_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void touch_at(int32_t x, int32_t y, bool pressed)
{
    touch_point.x = x;
    touch_point.y = y;
    touch_pressed = pressed;
    lv_tick_inc(40);
    lv_timer_handler();
}

// Drag from (x,y) by (dx,dy) in small steps, like a finger on the panel.
static void drag(int32_t x, int32_t y, int32_t dx, int32_t dy)
{
    touch_at(x, y, true);
    for (int i = 1; i <= 8; i++) touch_at(x + dx * i / 8, y + dy * i / 8, true);
    touch_at(x + dx, y + dy, false);
}

static void tap_obj(lv_obj_t *obj, int32_t fy_num, int32_t fy_den)
{
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    int32_t x = (a.x1 + a.x2) / 2, y = a.y1 + (a.y2 - a.y1) * fy_num / fy_den;
    touch_at(x, y, true);
    touch_at(x, y, true);
    touch_at(x, y, false);
}

static const char *last_char(lv_obj_t *kbd)
{
    const char *t = mypi_kbd_get_text(kbd);
    return t + strlen(t) - 1;
}

// Real pointer input: vertical drag steps characters, horizontal swipe changes range,
// taps on the outer rows and on the tabs.
static void test_kbd_gestures(void)
{
    static uint16_t pixels[240 * 20];
    lv_display_t *display = lv_display_create(240, 320);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, pixels, NULL, sizeof(pixels), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read);
    lv_indev_set_display(indev, display);
    lv_obj_t *kbd = mypi_kbd_create(lv_display_get_screen_active(display), 0, 0, 240, 320,
                                    MYPI_KBD_STYLE_WHEEL, "SSID", "", 63, false, kbd_cb, &kbd_calls);
    assert(kbd);
    lv_obj_update_layout(kbd);
    lv_obj_t *wheel = lv_obj_get_child(kbd, -5);
    lv_area_t a;
    lv_obj_get_coords(wheel, &a);
    int32_t cx = (a.x1 + a.x2) / 2, cy = (a.y1 + a.y2) / 2;
    int32_t small = lv_obj_get_height(wheel) * 20 / 112;

    drag(cx, cy - small, 0, 3 * (small / 2) + 2);    // Finger down 3 half-rows: a -> x
    click(kbd, 2);
    assert(*last_char(kbd) == 'x');
    drag(cx + 60, cy, -100, 0);                     // Swipe left: next range (AZ)
    click(kbd, 2);
    assert(*last_char(kbd) == 'A');
    tap_obj(wheel, 7, 10);                          // Tap row +1: A -> B
    click(kbd, 2);
    assert(*last_char(kbd) == 'B');
    tap_obj(wheel, 1, 20);                          // Tap row -2: B -> Z
    click(kbd, 2);
    assert(*last_char(kbd) == 'Z');
    tap_obj(lv_obj_get_child(kbd, 3), 1, 2);        // Tab "09"
    click(kbd, 2);
    assert(*last_char(kbd) == '0');
    tap_obj(lv_obj_get_child(kbd, -1), 1, 2);       // OK button via the pointer
    assert(kbd_confirmed && strcmp(kbd_last, "xABZ0") == 0);
    lv_refr_now(display);
    lv_indev_delete(indev);
    lv_display_delete(display);
}

int main(void)
{
    lv_init();
    test_font_coverage();
    test_view(240, 200);
    test_view(480, 320);
    test_kbd(240, 320, 0, 0, 240, 320);   // Hotspot 2" panel, full screen
    test_kbd(240, 320, 0, 32, 240, 288);  // Below a title bar
    test_kbd(480, 800, 40, 100, 400, 500);
    test_kbd_gestures();
    lv_deinit();
    puts("MyPi LVGL view tests passed");
    return 0;
}
