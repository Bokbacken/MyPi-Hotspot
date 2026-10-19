#include "logbuf.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "time_service.h"

static char *s_buf;
static size_t s_size;
static size_t s_head;       // Next write position
static bool s_wrapped;
static uint64_t s_total;    // Bytes ever written (monotonic)

// Crash tail: ring in PSRAM (v0.8.6, was 8 KB internal) that the startup code does not clear,
// so it survives esp_restart(), panics and watchdog resets (power loss wipes it).
#define TAIL_SIZE   32768
#define TAIL_MAGIC  0x4D795054u     // "MyPT"
typedef struct {
    uint32_t magic;
    uint32_t head;
    uint32_t wrapped;
    uint32_t flag;                  // Session was capturing -> persist this tail after a reset
    uint32_t check;                 // ~magic ^ head-independent marker, guards against garbage
    char buf[TAIL_SIZE];
} tail_t;
static EXT_RAM_NOINIT_ATTR tail_t s_tail;
static char *s_prev_tail;           // Copy of the previous session's tail (PSRAM)
static bool s_prev_flag;
static vprintf_like_t s_orig_vprintf;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static int output_text(vprintf_like_t output, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int result = output(fmt, args);
    va_end(args);
    return result;
}

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
    s_total += len;
    // Crash tail (len < s_size; keep only what fits)
    const char *t = data;
    size_t tl = len;
    if (tl >= TAIL_SIZE) {
        t += tl - (TAIL_SIZE - 1);
        tl = TAIL_SIZE - 1;
    }
    size_t tf = TAIL_SIZE - s_tail.head;
    if (tf > tl) tf = tl;
    memcpy(s_tail.buf + s_tail.head, t, tf);
    memcpy(s_tail.buf, t + tf, tl - tf);
    s_tail.head += tl;
    if (s_tail.head >= TAIL_SIZE) {
        s_tail.head -= TAIL_SIZE;
        s_tail.wrapped = 1;
    }
    taskEXIT_CRITICAL(&s_lock);
}

static int log_hook(const char *fmt, va_list args)
{
    // Preserve errno: callers such as httpd_sock_err() log first and inspect errno afterwards
    // (EAGAIN -> timeout). vsnprintf/vprintf may clobber it, turning timeouts into hard errors.
    int saved_errno = errno;
    char line[224];
    char stamped[288];
    char timestamp[48];
    if (!time_service_format(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S %z")) {
        uint32_t ms = esp_log_timestamp();
        snprintf(timestamp, sizeof(timestamp), "uptime+%lu.%03lu",
                 (unsigned long)(ms / 1000), (unsigned long)(ms % 1000));
    }
    int prefix_len = snprintf(stamped, sizeof(stamped), "[%s] ", timestamp);
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(line, sizeof(line), fmt, copy);
    va_end(copy);
    if (prefix_len < 0) prefix_len = 0;
    if ((size_t)prefix_len >= sizeof(stamped)) prefix_len = sizeof(stamped) - 1;
    size_t body_len = n > 0 ? (size_t)n : 0;
    if (body_len >= sizeof(line)) body_len = sizeof(line) - 1;
    if (body_len > sizeof(stamped) - (size_t)prefix_len - 1) {
        body_len = sizeof(stamped) - (size_t)prefix_len - 1;
    }
    memcpy(stamped + prefix_len, line, body_len);
    stamped[prefix_len + body_len] = '\0';
    if (n > 0 && s_buf) {
        ring_write(stamped, (size_t)prefix_len + body_len);
    }
    vprintf_like_t output = s_orig_vprintf ? s_orig_vprintf : vprintf;
    int ret = output_text(output, "%.*s", prefix_len, stamped);
    int body_ret = output(fmt, args);
    if (body_ret < 0) ret = body_ret;
    else ret += body_ret;
    errno = saved_errno;
    return ret;
}

static void tail_takeover(void)
{
    esp_reset_reason_t rr = esp_reset_reason();
    bool valid = s_tail.magic == TAIL_MAGIC && s_tail.check == ~TAIL_MAGIC && s_tail.head < TAIL_SIZE &&
                 rr != ESP_RST_POWERON && rr != ESP_RST_UNKNOWN && rr != ESP_RST_BROWNOUT;
    if (valid) {
        size_t used = s_tail.wrapped ? TAIL_SIZE : s_tail.head;
        size_t start = s_tail.wrapped ? s_tail.head : 0;
        if (used > 0) {
            s_prev_tail = heap_caps_malloc(used + 1, MALLOC_CAP_SPIRAM);
            if (!s_prev_tail) s_prev_tail = malloc(used + 1);
        }
        if (s_prev_tail) {
            size_t first = TAIL_SIZE - start;
            if (first > used) first = used;
            memcpy(s_prev_tail, s_tail.buf + start, first);
            memcpy(s_prev_tail + first, s_tail.buf, used - first);
            s_prev_tail[used] = '\0';
            // Drop non-text bytes (a partly overwritten line may start mid UTF-8 sequence; that is fine)
            for (size_t i = 0; i < used; i++) {
                if (s_prev_tail[i] == '\0') s_prev_tail[i] = ' ';
            }
            s_prev_flag = s_tail.flag != 0;
        }
    }
    memset(&s_tail, 0, sizeof(s_tail));
    s_tail.magic = TAIL_MAGIC;
    s_tail.check = ~TAIL_MAGIC;
}

esp_err_t logbuf_init(size_t size)
{
    tail_takeover();
    s_orig_vprintf = esp_log_set_vprintf(log_hook);
    s_buf = heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM);
    if (!s_buf) {
        s_buf = heap_caps_calloc(1, size, MALLOC_CAP_DEFAULT);
    }
    if (!s_buf) {
        return ESP_ERR_NO_MEM;
    }
    s_size = size;
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

uint64_t logbuf_total(void)
{
    taskENTER_CRITICAL(&s_lock);
    uint64_t t = s_total;
    taskEXIT_CRITICAL(&s_lock);
    return t;
}

size_t logbuf_read_since(uint64_t *pos, char *dst, size_t dst_size, uint64_t *lost)
{
    if (lost) *lost = 0;
    if (!pos || !dst || dst_size == 0) return 0;
    dst[0] = '\0';
    if (!s_buf) return 0;
    taskENTER_CRITICAL(&s_lock);
    uint64_t total = s_total;
    size_t used = s_wrapped ? s_size : s_head;
    uint64_t want = total > *pos ? total - *pos : 0;
    if (want > used) {
        if (lost) *lost = want - used;
        want = used;
    }
    size_t n = want < dst_size - 1 ? (size_t)want : dst_size - 1;
    if (lost && n < want) *lost += want - n;
    // The newest n bytes end at s_head
    size_t start = (s_head + s_size - n) % s_size;
    size_t first = s_size - start;
    if (first > n) first = n;
    memcpy(dst, s_buf + start, first);
    memcpy(dst + first, s_buf, n - first);
    taskEXIT_CRITICAL(&s_lock);
    dst[n] = '\0';
    *pos = total;
    return n;
}

void logbuf_set_tail_flag(bool on)
{
    s_tail.flag = on ? 1 : 0;
}

const char *logbuf_prev_tail(bool *flagged)
{
    if (flagged) *flagged = s_prev_flag;
    return s_prev_tail;
}
