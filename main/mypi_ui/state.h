#pragma once

#include <stddef.h>
#include <stdint.h>

#define MYPI_UI_STATUS_LINES 12

// Borrowed, NUL-terminated UTF-8 strings, valid until the render call returns.
// Product adapters supply public status only, never credentials.
typedef struct {
    const char *text;
    uint32_t color_rgb;
} mypi_ui_status_line_t;

typedef struct {
    const mypi_ui_status_line_t *lines;
    size_t count;
} mypi_ui_status_t;
