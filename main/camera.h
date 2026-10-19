#pragma once

// MIPI-CSI camera (v0.10.0): OV5647 module ("P5V04A SUNNY" / Raspberry Pi camera v1.3 type) on the
// ESP32-P4-NANO camera connector, driven by Espressif's esp_video (V4L2) + esp_cam_sensor stack.
//
// * camera_init() only probes the sensor (SCCB on the shared touch I2C bus, GPIO7/8). No frame
//   buffers are allocated until a preview or capture is requested.
// * The stream starts on demand and stops after CONFIG_MYPI_CAMERA_IDLE_STOP_S without use, so the
//   ISP/CSI do not load PSRAM bandwidth while nobody looks at the camera.
// * Photos are JPEG files (hardware encoder) in /store/photos on the FAT "storage" partition shared
//   with the log store (logstore_lock() serializes file access).
// All functions are thread-safe.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "camera_core.h"
#include "esp_err.h"

#define CAMERA_PHOTO_DIR    "/store/photos"
#define CAMERA_ERROR_LEN    96

typedef struct {
    camera_state_t state;
    char sensor[24];                // Sensor name reported by the driver (e.g. "OV5647")
    uint16_t width, height;         // Capture resolution
    uint32_t frames;                // Frames delivered since boot
    uint32_t photos_taken;          // Photos saved since boot
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

/** Probe the sensor once (call after touch_init(): the camera shares its I2C bus). */
esp_err_t camera_init(void);

void camera_get_status(camera_status_t *out);

/**
 * Grab the newest frame and scale it (aspect kept) into dst, at most max_w x max_h RGB565 pixels.
 * The real size is returned in *out_w / *out_h. Starts the stream if needed.
 */
esp_err_t camera_preview_rgb565(uint16_t *dst, int max_w, int max_h, int *out_w, int *out_h);

/** Fresh full-resolution JPEG frame (web preview). The caller frees *jpeg with free(). */
esp_err_t camera_snapshot_jpeg(int quality, uint8_t **jpeg, size_t *len);

/**
 * Take a photo and save it as /store/photos/IMGnnnnn.JPG (oldest photos beyond
 * CONFIG_MYPI_CAMERA_MAX_PHOTOS are deleted first). name_out receives the file name.
 * Errors: ESP_ERR_INVALID_STATE camera not detected, ESP_ERR_TIMEOUT / ESP_FAIL capture,
 * ESP_ERR_NOT_FOUND storage not mounted, ESP_ERR_NO_MEM storage full or out of memory,
 * ESP_ERR_INVALID_SIZE write failed. result reports this request's failure without changing
 * the shared status if the camera is busy; camera_get_status() reports the last completed operation.
 */
esp_err_t camera_capture_save(char *name_out, size_t name_size, uint32_t *bytes_out,
                              camera_capture_result_t *result);

/** Saved photos, newest first. Returns the count (<= max). */
int camera_photo_list(camera_photo_t *out, int max);

esp_err_t camera_photo_delete(const char *name);

/** Full path of a photo; false if the name is not a valid photo name. */
bool camera_photo_path(const char *name, char *dst, size_t dst_size);
