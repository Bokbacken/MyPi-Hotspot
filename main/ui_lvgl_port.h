#pragma once

#include <stdbool.h>
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

// Thread-safe: called by the touch owner (main loop) with rotation-corrected screen coordinates.
void ui_lvgl_port_touch_feed(bool pressed, int x, int y);
