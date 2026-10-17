#pragma once

#include <stdbool.h>
#include "mypi_ui/state.h"

// Transitional read-only viewport; branding and controls stay with display_ui.
#define MYPI_LVGL_STATUS_Y 32
#define MYPI_LVGL_STATUS_HEIGHT 200

bool ui_lvgl_port_init(void);
// Caller holds display_lock(); pixels are copied synchronously into the framebuffer.
bool ui_lvgl_port_render(const mypi_ui_status_t *state);
void ui_lvgl_port_deinit(void);
