#pragma once

// MIPI-CSI camera (v0.10.0): OV5647 module ("P5V04A SUNNY" / Raspberry Pi camera v1.3 type) on the
// ESP32-P4-NANO camera connector, driven by Espressif's esp_video (V4L2) + esp_cam_sensor stack.
//
// * camera_init() only probes the sensor (SCCB on the shared touch I2C bus, GPIO7/8). No frame
//   buffers are allocated until a preview or capture is requested.
// * The stream starts on demand and stops after the runtime idle timeout without use, so the
//   ISP/CSI do not load PSRAM bandwidth while nobody looks at the camera.
// * Photos are JPEGs (hardware encoder) kept only in external PSRAM (v0.11.0, photo_store.h): never
//   written to flash, so the photo list is empty after every reboot. Retention: max_photos and the
//   store_kb byte budget (oldest first). Photo files from older versions in /store/photos on the
//   FAT partition are left untouched and are not listed.
// All functions are thread-safe.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "camera_core.h"
#include "esp_err.h"

#define CAMERA_ERROR_LEN    96

typedef struct {
    camera_state_t state;
    char sensor[24];                // Sensor name reported by the driver (e.g. "OV5647")
    uint16_t width, height;         // Capture resolution
    uint32_t frames;                // Frames delivered since boot
    uint32_t photos_taken;          // Photos stored since boot
    esp_err_t last_err;             // ESP_OK when the last operation succeeded
    char last_error[CAMERA_ERROR_LEN];  // Human-readable reason of the last failure ("" = none)
    char last_photo[CAMERA_PHOTO_NAME_LEN];
    camera_stage_t capture_stage;   // Where the last completed camera_capture_save() failed
} camera_status_t;

typedef struct {
    char name[CAMERA_PHOTO_NAME_LEN];
    uint32_t size;
} camera_photo_t;

typedef struct {
    camera_stage_t stage;
    char error[CAMERA_ERROR_LEN];
} camera_capture_result_t;

/** Volatile PSRAM photo store usage. */
typedef struct {
    bool psram;                     // External PSRAM available (otherwise photos cannot be stored)
    int count, max_photos;
    uint32_t used_bytes;            // Listed photos
    uint32_t pending_bytes;         // Deleted/pruned photos still being downloaded
    uint32_t budget_bytes;          // store_kb * 1024
    uint32_t psram_free, psram_largest;     // Whole PSRAM heap (shared with the camera buffers)
} camera_store_info_t;

/** Opaque reference that keeps a photo's PSRAM memory alive while it is read. */
typedef struct photo_blob camera_photo_ref_t;

/** Probe the sensor once (call after touch_init(): the camera shares its I2C bus). */
esp_err_t camera_init(void);

void camera_get_status(camera_status_t *out);

/** Defaults from menuconfig; dimensions are 0/0 (keep the detected sensor mode). */
void camera_settings_defaults(camera_settings_t *out);
/** Settings/capability APIs return ESP_ERR_INVALID_STATE until initialization finishes loading. */
esp_err_t camera_get_settings(camera_settings_t *out);
/** Validate, apply and atomically persist. Dimensions must be 0/0 or an enumerated sensor mode.
 * Unsupported enabled flips/modes return ESP_ERR_NOT_SUPPORTED; bad ranges INVALID_ARG.
 * Quality 10..100, idle 2..600 s, retention 1..500, PSRAM budget 256..12288 KB. Lower retention
 * limits prune the oldest photos immediately.
 * live_preview is the web client's automatic-preview preference, not a capture gate.
 */
esp_err_t camera_save_settings(const camera_settings_t *in);
/** Cached during init; never probes a streaming sensor. Flips require safe readback/write support;
 *  modes are enumerated from the stopped capture device.
 */
esp_err_t camera_get_capabilities(camera_capabilities_t *out);

/**
 * Grab the newest frame and scale it (aspect kept) into dst, at most max_w x max_h RGB565 pixels.
 * The real size is returned in *out_w / *out_h. Starts the stream if needed.
 */
esp_err_t camera_preview_rgb565(uint16_t *dst, int max_w, int max_h, int *out_w, int *out_h);

/** Fresh full-resolution JPEG frame; quality=0 uses runtime preview quality. Caller frees *jpeg. */
esp_err_t camera_snapshot_jpeg(int quality, uint8_t **jpeg, size_t *len);

/** Returns a w x h 8-bit buffer to fill (e.g. quirc_resize + quirc_begin), or NULL. */
typedef uint8_t *(*camera_gray_alloc_t)(int w, int h, void *ctx);

/**
 * Fresh grayscale frame for QR decoding (luma, no JPEG round trip). Frames wider/taller than
 * 1280 px are subsampled by 2. alloc is called with the camera lock held.
 */
esp_err_t camera_snapshot_gray(camera_gray_alloc_t alloc, void *ctx, int *w_out, int *h_out);

/**
 * Take a photo and keep it in the PSRAM photo store as IMGnnnnn.JPG (the oldest photos beyond
 * max_photos / the store_kb budget are dropped once the new photo is allocated). name_out
 * receives the name. Errors: ESP_ERR_INVALID_STATE camera not detected, ESP_ERR_TIMEOUT /
 * ESP_FAIL capture, ESP_ERR_INVALID_SIZE photo larger than the budget, ESP_ERR_NO_MEM out of
 * PSRAM (existing photos are kept). Nothing is written to flash. result reports this request's
 * failure without changing the shared status if the camera is busy; camera_get_status()
 * reports the last completed operation.
 */
esp_err_t camera_capture_save(char *name_out, size_t name_size, uint32_t *bytes_out,
                              camera_capture_result_t *result);

/** Stored photos, newest first. Returns the count (<= max). */
int camera_photo_list(camera_photo_t *out, int max);

/** ESP_ERR_INVALID_ARG bad name, ESP_ERR_NOT_FOUND unknown photo. A running download keeps its copy. */
esp_err_t camera_photo_delete(const char *name);

/**
 * Reference a photo for reading (download). The data stays valid until camera_photo_close(),
 * even if the photo is deleted or pruned meanwhile. ESP_ERR_INVALID_ARG / ESP_ERR_NOT_FOUND.
 */
esp_err_t camera_photo_open(const char *name, camera_photo_ref_t **ref, const uint8_t **data, size_t *len);
void camera_photo_close(camera_photo_ref_t *ref);

void camera_store_info(camera_store_info_t *out);
