#include "qr_decode.h"

#include <stdlib.h>
#include <string.h>

static bool is_wifi(const uint8_t *p, size_t len)
{
    static const char prefix[] = "WIFI:";
    if (len < 5) return false;
    for (int i = 0; i < 5; i++) {
        char c = (char)p[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (c != prefix[i]) return false;
    }
    return true;
}

bool qr_scan(struct quirc *q, qr_scan_result_t *out)
{
    memset(out, 0, sizeof(*out));
    // Both are several KB: keep them off the stack.
    struct quirc_code *code = malloc(sizeof(*code));
    struct quirc_data *data = malloc(sizeof(*data));
    if (!code || !data) {
        free(code);
        free(data);
        return false;
    }
    out->codes = quirc_count(q);
    for (int i = 0; i < out->codes; i++) {
        quirc_extract(q, i, code);
        quirc_decode_error_t err = quirc_decode(code, data);
        if (err == QUIRC_ERROR_DATA_ECC) {      // Possibly a mirrored image
            quirc_flip(code);
            err = quirc_decode(code, data);
        }
        if (err != QUIRC_SUCCESS) continue;
        out->decoded++;
        size_t len = (size_t)data->payload_len;
        if (!is_wifi(data->payload, len)) {
            out->other++;
        } else if (len > QR_PAYLOAD_MAX) {
            if (out->wifi++ == 0) out->too_long = true;
        } else if (out->wifi == 0) {
            memcpy(out->payload, data->payload, len);
            out->payload[len] = '\0';
            out->payload_len = len;
            out->wifi = 1;
        } else if (out->too_long || len != out->payload_len || memcmp(out->payload, data->payload, len) != 0) {
            out->wifi++;    // A different Wi-Fi code (identical duplicates count once)
        }
        memset(data, 0, sizeof(*data));
    }
    memset(code, 0, sizeof(*code));
    free(code);
    free(data);
    return true;
}
