#pragma once

#define MYPI_UI_BG          0x0000
#define MYPI_UI_TEXT        0xFFFF
#define MYPI_UI_MUTED       0x8410
#define MYPI_UI_ORANGE      0xFD20
#define MYPI_UI_GREEN       0x07E0
#define MYPI_UI_RED         0xF800
#define MYPI_UI_CYAN        0x07FF
#define MYPI_UI_YELLOW      0xFFE0

#define MYPI_UI_MENU_X0     8
#define MYPI_UI_MENU_X1     231
#define MYPI_UI_MENU_Y0     40
#define MYPI_UI_MENU_ROW_H  30      // v0.10.0: 34 -> 30 so 7 rows end above the HOME button (y 280)
#define MYPI_UI_MENU_GAP    3
#define MYPI_UI_MENU_ITEMS  7

// RSSI color gradient: 20 linear steps from red (step 0, <= -80 dBm) to green (step 19, >= -40 dBm).
#define MYPI_RSSI_COLOR_STEPS 20
#define MYPI_RSSI_RED_DBM     (-80)
#define MYPI_RSSI_GREEN_DBM   (-40)

static inline int mypi_rssi_color_step(int rssi)
{
    int step = (rssi - MYPI_RSSI_RED_DBM) * (MYPI_RSSI_COLOR_STEPS - 1) /
               (MYPI_RSSI_GREEN_DBM - MYPI_RSSI_RED_DBM);
    return step < 0 ? 0 : (step > MYPI_RSSI_COLOR_STEPS - 1 ? MYPI_RSSI_COLOR_STEPS - 1 : step);
}

static inline int mypi_ui_row_hit(int y, int y0, int height, int gap, int count)
{
    if (y < y0 || height <= 0 || gap < 0 || count <= 0) return -1;
    int stride = height + gap;
    int row = (y - y0) / stride;
    return row < count && (y - y0) % stride < height ? row : -1;
}

static inline int mypi_ui_button_hit(int x, int y, int x0, int y0, int x1, int y1)
{
    return x >= x0 && x <= x1 && y >= y0 && y <= y1;
}
