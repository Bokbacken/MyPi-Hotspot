#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "camera_core.h"
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
    // v0.10.0: the 7-row menu (with "Camera") ends above the HOME button (y 280).
    int stride = MYPI_UI_MENU_ROW_H + MYPI_UI_MENU_GAP;
    assert(MYPI_UI_MENU_Y0 + (MYPI_UI_MENU_ITEMS - 1) * stride + MYPI_UI_MENU_ROW_H <= 280);
    assert(mypi_ui_row_hit(MYPI_UI_MENU_Y0 + 6 * stride, MYPI_UI_MENU_Y0, MYPI_UI_MENU_ROW_H, MYPI_UI_MENU_GAP,
                           MYPI_UI_MENU_ITEMS) == 6);
    assert(mypi_ui_row_hit(MYPI_UI_MENU_Y0 + 7 * stride, MYPI_UI_MENU_Y0, MYPI_UI_MENU_ROW_H, MYPI_UI_MENU_GAP,
                           MYPI_UI_MENU_ITEMS) == -1);
}

static void test_camera_core(void)
{
    assert(strcmp(camera_state_name(CAMERA_STATE_NOT_DETECTED), "not_detected") == 0);
    assert(strcmp(camera_state_name(CAMERA_STATE_STREAMING), "streaming") == 0);
    assert(camera_state_usable(CAMERA_STATE_READY) && camera_state_usable(CAMERA_STATE_ERROR));
    assert(!camera_state_usable(CAMERA_STATE_DISABLED) && !camera_state_usable(CAMERA_STATE_NOT_DETECTED));
    assert(!camera_state_usable(CAMERA_STATE_STARTING));
    assert(strcmp(camera_stage_name(CAMERA_STAGE_STORAGE), "storage") == 0);
    assert(strcmp(camera_stage_name(CAMERA_STAGE_NONE), "none") == 0);

    // Photo names: fixed IMGnnnnn.JPG names only (PSRAM store keys), never a path.
    assert(camera_photo_name_ok("IMG00001.JPG"));
    assert(!camera_photo_name_ok("IMG0001.JPG") && !camera_photo_name_ok("img00001.jpg"));
    assert(!camera_photo_name_ok("IMG0000/.JPG") && !camera_photo_name_ok("../IMG001.J") && !camera_photo_name_ok(NULL));
    assert(camera_photo_seq("IMG01234.JPG") == 1234 && camera_photo_seq("L0000001.TXT") == -1);
    char name[CAMERA_PHOTO_NAME_LEN];
    assert(camera_photo_name(42, name, sizeof(name)) && strcmp(name, "IMG00042.JPG") == 0);
    assert(camera_photo_name(CAMERA_PHOTO_SEQ_MAX, name, sizeof(name)) && camera_photo_seq(name) == CAMERA_PHOTO_SEQ_MAX);
    assert(!camera_photo_name(0, name, sizeof(name)) && !camera_photo_name(CAMERA_PHOTO_SEQ_MAX + 1, name, sizeof(name)));
    assert(!camera_photo_name(1, name, CAMERA_PHOTO_NAME_LEN - 1));

    int w, h;
    camera_fit(800, 640, 240, 200, &w, &h);
    assert(w == 240 && h == 192);
    camera_fit(640, 800, 240, 200, &w, &h);
    assert(w == 160 && h == 200);
    camera_fit(0, 0, 240, 200, &w, &h);
    assert(w == 240 && h == 200);

    // 4x2 -> 2x1 nearest neighbour, source stride 5 pixels (padding never sampled).
    static const uint16_t src[] = {1, 2, 3, 4, 0xFFFF, 5, 6, 7, 8, 0xFFFF};
    uint16_t dst[2] = {0};
    camera_scale_rgb565(src, 4, 2, 5, dst, 2, 1);
    assert(dst[0] == 2 && dst[1] == 4);
    uint16_t same[8];
    camera_scale_rgb565(src, 4, 2, 5, same, 4, 2);
    assert(same[0] == 1 && same[3] == 4 && same[4] == 5 && same[7] == 8);
}

static void test_camera_settings(void)
{
    camera_settings_t s = {
        .preview_quality = 95, .photo_quality = 95, .idle_stop_s = 10,
        .max_photos = 20, .store_kb = 4096, .live_preview = true,
    };
    camera_capabilities_t caps = {.modes = {{800, 640}}, .mode_count = 1};
    assert(camera_settings_valid(&s) && camera_settings_supported(&s, &caps));
    assert(!camera_settings_valid(NULL));
    camera_settings_blob_t blob = camera_settings_pack(&s);
    camera_settings_t loaded = {0};
    assert(camera_settings_unpack(&blob, sizeof(blob), &loaded));
    assert(loaded.preview_quality == 95 && loaded.photo_quality == 95 && loaded.live_preview);
    assert(blob.version == 3);
    assert(loaded.idle_stop_s == 10 && loaded.max_photos == 20 && loaded.store_kb == 4096);
    assert(loaded.width == 0 && loaded.height == 0);
    assert(!camera_settings_unpack(&blob, sizeof(blob) - 1, &loaded));
    blob.version++;
    assert(!camera_settings_unpack(&blob, sizeof(blob), &loaded));
    blob = camera_settings_pack(&s);
    blob.hflip = 2;
    assert(!camera_settings_unpack(&blob, sizeof(blob), &loaded));
    blob = camera_settings_pack(&s);
    blob.photo_quality = 101;
    assert(!camera_settings_unpack(&blob, sizeof(blob), &loaded));
    assert(loaded.photo_quality == 95); // A rejected blob never changes the active settings.

    // Older blobs carry the flash reserve_kb: it is ignored and store_kb keeps the caller's default.
    camera_settings_blob_v1_t old_blob = {
        .version = 1, .preview_quality = 95, .photo_quality = 95, .idle_stop_s = 10,
        .max_photos = 20, .reserve_kb = 512, .live_preview = 1,
    };
    loaded.store_kb = 2048;
    assert(camera_settings_unpack(&old_blob, sizeof(old_blob), &loaded));
    assert(loaded.width == 0 && loaded.height == 0 && loaded.photo_quality == 95 && loaded.store_kb == 2048);
    camera_settings_blob_v2_t v2_blob = {
        .version = 2, .preview_quality = 80, .photo_quality = 90, .idle_stop_s = 10,
        .max_photos = 30, .reserve_kb = 0, .live_preview = 0, .width = 800, .height = 640,
    };
    loaded.store_kb = 1024;
    assert(camera_settings_unpack(&v2_blob, sizeof(v2_blob), &loaded));
    assert(loaded.max_photos == 30 && loaded.width == 800 && loaded.store_kb == 1024 && !loaded.live_preview);
    v2_blob.reserve_kb = 4096;      // Out of the old 0..2048 range, but ignored now
    assert(camera_settings_unpack(&v2_blob, sizeof(v2_blob), &loaded) && loaded.store_kb == 1024);

    s.width = 800; s.height = 640;
    assert(camera_settings_supported(&s, &caps));
    s.height = 600;
    assert(!camera_settings_supported(&s, &caps));
    s.width = 0; s.height = 640;
    assert(!camera_settings_valid(&s));
    s.width = s.height = 0;
    s.hflip = true;
    assert(!camera_settings_supported(&s, &caps));
    caps.hflip = true;
    assert(camera_settings_supported(&s, &caps));
    s.vflip = true;
    assert(!camera_settings_supported(&s, &caps));
    caps.vflip = true;
    assert(camera_settings_supported(&s, &caps));
    caps.modes[1] = (camera_mode_t){800, 800};
    caps.modes[2] = (camera_mode_t){800, 1280};
    caps.modes[3] = (camera_mode_t){1280, 960};
    caps.modes[4] = (camera_mode_t){1920, 1080};
    caps.mode_count = CAMERA_MODE_MAX;
    s.width = 1920; s.height = 1080;
    assert(camera_settings_supported(&s, &caps));
    blob = camera_settings_pack(&s);
    assert(camera_settings_unpack(&blob, sizeof(blob), &loaded));
    assert(loaded.width == 1920 && loaded.height == 1080);
    s.width = 2592; s.height = 1944;
    assert(!camera_settings_supported(&s, &caps));
    int *fields[] = {&s.preview_quality, &s.photo_quality, &s.idle_stop_s, &s.max_photos, &s.store_kb};
    const int lo[] = {10, 10, 2, 1, CAMERA_STORE_KB_MIN}, hi[] = {100, 100, 600, 500, CAMERA_STORE_KB_MAX};
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        int saved = *fields[i];
        *fields[i] = lo[i]; assert(camera_settings_valid(&s));
        *fields[i] = hi[i]; assert(camera_settings_valid(&s));
        *fields[i] = lo[i] - 1; assert(!camera_settings_valid(&s));
        *fields[i] = hi[i] + 1; assert(!camera_settings_valid(&s));
        *fields[i] = saved;
    }
}

static void test_camera_gray(void)
{
    // 2x2 RGB888 frame stored B,G,R with a 1-byte row pad: white, black / red, blue.
    static const uint8_t bgr[] = {255, 255, 255, 0, 0, 0, 0xEE,
                                  0, 0, 255, 255, 0, 0, 0xEE};
    uint8_t g[4];
    assert(camera_frame_to_gray(bgr, 2, 2, 7, 3, sizeof(bgr), 1, g, 2, 2));
    assert(g[0] == 255 && g[1] == 0 && g[2] == (77 * 255) >> 8 && g[3] == (29 * 255) >> 8);
    assert(camera_frame_to_gray(bgr, 2, 2, 7, 3, sizeof(bgr), 2, g, 1, 1) && g[0] == 255);
    assert(!camera_frame_to_gray(bgr, 2, 2, 7, 3, sizeof(bgr) - 1, 1, g, 2, 2));    // Truncated
    assert(!camera_frame_to_gray(bgr, 2, 2, 7, 3, sizeof(bgr), 1, g, 1, 1));        // Wrong size
    // RGB565 little-endian: pure green 0x07E0.
    static const uint8_t rgb565[] = {0xE0, 0x07};
    assert(camera_frame_to_gray(rgb565, 1, 1, 2, 2, sizeof(rgb565), 1, g, 1, 1) && g[0] == (150 * 255) >> 8);
    assert(camera_gray_step(1280, 960) == 1 && camera_gray_step(1920, 1080) == 2 && camera_gray_step(800, 640) == 1);
}

static void test_camera_rgb_conversion(void)
{
    size_t stride = 0;
    // esp_video's G_FMT omits optional layout metadata, but QUERYBUF supplies real lengths.
    assert(camera_frame_layout(800, 640, 3, 0, 0, &stride));
    assert(stride == 2400);
    assert(camera_frame_valid(800, 640, stride, 3, 1536000));
    assert(!camera_frame_valid(800, 640, stride, 3, 1535999));
    assert(!camera_frame_valid(800, 640, stride, 3, 0));
    assert(camera_frame_layout(800, 640, 2, 0, 0, &stride));
    assert(stride == 1600);
    assert(camera_frame_layout(3, 2, 3, 12, 0, &stride) && stride == 12);
    assert(camera_frame_layout(3, 2, 3, 0, 18, &stride) && stride == 9);
    assert(camera_frame_layout(3, 2, 3, 12, 24, &stride));
    assert(!camera_frame_layout(3, 2, 3, 12, 23, &stride));
    assert(!camera_frame_layout(3, 2, 3, 8, 0, &stride));
    assert(!camera_frame_layout(1, 2, 3, SIZE_MAX, 0, &stride));
    assert(!camera_frame_layout(0, 2, 3, 0, 0, &stride));
    assert(!camera_frame_layout(3, 0, 3, 0, 0, &stride));
    assert(!camera_frame_layout(3, 2, 4, 0, 0, &stride));
    assert(!camera_frame_layout(3, 2, 3, 0, 0, NULL));
    // ISP RGB888 uses BGR24 byte order, including row padding that must never become a pixel.
    const uint8_t rgb[] = {0,0,255, 0,255,0, 255,0,0, 0xaa,0xbb,0xcc,
                           255,255,255, 0,0,0, 255,0,255, 0xaa,0xbb,0xcc};
    uint16_t dst[6] = {0};
    assert(camera_scale_to_rgb565(rgb, 3, 2, 12, 3, sizeof(rgb), dst, 3, 2));
    assert(dst[0] == 0xf800 && dst[1] == 0x07e0 && dst[2] == 0x001f);
    assert(dst[3] == 0xffff && dst[4] == 0 && dst[5] == 0xf81f);
    const uint8_t rgb565[] = {0,0xf8, 0xe0,7, 0x1f,0, 0xff,0xff};
    assert(camera_scale_to_rgb565(rgb565, 3, 1, 8, 2, sizeof(rgb565), dst, 3, 1));
    assert(dst[0] == 0xf800 && dst[1] == 0x07e0 && dst[2] == 0x001f);
    assert(!camera_scale_to_rgb565(rgb, 3, 2, 8, 3, sizeof(rgb), dst, 3, 2));
    assert(!camera_scale_to_rgb565(rgb, 3, 2, 12, 3, sizeof(rgb) - 1, dst, 3, 2));
    assert(!camera_frame_valid(3, 2, 12, 4, 24));
    assert(!camera_frame_valid(0, 2, 0, 3, 24));
    assert(!camera_frame_valid(1, 2, SIZE_MAX, 3, SIZE_MAX));
    assert(camera_scale_to_rgb565(rgb, 3, 2, 12, 3, sizeof(rgb), dst, 1, 1));
    assert(dst[0] == 0x07e0);
    uint8_t jpeg_pixels[18] = {0};
    assert(camera_pack_jpeg_pixels(rgb, 3, 2, 12, 3, sizeof(rgb), jpeg_pixels, sizeof(jpeg_pixels)));
    static const uint8_t bgr[] = {0,0,255, 0,255,0, 255,0,0, 255,255,255, 0,0,0, 255,0,255};
    assert(memcmp(jpeg_pixels, bgr, sizeof(bgr)) == 0);
    assert(!camera_pack_jpeg_pixels(rgb, 3, 2, 12, 3, sizeof(rgb), jpeg_pixels, sizeof(jpeg_pixels) - 1));
    assert(!camera_pack_jpeg_pixels(rgb, 3, 2, 12, 3, sizeof(rgb) - 1, jpeg_pixels, sizeof(jpeg_pixels)));
    assert(camera_pack_jpeg_pixels(rgb565, 3, 1, 8, 2, sizeof(rgb565), jpeg_pixels, sizeof(jpeg_pixels)));
    assert(memcmp(jpeg_pixels, rgb565, 6) == 0);
}

// --- opt_config (v0.9.0): mock NVS key/value store ---
typedef struct {
    char key[16][16];
    uint8_t val[16];
    int n, commits, fail_set;
    char key16[4][16];
    uint16_t val16[4];
    int n16;
    char keys[4][16], vals[4][8];
    int ns;
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
    assert(m->n < 16 && strlen(key) < 16);
    strcpy(m->key[m->n], key);
    m->val[m->n++] = v;
    return 0;
}

static int mock_get16(void *ctx, const char *key, uint16_t *out)
{
    mock_nvs_t *m = ctx;
    for (int i = 0; i < m->n16; i++) {
        if (strcmp(m->key16[i], key) == 0) { *out = m->val16[i]; return 0; }
    }
    return -1;
}

static int mock_set16(void *ctx, const char *key, uint16_t v)
{
    mock_nvs_t *m = ctx;
    if (m->fail_set) return -2;
    for (int i = 0; i < m->n16; i++) {
        if (strcmp(m->key16[i], key) == 0) { m->val16[i] = v; return 0; }
    }
    assert(m->n16 < 4 && strlen(key) < 16);
    strcpy(m->key16[m->n16], key);
    m->val16[m->n16++] = v;
    return 0;
}

static int mock_getstr(void *ctx, const char *key, char *out, unsigned len)
{
    mock_nvs_t *m = ctx;
    for (int i = 0; i < m->ns; i++) {
        if (strcmp(m->keys[i], key) == 0) {
            if (strlen(m->vals[i]) + 1 > len) return -3;    // ESP_ERR_NVS_INVALID_LENGTH
            strcpy(out, m->vals[i]);
            return 0;
        }
    }
    return -1;
}

static int mock_setstr(void *ctx, const char *key, const char *v)
{
    mock_nvs_t *m = ctx;
    if (m->fail_set) return -2;
    assert(strlen(v) < 8);
    for (int i = 0; i < m->ns; i++) {
        if (strcmp(m->keys[i], key) == 0) { strcpy(m->vals[i], v); return 0; }
    }
    assert(m->ns < 4 && strlen(key) < 16);
    strcpy(m->keys[m->ns], key);
    strcpy(m->vals[m->ns++], v);
    return 0;
}

static int mock_commit(void *ctx) { ((mock_nvs_t *)ctx)->commits++; return 0; }

static void test_opt_config(void)
{
    mock_nvs_t m = {0};
    opt_store_t st = {mock_get, mock_set, mock_get16, mock_set16, mock_getstr, mock_setstr, mock_commit, &m};
    opt_config_t c, d;

    // Load from an empty store = defaults (driver TX power, PS off, 100 ms, SE).
    memset(&c, 0xAA, sizeof(c));
    memset(&d, 0xAA, sizeof(d));
    opt_config_load_from(&st, &c);
    assert(c.wifi_tx_power_dbm == 0 && c.ps_mode == 0 && c.ap_beacon_ms == 100 && strcmp(c.country, "SE") == 0);
    assert(c.tcp_wnd_kb == 32 && c.lwip_core == 0 && c.napt_max == 5 && c.napt_tcp_to == 2);
    assert(c.bw_5g == 40 && c.sdio_mhz == 40);          // v0.9.5: HT40 on 5 GHz, SDIO 40 MHz
    assert(opt_config_valid(&c));
    opt_config_load_from(NULL, &d);
    assert(memcmp(&c, &d, sizeof(c)) == 0);

    // Fixed values of the settings removed in v0.9.3; defaults of the v0.9.5 settings.
    assert(OPT_FIXED_LOG_LVL == 2 && OPT_DEFAULT_SDIO_MHZ == 40 && OPT_DEFAULT_BW_5G == 40);
    assert(opt_bw_5g_valid(20) && opt_bw_5g_valid(40) && !opt_bw_5g_valid(0) && !opt_bw_5g_valid(80));
    assert(opt_sdio_mhz_valid(40) && opt_sdio_mhz_valid(50) && !opt_sdio_mhz_valid(0) && !opt_sdio_mhz_valid(45));

    // v0.9.4 TCP / LwIP / NAT helpers.
    assert(opt_tcp_wnd_bytes(32) == 32768 && opt_tcp_wnd_bytes(64) == 65536 && opt_tcp_wnd_bytes(128) == 131072);
    assert(opt_tcp_wnd_bytes(0) == 32768 && opt_tcp_wnd_bytes(100) == 32768);
    assert(opt_napt_max_entries(5) == 512 && opt_napt_max_entries(10) == 1024 && opt_napt_max_entries(20) == 2048);
    assert(opt_napt_max_entries(7) == 512);
    assert(opt_napt_tcp_timeout_s(2) == 7200 && opt_napt_tcp_timeout_s(1) == 3600 && opt_napt_tcp_timeout_s(0) == 1800);
    assert(opt_lwip_core_id(0) == -1 && opt_lwip_core_id(1) == 0 && opt_lwip_core_id(2) == 1);
    // Restored keys are not in the obsolete list (they would be erased at boot).
    for (int i = 0; i < OPT_OBSOLETE_KEY_COUNT; i++) {
        assert(strcmp(OPT_OBSOLETE_KEYS[i], OPT_KEY_TCP_WND_KB) && strcmp(OPT_OBSOLETE_KEYS[i], OPT_KEY_LWIP_CORE) &&
               strcmp(OPT_OBSOLETE_KEYS[i], OPT_KEY_NAPT_MAX) && strcmp(OPT_OBSOLETE_KEYS[i], OPT_KEY_NAPT_TCP_TO) &&
               strcmp(OPT_OBSOLETE_KEYS[i], OPT_KEY_SDIO_MHZ) && strcmp(OPT_OBSOLETE_KEYS[i], OPT_KEY_BW_5G));
    }

    // Country table (wifi_country_t values).
    assert(OPT_COUNTRY_COUNT == 4);
    const opt_country_t *k = opt_country_find("US");
    assert(k && k->schan == 1 && k->nchan == 11 && k->max_tx_power == 30);
    k = opt_country_find("JP"); assert(k && k->nchan == 14 && k->max_tx_power == 23);
    k = opt_country_find("SE"); assert(k && k->nchan == 13 && k->max_tx_power == 20);
    k = opt_country_find("EU"); assert(k && k->nchan == 13 && k->max_tx_power == 20);
    assert(!opt_country_find("XX") && !opt_country_find(NULL));

    // Save round-trip.
    c.wifi_tx_power_dbm = 14; c.ps_mode = 2; c.ap_beacon_ms = 300; memcpy(c.country, "JP", 3);
    c.tcp_wnd_kb = 128; c.lwip_core = 2; c.napt_max = 20; c.napt_tcp_to = 0;
    c.bw_5g = 20; c.sdio_mhz = 50;
    assert(opt_config_save_to(&st, &c) == 0 && m.commits == 1);
    opt_config_load_from(&st, &d);
    assert(d.wifi_tx_power_dbm == 14 && d.ps_mode == 2 && d.ap_beacon_ms == 300 && strcmp(d.country, "JP") == 0);
    assert(d.tcp_wnd_kb == 128 && d.lwip_core == 2 && d.napt_max == 20 && d.napt_tcp_to == 0);
    assert(d.bw_5g == 20 && d.sdio_mhz == 50);
    uint8_t raw;
    uint16_t raw16;
    char rawcc[4];
    assert(mock_get(&m, OPT_KEY_TX_DBM, &raw) == 0 && raw == 14);
    assert(mock_get(&m, OPT_KEY_PS_MODE, &raw) == 0 && raw == 2);
    assert(mock_get(&m, OPT_KEY_TCP_WND_KB, &raw) == 0 && raw == 128);
    assert(mock_get(&m, OPT_KEY_LWIP_CORE, &raw) == 0 && raw == 2);
    assert(mock_get(&m, OPT_KEY_NAPT_MAX, &raw) == 0 && raw == 20);
    assert(mock_get(&m, OPT_KEY_NAPT_TCP_TO, &raw) == 0 && raw == 0);
    assert(mock_get16(&m, OPT_KEY_BEACON_MS, &raw16) == 0 && raw16 == 300);
    assert(mock_getstr(&m, OPT_KEY_COUNTRY, rawcc, sizeof(rawcc)) == 0 && strcmp(rawcc, "JP") == 0);
    assert(mock_get(&m, OPT_KEY_BW_5G, &raw) == 0 && raw == 20);
    assert(mock_get(&m, OPT_KEY_SDIO_MHZ, &raw) == 0 && raw == 50);    // v0.9.5: stored again
    for (int i = 0; i < OPT_OBSOLETE_KEY_COUNT; i++) assert(mock_get(&m, OPT_OBSOLETE_KEYS[i], &raw) != 0);

    // Stale values of removed settings in NVS do not change anything.
    opt_config_t before = d;
    for (int i = 0; i < OPT_OBSOLETE_KEY_COUNT; i++) mock_set(&m, OPT_OBSOLETE_KEYS[i], 3);
    opt_config_load_from(&st, &d);
    assert(memcmp(&before, &d, sizeof(d)) == 0);

    // Invalid configs are rejected and nothing is written.
    opt_config_t bad = d;
    bad.ps_mode = 3;
    assert(opt_config_save_to(&st, &bad) == -1 && m.commits == 1);
    bad = d; bad.ap_beacon_ms = 150;    assert(!opt_config_valid(&bad));
    bad = d; memcpy(bad.country, "XX", 3); assert(!opt_config_valid(&bad));
    bad = d; memcpy(bad.country, "SEX", 3); assert(!opt_config_valid(&bad));   // not NUL-terminated
    bad = d; bad.wifi_tx_power_dbm = 1; assert(!opt_config_valid(&bad));
    bad = d; bad.wifi_tx_power_dbm = 22; assert(!opt_config_valid(&bad));
    bad = d; bad.tcp_wnd_kb = 48;       assert(!opt_config_valid(&bad));
    bad = d; bad.lwip_core = 3;         assert(!opt_config_valid(&bad));
    bad = d; bad.napt_max = 15;         assert(!opt_config_valid(&bad));
    bad = d; bad.napt_tcp_to = 3;       assert(!opt_config_valid(&bad));
    bad = d; bad.bw_5g = 80;            assert(!opt_config_valid(&bad));
    bad = d; bad.bw_5g = 0;             assert(!opt_config_valid(&bad));
    bad = d; bad.sdio_mhz = 45;         assert(!opt_config_valid(&bad));
    bad = d; bad.sdio_mhz = 0;          assert(!opt_config_valid(&bad));
    m.fail_set = 1;
    assert(opt_config_save_to(&st, &d) == -2);
    m.fail_set = 0;

    // Corrupt values in NVS fall back to the default field by field.
    mock_set(&m, OPT_KEY_TX_DBM, 30);
    mock_set(&m, OPT_KEY_PS_MODE, 7);
    mock_set16(&m, OPT_KEY_BEACON_MS, 250);
    mock_setstr(&m, OPT_KEY_COUNTRY, "XX");
    mock_set(&m, OPT_KEY_TCP_WND_KB, 100);
    mock_set(&m, OPT_KEY_LWIP_CORE, 9);
    mock_set(&m, OPT_KEY_NAPT_MAX, 7);
    mock_set(&m, OPT_KEY_NAPT_TCP_TO, 5);
    mock_set(&m, OPT_KEY_BW_5G, 80);
    mock_set(&m, OPT_KEY_SDIO_MHZ, 60);
    opt_config_load_from(&st, &d);
    assert(d.wifi_tx_power_dbm == 0 && d.ps_mode == 0 && d.ap_beacon_ms == 100 && strcmp(d.country, "SE") == 0);
    assert(d.tcp_wnd_kb == 32 && d.lwip_core == 0 && d.napt_max == 5 && d.napt_tcp_to == 2);
    assert(d.bw_5g == 40 && d.sdio_mhz == 40);
    mock_setstr(&m, OPT_KEY_COUNTRY, "SWED");                  // too long for the 3-byte field
    opt_config_load_from(&st, &d);
    assert(strcmp(d.country, "SE") == 0);

    // Reboot required only for the boot-only fields (beacon, country, TCP window, LwIP core, NAPT size).
    opt_config_t a, s2;
    opt_config_defaults(&a);
    s2 = a; s2.wifi_tx_power_dbm = 8; s2.ps_mode = 1; s2.napt_tcp_to = 0;
    assert(!opt_config_reboot_required(&a, &s2));
    s2 = a; s2.tcp_wnd_kb = 64;  assert(opt_config_reboot_required(&a, &s2));
    s2 = a; s2.lwip_core = 1;    assert(opt_config_reboot_required(&a, &s2));
    s2 = a; s2.napt_max = 10;    assert(opt_config_reboot_required(&a, &s2));
    s2 = a; s2.bw_5g = 20;       assert(opt_config_reboot_required(&a, &s2));
    s2 = a; s2.sdio_mhz = 50;    assert(opt_config_reboot_required(&a, &s2));
    s2 = a; s2.ap_beacon_ms = 200;
    assert(opt_config_reboot_required(&a, &s2));
    s2 = a; memcpy(s2.country, "US", 3);
    assert(opt_config_reboot_required(&a, &s2));

    // SDIO trial / auto-fallback decision; the configured clock is the stored setting (v0.9.5).
    bool fb;
    assert(opt_sdio_boot_mhz(40, false, &fb) == 40 && !fb);
    assert(opt_sdio_boot_mhz(40, true, &fb) == 40 && !fb);
    assert(opt_sdio_boot_mhz(50, false, &fb) == 50 && !fb);
    assert(opt_sdio_boot_mhz(50, true, &fb) == 40 && fb);

    // Boot sequence as in opt_config_load(): stored 50 MHz -> trial boot at 50; an unconfirmed trial
    // -> 40 MHz, and the fallback rewrites the stored setting to 40 (next boot is a plain 40 MHz boot).
    mock_nvs_t m2 = {0};
    opt_store_t st2 = {mock_get, mock_set, mock_get16, mock_set16, mock_getstr, mock_setstr, mock_commit, &m2};
    opt_config_defaults(&c);
    c.sdio_mhz = 50;
    assert(opt_config_save_to(&st2, &c) == 0);
    opt_config_load_from(&st2, &d);
    assert(opt_sdio_boot_mhz(d.sdio_mhz, false, &fb) == 50 && !fb);     // first boot: 50 MHz trial
    assert(opt_sdio_boot_mhz(d.sdio_mhz, true, &fb) == 40 && fb);       // trial never confirmed
    if (fb) mock_set(&m2, OPT_KEY_SDIO_MHZ, 40);                        // opt_sdio_fallback_to_40 / load
    opt_config_load_from(&st2, &d);
    assert(d.sdio_mhz == 40 && opt_sdio_boot_mhz(d.sdio_mhz, false, &fb) == 40 && !fb);
    // Active 50 vs saved 40 after a runtime fallback = reboot pending (the caller restarts).
    a = d; a.sdio_mhz = 50;
    assert(opt_config_reboot_required(&a, &d));
}

// v0.9.3: last STA channel persistence and AP channel choice.
static void test_sta_channel(void)
{
    const int ok[] = {1, 6, 13, 14, 36, 40, 64, 100, 144, 149, 165, 177};
    const int no[] = {0, -1, 15, 35, 37, 65, 99, 145, 148, 178, 255};
    for (unsigned i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) assert(opt_sta_ch_valid(ok[i]));
    for (unsigned i = 0; i < sizeof(no) / sizeof(no[0]); i++) assert(!opt_sta_ch_valid(no[i]));
    assert(opt_ch_band(6) == 1 && opt_ch_band(36) == 2 && opt_ch_band(15) == 0);

    // Band mode and country range.
    assert(opt_ch_usable(6, 0, 13) && opt_ch_usable(6, 1, 13) && !opt_ch_usable(6, 2, 13));
    assert(opt_ch_usable(44, 0, 13) && opt_ch_usable(44, 2, 13) && !opt_ch_usable(44, 1, 13));
    assert(!opt_ch_usable(13, 0, 11) && opt_ch_usable(11, 0, 11) && opt_ch_usable(14, 1, 14));
    assert(!opt_ch_usable(0, 0, 13));

    // Write only valid, changed channels (flash wear).
    assert(opt_sta_ch_should_store(0, 6) && !opt_sta_ch_should_store(6, 6));
    assert(opt_sta_ch_should_store(6, 44) && !opt_sta_ch_should_store(6, 0) && !opt_sta_ch_should_store(6, 15));

    // AP channel: STA > AP > persisted (if usable) > 1 / 36.
    assert(opt_ap_channel_pick(44, 6, 1, 0, 13) == 44);
    assert(opt_ap_channel_pick(0, 6, 44, 0, 13) == 6);
    assert(opt_ap_channel_pick(0, 0, 44, 0, 13) == 44);
    assert(opt_ap_channel_pick(0, 0, 44, 1, 13) == 1);     // 5 GHz channel with 2.4-only band: ignored
    assert(opt_ap_channel_pick(0, 0, 6, 2, 13) == 36);     // 2.4 GHz channel with 5-only band: ignored
    assert(opt_ap_channel_pick(0, 0, 13, 0, 11) == 1);     // outside the country range
    assert(opt_ap_channel_pick(0, 0, 0, 0, 13) == 1);

    // Shared protocol mask migration: STA wins, else AP; ax kept, unknown bits dropped.
    assert(opt_proto_merge(0x37, 0x07) == 0x37);
    assert(opt_proto_merge(0, 0x36) == 0x36);
    assert(opt_proto_merge(0, 0) == 0);
    assert(opt_proto_merge(0x74, 0) == 0x74);
    assert(opt_proto_merge(0x88, 0x04) == 0x04);
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
    test_camera_core();
    test_camera_settings();
    test_camera_gray();
    test_camera_rgb_conversion();
    test_opt_config();
    test_sta_channel();
    puts("UI primitive tests passed");
    return 0;
}
