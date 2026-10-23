#pragma once

// QR code extraction from a grayscale image with the vendored quirc library. Portable C (no
// ESP-IDF dependency) so the host test can decode real QR images. quirc_decode() needs ~10 KB of
// stack: on the device qr_scan() runs in wifi_prov's worker task.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "quirc.h"

#define QR_PAYLOAD_MAX 512      // Same limit as WQ_PAYLOAD_MAX: longer texts are not Wi-Fi codes we accept

typedef struct {
    int codes;              // QR symbols located
    int decoded;            // ... and decoded
    int wifi;               // Distinct WIFI: payloads
    int other;              // Decoded non-Wi-Fi payloads (never used to change settings)
    bool too_long;          // The Wi-Fi payload exceeded QR_PAYLOAD_MAX (payload is empty)
    char payload[QR_PAYLOAD_MAX + 1];   // The first WIFI: payload (NUL-terminated copy)
    size_t payload_len;
} qr_scan_result_t;

/**
 * Decode every QR code in the image currently loaded into `q` (quirc_begin/quirc_end done by the
 * caller). Mirrored codes are retried flipped. Returns false only if working memory could not be
 * allocated. The caller must wipe `out` (it may hold a password) when done.
 */
bool qr_scan(struct quirc *q, qr_scan_result_t *out);
