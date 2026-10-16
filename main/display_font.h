#pragma once

#include <stdint.h>

// 5x7 bitmap font for printable ASCII (0x20-0x7E). Index = ch - 0x20.
// Each glyph is 5 column bytes, LSB = top row.
extern const uint8_t g_font5x7[95][5];

// Extra glyphs, drawn by display_draw_char() for DISPLAY_CH_UP / DISPLAY_CH_DOWN.
extern const uint8_t g_font_arrow_up[5];
extern const uint8_t g_font_arrow_down[5];
