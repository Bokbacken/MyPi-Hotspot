#include "logbuf.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static char *s_buf;
static size_t s_size;
static size_t s_head;       // Next write position
static bool s_wrapped;
static vprintf_like_t s_orig_vprintf;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static void ring_write(const char *data, size_t len)
{
    if (len >= s_size) {    // Keep only the tail of an oversized line
        data += len - (s_size - 1);
        len = s_size - 1;
    }
    taskENTER_CRITICAL(&s_lock);
    size_t first = s_size - s_head;
    if (first > len) first = len;
    memcpy(s_buf + s_head, data, first);
    memcpy(s_buf, data + first, len - first);
    s_head += len;
    if (s_head >= s_size) {
        s_head -= s_size;
        s_wrapped = true;
    }
    taskEXIT_CRITICAL(&s_lock);
}

static int log_hook(const char *fmt, va_list args)
{
    char line[192];
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(line, sizeof(line), fmt, copy);
    va_end(copy);
    if (n > 0 && s_buf) {
        size_t len = (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1;
        ring_write(line, len);
    }
    return s_orig_vprintf ? s_orig_vprintf(fmt, args) : vprintf(fmt, args);
}

esp_err_t logbuf_init(size_t size)
{
    s_buf = heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM);
    if (!s_buf) {
        s_buf = heap_caps_calloc(1, size, MALLOC_CAP_DEFAULT);
    }
    if (!s_buf) {
        return ESP_ERR_NO_MEM;
    }
    s_size = size;
    s_orig_vprintf = esp_log_set_vprintf(log_hook);
    return ESP_OK;
}

size_t logbuf_read(char *dst, size_t dst_size)
{
    if (!dst || dst_size == 0) return 0;
    if (!s_buf) {
        dst[0] = '\0';
        return 0;
    }
    taskENTER_CRITICAL(&s_lock);
    size_t used = s_wrapped ? s_size : s_head;
    size_t start = s_wrapped ? s_head : 0;
    size_t n = used < dst_size - 1 ? used : dst_size - 1;
    // Skip the oldest bytes if dst is smaller than the buffer
    start = (start + (used - n)) % s_size;
    size_t first = s_size - start;
    if (first > n) first = n;
    memcpy(dst, s_buf + start, first);
    memcpy(dst + first, s_buf, n - first);
    taskEXIT_CRITICAL(&s_lock);
    dst[n] = '\0';
    return n;
}

size_t logbuf_capacity(void)
{
    return s_size;
}
