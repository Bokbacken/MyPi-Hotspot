#pragma once

// Text rendering helpers and the LCD status screen (uplink, AP, Ethernet,
// throughput, traffic log), redrawn every 2 s by a background task.

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define COL_BLACK   0x0000
#define COL_WHITE   0xFFFF
#define COL_GREEN   0x07E0
#define COL_RED     0xF800
#define COL_YELLOW  0xFFE0
#define COL_CYAN    0x07FF
#define COL_ORANGE  0xFD20
#define COL_GRAY    0x8410

// Extra glyphs understood by display_draw_char() / display_draw_str().
#define DISPLAY_CH_UP   '\x18'
#define DISPLAY_CH_DOWN '\x19'

/** Draw one 5x7 character in a 6x8 cell (times scale) at top-left (x, y). */
void display_draw_char(int x, int y, char ch, uint16_t fg, uint16_t bg, int scale);
/** Draw a string (6 px per character times scale). Returns the x after the last character. */
int display_draw_str(int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale);
/** 1 px rectangle outline, corners inclusive. */
void display_draw_rect_outline(int x0, int y0, int x1, int y1, uint16_t color);
/** Wi-Fi symbol: three arcs above a dot at (cx, cy) - about 21x12 px. */
void display_draw_wifi_icon(int cx, int cy, uint16_t color);

/** Start the status screen task (call after display_init() succeeded). */
esp_err_t display_ui_init(void);
/** Stop the status screen task (the last frame stays on the panel). */
void display_ui_stop(void);
/** Route a completed touch/drag to the Wi-Fi setup wheel, if it owns the screen. */
bool display_ui_touch(int x0, int y0, int x1, int y1, bool dragged);
/** Emergency mode screen: option index (RESCUE_OPT_*) at touch point (x, y), or -1. */
int display_ui_rescue_hit(int x, int y);
