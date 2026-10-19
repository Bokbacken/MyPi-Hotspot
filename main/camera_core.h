#pragma once

// Camera helpers without ESP-IDF dependencies (host-tested in tests/test_ui_primitives.c):
// photo file names, preview geometry/scaling, storage reserve and pruning, state names.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Photos are 8.3 names (FAT without long file names): IMG00001.JPG .. IMG99999.JPG.
#define CAMERA_PHOTO_NAME_LEN   13
#define CAMERA_PHOTO_SEQ_MAX    99999

typedef enum {
    CAMERA_STATE_DISABLED,      // CONFIG_MYPI_CAMERA_ENABLE is off
    CAMERA_STATE_STARTING,      // camera_init() not finished yet
    CAMERA_STATE_NOT_DETECTED,  // Init failed / no sensor answered on SCCB
    CAMERA_STATE_READY,         // Sensor found, stream stopped (no PSRAM buffers)
    CAMERA_STATE_STREAMING,     // Stream running (preview/capture in use)
    CAMERA_STATE_ERROR,         // Last stream/capture attempt failed; retried on the next request
} camera_state_t;

static inline const char *camera_state_name(camera_state_t state)
{
    switch (state) {
    case CAMERA_STATE_DISABLED:     return "disabled";
    case CAMERA_STATE_STARTING:     return "starting";
    case CAMERA_STATE_NOT_DETECTED: return "not_detected";
    case CAMERA_STATE_READY:        return "ready";
    case CAMERA_STATE_STREAMING:    return "streaming";
    case CAMERA_STATE_ERROR:        return "error";
    }
    return "unknown";
}

/** Where the last photo capture failed: shown as the error source in both UIs. */
typedef enum {
    CAMERA_STAGE_NONE,          // No failure
    CAMERA_STAGE_CAMERA,        // Camera disabled / not detected / init failed
    CAMERA_STAGE_CAPTURE,       // Stream, frame or JPEG encoding failed
    CAMERA_STAGE_STORAGE,       // Storage not mounted, full or the write failed
} camera_stage_t;

static inline const char *camera_stage_name(camera_stage_t stage)
{
    switch (stage) {
    case CAMERA_STAGE_CAMERA:  return "camera";
    case CAMERA_STAGE_CAPTURE: return "capture";
    case CAMERA_STAGE_STORAGE: return "storage";
    default:                   return "none";
    }
}

/** True for preview/capture capable states (the sensor was detected). */
static inline bool camera_state_usable(camera_state_t state)
{
    return state == CAMERA_STATE_READY || state == CAMERA_STATE_STREAMING || state == CAMERA_STATE_ERROR;
}

static inline bool camera_photo_name_ok(const char *name)
{
    if (!name || strlen(name) != CAMERA_PHOTO_NAME_LEN - 1) return false;
    if (memcmp(name, "IMG", 3) != 0 || memcmp(name + 8, ".JPG", 4) != 0) return false;
    for (int i = 3; i < 8; i++) {
        if (name[i] < '0' || name[i] > '9') return false;
    }
    return true;
}

/** Sequence number of a photo name, -1 if the name is not a photo. */
static inline long camera_photo_seq(const char *name)
{
    if (!camera_photo_name_ok(name)) return -1;
    long seq = 0;
    for (int i = 3; i < 8; i++) seq = seq * 10 + (name[i] - '0');
    return seq;
}

/** Writes "IMGnnnnn.JPG"; false if seq is out of range or dst is too small. */
static inline bool camera_photo_name(long seq, char *dst, size_t dst_size)
{
    if (!dst || dst_size < CAMERA_PHOTO_NAME_LEN || seq < 1 || seq > CAMERA_PHOTO_SEQ_MAX) return false;
    snprintf(dst, dst_size, "IMG%05ld.JPG", seq);
    return true;
}

/** Number of oldest photos to delete before saving one more, so at most `keep` remain. */
static inline int camera_prune_count(int count, int keep)
{
    if (keep < 1) keep = 1;
    return count >= keep ? count - keep + 1 : 0;
}

/** Enough free space for a photo of need_kb while leaving reserve_kb for the log store. */
static inline bool camera_storage_ok(uint32_t free_kb, uint32_t need_kb, uint32_t reserve_kb)
{
    return free_kb >= need_kb + reserve_kb;
}

/** Largest size with the source aspect ratio that fits into max_w x max_h (at least 1x1). */
static inline void camera_fit(int src_w, int src_h, int max_w, int max_h, int *out_w, int *out_h)
{
    int w = max_w, h = max_h;
    if (src_w > 0 && src_h > 0 && max_w > 0 && max_h > 0) {
        if ((long)src_w * max_h > (long)src_h * max_w) {
            h = (int)((long)src_h * max_w / src_w);
        } else {
            w = (int)((long)src_w * max_h / src_h);
        }
    }
    *out_w = w < 1 ? 1 : w;
    *out_h = h < 1 ? 1 : h;
}

/**
 * Nearest-neighbour downscale of an RGB565 frame (src_stride in pixels) into dst (dst_w x dst_h,
 * packed). Cheap enough for a 5 Hz LCD preview; pixels are copied as-is (same byte order).
 */
static inline void camera_scale_rgb565(const uint16_t *src, int src_w, int src_h, int src_stride,
                                       uint16_t *dst, int dst_w, int dst_h)
{
    if (!src || !dst || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) return;
    if (src_stride < src_w) src_stride = src_w;
    uint32_t x_step = ((uint32_t)src_w << 16) / (uint32_t)dst_w;
    for (int y = 0; y < dst_h; y++) {
        const uint16_t *row = src + (size_t)((long)y * src_h / dst_h) * (size_t)src_stride;
        uint32_t sx = x_step / 2;
        for (int x = 0; x < dst_w; x++) {
            *dst++ = row[sx >> 16];
            sx += x_step;
        }
    }
}
