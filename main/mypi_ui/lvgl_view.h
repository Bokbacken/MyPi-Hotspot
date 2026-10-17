#pragma once

#include <stdbool.h>
#include "lvgl.h"
#include "state.h"

typedef struct {
    lv_obj_t *root;
    lv_obj_t *labels[MYPI_UI_STATUS_LINES];
} mypi_ui_lvgl_view_t;

// All calls belong to the UI owner task. The parent owns the widget lifetime.
void mypi_ui_lvgl_view_init(mypi_ui_lvgl_view_t *view, lv_obj_t *parent);
bool mypi_ui_lvgl_view_update(mypi_ui_lvgl_view_t *view, const mypi_ui_status_t *state);
