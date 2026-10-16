#include "pintest.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "pins.h"

static const char *TAG = "pintest";

typedef struct {
    const char *name;
    int gpio;
} pin_t;

// Module labels -> GPIO (see pins.h). Pins set to -1 are skipped.
static const pin_t PINS[] = {
    { "SCLK", PIN_SCLK },       { "MOSI", PIN_MOSI },       { "MISO", PIN_MISO },
    { "SD_CS", PIN_SD_CS },     { "LCD_CS", PIN_LCD_CS },   { "LCD_DC", PIN_LCD_DC },
    { "LCD_RESET", PIN_LCD_RESET }, { "LCD_BL", PIN_LCD_BL },
    { "TP_SDA", PIN_TP_SDA },   { "TP_SCL", PIN_TP_SCL },   { "TP_INT", PIN_TP_INT },
    { "TP_RST", PIN_TP_RST },
};
#define NPINS (sizeof(PINS) / sizeof(PINS[0]))

static char s_report[1536];
static size_t s_report_len;

static void report(const char *fmt, ...)
{
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "%s", line);
    if (s_report_len < sizeof(s_report) - 1) {
        s_report_len += snprintf(s_report + s_report_len, sizeof(s_report) - s_report_len, "%s\n", line);
        if (s_report_len >= sizeof(s_report)) {
            s_report_len = sizeof(s_report) - 1;
        }
    }
}

static void set_input(int gpio, bool pullup)
{
    gpio_reset_pin(gpio);
    gpio_set_direction(gpio, GPIO_MODE_INPUT);
    gpio_set_pull_mode(gpio, pullup ? GPIO_PULLUP_ONLY : GPIO_PULLDOWN_ONLY);
}

void pintest_boot_check(void)
{
    s_report_len = 0;
    report("Wiring check (internal pull-up vs pull-down, then drive each pin and watch the others)");
    report("  The Waveshare 2\" module routes every signal except LCD_BL through an on-board level");
    report("  translator with pull-ups to the module supply, so 'held HIGH' = wire reaches the module (OK),");
    report("  'floating' = wire probably broken. LCD_BL drives a transistor: 'held LOW' is OK there.");
    report("  Power the module from ONE supply pin only: with both VCC and 3V3 fed the screen stayed");
    report("  dark and touch I2C timed out; leaving VCC unconnected fixed both.");

    // 1) Pull test
    int pu[NPINS] = {0}, pd[NPINS] = {0};
    bool reported[NPINS][NPINS] = {{false}};
    for (size_t i = 0; i < NPINS; i++) {
        int g = PINS[i].gpio;
        if (g < 0) continue;
        set_input(g, true);
        esp_rom_delay_us(200);
        int up = pu[i] = gpio_get_level(g);
        set_input(g, false);
        esp_rom_delay_us(200);
        int down = pd[i] = gpio_get_level(g);
        const char *verdict = (up && !down) ? "floating" : (up && down) ? "held HIGH" : (!up && !down) ? "held LOW" : "odd (inverted?)";
        report("  %-9s GPIO%-2d pu=%d pd=%d -> %s", PINS[i].name, g, up, down, verdict);
    }

    // 2) Short test: drive pin i, put all others on the opposite pull, look for followers.
    int shorts = 0;
    for (int drive_level = 0; drive_level <= 1; drive_level++) {
        for (size_t i = 0; i < NPINS; i++) {
            int gi = PINS[i].gpio;
            if (gi < 0) continue;
            for (size_t k = 0; k < NPINS; k++) {
                if (k != i && PINS[k].gpio >= 0) set_input(PINS[k].gpio, drive_level == 0);
            }
            gpio_reset_pin(gi);
            gpio_set_direction(gi, GPIO_MODE_OUTPUT);
            gpio_set_level(gi, drive_level);
            esp_rom_delay_us(200);
            for (size_t k = 0; k < NPINS; k++) {
                int gk = PINS[k].gpio;
                if (k == i || gk < 0 || reported[i][k]) continue;
                // k is pulled to !drive_level. If it reads drive_level although the
                // pull test showed it is not held there on its own, it follows pin i.
                bool held_at_drive = drive_level ? (pu[k] && pd[k]) : (!pu[k] && !pd[k]);
                if (!held_at_drive && gpio_get_level(gk) == drive_level) {
                    reported[i][k] = reported[k][i] = true;
                    report("  SHORT? %s (GPIO%d) <-> %s (GPIO%d)", PINS[i].name, gi, PINS[k].name, gk);
                    shorts++;
                }
            }
            set_input(gi, true);
        }
    }
    report("Wiring check done: %d possible short(s)", shorts);

    // Leave everything as pulled-up inputs; the drivers reconfigure their pins.
    for (size_t i = 0; i < NPINS; i++) {
        if (PINS[i].gpio >= 0) set_input(PINS[i].gpio, true);
    }
    // Keep the SD card deselected so it never drives MISO during LCD init.
    if (PIN_SD_CS >= 0) {
        gpio_set_direction(PIN_SD_CS, GPIO_MODE_OUTPUT);
        gpio_set_level(PIN_SD_CS, 1);
    }
}

size_t pintest_report(char *buf, size_t len)
{
    if (s_report_len == 0) {
        return snprintf(buf, len, "Boot-time wiring check was skipped (debug mode off).\n"
                                  "Turn debug mode on from the status page and reboot to run it.\n"
                                  "Manual pin control below works either way.\n");
    }
    size_t n = s_report_len < len - 1 ? s_report_len : len - 1;
    memcpy(buf, s_report, n);
    buf[n] = 0;
    return n;
}

const char *pintest_names(void)
{
    return "SCLK,MOSI,MISO,SD_CS,LCD_CS,LCD_DC,LCD_RESET,LCD_BL,TP_SDA,TP_SCL,TP_INT,TP_RST";
}

esp_err_t pintest_set(const char *name, const char *mode, int *level)
{
    for (size_t i = 0; i < NPINS; i++) {
        if (strcmp(PINS[i].name, name) != 0) continue;
        int g = PINS[i].gpio;
        if (g < 0) return ESP_ERR_NOT_FOUND;
        gpio_reset_pin(g);   // Detaches the pin from SPI/I2C until reboot
        if (!strcmp(mode, "high") || !strcmp(mode, "low")) {
            gpio_set_direction(g, GPIO_MODE_INPUT_OUTPUT);
            gpio_set_level(g, mode[0] == 'h');
        } else if (!strcmp(mode, "in") || !strcmp(mode, "inpd")) {
            set_input(g, mode[2] == 0);
        } else {
            return ESP_ERR_INVALID_ARG;
        }
        esp_rom_delay_us(100);
        *level = gpio_get_level(g);
        ESP_LOGW(TAG, "Manual: %s (GPIO%d) -> %s, reads %d (reboot to restore drivers)", name, g, mode, *level);
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}
