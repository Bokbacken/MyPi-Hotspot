#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/**
 * Install a vprintf hook that mirrors every ESP_LOG line into an in-RAM ring
 * buffer (in PSRAM) while still printing to the UART console.
 *
 * The hook also feeds a 32 KB "crash tail" ring in no-init PSRAM that
 * survives software resets, panics and watchdog resets (not power loss). The
 * previous session's tail is copied out here, before it is overwritten.
 */
esp_err_t logbuf_init(size_t size);

/**
 * Copy the buffer contents (oldest first) into dst, NUL-terminated.
 * Returns the number of bytes written (excluding the NUL).
 */
size_t logbuf_read(char *dst, size_t dst_size);

/** Capacity of the ring buffer in bytes (0 if not initialized). */
size_t logbuf_capacity(void);

/** Total number of bytes ever written to the ring (monotonic). */
uint64_t logbuf_total(void);

/**
 * Copy everything written since *pos (a previous logbuf_total() value) into dst,
 * at most dst_size - 1 bytes (newest kept), NUL-terminated. Advances *pos to the
 * current total. *lost receives the number of bytes that were overwritten before
 * they could be read (0 normally).
 */
size_t logbuf_read_since(uint64_t *pos, char *dst, size_t dst_size, uint64_t *lost);

/** Mark the current session as "capturing": its crash tail is persisted after a reset. */
void logbuf_set_tail_flag(bool on);

/**
 * Crash tail of the previous session (valid if it ended with a non-power-on reset).
 * Returns the text (NUL-terminated, owned by logbuf) or NULL; *flagged tells whether
 * that session had logbuf_set_tail_flag(true) set when it ended.
 */
const char *logbuf_prev_tail(bool *flagged);
