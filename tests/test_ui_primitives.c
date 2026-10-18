#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ui_input.h"
#include "ui_layout.h"
#include "ui_serial.h"

static void test_frame(uint8_t byte)
{
    uint8_t bits[MYPI_UART_BITS_PER_BYTE];
    mypi_uart_frame(byte, bits);
    assert(bits[0] == 0);
    for (unsigned i = 0; i < 8; i++) assert(bits[i + 1] == ((byte >> i) & 1u));
    assert(bits[9] == 1);
}

static void test_serial(void)
{
    static const uint8_t h_expected[] = {0, 0, 0, 0, 1, 0, 0, 1, 0, 1};
    uint8_t bits[80];
    assert(mypi_uart_frame_text("H", bits, sizeof(bits)) == sizeof(h_expected));
    assert(memcmp(bits, h_expected, sizeof(h_expected)) == 0);
    test_frame('H');

    static const char text[] = "Hotspot";
    assert(mypi_uart_frame_text(text, bits, sizeof(bits)) == 70);
    for (size_t i = 0; i < sizeof(text) - 1; i++) {
        test_frame((uint8_t)text[i]);
        uint8_t expected[MYPI_UART_BITS_PER_BYTE];
        mypi_uart_frame((uint8_t)text[i], expected);
        assert(memcmp(bits + i * MYPI_UART_BITS_PER_BYTE, expected, sizeof(expected)) == 0);
    }
    assert(mypi_uart_frame_text("Hotspot", bits, 69) == 60);
    assert(mypi_uart_frame_text("H", bits, 9) == 0);
}

static void test_waveform_geometry(void)
{
    uint8_t bits[MYPI_UART_BITS_PER_BYTE];
    mypi_uart_frame('H', bits);
    const int origin_x = 11, bit_width = 7, high_y = 12, low_y = 23;
    assert(mypi_uart_x(origin_x, 0, bit_width) == 11);
    assert(mypi_uart_x(origin_x, 10, bit_width) == 81);
    for (size_t i = 0; i < MYPI_UART_BITS_PER_BYTE; i++) {
        int x0 = mypi_uart_x(origin_x, i, bit_width);
        int x1 = mypi_uart_x(origin_x, i + 1, bit_width);
        assert(x1 - x0 == bit_width);
        assert(mypi_uart_y(high_y, low_y, bits[i]) == (bits[i] ? high_y : low_y));
    }
}

static void test_full_width_divider(void)
{
    // LCD divider: the whole word "Hotspot" (70 bits) spread over the 240 px screen width.
    uint8_t bits[8 * MYPI_UART_BITS_PER_BYTE];
    size_t total = mypi_uart_frame_text("Hotspot", bits, sizeof(bits));
    assert(total == 70);
    assert(mypi_uart_x_span(0, 0, total, 240) == 0);
    assert(mypi_uart_x_span(0, total, total, 240) == 240);
    assert(mypi_uart_x_span(5, total, total, 240) == 245);
    for (size_t i = 0; i < total; i++) {
        int w = mypi_uart_x_span(0, i + 1, total, 240) - mypi_uart_x_span(0, i, total, 240);
        assert(w == 3 || w == 4);   // 240/70 = 3.43 px per bit, no gaps or overlaps
    }
    assert(mypi_uart_x_span(7, 3, 0, 240) == 7);
}

static void test_rssi_color_steps(void)
{
    assert(mypi_rssi_color_step(-95) == 0);
    assert(mypi_rssi_color_step(-80) == 0);
    assert(mypi_rssi_color_step(-40) == MYPI_RSSI_COLOR_STEPS - 1);
    assert(mypi_rssi_color_step(-20) == MYPI_RSSI_COLOR_STEPS - 1);
    int seen[MYPI_RSSI_COLOR_STEPS] = {0};
    int previous = 0;
    for (int rssi = -80; rssi <= -40; rssi++) {
        int step = mypi_rssi_color_step(rssi);
        assert(step >= previous && step - previous <= 1);
        seen[step] = 1;
        previous = step;
    }
    for (int i = 0; i < MYPI_RSSI_COLOR_STEPS; i++) assert(seen[i]);
}

static void test_password_utf8_and_length(void)
{
    char password[64] = "";
    assert(mypi_wheel_codepoint(0) == 0x20);
    assert(mypi_wheel_codepoint(MYPI_WHEEL_ASCII_COUNT - 1) == 0x7E);
    assert(mypi_wheel_codepoint(MYPI_WHEEL_ASCII_COUNT) == 0xA0);
    assert(mypi_wheel_codepoint(MYPI_WHEEL_COUNT - 1) == 0x24F);
    assert(mypi_wheel_codepoint(MYPI_WHEEL_COUNT) == 0);
    assert(mypi_wheel_step(0, -1) == MYPI_WHEEL_COUNT - 1);
    assert(mypi_wheel_step(MYPI_WHEEL_COUNT - 1, 1) == 0);
    assert(mypi_wheel_range_count(0) == MYPI_WHEEL_ASCII_COUNT);
    assert(mypi_wheel_range_count(1) == MYPI_WHEEL_LATIN_COUNT);
    assert(mypi_wheel_range_codepoint(1, 0) == 0xA0);
    assert(mypi_wheel_range_codepoint(1, MYPI_WHEEL_LATIN_COUNT - 1) == 0x24F);
    assert(mypi_wheel_step_range(1, 0, -1) == MYPI_WHEEL_LATIN_COUNT - 1);
    assert(mypi_wheel_step_range(0, MYPI_WHEEL_ASCII_COUNT - 1, 1) == 0);
    assert(mypi_wifi_pass_length_valid(password));
    assert(mypi_utf8_append(password, sizeof(password), 0x00E5));
    assert(strcmp(password, "\xC3\xA5") == 0);
    assert(mypi_utf8_backspace(password) && password[0] == '\0');
    for (unsigned i = 0; i < 8; i++) assert(mypi_utf8_append(password, sizeof(password), 'a'));
    assert(mypi_wifi_pass_length_valid(password));
    assert(!mypi_utf8_append(password, sizeof(password), 0xD800));
    assert(!mypi_utf8_append(password, 9, 'b'));
    assert(!mypi_wifi_pass_length_valid("short"));
    assert(mypi_wifi_pass_length_valid(""));
    assert(!mypi_wifi_pass_length_valid(
        "0123456789012345678901234567890123456789012345678901234567890123"));

    char max_password[64] = "";
    for (int i = 0; i < 61; i++) assert(mypi_utf8_append(max_password, sizeof(max_password), 'a'));
    assert(mypi_utf8_append(max_password, sizeof(max_password), 0x00E5));
    assert(strlen(max_password) == 63 && mypi_wifi_pass_length_valid(max_password));
    assert(!mypi_utf8_append(max_password, sizeof(max_password), 0x00E5));
    assert(mypi_utf8_backspace(max_password) && strlen(max_password) == 61);
}

static void test_touch_geometry(void)
{
    assert(mypi_ui_row_hit(40, 40, 34, 3, 6) == 0);
    assert(mypi_ui_row_hit(73, 40, 34, 3, 6) == 0);
    assert(mypi_ui_row_hit(74, 40, 34, 3, 6) == -1);
    assert(mypi_ui_row_hit(77, 40, 34, 3, 6) == 1);
    assert(mypi_ui_row_hit(258, 40, 34, 3, 6) == 5);
    assert(mypi_ui_row_hit(262, 40, 34, 3, 6) == -1);
    assert(mypi_ui_button_hit(8, 268, 8, 268, 231, 315));
    assert(!mypi_ui_button_hit(7, 268, 8, 268, 231, 315));
    assert(!mypi_ui_button_hit(231, 316, 8, 268, 231, 315));
    // Touch coordinates are normalized by touch_read() before menu hit-testing.
    assert(mypi_ui_button_hit(239 - 8, 319 - 268, 8, 319 - 315, 231, 319 - 268));
}

int main(void)
{
    test_serial();
    test_waveform_geometry();
    test_full_width_divider();
    test_rssi_color_steps();
    test_password_utf8_and_length();
    test_touch_geometry();
    puts("UI primitive tests passed");
    return 0;
}
