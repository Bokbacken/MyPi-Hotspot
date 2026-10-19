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
#define CAMERA_MODE_MAX        1

typedef struct {
    int preview_quality, photo_quality;
    int idle_stop_s, max_photos, reserve_kb;
    bool hflip, vflip, live_preview;
    int width, height;          // Current sensor mode; not runtime-selectable.
} camera_settings_t;

typedef struct { int width, height; } camera_mode_t;

typedef struct {
    bool hflip, vflip;
    camera_mode_t modes[CAMERA_MODE_MAX];
    int mode_count;
} camera_capabilities_t;

static inline bool camera_settings_valid(const camera_settings_t *s)
{
    return s && s->preview_quality >= 10 && s->preview_quality <= 100 &&
           s->photo_quality >= 10 && s->photo_quality <= 100 &&
           s->idle_stop_s >= 2 && s->idle_stop_s <= 600 &&
           s->max_photos >= 1 && s->max_photos <= 500 &&
           s->reserve_kb >= 0 && s->reserve_kb <= 2048 &&
           ((s->width == 0 && s->height == 0) || (s->width > 0 && s->height > 0));
}

static inline bool camera_settings_supported(const camera_settings_t *s, const camera_capabilities_t *c)
{
    if (!camera_settings_valid(s) || !c || (s->hflip && !c->hflip) || (s->vflip && !c->vflip)) return false;
    if (!s->width && !s->height) return true;
    for (int i = 0; i < c->mode_count && i < CAMERA_MODE_MAX; i++) {
        if (s->width == c->modes[i].width && s->height == c->modes[i].height) return true;
    }
    return false;
}

// Fixed-width versioned NVS representation: no structure padding or C bool representation.
typedef struct {
    uint32_t version;
    int32_t preview_quality, photo_quality, idle_stop_s, max_photos, reserve_kb;
    uint32_t hflip, vflip, live_preview;
} camera_settings_blob_t;

static inline camera_settings_blob_t camera_settings_pack(const camera_settings_t *s)
{
    return (camera_settings_blob_t){1, s->preview_quality, s->photo_quality, s->idle_stop_s,
                                   s->max_photos, s->reserve_kb, s->hflip, s->vflip, s->live_preview};
}

static inline bool camera_settings_unpack(const camera_settings_blob_t *b, size_t len, camera_settings_t *s)
{
    if (!b || !s || len != sizeof(*b) || b->version != 1 ||
        b->hflip > 1 || b->vflip > 1 || b->live_preview > 1) return false;
    camera_settings_t v = {
        .preview_quality = b->preview_quality, .photo_quality = b->photo_quality,
        .idle_stop_s = b->idle_stop_s, .max_photos = b->max_photos, .reserve_kb = b->reserve_kb,
        .hflip = b->hflip, .vflip = b->vflip, .live_preview = b->live_preview,
    };
    if (!camera_settings_valid(&v)) return false;
    *s = v;
    return true;
}

/** Validate packed/padded rows before either the LCD scaler or JPEG encoder reads them. */
static inline bool camera_frame_valid(int w, int h, size_t stride, int bpp, size_t available)
{
    if (w <= 0 || h <= 0 || (bpp != 2 && bpp != 3) || (size_t)w > SIZE_MAX / (size_t)bpp) return false;
    size_t row = (size_t)w * bpp;
    return stride >= row && (size_t)h <= SIZE_MAX / stride && available >= stride * (size_t)h;
}

/** esp_video can omit bytesperline/sizeimage; actual buffer lengths are checked separately. */
static inline bool camera_frame_layout(int w, int h, int bpp, size_t bytesperline,
                                        size_t sizeimage, size_t *stride)
{
    if (!stride || w <= 0 || (bpp != 2 && bpp != 3) ||
        (size_t)w > SIZE_MAX / (size_t)bpp) return false;
    size_t row_stride = bytesperline ? bytesperline : (size_t)w * bpp;
    if (!camera_frame_valid(w, h, row_stride, bpp, sizeimage ? sizeimage : SIZE_MAX)) return false;
    *stride = row_stride;
    return true;
}

/** esp_video RGB24 maps to ISP_COLOR_RGB888 (BGR24 in IDF 6.1), matching JPEG RGB888 input. */
static inline bool camera_pack_jpeg_pixels(const uint8_t *src, int w, int h, size_t stride,
                                           int bpp, size_t available, uint8_t *dst, size_t dst_len)
{
    if (!src || !dst || !camera_frame_valid(w, h, stride, bpp, available)) return false;
    size_t row = (size_t)w * bpp;
    if (dst_len < row * (size_t)h) return false;
    for (int y = 0; y < h; y++) {
        memcpy(dst + (size_t)y * row, src + (size_t)y * stride, row);
    }
    return true;
}

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
    return free_kb >= reserve_kb && free_kb - reserve_kb >= need_kb;
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

/** CSI's ISP RGB888 is B,G,R byte order; RGB565 is little-endian (IDF hal/isp_types.h). */
static inline bool camera_scale_to_rgb565(const uint8_t *src, int src_w, int src_h, size_t stride,
                                          int bpp, size_t available, uint16_t *dst, int dst_w, int dst_h)
{
    if (!src || !dst || dst_w <= 0 || dst_h <= 0 ||
        !camera_frame_valid(src_w, src_h, stride, bpp, available)) return false;
    for (int y = 0; y < dst_h; y++) {
        const uint8_t *row = src + (size_t)((int64_t)y * src_h / dst_h) * stride;
        for (int x = 0; x < dst_w; x++) {
            size_t sx = (size_t)(((int64_t)x * 2 + 1) * src_w / ((int64_t)dst_w * 2));
            if (sx >= (size_t)src_w) sx = src_w - 1;
            const uint8_t *p = row + sx * bpp;
            *dst++ = bpp == 2 ? (uint16_t)(p[0] | (uint16_t)p[1] << 8) :
                       (uint16_t)((p[2] & 0xf8) << 8 | (p[1] & 0xfc) << 3 | p[0] >> 3);
        }
    }
    return true;
}
