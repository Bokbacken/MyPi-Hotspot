#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mypi_kbd_core.h"
#include "opt_config_core.h"
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

// Mirrors mypi_font_14: ASCII plus å ä ö Å Ä Ö (other Latin letters are absent).
static bool fake_font(uint32_t cp, void *ctx)
{
    (void)ctx;
    return (cp >= 0x20 && cp <= 0x7E) || cp == 0xE5 || cp == 0xE4 || cp == 0xF6 ||
           cp == 0xC5 || cp == 0xC4 || cp == 0xD6;
}

static bool no_digits(uint32_t cp, void *ctx)
{
    (void)ctx;
    return cp < '0' || cp > '9';
}

static void test_kbd_core(void)
{
    mypi_kbd_core_t k;
    // Unfiltered tables.
    mypi_kbd_core_init(&k, NULL, 63, NULL, NULL);
    assert(mypi_kbd_range_count(&k, MYPI_KBD_RANGE_LOWER) == 26);
    assert(mypi_kbd_range_count(&k, MYPI_KBD_RANGE_DIGIT) == 10);
    assert(mypi_kbd_range_count(&k, MYPI_KBD_RANGE_SYMBOL) == 33);   // space + 32 punctuation
    assert(mypi_kbd_range_codepoint(&k, MYPI_KBD_RANGE_SYMBOL, 0) == ' ');
    assert(mypi_kbd_range_count(&k, MYPI_KBD_RANGE_LATIN) == MYPI_KBD_LATIN_COUNT);
    assert(mypi_kbd_range_count(&k, MYPI_KBD_RANGE_COUNT) == 0);

    // Font filter: only the six Swedish letters survive in the Latin range.
    mypi_kbd_core_init(&k, "", 63, fake_font, NULL);
    assert(k.range == MYPI_KBD_RANGE_LOWER && k.index == 0);
    assert(mypi_kbd_range_count(&k, MYPI_KBD_RANGE_LATIN) == 6);
    assert(mypi_kbd_range_codepoint(&k, MYPI_KBD_RANGE_LATIN, 0) == 0xE5);
    assert(mypi_kbd_range_codepoint(&k, MYPI_KBD_RANGE_LATIN, 5) == 0xD6);
    assert(mypi_kbd_range_codepoint(&k, MYPI_KBD_RANGE_LATIN, 6) == 0);

    // Wheel stepping wraps in both directions; the 5 visible rows wrap too.
    assert(mypi_kbd_current(&k, 0) == 'a');
    assert(mypi_kbd_current(&k, -1) == 'z' && mypi_kbd_current(&k, -2) == 'y');
    assert(mypi_kbd_current(&k, 2) == 'c');
    mypi_kbd_step(&k, -1);
    assert(mypi_kbd_current(&k, 0) == 'z');
    mypi_kbd_step(&k, 1);
    mypi_kbd_step(&k, 27);
    assert(mypi_kbd_current(&k, 0) == 'b');
    mypi_kbd_step(&k, -53);
    assert(mypi_kbd_current(&k, 0) == 'a');

    // Ranges: next/previous wrap around, set_range resets the index.
    mypi_kbd_step(&k, 3);
    assert(mypi_kbd_set_range(&k, MYPI_KBD_RANGE_UPPER) && k.index == 0 && mypi_kbd_current(&k, 0) == 'A');
    mypi_kbd_next_range(&k, -1);
    mypi_kbd_next_range(&k, -1);
    assert(k.range == MYPI_KBD_RANGE_LATIN && mypi_kbd_current(&k, 0) == 0xE5);
    mypi_kbd_next_range(&k, 1);
    assert(k.range == MYPI_KBD_RANGE_LOWER);
    assert(!mypi_kbd_set_range(&k, -1) && !mypi_kbd_set_range(&k, MYPI_KBD_RANGE_COUNT));

    // Empty ranges are skipped.
    mypi_kbd_core_init(&k, "", 63, no_digits, NULL);
    assert(!mypi_kbd_set_range(&k, MYPI_KBD_RANGE_DIGIT));
    mypi_kbd_set_range(&k, MYPI_KBD_RANGE_UPPER);
    mypi_kbd_next_range(&k, 1);
    assert(k.range == MYPI_KBD_RANGE_SYMBOL);

    // Insert/delete through the module with UTF-8 and a byte limit.
    mypi_kbd_core_init(&k, "", 5, fake_font, NULL);
    mypi_kbd_set_range(&k, MYPI_KBD_RANGE_LATIN);
    assert(mypi_kbd_insert(&k) && mypi_kbd_insert(&k));          // "åå" = 4 bytes
    assert(strcmp(k.text, "\xC3\xA5\xC3\xA5") == 0);
    assert(!mypi_kbd_insert(&k));                                // 6 bytes > max_len 5
    mypi_kbd_set_range(&k, MYPI_KBD_RANGE_DIGIT);
    assert(mypi_kbd_insert(&k) && strlen(k.text) == 5);          // 1-byte '0' still fits
    assert(!mypi_kbd_insert(&k));
    assert(mypi_kbd_char_count(k.text) == 3);
    assert(mypi_kbd_delete_char(&k) && mypi_kbd_delete_char(&k) && strcmp(k.text, "\xC3\xA5") == 0);
    assert(mypi_kbd_delete_char(&k) && k.text[0] == '\0' && !mypi_kbd_delete_char(&k));

    // Initial text is truncated on a UTF-8 boundary; max_len is clamped to the buffer.
    mypi_kbd_core_init(&k, "ab\xC3\xA5", 3, NULL, NULL);
    assert(strcmp(k.text, "ab") == 0);
    mypi_kbd_core_init(&k, "x", 1000, NULL, NULL);
    assert(k.max_len == MYPI_KBD_TEXT_CAP - 1 && strcmp(k.text, "x") == 0);
    mypi_kbd_core_init(&k, "", 63, NULL, NULL);
    for (int i = 0; i < 63; i++) assert(mypi_kbd_insert(&k));
    assert(!mypi_kbd_insert(&k) && strlen(k.text) == 63 && mypi_wifi_pass_length_valid(k.text));
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

// --- opt_config (v0.9.0): mock NVS key/value store ---
typedef struct {
    char key[8][16];
    uint8_t val[8];
    int n, commits, fail_set;
} mock_nvs_t;

static int mock_get(void *ctx, const char *key, uint8_t *out)
{
    mock_nvs_t *m = ctx;
    for (int i = 0; i < m->n; i++) {
        if (strcmp(m->key[i], key) == 0) { *out = m->val[i]; return 0; }
    }
    return -1;      // ESP_ERR_NVS_NOT_FOUND
}

static int mock_set(void *ctx, const char *key, uint8_t v)
{
    mock_nvs_t *m = ctx;
    if (m->fail_set) return -2;
    for (int i = 0; i < m->n; i++) {
        if (strcmp(m->key[i], key) == 0) { m->val[i] = v; return 0; }
    }
    assert(m->n < 8 && strlen(key) < 16);
    strcpy(m->key[m->n], key);
    m->val[m->n++] = v;
    return 0;
}

static int mock_commit(void *ctx) { ((mock_nvs_t *)ctx)->commits++; return 0; }

static void test_opt_config(void)
{
    mock_nvs_t m = {0};
    opt_store_t st = {mock_get, mock_set, mock_commit, &m};
    opt_config_t c, d;

    // Load from an empty store = defaults (40 MHz, radio TX power, 64 KB, NAPT on, INFO).
    memset(&c, 0xAA, sizeof(c));
    opt_config_load_from(&st, &c);
    assert(c.sdio_freq_mhz == 40 && c.wifi_tx_power_dbm == 0 && c.tcp_window_kb == 64 && c.napt && c.log_level == 2);
    assert(opt_config_valid(&c));
    opt_config_load_from(NULL, &d);
    assert(memcmp(&c, &d, sizeof(c)) == 0);

    // Save round-trip.
    c.sdio_freq_mhz = 50; c.wifi_tx_power_dbm = 14; c.tcp_window_kb = 128; c.napt = false; c.log_level = 3;
    assert(opt_config_save_to(&st, &c) == 0 && m.commits == 1);
    opt_config_load_from(&st, &d);
    assert(d.sdio_freq_mhz == 50 && d.wifi_tx_power_dbm == 14 && d.tcp_window_kb == 128 && !d.napt && d.log_level == 3);
    uint8_t raw;
    assert(mock_get(&m, OPT_KEY_SDIO_MHZ, &raw) == 0 && raw == 50);
    assert(mock_get(&m, OPT_KEY_NAPT, &raw) == 0 && raw == 0);

    // Invalid configs are rejected and nothing is written.
    opt_config_t bad = d;
    bad.sdio_freq_mhz = 45;
    assert(opt_config_save_to(&st, &bad) == -1 && m.commits == 1);
    bad = d; bad.tcp_window_kb = 96;   assert(!opt_config_valid(&bad));
    bad = d; bad.wifi_tx_power_dbm = 1; assert(!opt_config_valid(&bad));
    bad = d; bad.wifi_tx_power_dbm = 22; assert(!opt_config_valid(&bad));
    bad = d; bad.log_level = 4;         assert(!opt_config_valid(&bad));
    m.fail_set = 1;
    assert(opt_config_save_to(&st, &d) == -2);
    m.fail_set = 0;

    // Corrupt values in NVS fall back to the default field by field.
    mock_set(&m, OPT_KEY_SDIO_MHZ, 77);
    mock_set(&m, OPT_KEY_LOG_LVL, 9);
    opt_config_load_from(&st, &d);
    assert(d.sdio_freq_mhz == 40 && d.log_level == 2 && d.tcp_window_kb == 128 && d.wifi_tx_power_dbm == 14);

    // Reboot required only for the boot-only fields (SDIO clock, TCP window).
    opt_config_t a, s2;
    opt_config_defaults(&a);
    s2 = a; s2.napt = false; s2.log_level = 0; s2.wifi_tx_power_dbm = 8;
    assert(!opt_config_reboot_required(&a, &s2));
    s2.sdio_freq_mhz = 50;
    assert(opt_config_reboot_required(&a, &s2));
    s2 = a; s2.tcp_window_kb = 32;
    assert(opt_config_reboot_required(&a, &s2));

    // SDIO 50 MHz trial / auto-fallback decision.
    bool fb;
    assert(opt_sdio_boot_mhz(40, false, &fb) == 40 && !fb);
    assert(opt_sdio_boot_mhz(40, true, &fb) == 40 && !fb);
    assert(opt_sdio_boot_mhz(50, false, &fb) == 50 && !fb);     // trial starts
    assert(opt_sdio_boot_mhz(50, true, &fb) == 40 && fb);       // previous trial never confirmed
}

int main(void)
{
    test_serial();
    test_waveform_geometry();
    test_full_width_divider();
    test_rssi_color_steps();
    test_password_utf8_and_length();
    test_kbd_core();
    test_touch_geometry();
    test_opt_config();
    puts("UI primitive tests passed");
    return 0;
}
