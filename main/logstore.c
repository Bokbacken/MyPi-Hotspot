// Persistent log store on the FAT "storage" partition. See logstore.h for the behavior.
//
// Flash wear / OTA impact: the live file is only ever appended with the bytes logged since
// the previous flush (typically a few hundred bytes), so one flush costs one or two
// wear-levelled sector writes. During an OTA that happens at most every 2 s.

#include "logstore.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "diag.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "logbuf.h"
#include "nvs.h"

static const char *TAG = "logstore";

#define BASE_PATH           "/store"
#define LOG_DIR             BASE_PATH "/logs"
#define LIVE_NAME           "LIVE.TXT"
#define PARTITION_LABEL     "storage"
#define NVS_NS              "mypi"
#define NVS_KEY_ENABLED     "logcap"
#define NVS_KEY_KEEP        "logkeep"
#define NVS_KEY_BOOTS       "boots"

#define TICK_MS             1000
#define FLUSH_OTA_MS        2000        // Append interval while an OTA runs
#define FLUSH_SESSION_MS    15000       // Append interval for normal session capture
#define LIVE_MAX_BYTES      (512 * 1024)    // Session log rolls over into a new entry beyond this
#define OTA_START_CONTEXT   8192        // Bytes of earlier log copied into an OTA log at its start
#define CHUNK_SIZE          (16 * 1024) // Scratch buffer for appends

static SemaphoreHandle_t s_mutex;
static wl_handle_t s_wl = WL_INVALID_HANDLE;
static bool s_mounted;
static bool s_enabled;
static uint8_t s_keep = LOGSTORE_KEEP_DEFAULT;
static uint32_t s_boot;
static uint32_t s_next_seq;             // Next Lnnnnnnn number (0 = not scanned yet)

// Live capture state (guarded by s_mutex)
static bool s_live;
static bool s_live_ota;
static uint64_t s_live_pos;             // logbuf_total() up to which the live file is written
static int64_t s_live_flush_us;
static char *s_chunk;                   // CHUNK_SIZE scratch (PSRAM)

// ---------------------------------------------------------------------------
// Helpers (call with s_mutex held)
// ---------------------------------------------------------------------------

bool logstore_lock(uint32_t timeout_ms)
{
    return s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void logstore_unlock(void)
{
    xSemaphoreGive(s_mutex);
}

void logstore_path(const char *name, char *dst, size_t dst_size)
{
    snprintf(dst, dst_size, LOG_DIR "/%s", name);
}

bool logstore_name_ok(const char *name)
{
    if (!name) return false;
    if (strcmp(name, LIVE_NAME) == 0) return true;
    if (strlen(name) != 12 || name[0] != 'L' || strcmp(name + 8, ".TXT") != 0) return false;
    for (int i = 1; i < 8; i++) {
        if (name[i] < '0' || name[i] > '9') return false;
    }
    return true;
}

static uint32_t name_seq(const char *name)
{
    return (uint32_t)strtoul(name + 1, NULL, 10);
}

/** Scan the log directory: count saved entries, find min/max sequence numbers. */
static int scan_dir(uint32_t *min_seq, uint32_t *max_seq)
{
    int count = 0;
    uint32_t mn = UINT32_MAX, mx = 0;
    DIR *d = opendir(LOG_DIR);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (!logstore_name_ok(e->d_name) || strcmp(e->d_name, LIVE_NAME) == 0) continue;
            uint32_t s = name_seq(e->d_name);
            if (s < mn) mn = s;
            if (s > mx) mx = s;
            count++;
        }
        closedir(d);
    }
    if (min_seq) *min_seq = mn;
    if (max_seq) *max_seq = mx;
    return count;
}

/** Delete the oldest saved entries until at most s_keep remain. */
static void prune(void)
{
    for (int guard = 0; guard < 64; guard++) {
        uint32_t mn;
        if (scan_dir(&mn, NULL) <= s_keep) return;
        char name[LOGSTORE_NAME_LEN], path[48];
        snprintf(name, sizeof(name), "L%07lu.TXT", (unsigned long)mn);
        logstore_path(name, path, sizeof(path));
        if (unlink(path) != 0) return;
    }
}

static void new_entry_name(char *name, size_t size)
{
    if (s_next_seq == 0) {
        uint32_t mx;
        scan_dir(NULL, &mx);
        s_next_seq = mx + 1;
    }
    snprintf(name, size, "L%07lu.TXT", (unsigned long)(s_next_seq++ % 10000000u));
}

static void write_header(FILE *f, const char *reason)
{
    const esp_app_desc_t *app = esp_app_get_description();
    fprintf(f, "# MyPi log | %s | fw %s | boot %lu | uptime %llu s | free internal heap %u\n", reason,
            app->version, (unsigned long)s_boot, (unsigned long long)(esp_timer_get_time() / 1000000),
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

/** Append the log written since s_live_pos to the live file. */
static void live_append(void)
{
    uint64_t lost = 0;
    size_t n = logbuf_read_since(&s_live_pos, s_chunk, CHUNK_SIZE, &lost);
    s_live_flush_us = esp_timer_get_time();
    if (n == 0 && lost == 0) return;
    char path[48];
    logstore_path(LIVE_NAME, path, sizeof(path));
    FILE *f = fopen(path, "a");
    if (!f) {
        ESP_LOGW(TAG, "Cannot append %s", path);
        return;
    }
    if (lost) fprintf(f, "\n# ... %llu bytes of log lost (RAM ring overrun) ...\n", (unsigned long long)lost);
    fwrite(s_chunk, 1, n, f);
    fclose(f);   // Close every time: data and FAT are on flash if the device dies next
}

static void live_start(const char *reason, size_t context_bytes)
{
    char path[48];
    logstore_path(LIVE_NAME, path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGW(TAG, "Cannot create %s", path);
        return;
    }
    write_header(f, reason);
    // Start with some earlier log for context
    uint64_t total = logbuf_total();
    s_live_pos = total > context_bytes ? total - context_bytes : 0;
    uint64_t lost = 0;
    size_t n = logbuf_read_since(&s_live_pos, s_chunk, CHUNK_SIZE, &lost);
    fwrite(s_chunk, 1, n, f);
    fclose(f);
    s_live = true;
    s_live_flush_us = esp_timer_get_time();
    logbuf_set_tail_flag(true);
}

/** Finish the live file with an end line and move it into the ring. */
static void live_commit(const char *end_fmt, ...) __attribute__((format(printf, 1, 2)));
static void live_commit(const char *end_fmt, ...)
{
    char path[48], dst[48], name[LOGSTORE_NAME_LEN];
    logstore_path(LIVE_NAME, path, sizeof(path));
    if (s_live) live_append();
    FILE *f = fopen(path, "a");
    if (f) {
        char line[200];
        va_list ap;
        va_start(ap, end_fmt);
        vsnprintf(line, sizeof(line), end_fmt, ap);
        va_end(ap);
        fprintf(f, "\n# end: %s (uptime %llu s)\n", line, (unsigned long long)(esp_timer_get_time() / 1000000));
        fclose(f);
    }
    new_entry_name(name, sizeof(name));
    logstore_path(name, dst, sizeof(dst));
    if (rename(path, dst) != 0) {
        ESP_LOGW(TAG, "rename %s -> %s failed", path, dst);
    } else {
        ESP_LOGI(TAG, "Saved log %s", name);
    }
    s_live = false;
    s_live_ota = false;
    logbuf_set_tail_flag(false);
    prune();
}

static esp_err_t write_entry(const char *reason, const char *text, size_t len, const char *end)
{
    char name[LOGSTORE_NAME_LEN], path[48];
    new_entry_name(name, sizeof(name));
    logstore_path(name, path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (!f) return ESP_FAIL;
    write_header(f, reason);
    fwrite(text, 1, len, f);
    fprintf(f, "\n# end: %s\n", end);
    fclose(f);
    ESP_LOGI(TAG, "Saved log %s (%s)", name, reason);
    prune();
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Background task
// ---------------------------------------------------------------------------

static bool mount_fs(void)
{
    esp_vfs_fat_mount_config_t cfg = {
        .format_if_mount_failed = true,     // First boot: the partition was never formatted
        .max_files = 4,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
    };
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(BASE_PATH, PARTITION_LABEL, &cfg, &s_wl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Mounting FAT partition '%s' failed: %s - persistent logs disabled", PARTITION_LABEL,
                 esp_err_to_name(err));
        return false;
    }
    mkdir(LOG_DIR, 0775);
    ESP_LOGI(TAG, "FAT '%s' mounted on %s in %lld ms", PARTITION_LABEL, BASE_PATH,
             (long long)((esp_timer_get_time() - t0) / 1000));
    return true;
}

/** After boot: rescue an unfinished live file and the crash tail of the previous session. */
static void boot_recovery(void)
{
    char path[48];
    logstore_path(LIVE_NAME, path, sizeof(path));
    struct stat st;
    if (stat(path, &st) == 0) {
        ESP_LOGW(TAG, "Previous capture was not finished (%u bytes) - saving it", (unsigned)st.st_size);
        live_commit("NOT FINISHED - the device restarted or lost power while capturing; "
                    "recovered at boot %lu, reset reason: %s", (unsigned long)s_boot, diag_reset_reason_name());
    }
    bool flagged = false;
    const char *tail = logbuf_prev_tail(&flagged);
    if (tail && flagged) {
        char reason[96];
        snprintf(reason, sizeof(reason), "crash tail of boot %lu (last ~8 KB before the reset)",
                 (unsigned long)(s_boot ? s_boot - 1 : 0));
        char end[96];
        snprintf(end, sizeof(end), "reset reason at boot %lu: %s", (unsigned long)s_boot, diag_reset_reason_name());
        write_entry(reason, tail, strlen(tail), end);
    }
}

static void logstore_task(void *arg)
{
    s_chunk = heap_caps_malloc(CHUNK_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_chunk) s_chunk = malloc(CHUNK_SIZE);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_mounted = s_chunk && mount_fs();
    if (s_mounted) boot_recovery();
    xSemaphoreGive(s_mutex);
    if (!s_mounted) {
        vTaskDelete(NULL);
        return;
    }

    bool prev_ota = false;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
        diag_ota_t ota;
        diag_get_ota(&ota);
        const char *tgt = ota.target == OTA_TARGET_C5 ? "C5" : "P4";

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        if (ota.active && !s_live_ota) {
            // OTA started: always capture, in its own log
            if (s_live) live_commit("OTA (%s) started - continued in the next log", tgt);
            char reason[64];
            snprintf(reason, sizeof(reason), "OTA %s, %lu bytes", tgt, (unsigned long)ota.bytes_total);
            live_start(reason, OTA_START_CONTEXT);
            s_live_ota = s_live;
        } else if (s_live_ota && !ota.active) {
            // OTA ended without a deliberate reboot (failure); success normally ends via logstore_before_restart()
            if (ota.phase == OTA_PHASE_FAILED) {
                live_commit("OTA %s FAILED at %d%% (%lu / %lu bytes): %s", tgt, ota.pct, (unsigned long)ota.bytes_done,
                            (unsigned long)ota.bytes_total, ota.err);
            } else {
                live_commit("OTA %s ended, phase %s", tgt, diag_ota_phase_name(ota.phase));
            }
        } else if (!s_live && s_enabled && !ota.active) {
            live_start("session capture", logbuf_capacity());
        } else if (s_live && !s_enabled && !s_live_ota) {
            live_commit("capturing switched off");
        }

        if (s_live) {
            int64_t interval = (s_live_ota ? FLUSH_OTA_MS : FLUSH_SESSION_MS) * 1000LL;
            if (esp_timer_get_time() - s_live_flush_us >= interval && logbuf_total() != s_live_pos) {
                live_append();
                if (!s_live_ota) {
                    char path[48];
                    struct stat st;
                    logstore_path(LIVE_NAME, path, sizeof(path));
                    if (stat(path, &st) == 0 && st.st_size > LIVE_MAX_BYTES) {
                        live_commit("size limit reached - continued in the next log");
                    }
                }
            }
        }
        xSemaphoreGive(s_mutex);
        prev_ota = ota.active;
    }
    (void)prev_ota;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void logstore_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, NVS_KEY_ENABLED, &v) == ESP_OK) s_enabled = v != 0;
        if (nvs_get_u8(h, NVS_KEY_KEEP, &v) == ESP_OK && v >= LOGSTORE_KEEP_MIN && v <= LOGSTORE_KEEP_MAX) s_keep = v;
        uint32_t b = 0;
        nvs_get_u32(h, NVS_KEY_BOOTS, &b);
        s_boot = b + 1;
        nvs_set_u32(h, NVS_KEY_BOOTS, s_boot);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "Boot #%lu, session log capture %s, keep %u logs", (unsigned long)s_boot,
             s_enabled ? "ON" : "off (OTA is always captured)", s_keep);
    if (xTaskCreate(logstore_task, "logstore", 6144, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Cannot start logstore task");
    }
}

void logstore_get_info(logstore_info_t *out)
{
    memset(out, 0, sizeof(*out));
    out->mounted = s_mounted;
    out->enabled = s_enabled;
    out->capturing = s_live;
    out->ota = s_live_ota;
    out->keep = s_keep;
    out->boot = s_boot;
    uint64_t total = 0, free_b = 0;
    if (s_mounted && esp_vfs_fat_info(BASE_PATH, &total, &free_b) == ESP_OK) {
        out->total_kb = (uint32_t)(total / 1024);
        out->free_kb = (uint32_t)(free_b / 1024);
    }
}

esp_err_t logstore_configure(bool enabled, int keep)
{
    if (keep < LOGSTORE_KEEP_MIN || keep > LOGSTORE_KEEP_MAX) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_u8(h, NVS_KEY_ENABLED, enabled ? 1 : 0);
    nvs_set_u8(h, NVS_KEY_KEEP, (uint8_t)keep);
    err = nvs_commit(h);
    nvs_close(h);
    s_enabled = enabled;
    if (s_keep != keep) {
        s_keep = (uint8_t)keep;
        if (s_mounted && logstore_lock(5000)) {
            prune();
            logstore_unlock();
        }
    }
    ESP_LOGI(TAG, "Session log capture %s, keep %d logs", enabled ? "ON" : "off", keep);
    return err;
}

esp_err_t logstore_save_now(const char *reason)
{
    if (!s_mounted) return ESP_ERR_INVALID_STATE;
    size_t cap = logbuf_capacity() + 1;
    char *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!buf) return ESP_ERR_NO_MEM;
    size_t n = logbuf_read(buf, cap);
    esp_err_t err = ESP_ERR_TIMEOUT;
    if (logstore_lock(5000)) {
        err = write_entry(reason, buf, n, "snapshot of the RAM log");
        logstore_unlock();
    }
    free(buf);
    return err;
}

void logstore_before_restart(const char *reason)
{
    if (!s_mounted || !logstore_lock(3000)) return;
    if (s_live) {
        live_commit("%s", reason);
    } else if (s_enabled) {
        // Not capturing right now (e.g. between files): keep at least a snapshot
        size_t n = logbuf_read(s_chunk, CHUNK_SIZE);
        write_entry("before restart", s_chunk, n, reason);
    }
    logbuf_set_tail_flag(false);
    logstore_unlock();
}

int logstore_list(logstore_entry_t *out, int max)
{
    if (!s_mounted || !logstore_lock(3000)) return 0;
    // Collect names, newest first: LIVE first, then descending sequence
    char (*names)[LOGSTORE_NAME_LEN] = calloc(64, LOGSTORE_NAME_LEN);
    int n = 0;
    DIR *d = names ? opendir(LOG_DIR) : NULL;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL && n < 64) {
            if (logstore_name_ok(e->d_name)) strlcpy(names[n++], e->d_name, LOGSTORE_NAME_LEN);
        }
        closedir(d);
    }
    // Simple insertion sort: LIVE.TXT sorts first ('L' 'I' > digits), others by number descending
    for (int i = 1; i < n; i++) {
        char tmp[LOGSTORE_NAME_LEN];
        strlcpy(tmp, names[i], sizeof(tmp));
        int j = i - 1;
        while (j >= 0 && strcmp(names[j], tmp) < 0) {
            strlcpy(names[j + 1], names[j], LOGSTORE_NAME_LEN);
            j--;
        }
        strlcpy(names[j + 1], tmp, LOGSTORE_NAME_LEN);
    }
    int count = 0;
    for (int i = 0; i < n && count < max; i++) {
        logstore_entry_t *o = &out[count];
        memset(o, 0, sizeof(*o));
        strlcpy(o->name, names[i], sizeof(o->name));
        char path[48];
        logstore_path(o->name, path, sizeof(path));
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (fgets(o->head, sizeof(o->head), f)) o->head[strcspn(o->head, "\r\n")] = '\0';
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        o->size = size > 0 ? (uint32_t)size : 0;
        // Look for the last "# end:" line in the final 400 bytes
        char tail[401];
        long off = size > 400 ? size - 400 : 0;
        fseek(f, off, SEEK_SET);
        size_t r = fread(tail, 1, sizeof(tail) - 1, f);
        tail[r] = '\0';
        char *p = NULL, *q = tail;
        while ((q = strstr(q, "# end:")) != NULL) p = q++;
        if (p) {
            strlcpy(o->end, p + 7, sizeof(o->end));
            o->end[strcspn(o->end, "\r\n")] = '\0';
        }
        fclose(f);
        count++;
    }
    free(names);
    logstore_unlock();
    return count;
}

esp_err_t logstore_clear(void)
{
    if (!s_mounted) return ESP_ERR_INVALID_STATE;
    if (!logstore_lock(5000)) return ESP_ERR_TIMEOUT;
    DIR *d = opendir(LOG_DIR);
    if (d) {
        struct dirent *e;
        char path[48];
        while ((e = readdir(d)) != NULL) {
            if (!logstore_name_ok(e->d_name) || strcmp(e->d_name, LIVE_NAME) == 0) continue;
            logstore_path(e->d_name, path, sizeof(path));
            unlink(path);
        }
        closedir(d);
    }
    s_next_seq = 0;
    logstore_unlock();
    ESP_LOGI(TAG, "Saved logs cleared");
    return ESP_OK;
}
