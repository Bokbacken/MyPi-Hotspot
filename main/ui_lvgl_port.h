#pragma once

#include <stdbool.h>
#include "mypi_kbd.h"
#include "mypi_ui/state.h"

// Status viewport below the brand header; branding, menus and OTA/rescue stay with display_ui.
#define MYPI_LVGL_STATUS_Y 32
#define MYPI_LVGL_STATUS_HEIGHT 200

// All functions below must only be called from the display UI task (LVGL is single-threaded).
bool ui_lvgl_port_init(void);
// Caller holds display_lock(); pixels are copied synchronously into the framebuffer.
bool ui_lvgl_port_render(const mypi_ui_status_t *state);
// Runs LVGL timers (pointer indev polling). Never draws: only render() may touch the framebuffer.
void ui_lvgl_port_poll(void);
void ui_lvgl_port_deinit(void);

// Full-screen scroll-wheel keyboard (mypi_kbd) on its own LVGL display. While it is open the
// pointer indev targets it and ui_lvgl_port_kbd_tick() (not poll) runs LVGL and draws it.
// The callback runs on the UI task inside kbd_tick(); BACK closes the keyboard by itself,
// after OK call ui_lvgl_port_kbd_close() (or ui_lvgl_port_kbd_set_hint() to keep it open).
bool ui_lvgl_port_kbd_open(const char *label, const char *initial, size_t max_len, bool mask,
                           mypi_kbd_cb_t cb, void *user);
void ui_lvgl_port_kbd_close(void);
bool ui_lvgl_port_kbd_active(void);
void ui_lvgl_port_kbd_set_hint(const char *message);
// Takes display_lock() itself; call every ~20 ms while the keyboard is active.
void ui_lvgl_port_kbd_tick(void);

// Thread-safe: called by the touch owner (main loop) with rotation-corrected screen coordinates.
void ui_lvgl_port_touch_feed(bool pressed, int x, int y);
