#include "camera.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "photo_store.h"
#include "sdkconfig.h"

#if CONFIG_MYPI_CAMERA_ENABLE
#include <sys/ioctl.h>
#include "driver/jpeg_encode.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "esp_video_ioctl.h"
#include "linux/videodev2.h"
#include "pins.h"
#include "sys/mman.h"
#include "touch.h"
#endif

static const char *TAG = "camera";

static SemaphoreHandle_t s_lock;
static camera_settings_t s_settings;
static camera_capabilities_t s_caps;
static bool s_settings_ready;
static portMUX_TYPE s_status_mux = portMUX_INITIALIZER_UNLOCKED;
#if CONFIG_MYPI_CAMERA_ENABLE
static camera_status_t s_st = {.state = CAMERA_STATE_STARTING};
static camera_status_t s_status_snapshot = {.state = CAMERA_STATE_STARTING};
#else
static camera_status_t s_st = {.state = CAMERA_STATE_DISABLED};
static camera_status_t s_status_snapshot = {.state = CAMERA_STATE_DISABLED};
#endif

#define LOCK_TIMEOUT_MS     4000
#ifdef CONFIG_MYPI_CAMERA_HFLIP
#define CAM_HFLIP 1
#else
#define CAM_HFLIP 0
#endif
#ifdef CONFIG_MYPI_CAMERA_VFLIP
#define CAM_VFLIP 1
#else
#define CAM_VFLIP 0
#endif
// PSRAM that must stay free after a photo is stored (camera restarts, LVGL, HTTP, Wi-Fi buffers).
#define PSRAM_HEADROOM      (1024 * 1024)

// --- Volatile photo store (v0.11.0): JPEGs in PSRAM only, nothing on flash ---
static SemaphoreHandle_t s_store_lock;
EXT_RAM_BSS_ATTR static photo_store_t s_store;     // 4 KB index, photos are separate PSRAM blocks

static void *store_alloc(size_t size, void *ctx)
{
    (void)ctx;
    if (!esp_psram_is_initialized()) return NULL;
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < size + PSRAM_HEADROOM) return NULL;
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void store_free(void *ptr, void *ctx)
{
    (void)ctx;
    heap_caps_free(ptr);
}

static void store_lock(void *ctx) { (void)ctx; xSemaphoreTake(s_store_lock, portMAX_DELAY); }
static void store_unlock(void *ctx) { (void)ctx; xSemaphoreGive(s_store_lock); }

static const photo_store_ops_t STORE_OPS = {store_alloc, store_free, store_lock, store_unlock, NULL};

static bool store_ready(void) { return s_store_lock != NULL; }

static void store_init(void)
{
    if (s_store_lock) return;
    SemaphoreHandle_t m = xSemaphoreCreateMutex();
    if (!m) return;
    photo_store_init(&s_store, &STORE_OPS);
    s_store_lock = m;
}

void camera_settings_defaults(camera_settings_t *out)
{
    if (!out) return;
    *out = (camera_settings_t){
        .preview_quality = CONFIG_MYPI_CAMERA_PREVIEW_QUALITY,
        .photo_quality = CONFIG_MYPI_CAMERA_JPEG_QUALITY,
        .idle_stop_s = CONFIG_MYPI_CAMERA_IDLE_STOP_S,
        .max_photos = CONFIG_MYPI_CAMERA_MAX_PHOTOS,
        .store_kb = CONFIG_MYPI_CAMERA_PSRAM_STORE_KB,
        .hflip = CAM_HFLIP, .vflip = CAM_VFLIP, .live_preview = true,
    };
}

static void settings_load(void)
{
    camera_settings_defaults(&s_settings);
    nvs_handle_t h;
    if (nvs_open("camera", NVS_READONLY, &h) != ESP_OK) return;
    camera_settings_blob_t blob;
    size_t len = sizeof(blob);
    if (nvs_get_blob(h, "settings", &blob, &len) == ESP_OK &&
        !camera_settings_unpack(&blob, len, &s_settings)) {
        ESP_LOGW(TAG, "Ignoring invalid saved camera settings");
    }
    nvs_close(h);
}

static void set_error(esp_err_t err, const char *fmt, ...)
{
    s_st.last_err = err;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_st.last_error, sizeof(s_st.last_error), fmt, ap);
    va_end(ap);
    ESP_LOGW(TAG, "%s (%s)", s_st.last_error, esp_err_to_name(err));
}

#if CONFIG_MYPI_CAMERA_ENABLE
static void clear_error(void)
{
    s_st.last_err = ESP_OK;
    s_st.last_error[0] = '\0';
}
#endif

static bool lock(void)
{
    return s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(LOCK_TIMEOUT_MS)) == pdTRUE;
}

static void unlock(void)
{
    portENTER_CRITICAL(&s_status_mux);
    s_status_snapshot = s_st;
    portEXIT_CRITICAL(&s_status_mux);
    xSemaphoreGive(s_lock);
}

static bool settings_ready(void)
{
    portENTER_CRITICAL(&s_status_mux);
    bool ready = s_settings_ready;
    portEXIT_CRITICAL(&s_status_mux);
    return ready;
}

static void settings_mark_ready(void)
{
    portENTER_CRITICAL(&s_status_mux);
    s_settings_ready = true;
    portEXIT_CRITICAL(&s_status_mux);
}

esp_err_t camera_get_settings(camera_settings_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!settings_ready()) return ESP_ERR_INVALID_STATE;
    if (!lock()) return ESP_ERR_TIMEOUT;
    *out = s_settings;
    unlock();
    return ESP_OK;
}

esp_err_t camera_get_capabilities(camera_capabilities_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!settings_ready()) return ESP_ERR_INVALID_STATE;
    if (!lock()) return ESP_ERR_TIMEOUT;
    *out = s_caps;
    unlock();
    return ESP_OK;
}

#if CONFIG_MYPI_CAMERA_ENABLE
static esp_err_t settings_apply_flips(const camera_settings_t *settings);
static esp_err_t settings_apply_mode(int width, int height);
#endif

esp_err_t camera_save_settings(const camera_settings_t *in)
{
    if (!camera_settings_valid(in)) return ESP_ERR_INVALID_ARG;
    if (!settings_ready()) return ESP_ERR_INVALID_STATE;
    if (!lock()) return ESP_ERR_TIMEOUT;
    if (!camera_settings_supported(in, &s_caps)) {
        unlock();
        return ESP_ERR_NOT_SUPPORTED;
    }
    camera_settings_t previous = s_settings;
    camera_settings_t next = *in;
    next.width = in->width ? in->width : s_st.width;
    next.height = in->height ? in->height : s_st.height;
    bool mode_changed = next.width != s_st.width || next.height != s_st.height;
    bool flip_changed = next.hflip != s_settings.hflip || next.vflip != s_settings.vflip;
    esp_err_t err = ESP_OK;
    nvs_handle_t h;
    err = nvs_open("camera", NVS_READWRITE, &h);
    if (err == ESP_OK) {
#if CONFIG_MYPI_CAMERA_ENABLE
        if (mode_changed) err = settings_apply_mode(next.width, next.height);
        if (err == ESP_OK && flip_changed) err = settings_apply_flips(&next);
#else
        (void)mode_changed;
        (void)flip_changed;
#endif
        if (err == ESP_OK) {
            camera_settings_blob_t blob = camera_settings_pack(&next);
            err = nvs_set_blob(h, "settings", &blob, sizeof(blob));
            if (err == ESP_OK) err = nvs_commit(h);
        }
        nvs_close(h);
#if CONFIG_MYPI_CAMERA_ENABLE
        if (err != ESP_OK && mode_changed) {
            if (settings_apply_mode(previous.width, previous.height) != ESP_OK) {
                set_error(ESP_FAIL, "Camera resolution rollback failed");
            }
        }
        if (err != ESP_OK && flip_changed) {
            if (settings_apply_flips(&previous) != ESP_OK) {
                set_error(ESP_FAIL, "Camera flip rollback failed");
            }
        }
#endif
    }
    if (err == ESP_OK) s_settings = next;
    unlock();
    if (err == ESP_OK && store_ready()) {
        int pruned = photo_store_trim(&s_store, next.max_photos, (size_t)next.store_kb * 1024);
        if (pruned) ESP_LOGI(TAG, "Dropped %d old photo(s) for the new retention limits", pruned);
    }
    return err;
}

void camera_get_status(camera_status_t *out)
{
    if (lock()) {
        *out = s_st;
        unlock();
    } else {
        portENTER_CRITICAL(&s_status_mux);
        *out = s_status_snapshot;
        portEXIT_CRITICAL(&s_status_mux);
    }
}

int camera_photo_list(camera_photo_t *out, int max)
{
    if (!store_ready() || !out || max <= 0) return 0;
    if (max > PHOTO_STORE_MAX) max = PHOTO_STORE_MAX;
    photo_store_info_t *list = calloc((size_t)max, sizeof(*list));
    if (!list) return 0;
    int n = photo_store_list(&s_store, list, max);
    for (int i = 0; i < n; i++) {
        strlcpy(out[i].name, list[i].name, sizeof(out[i].name));
        out[i].size = list[i].size;
    }
    free(list);
    return n;
}

esp_err_t camera_photo_delete(const char *name)
{
    if (!camera_photo_name_ok(name)) return ESP_ERR_INVALID_ARG;
    if (!store_ready()) return ESP_ERR_NOT_FOUND;
    return photo_store_delete(&s_store, name) == PHOTO_STORE_OK ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t camera_photo_open(const char *name, camera_photo_ref_t **ref, const uint8_t **data, size_t *len)
{
    if (!ref || !data || !len) return ESP_ERR_INVALID_ARG;
    *ref = NULL;
    *data = NULL;
    *len = 0;
    if (!camera_photo_name_ok(name)) return ESP_ERR_INVALID_ARG;
    photo_blob_t *b = store_ready() ? photo_store_get(&s_store, name) : NULL;
    if (!b) return ESP_ERR_NOT_FOUND;
    *ref = b;
    *data = photo_blob_data(b);
    *len = photo_blob_len(b);
    return ESP_OK;
}

void camera_photo_close(camera_photo_ref_t *ref)
{
    if (ref && store_ready()) photo_store_put(&s_store, ref);
}

void camera_store_info(camera_store_info_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->psram = esp_psram_is_initialized();
    // Plain int reads (no camera lock): the status must not wait for a running capture.
    camera_settings_t st;
    if (settings_ready()) st = s_settings;
    else camera_settings_defaults(&st);
    out->max_photos = st.max_photos;
    out->budget_bytes = (uint32_t)st.store_kb * 1024;
    if (store_ready()) {
        photo_store_stats_t ps;
        photo_store_stats(&s_store, &ps);
        out->count = ps.count;
        out->used_bytes = (uint32_t)ps.used_bytes;
        out->pending_bytes = (uint32_t)ps.pending_bytes;
    }
    if (out->psram) {
        out->psram_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        out->psram_largest = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    }
}

#if CONFIG_MYPI_CAMERA_ENABLE

#define CAMERA_BUF_COUNT    2       // >= 2: the CSI driver keeps one buffer to receive into
#define DQBUF_TIMEOUT_MS    1000
#define WARMUP_MS           600     // Let auto exposure/white balance settle after stream start
#define IDLE_CHECK_MS       1000
#define JPEG_TIMEOUT_MS     500
#define RETRY_BACKOFF_MS    3000    // After a stream failure: fail fast instead of blocking every UI tick

static volatile int s_fd = -1;     // Read unlocked by idle_task as a cheap pre-check
static uint8_t *s_buf[CAMERA_BUF_COUNT];
static size_t s_buf_len[CAMERA_BUF_COUNT];
static uint32_t s_stride;
static int s_bpp;
static uint8_t *s_jpeg_input;
static int64_t s_stream_start_us;
static int64_t s_last_use_us;
static int64_t s_fail_us;
static jpeg_encoder_handle_t s_jpeg;
static uint8_t *s_jpeg_out;
static size_t s_jpeg_out_size;

static void set_failed(void)
{
    s_st.state = CAMERA_STATE_ERROR;
    s_fail_us = esp_timer_get_time();
}

static void stream_stop(void)
{
    if (s_fd < 0) return;
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(s_fd, VIDIOC_STREAMOFF, &type);
    for (int i = 0; i < CAMERA_BUF_COUNT; i++) {
        if (s_buf[i]) munmap(s_buf[i], s_buf_len[i]);
        s_buf[i] = NULL;
        s_buf_len[i] = 0;
    }
    close(s_fd);    // Frees the video buffers
    s_fd = -1;
    if (s_jpeg_out) {
        free(s_jpeg_out);
        s_jpeg_out = NULL;
        s_jpeg_out_size = 0;
    }
    free(s_jpeg_input);
    s_jpeg_input = NULL;
    if (s_st.state == CAMERA_STATE_STREAMING) s_st.state = CAMERA_STATE_READY;
    ESP_LOGI(TAG, "Stream stopped");
}

static esp_err_t apply_flip(int fd, const camera_settings_t *settings)
{
    struct v4l2_ext_control ctrl[2] = {
        {.id = V4L2_CID_HFLIP, .value = settings->hflip},
        {.id = V4L2_CID_VFLIP, .value = settings->vflip},
    };
    for (int i = 0; i < 2; i++) {
        if (!(i ? s_caps.vflip : s_caps.hflip)) continue;
        struct v4l2_ext_controls ctrls = {
            .ctrl_class = V4L2_CTRL_CLASS_USER,
            .count = 1,
            .controls = &ctrl[i],
        };
        if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &ctrls) != 0) return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_OK;
}

static bool probe_flip(int fd, uint32_t id)
{
    // This driver's get_para_value always returns NOT_SUPPORTED; do not trigger error logs.
    if (strcmp(s_st.sensor, "OV5647") == 0) return false;
    struct v4l2_ext_control ctrl = {.id = id};
    struct v4l2_ext_controls ctrls = {
        .ctrl_class = V4L2_CTRL_CLASS_USER, .count = 1, .controls = &ctrl,
    };
    // No readback means no safe validation/rollback (OV5647 currently lacks get_para_value).
    // Probe once, before any stream exists. Writing the current value verifies writability.
    return ioctl(fd, VIDIOC_G_EXT_CTRLS, &ctrls) == 0 &&
           (ctrl.value == 0 || ctrl.value == 1) && ioctl(fd, VIDIOC_S_EXT_CTRLS, &ctrls) == 0;
}

static esp_err_t settings_apply_flips(const camera_settings_t *settings)
{
    // Never open a second handle over an active stream or change controls during a capture.
    stream_stop();
    int fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDWR);
    if (fd < 0) return ESP_FAIL;
    esp_err_t err = apply_flip(fd, settings);
    close(fd);
    return err;
}

static bool select_format(int fd, struct v4l2_format *fmt, uint32_t pixel_format, int width, int height)
{
    fmt->fmt.pix.width = width;
    fmt->fmt.pix.height = height;
    fmt->fmt.pix.pixelformat = pixel_format;
    return ioctl(fd, VIDIOC_S_FMT, fmt) == 0 && ioctl(fd, VIDIOC_G_FMT, fmt) == 0 &&
           fmt->fmt.pix.pixelformat == pixel_format && fmt->fmt.pix.width == (uint32_t)width &&
           fmt->fmt.pix.height == (uint32_t)height;
}

static bool capture_format_set(int fd, int width, int height, struct v4l2_format *selected)
{
    const uint32_t formats[] = {V4L2_PIX_FMT_RGB24, V4L2_PIX_FMT_RGB565};
    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
        struct v4l2_format fmt = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
        if (ioctl(fd, VIDIOC_G_FMT, &fmt) == 0 &&
            select_format(fd, &fmt, formats[i], width, height)) {
            if (selected) *selected = fmt;
            return true;
        }
    }
    return false;
}

static bool sensor_format_set(int fd, int width, int height)
{
    for (uint32_t index = 0; index < CAMERA_MODE_MAX; index++) {
        struct v4l2_sensor_format_enum sensor_format = {.index = index};
        if (ioctl(fd, VIDIOC_ENUM_SENSOR_FMT, &sensor_format) != 0) break;
        if (sensor_format.format.width == width && sensor_format.format.height == height) {
            return ioctl(fd, VIDIOC_S_SENSOR_FMT, &sensor_format.format) == 0;
        }
    }
    return false;
}

static void camera_mode_add(camera_capabilities_t *caps, uint32_t width, uint32_t height)
{
    if (width < 16 || height < 16 || width > UINT16_MAX || height > UINT16_MAX) return;
    for (int i = 0; i < caps->mode_count; i++) {
        if ((uint32_t)caps->modes[i].width == width && (uint32_t)caps->modes[i].height == height) return;
    }
    if (caps->mode_count < CAMERA_MODE_MAX) {
        caps->modes[caps->mode_count++] = (camera_mode_t){(int)width, (int)height};
    }
}

static void camera_modes_enumerate(int fd, camera_capabilities_t *caps, uint32_t current_width,
                                   uint32_t current_height)
{
    caps->mode_count = 0;
    for (uint32_t index = 0; index < CAMERA_MODE_MAX; index++) {
        struct v4l2_sensor_format_enum sensor_format = {.index = index};
        if (ioctl(fd, VIDIOC_ENUM_SENSOR_FMT, &sensor_format) != 0) break;
        camera_mode_add(caps, sensor_format.format.width, sensor_format.format.height);
    }
    if (caps->mode_count == 0) {
        const uint32_t formats[] = {V4L2_PIX_FMT_RGB24, V4L2_PIX_FMT_RGB565};
        for (size_t f = 0; f < sizeof(formats) / sizeof(formats[0]); f++) {
            for (uint32_t index = 0; index < CAMERA_MODE_MAX; index++) {
                struct v4l2_frmsizeenum size = {
                    .index = index, .pixel_format = formats[f],
                };
                if (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &size) != 0) break;
                if (size.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
                    camera_mode_add(caps, size.discrete.width, size.discrete.height);
                }
            }
        }
    }
    camera_mode_add(caps, current_width, current_height);
}

static esp_err_t settings_apply_mode(int width, int height)
{
    stream_stop();
    int fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDWR);
    if (fd < 0) {
        set_error(ESP_FAIL, "Camera open failed while setting resolution (errno %d)", errno);
        return ESP_FAIL;
    }
    struct v4l2_format fmt;
    bool applied = sensor_format_set(fd, width, height) &&
                   capture_format_set(fd, width, height, &fmt);
    close(fd);
    if (!applied) {
        set_error(ESP_ERR_NOT_SUPPORTED, "Camera rejected resolution %dx%d", width, height);
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_st.width = (uint16_t)fmt.fmt.pix.width;
    s_st.height = (uint16_t)fmt.fmt.pix.height;
    s_st.state = CAMERA_STATE_READY;
    clear_error();
    return ESP_OK;
}

/** Open the device, prefer full-colour RGB888 and start streaming. Lock held. */
static esp_err_t stream_start(void)
{
    if (s_fd >= 0) return ESP_OK;
    if (!camera_state_usable(s_st.state)) return ESP_ERR_INVALID_STATE;

    int fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDWR);
    if (fd < 0) {
        set_error(ESP_FAIL, "Camera open failed (errno %d)", errno);
        return ESP_FAIL;
    }
    esp_err_t err = ESP_FAIL;
    struct v4l2_format fmt = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
    if (ioctl(fd, VIDIOC_G_FMT, &fmt) != 0) {
        set_error(ESP_FAIL, "Camera format query failed (errno %d)", errno);
        goto fail;
    }
    if (!capture_format_set(fd, s_st.width, s_st.height, &fmt)) {
        set_error(ESP_ERR_NOT_SUPPORTED, "Camera cannot output %ux%u as RGB888 or RGB565 (errno %d)",
                  s_st.width, s_st.height, errno);
        err = ESP_ERR_NOT_SUPPORTED;
        goto fail;
    }
    if (fmt.fmt.pix.width < 16 || fmt.fmt.pix.height < 16 ||
        fmt.fmt.pix.width != s_st.width || fmt.fmt.pix.height != s_st.height) {
        set_error(ESP_ERR_INVALID_SIZE, "Camera reported an invalid frame size");
        err = ESP_ERR_INVALID_SIZE;
        goto fail;
    }
    s_st.width = (uint16_t)fmt.fmt.pix.width;
    s_st.height = (uint16_t)fmt.fmt.pix.height;
    s_bpp = fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_RGB24 ? 3 : 2;
    size_t stride;
    if (!camera_frame_layout(s_st.width, s_st.height, s_bpp, fmt.fmt.pix.bytesperline,
                             fmt.fmt.pix.sizeimage, &stride) || stride > UINT32_MAX) {
        set_error(ESP_ERR_INVALID_SIZE, "Camera reported an invalid frame stride/size");
        err = ESP_ERR_INVALID_SIZE;
        goto fail;
    }
    s_stride = (uint32_t)stride;
    err = apply_flip(fd, &s_settings);
    if (err != ESP_OK) {
        set_error(err, "Camera orientation could not be applied");
        goto fail;
    }
    err = ESP_FAIL;

#ifdef VIDIOC_S_DQBUF_TIMEOUT
    struct timeval tv = {.tv_sec = DQBUF_TIMEOUT_MS / 1000, .tv_usec = (DQBUF_TIMEOUT_MS % 1000) * 1000};
    ioctl(fd, VIDIOC_S_DQBUF_TIMEOUT, &tv);     // Never block the UI task forever on a dead stream
#endif

    struct v4l2_requestbuffers req = {
        .count = CAMERA_BUF_COUNT,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    if (ioctl(fd, VIDIOC_REQBUFS, &req) != 0) {
        set_error(ESP_ERR_NO_MEM, "Camera buffer allocation failed (PSRAM)");
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    for (int i = 0; i < CAMERA_BUF_COUNT; i++) {
        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
            .index = i,
        };
        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) != 0) {
            set_error(ESP_FAIL, "Camera buffer query failed");
            goto fail;
        }
        if (!camera_frame_valid(s_st.width, s_st.height, s_stride, s_bpp, buf.length)) {
            set_error(ESP_ERR_INVALID_SIZE, "Camera buffer is smaller than the frame layout");
            err = ESP_ERR_INVALID_SIZE;
            goto fail;
        }
        s_buf[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
        if (!s_buf[i] || s_buf[i] == MAP_FAILED) {
            s_buf[i] = NULL;
            set_error(ESP_ERR_NO_MEM, "Camera buffer mapping failed");
            err = ESP_ERR_NO_MEM;
            goto fail;
        }
        s_buf_len[i] = buf.length;
        if (ioctl(fd, VIDIOC_QBUF, &buf) != 0) {
            set_error(ESP_FAIL, "Camera buffer queue failed");
            goto fail;
        }
    }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_STREAMON, &type) != 0) {
        set_error(ESP_FAIL, "Camera stream start failed (errno %d)", errno);
        goto fail;
    }
    s_fd = fd;
    s_stream_start_us = esp_timer_get_time();
    s_last_use_us = s_stream_start_us;
    s_st.state = CAMERA_STATE_STREAMING;
    clear_error();
    ESP_LOGI(TAG, "Stream started: %ux%u RGB%d, %d buffers", s_st.width, s_st.height, s_bpp * 8, CAMERA_BUF_COUNT);
    return ESP_OK;

fail:
    for (int i = 0; i < CAMERA_BUF_COUNT; i++) {
        if (s_buf[i]) munmap(s_buf[i], s_buf_len[i]);
        s_buf[i] = NULL;
        s_buf_len[i] = 0;
    }
    close(fd);
    set_failed();
    return err;
}

/** Dequeue one frame (lock held). On failure the stream is closed so the next call restarts it. */
static esp_err_t frame_get(struct v4l2_buffer *buf)
{
    esp_err_t err = stream_start();
    if (err != ESP_OK) return err;
    memset(buf, 0, sizeof(*buf));
    buf->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf->memory = V4L2_MEMORY_MMAP;
    if (ioctl(s_fd, VIDIOC_DQBUF, buf) != 0 || buf->index >= CAMERA_BUF_COUNT || !s_buf[buf->index]) {
        set_error(ESP_ERR_TIMEOUT, "No frame from the camera within %d ms", DQBUF_TIMEOUT_MS);
        stream_stop();
        set_failed();
        return ESP_ERR_TIMEOUT;
    }
    size_t available = buf->bytesused ? buf->bytesused : s_buf_len[buf->index];
    if (available > s_buf_len[buf->index] ||
        !camera_frame_valid(s_st.width, s_st.height, s_stride, s_bpp, available)) {
        set_error(ESP_ERR_INVALID_SIZE, "Camera returned a truncated/invalid frame");
        stream_stop();
        set_failed();
        return ESP_ERR_INVALID_SIZE;
    }
    s_last_use_us = esp_timer_get_time();
    s_st.frames++;
    return ESP_OK;
}

static void frame_put(struct v4l2_buffer *buf)
{
    if (s_fd >= 0) ioctl(s_fd, VIDIOC_QBUF, buf);
}

/** A frame captured after this call (skips the queued one and the AE/AWB warm-up). Lock held. */
static esp_err_t frame_get_fresh(struct v4l2_buffer *buf)
{
    esp_err_t err = frame_get(buf);
    // The first queued buffer may hold an old frame; after a stream start, wait for AE/AWB.
    bool dropped = false;
    while (err == ESP_OK && (!dropped || esp_timer_get_time() - s_stream_start_us < WARMUP_MS * 1000LL)) {
        frame_put(buf);
        err = frame_get(buf);
        dropped = true;
    }
    return err;
}

/** Encode the dequeued frame into s_jpeg_out (lock held). */
static esp_err_t encode(const struct v4l2_buffer *buf, int quality, size_t *len)
{
    if (!buf || buf->index >= CAMERA_BUF_COUNT || !s_buf[buf->index] ||
        !camera_frame_valid(s_st.width, s_st.height, s_stride, s_bpp,
                            buf->bytesused ? buf->bytesused : s_buf_len[buf->index])) {
        set_error(ESP_ERR_INVALID_SIZE, "Invalid JPEG input frame");
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = ESP_OK;
    if (!s_jpeg) {
        jpeg_encode_engine_cfg_t cfg = {.timeout_ms = JPEG_TIMEOUT_MS};
        err = jpeg_new_encoder_engine(&cfg, &s_jpeg);
        if (err != ESP_OK) {
            s_jpeg = NULL;
            set_error(err, "JPEG encoder init failed");
            return err;
        }
    }
    if (!s_jpeg_out) {
        // Allow full-colour, high-quality output plus header/MCU overhead.
        jpeg_encode_memory_alloc_cfg_t mem = {.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER};
        s_jpeg_out = jpeg_alloc_encoder_mem((size_t)s_st.width * s_st.height * 3 + 1024, &mem, &s_jpeg_out_size);
        if (!s_jpeg_out) {
            s_jpeg_out_size = 0;
            set_error(ESP_ERR_NO_MEM, "No memory for the JPEG buffer");
            return ESP_ERR_NO_MEM;
        }
    }
    size_t row = (size_t)s_st.width * s_bpp;
    size_t input_len = row * s_st.height;
    uint8_t *input = s_buf[buf->index];
    // esp_video's RGB24 is ISP_COLOR_RGB888 = BGR24 (IDF hal/isp_types.h), as JPEG expects.
    // No colour swap is needed; the JPEG API only requires removal of row padding.
    if (s_stride != row) {
        if (!s_jpeg_input) {
            size_t allocated = 0;
            jpeg_encode_memory_alloc_cfg_t mem = {.buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER};
            s_jpeg_input = jpeg_alloc_encoder_mem(input_len, &mem, &allocated);
            if (!s_jpeg_input) {
                set_error(ESP_ERR_NO_MEM, "No memory for packed JPEG input");
                return ESP_ERR_NO_MEM;
            }
        }
        if (!camera_pack_jpeg_pixels(input, s_st.width, s_st.height, s_stride, s_bpp,
                                     buf->bytesused ? buf->bytesused : s_buf_len[buf->index],
                                     s_jpeg_input, input_len)) {
            set_error(ESP_ERR_INVALID_SIZE, "Invalid packed JPEG input");
            return ESP_ERR_INVALID_SIZE;
        }
        input = s_jpeg_input;
    }
    jpeg_encode_cfg_t cfg = {
        .width = s_st.width,
        .height = s_st.height,
        .src_type = s_bpp == 3 ? JPEG_ENCODE_IN_FORMAT_RGB888 : JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample = JPEG_DOWN_SAMPLING_YUV444,
        .image_quality = quality < 1 ? 1 : (quality > 100 ? 100 : quality),
        .pixel_reverse = false,
    };
    uint32_t out_len = 0;
    err = jpeg_encoder_process(s_jpeg, &cfg, input, input_len,
                               s_jpeg_out, s_jpeg_out_size, &out_len);
    if (err != ESP_OK || out_len == 0 || out_len > s_jpeg_out_size) {
        set_error(err != ESP_OK ? err : ESP_FAIL, "JPEG encoding failed");
        return err != ESP_OK ? err : ESP_FAIL;
    }
    *len = out_len;
    return ESP_OK;
}

/** Lock held. Not detected keeps the init failure text: it explains why the camera is unavailable. */
static esp_err_t check_usable(void)
{
    if (s_st.state == CAMERA_STATE_ERROR && esp_timer_get_time() - s_fail_us < RETRY_BACKOFF_MS * 1000LL) {
        return s_st.last_err != ESP_OK ? s_st.last_err : ESP_FAIL;
    }
    if (camera_state_usable(s_st.state)) return ESP_OK;
    if (s_st.state == CAMERA_STATE_STARTING) set_error(ESP_ERR_INVALID_STATE, "Camera is not initialized yet");
    return ESP_ERR_INVALID_STATE;
}

esp_err_t camera_preview_rgb565(uint16_t *dst, int max_w, int max_h, int *out_w, int *out_h)
{
    if (!dst || max_w <= 0 || max_h <= 0 || !out_w || !out_h) return ESP_ERR_INVALID_ARG;
    if (!lock()) return ESP_ERR_TIMEOUT;
    esp_err_t err = check_usable();
    struct v4l2_buffer buf;
    if (err == ESP_OK) err = frame_get(&buf);
    if (err == ESP_OK) {
        camera_fit(s_st.width, s_st.height, max_w, max_h, out_w, out_h);
        if (!camera_scale_to_rgb565(s_buf[buf.index], s_st.width, s_st.height, s_stride, s_bpp,
                                   buf.bytesused ? buf.bytesused : s_buf_len[buf.index], dst, *out_w, *out_h)) {
            err = ESP_ERR_INVALID_SIZE;
        }
        frame_put(&buf);
    }
    unlock();
    return err;
}

esp_err_t camera_snapshot_jpeg(int quality, uint8_t **jpeg, size_t *len)
{
    if (!jpeg || !len) return ESP_ERR_INVALID_ARG;
    *jpeg = NULL;
    *len = 0;
    if (!lock()) return ESP_ERR_TIMEOUT;
    esp_err_t err = check_usable();
    struct v4l2_buffer buf;
    size_t n = 0;
    if (err == ESP_OK) err = frame_get_fresh(&buf);
    if (err == ESP_OK) {
        err = encode(&buf, quality == 0 ? s_settings.preview_quality : quality, &n);
        frame_put(&buf);
    }
    if (err == ESP_OK) {
        // Copy out so the (slow) HTTP send does not hold the camera.
        *jpeg = malloc(n);
        if (*jpeg) {
            memcpy(*jpeg, s_jpeg_out, n);
            *len = n;
        } else {
            set_error(ESP_ERR_NO_MEM, "No memory for the preview frame");
            err = ESP_ERR_NO_MEM;
        }
    }
    unlock();
    return err;
}

esp_err_t camera_snapshot_gray(camera_gray_alloc_t alloc, void *ctx, int *w_out, int *h_out)
{
    if (!alloc || !w_out || !h_out) return ESP_ERR_INVALID_ARG;
    *w_out = *h_out = 0;
    if (!lock()) return ESP_ERR_TIMEOUT;
    esp_err_t err = check_usable();
    struct v4l2_buffer buf;
    if (err == ESP_OK) err = frame_get_fresh(&buf);
    if (err == ESP_OK) {
        int step = camera_gray_step(s_st.width, s_st.height);
        int w = s_st.width / step, h = s_st.height / step;
        uint8_t *dst = alloc(w, h, ctx);
        if (!dst) {
            err = ESP_ERR_NO_MEM;
        } else if (!camera_frame_to_gray(s_buf[buf.index], s_st.width, s_st.height, s_stride, s_bpp,
                                         buf.bytesused ? buf.bytesused : s_buf_len[buf.index], step, dst, w, h)) {
            err = ESP_ERR_INVALID_SIZE;
        } else {
            *w_out = w;
            *h_out = h;
        }
        frame_put(&buf);
    }
    unlock();
    return err;
}

esp_err_t camera_capture_save(char *name_out, size_t name_size, uint32_t *bytes_out,
                              camera_capture_result_t *result)
{
    if (name_out && name_size) name_out[0] = '\0';
    if (bytes_out) *bytes_out = 0;
    if (result) {
        result->stage = CAMERA_STAGE_NONE;
        result->error[0] = '\0';
    }
    if (!lock()) {
        if (result) {
            result->stage = CAMERA_STAGE_CAPTURE;
            strlcpy(result->error, "Camera busy, try again", sizeof(result->error));
        }
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = check_usable();
    s_st.capture_stage = err == ESP_OK ? CAMERA_STAGE_CAPTURE :
                         (s_st.state == CAMERA_STATE_ERROR ? CAMERA_STAGE_CAPTURE : CAMERA_STAGE_CAMERA);
    struct v4l2_buffer buf;
    size_t len = 0;
    if (err == ESP_OK) err = frame_get_fresh(&buf);
    if (err == ESP_OK) {
        err = encode(&buf, s_settings.photo_quality, &len);
        frame_put(&buf);
    }
    if (err != ESP_OK) {
        if (result) {
            result->stage = s_st.capture_stage;
            strlcpy(result->error, s_st.last_error[0] ? s_st.last_error : esp_err_to_name(err),
                    sizeof(result->error));
        }
        unlock();
        return err;
    }
    s_st.capture_stage = CAMERA_STAGE_STORAGE;

    // Storage: a copy in PSRAM only (never flash). Allocation happens before old photos are dropped.
    unsigned long kb = (unsigned long)(len + 1023) / 1024;
    char name[CAMERA_PHOTO_NAME_LEN] = "";
    photo_store_err_t pe = store_ready() ?
        photo_store_add(&s_store, s_jpeg_out, len, s_settings.max_photos, (size_t)s_settings.store_kb * 1024, name) :
        PHOTO_STORE_ERR_NO_MEM;
    if (pe == PHOTO_STORE_ERR_TOO_LARGE) {
        set_error(ESP_ERR_INVALID_SIZE, "Photo (%lu KB) exceeds the PSRAM photo budget (%d KB)", kb, s_settings.store_kb);
        err = ESP_ERR_INVALID_SIZE;
    } else if (pe != PHOTO_STORE_OK) {
        set_error(ESP_ERR_NO_MEM, esp_psram_is_initialized() ?
                  "Out of PSRAM for the photo (%lu KB): delete photos or lower quality/resolution" :
                  "No PSRAM: photos cannot be stored (%lu KB)", kb);
        err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        clear_error();
        s_st.capture_stage = CAMERA_STAGE_NONE;
        s_st.photos_taken++;
        strlcpy(s_st.last_photo, name, sizeof(s_st.last_photo));
        if (name_out) strlcpy(name_out, name, name_size);
        if (bytes_out) *bytes_out = (uint32_t)len;
        ESP_LOGI(TAG, "Stored %s in PSRAM (%u bytes, %ux%u)", name, (unsigned)len, s_st.width, s_st.height);
    } else if (result) {
        result->stage = s_st.capture_stage;
        strlcpy(result->error, s_st.last_error[0] ? s_st.last_error : esp_err_to_name(err),
                sizeof(result->error));
    }
    unlock();
    return err;
}

static void idle_task(void *arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(IDLE_CHECK_MS));
        if (s_fd < 0) continue;
        if (xSemaphoreTake(s_lock, 0) != pdTRUE) continue;  // In use right now
        if (s_fd >= 0 && esp_timer_get_time() - s_last_use_us > s_settings.idle_stop_s * 1000000LL) {
            stream_stop();
        }
        unlock();
    }
}

esp_err_t camera_init(void)
{
    if (s_lock) return ESP_OK;
    store_init();
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_caps = (camera_capabilities_t){0};
    settings_load();

    // SCCB: the camera connector shares the board I2C bus (GPIO7/8) with the touch controller.
    // Reuse the touch driver's bus; create it here only if touch did not.
    esp_video_init_csi_config_t csi = {
        .reset_pin = CONFIG_MYPI_CAMERA_RESET_GPIO,
        .pwdn_pin = CONFIG_MYPI_CAMERA_PWDN_GPIO,
    };
    i2c_master_bus_handle_t bus = touch_i2c_bus();   // Marks the bus as shared (no bus re-create)
    if (bus) {
        csi.sccb_config.init_sccb = false;
        csi.sccb_config.i2c_handle = bus;
    } else {
        csi.sccb_config.init_sccb = true;
        csi.sccb_config.i2c_config.port = TOUCH_I2C_PORT;
        csi.sccb_config.i2c_config.scl_pin = PIN_TP_SCL;
        csi.sccb_config.i2c_config.sda_pin = PIN_TP_SDA;
    }
    csi.sccb_config.freq = CONFIG_MYPI_CAMERA_SCCB_KHZ * 1000;
    esp_video_init_config_t cfg = {.csi = &csi};

    esp_err_t err = esp_video_init(&cfg);
    int fd = err == ESP_OK ? open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDWR) : -1;
    struct v4l2_capability cap = {0};
    struct v4l2_format fmt = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
    if (fd >= 0 && ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0 && ioctl(fd, VIDIOC_G_FMT, &fmt) == 0 &&
        fmt.fmt.pix.width >= 16 && fmt.fmt.pix.width <= UINT16_MAX &&
        fmt.fmt.pix.height >= 16 && fmt.fmt.pix.height <= UINT16_MAX) {
        uint32_t detected_width = fmt.fmt.pix.width, detected_height = fmt.fmt.pix.height;
        camera_modes_enumerate(fd, &s_caps, detected_width, detected_height);
        camera_settings_t requested = s_settings;
        requested.hflip = requested.vflip = false;
        if ((requested.width || requested.height) && camera_settings_supported(&requested, &s_caps) &&
            (requested.width != (int)detected_width || requested.height != (int)detected_height)) {
            if (sensor_format_set(fd, requested.width, requested.height) &&
                capture_format_set(fd, requested.width, requested.height, &fmt)) {
                detected_width = fmt.fmt.pix.width;
                detected_height = fmt.fmt.pix.height;
            } else {
                sensor_format_set(fd, detected_width, detected_height);
                capture_format_set(fd, detected_width, detected_height, NULL);
            }
        }
        // QUERYCAP reports the interface ("MIPI-CSI"); the sensor is the one enabled driver
        // that answered the auto-detect (the project enables only OV5647 by default).
#ifdef CONFIG_CAMERA_OV5647
        strlcpy(s_st.sensor, "OV5647", sizeof(s_st.sensor));
#else
        strlcpy(s_st.sensor, (const char *)cap.card, sizeof(s_st.sensor));
#endif
        s_st.width = (uint16_t)detected_width;
        s_st.height = (uint16_t)detected_height;
        s_st.state = CAMERA_STATE_READY;
        s_caps.hflip = probe_flip(fd, V4L2_CID_HFLIP);
        s_caps.vflip = probe_flip(fd, V4L2_CID_VFLIP);
        if (!s_caps.hflip) s_settings.hflip = false;
        if (!s_caps.vflip) s_settings.vflip = false;
        s_settings.width = s_st.width;
        s_settings.height = s_st.height;
        clear_error();
    } else {
        s_st.state = CAMERA_STATE_NOT_DETECTED;
        s_settings.hflip = s_settings.vflip = false;
        if (err != ESP_OK) {
            set_error(err, "Camera init failed: %s", esp_err_to_name(err));
        } else {
            set_error(ESP_ERR_NOT_FOUND, "Camera not detected: check the CSI ribbon cable");
        }
    }
    if (fd >= 0) close(fd);
    settings_mark_ready();
    if (s_st.state != CAMERA_STATE_READY) {
        unlock();
        return s_st.last_err;
    }

    ESP_LOGI(TAG, "Camera %s detected, %ux%u (SCCB %s bus, %d kHz)", s_st.sensor, s_st.width, s_st.height,
             bus ? "shared touch" : "own", CONFIG_MYPI_CAMERA_SCCB_KHZ);
    if (xTaskCreate(idle_task, "camera_idle", 3072, NULL, 1, NULL) != pdPASS) {
        ESP_LOGW(TAG, "Idle-stop task not started; the stream keeps running once started");
    }
    unlock();
    return ESP_OK;
}

#else   // !CONFIG_MYPI_CAMERA_ENABLE

esp_err_t camera_init(void)
{
    if (s_lock) return ESP_ERR_NOT_SUPPORTED;
    store_init();
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    settings_load();
    s_settings.hflip = s_settings.vflip = false;
    set_error(ESP_ERR_NOT_SUPPORTED, "Camera support disabled in menuconfig (MyPi-Hotspot > Camera)");
    settings_mark_ready();
    unlock();
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t camera_preview_rgb565(uint16_t *dst, int max_w, int max_h, int *out_w, int *out_h)
{
    (void)dst; (void)max_w; (void)max_h; (void)out_w; (void)out_h;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t camera_snapshot_jpeg(int quality, uint8_t **jpeg, size_t *len)
{
    (void)quality;
    if (jpeg) *jpeg = NULL;
    if (len) *len = 0;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t camera_snapshot_gray(camera_gray_alloc_t alloc, void *ctx, int *w_out, int *h_out)
{
    (void)alloc; (void)ctx;
    if (w_out) *w_out = 0;
    if (h_out) *h_out = 0;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t camera_capture_save(char *name_out, size_t name_size, uint32_t *bytes_out,
                              camera_capture_result_t *result)
{
    if (name_out && name_size) name_out[0] = '\0';
    if (bytes_out) *bytes_out = 0;
    if (result) {
        result->stage = CAMERA_STAGE_CAMERA;
        strlcpy(result->error, "Camera support is disabled", sizeof(result->error));
    }
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
