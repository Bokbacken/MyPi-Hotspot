#pragma once

#include <stddef.h>
#include "esp_err.h"

/**
 * Install a vprintf hook that mirrors every ESP_LOG line into an in-RAM ring
 * buffer (in PSRAM) while still printing to the UART console.
 */
esp_err_t logbuf_init(size_t size);

/**
 * Copy the buffer contents (oldest first) into dst, NUL-terminated.
 * Returns the number of bytes written (excluding the NUL).
 */
size_t logbuf_read(char *dst, size_t dst_size);

/** Capacity of the ring buffer in bytes (0 if not initialized). */
size_t logbuf_capacity(void);
