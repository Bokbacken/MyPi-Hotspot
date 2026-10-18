#pragma once

// Persistent log store (v0.8.5+).
//
// Logs are kept as plain text files on the FAT "storage" partition (/store/logs):
//   LIVE.TXT      log of the session being captured right now (appended in small steps)
//   Lnnnnnnn.TXT  saved logs, newest = highest number; the oldest are deleted beyond "keep"
//
// Capturing is OFF by default, except during an OTA (P4 or C5), which is ALWAYS captured:
// the log is appended every ~2 s while the upload runs, so it survives a hang, a reboot or
// a power cycle, and is moved into the ring at the next boot if the OTA never finished.
// With capturing switched on (web UI, saved in NVS), every session is captured the same
// way (every ~15 s) plus the crash tail of the previous session (exact last lines before a
// panic / watchdog / software reset).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define LOGSTORE_KEEP_MIN     5
#define LOGSTORE_KEEP_MAX     10
#define LOGSTORE_KEEP_DEFAULT 8
#define LOGSTORE_NAME_LEN     16

typedef struct {
    char name[LOGSTORE_NAME_LEN];
    uint32_t size;
    char head[160];     // First header line ("# MyPi log ...")
    char end[160];      // Last "# end ..." line, empty if the log never got one
} logstore_entry_t;

typedef struct {
    bool mounted;
    bool enabled;       // User switch (NVS)
    bool capturing;     // Live capture running now (enabled or OTA)
    bool ota;           // Capture forced by an OTA
    uint8_t keep;
    uint32_t boot;      // Boot counter (NVS), also written into every log header
    uint32_t total_kb, free_kb;
} logstore_info_t;

/** Load settings, bump the boot counter and start the background task (mounts FAT). Needs NVS. */
void logstore_init(void);

void logstore_get_info(logstore_info_t *out);

/** Switch session capturing on/off and set how many saved logs to keep (5-10). Saved in NVS. */
esp_err_t logstore_configure(bool enabled, int keep);

/** Save a snapshot of the current RAM log as a new entry. */
esp_err_t logstore_save_now(const char *reason);

/**
 * Call right before a deliberate esp_restart(): finishes the live capture (if any) into the
 * ring with the given reason and clears the crash-tail flag (so no duplicate after boot).
 */
void logstore_before_restart(const char *reason);

/** List saved entries, newest first (LIVE.TXT first if present). Returns the count. */
int logstore_list(logstore_entry_t *out, int max);

/** True if name is a valid log file name (LIVE.TXT or Lnnnnnnn.TXT). */
bool logstore_name_ok(const char *name);

/** Build the full path of a log file into dst. */
void logstore_path(const char *name, char *dst, size_t dst_size);

/** Lock/unlock file access (hold while streaming a file to a client). */
bool logstore_lock(uint32_t timeout_ms);
void logstore_unlock(void);

/** Delete all saved logs (the live file is restarted if capturing). */
esp_err_t logstore_clear(void);
