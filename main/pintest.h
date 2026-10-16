#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

// Wiring diagnostics for the LCD module pins (no extra hardware needed).
//
// pintest_boot_check() must run BEFORE display_init()/touch_init(): it briefly
// reconfigures every module pin as GPIO to detect
//   * pins held HIGH/LOW externally (pull-up/down resistors on the module,
//     shorts to VCC/GND) vs. floating CMOS inputs, and
//   * shorts between two module pins (drive one, watch the others).
// Results are logged and kept for the web UI (/pins, /api/pins).

/** Run the boot-time wiring check and log the result. */
void pintest_boot_check(void);

/** Copy the boot check report (plain text) into buf. Returns length. */
size_t pintest_report(char *buf, size_t len);

/** Manual pin control from the web UI. mode: "high", "low", "in" (input, pull-up),
 *  "inpd" (input, pull-down). Takes the pin away from SPI/I2C until reboot.
 *  On success *level is the level read back from the pad. */
esp_err_t pintest_set(const char *name, const char *mode, int *level);

/** Comma-separated list of the pin names accepted by pintest_set(). */
const char *pintest_names(void);
