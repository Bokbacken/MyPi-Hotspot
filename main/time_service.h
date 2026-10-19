#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

#define TIME_ZONE_COUNT 7

typedef struct {
    bool valid;
    bool ntp_synced;
    uint8_t timezone;
    char clock[24];
} time_service_status_t;

void time_service_init(void);
void time_service_network_connected(void);
esp_err_t time_service_set_timezone(uint8_t timezone);
void time_service_get_status(time_service_status_t *status);
const char *time_service_timezone_name(uint8_t timezone);
time_t time_service_now(void);
bool time_service_format(char *dst, size_t dst_size, const char *format);
