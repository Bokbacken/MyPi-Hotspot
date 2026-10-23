#pragma once

// Wi-Fi QR provisioning helpers without ESP-IDF dependencies (host-tested in tests/test_wifi_qr.c):
// strict "WIFI:" payload parser, scan visibility, profile slot choice and the connect/rollback
// transaction state machine used by wifi_prov.c.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define WQ_PAYLOAD_MAX  512     // Longer QR texts are rejected before parsing
#define WQ_SSID_MAX     32      // Bytes (802.11)
#define WQ_PASS_MAX     64      // 8-63 character passphrase or 64 hex digit PSK

// Security of a QR network / saved profile. Same values as STA_SEC_* in profiles.h
// (0 = legacy profile without a stored security type).
typedef enum {
    WQ_SEC_OPEN = 1,    // T:nopass, or no T and no password
    WQ_SEC_WPA = 2,     // T:WPA (or no T with a password): WPA/WPA2/WPA3 personal, minimum WPA-PSK
    WQ_SEC_WPA2 = 3,    // T:WPA2: minimum WPA2-PSK
    WQ_SEC_WPA3 = 4,    // T:SAE / T:WPA3, or R:1 (transition disable): WPA3-SAE only
} wq_security_t;

typedef enum {
    WQ_OK = 0,
    WQ_ERR_NOT_WIFI,            // Not a WIFI: QR code (nothing is changed)
    WQ_ERR_TOO_LONG,
    WQ_ERR_MALFORMED,           // Syntax: bad field, bad escape, text after the end, NUL byte
    WQ_ERR_DUPLICATE_FIELD,
    WQ_ERR_SSID,                // Missing/empty, > 32 bytes, control characters or invalid UTF-8
    WQ_ERR_PASSWORD,            // Not 8-63 printable ASCII characters or 64 hex digits
    WQ_ERR_PASSWORD_MISSING,    // Secured network without P
    WQ_ERR_OPEN_WITH_PASSWORD,  // T:nopass with a password: ambiguous
    WQ_ERR_UNSUPPORTED_SECURITY,// WEP, WPA2/WPA3-Enterprise (EAP fields), unknown T value
    WQ_ERR_SAE_PK,              // K: SAE-PK public key verification is not supported
    WQ_ERR_HIDDEN,              // H is not true/false
    WQ_ERR_TRANSITION,          // R is not hex, requests an unsupported mode, or R:1 on an open network
} wq_err_t;

typedef struct {
    char ssid[WQ_SSID_MAX + 1];
    size_t ssid_len;
    char pass[WQ_PASS_MAX + 1];
    size_t pass_len;
    bool pass_is_psk;           // 64 hex digits (raw PSK)
    wq_security_t security;
    char type[16];              // T value as written (display only), "" if absent
    bool hidden;                // H:true
    bool transition_disable;    // R bit 0 (WPA3 required)
    char ignored[48];           // Unknown field names that were ignored, comma separated
} wq_wifi_t;

static inline const char *wq_err_text(wq_err_t e)
{
    switch (e) {
    case WQ_OK:                       return "ok";
    case WQ_ERR_NOT_WIFI:             return "not a Wi-Fi QR code (no WIFI: prefix)";
    case WQ_ERR_TOO_LONG:             return "QR text too long for a Wi-Fi code";
    case WQ_ERR_MALFORMED:            return "malformed Wi-Fi QR code";
    case WQ_ERR_DUPLICATE_FIELD:      return "Wi-Fi QR code repeats a field";
    case WQ_ERR_SSID:                 return "SSID missing, longer than 32 bytes, or not printable UTF-8";
    case WQ_ERR_PASSWORD:             return "password must be 8-63 printable ASCII characters or 64 hex digits";
    case WQ_ERR_PASSWORD_MISSING:     return "secured network without a password";
    case WQ_ERR_OPEN_WITH_PASSWORD:   return "open network (T:nopass) with a password";
    case WQ_ERR_UNSUPPORTED_SECURITY: return "unsupported security (WEP and enterprise/EAP networks are not supported)";
    case WQ_ERR_SAE_PK:               return "WPA3 SAE-PK (K field) is not supported";
    case WQ_ERR_HIDDEN:               return "H (hidden) must be true or false";
    case WQ_ERR_TRANSITION:           return "unsupported R (transition disable) value";
    }
    return "error";
}

static inline const char *wq_security_name(int sec)
{
    switch (sec) {
    case WQ_SEC_OPEN: return "open";
    case WQ_SEC_WPA:  return "wpa";
    case WQ_SEC_WPA2: return "wpa2";
    case WQ_SEC_WPA3: return "wpa3";
    default:          return "auto";
    }
}

static inline char wq_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

static inline bool wq_ieq(const char *a, size_t alen, const char *b)
{
    size_t bl = strlen(b);
    if (alen != bl) return false;
    for (size_t i = 0; i < alen; i++) {
        if (wq_lower(a[i]) != wq_lower(b[i])) return false;
    }
    return true;
}

static inline bool wq_is_hex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/** Valid UTF-8 without control characters (SSIDs are shown in the UI and JSON). */
static inline bool wq_printable_utf8(const char *s, size_t len)
{
    const unsigned char *p = (const unsigned char *)s;
    for (size_t i = 0; i < len;) {
        unsigned c = p[i];
        size_t n;
        uint32_t cp;
        if (c < 0x20 || c == 0x7F) return false;
        if (c < 0x80) { i++; continue; }
        if (c >= 0xC2 && c <= 0xDF) { n = 1; cp = c & 0x1F; }
        else if (c >= 0xE0 && c <= 0xEF) { n = 2; cp = c & 0x0F; }
        else if (c >= 0xF0 && c <= 0xF4) { n = 3; cp = c & 0x07; }
        else return false;
        if (i + n >= len) return false;     // Truncated sequence
        for (size_t k = 1; k <= n; k++) {
            if (i + k >= len || (p[i + k] & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (p[i + k] & 0x3F);
        }
        if ((n == 2 && cp < 0x800) || (n == 3 && (cp < 0x10000 || cp > 0x10FFFF)) ||
            (cp >= 0xD800 && cp <= 0xDFFF) || (cp >= 0x80 && cp < 0xA0)) {
            return false;   // Overlong, out of range, surrogate or C1 control
        }
        i += n + 1;
    }
    return true;
}

static inline void wq_add_ignored(wq_wifi_t *w, const char *key, size_t klen)
{
    size_t used = strlen(w->ignored);
    if (used + klen + 2 >= sizeof(w->ignored)) return;
    if (used) w->ignored[used++] = ',';
    memcpy(w->ignored + used, key, klen);
    w->ignored[used + klen] = '\0';
}

/**
 * Parse a WIFI: QR payload (ZXing "MECARD-like" format, plus the WPA3 specification's R/K fields):
 *   WIFI:S:<ssid>;T:<WPA|WPA2|SAE|WPA3|nopass>;P:<password>;H:<true|false>;R:<hex>;;
 * Fields may appear in any order and are separated by ';'. '\' escapes one of \ ; , : " (other
 * escapes are malformed). A value wrapped in unescaped double quotes is unquoted (a quoted
 * 64-character password is a passphrase, not a hex PSK). The final ";" may be omitted; trailing
 * whitespace after ";;" is ignored. Unknown fields are ignored and listed in `ignored`, except
 * enterprise (E, A, I, PH2) and SAE-PK (K) fields, which are rejected because ignoring them would
 * connect with weaker security than the code describes. On error `out` holds no password.
 */
static inline wq_err_t wq_parse(const char *p, size_t len, wq_wifi_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!p || len < 5 || !wq_ieq(p, 5, "WIFI:")) return WQ_ERR_NOT_WIFI;
    if (len > WQ_PAYLOAD_MAX) return WQ_ERR_TOO_LONG;

    enum { F_S = 1, F_T = 2, F_P = 4, F_H = 8, F_R = 16 };
    unsigned seen = 0;
    bool pass_quoted = false;
    char hval[8] = "", rval[12] = "";
    size_t hlen = 0, rlen = 0;
    wq_err_t err = WQ_OK;
    size_t i = 5;

    while (i < len && err == WQ_OK) {
        if (p[i] == ';') {      // Empty field: end of the payload (";;")
            i++;
            while (i < len && (p[i] == ' ' || p[i] == '\t' || p[i] == '\r' || p[i] == '\n')) i++;
            if (i != len) err = WQ_ERR_MALFORMED;
            break;
        }
        size_t ks = i;
        while (i < len && p[i] != ':' && p[i] != ';') {
            char c = p[i];
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) break;
            i++;
        }
        size_t klen = i - ks;
        if (i >= len || p[i] != ':' || klen == 0 || klen > 3) { err = WQ_ERR_MALFORMED; break; }
        const char *key = p + ks;
        i++;

        char val[WQ_PAYLOAD_MAX + 1];
        size_t vl = 0;
        bool first_quote = false, last_quote = false;
        while (i < len && p[i] != ';') {
            char c = p[i];
            bool escaped = false;
            if (c == '\0') { err = WQ_ERR_MALFORMED; break; }
            if (c == '\\') {
                if (i + 1 >= len || !strchr("\\;,:\"", p[i + 1]) || p[i + 1] == '\0') { err = WQ_ERR_MALFORMED; break; }
                c = p[i + 1];
                escaped = true;
                i += 2;
            } else {
                i++;
            }
            if (vl == 0) first_quote = !escaped && c == '"';
            last_quote = !escaped && c == '"';
            val[vl++] = c;      // vl < len <= WQ_PAYLOAD_MAX
        }
        if (err != WQ_OK) break;
        if (i < len) i++;       // Field terminator (missing only at the very end)
        bool quoted = false;
        if (vl >= 2 && first_quote && last_quote) {
            memmove(val, val + 1, vl - 2);
            vl -= 2;
            quoted = true;
        }
        val[vl] = '\0';

        unsigned flag = 0;
        if (wq_ieq(key, klen, "S")) flag = F_S;
        else if (wq_ieq(key, klen, "T")) flag = F_T;
        else if (wq_ieq(key, klen, "P")) flag = F_P;
        else if (wq_ieq(key, klen, "H")) flag = F_H;
        else if (wq_ieq(key, klen, "R")) flag = F_R;
        else if (wq_ieq(key, klen, "K")) err = WQ_ERR_SAE_PK;
        else if (wq_ieq(key, klen, "E") || wq_ieq(key, klen, "A") || wq_ieq(key, klen, "I") ||
                 wq_ieq(key, klen, "PH2")) err = WQ_ERR_UNSUPPORTED_SECURITY;
        else wq_add_ignored(out, key, klen);
        if (!flag) {
            memset(val, 0, sizeof(val));
            continue;
        }
        if (seen & flag) err = WQ_ERR_DUPLICATE_FIELD;
        seen |= flag;
        if (err == WQ_OK) {
            switch (flag) {
            case F_S:
                if (vl == 0 || vl > WQ_SSID_MAX) err = WQ_ERR_SSID;
                else { memcpy(out->ssid, val, vl + 1); out->ssid_len = vl; }
                break;
            case F_T:
                if (vl >= sizeof(out->type)) err = WQ_ERR_UNSUPPORTED_SECURITY;
                else memcpy(out->type, val, vl + 1);
                break;
            case F_P:
                if (vl > WQ_PASS_MAX) err = WQ_ERR_PASSWORD;
                else { memcpy(out->pass, val, vl + 1); out->pass_len = vl; pass_quoted = quoted; }
                break;
            case F_H:
                if (vl >= sizeof(hval)) err = WQ_ERR_HIDDEN;
                else { memcpy(hval, val, vl + 1); hlen = vl; }
                break;
            case F_R:
                if (vl >= sizeof(rval)) err = WQ_ERR_TRANSITION;
                else { memcpy(rval, val, vl + 1); rlen = vl; }
                break;
            }
        }
        memset(val, 0, sizeof(val));    // May have held the password
    }

    if (err == WQ_OK && !(seen & F_S)) err = WQ_ERR_SSID;
    if (err == WQ_OK && !wq_printable_utf8(out->ssid, out->ssid_len)) err = WQ_ERR_SSID;

    // Security type
    size_t tl = strlen(out->type);
    if (err == WQ_OK) {
        if (tl == 0) out->security = out->pass_len ? WQ_SEC_WPA : WQ_SEC_OPEN;
        else if (wq_ieq(out->type, tl, "nopass")) out->security = WQ_SEC_OPEN;
        else if (wq_ieq(out->type, tl, "WPA")) out->security = WQ_SEC_WPA;
        else if (wq_ieq(out->type, tl, "WPA2")) out->security = WQ_SEC_WPA2;
        else if (wq_ieq(out->type, tl, "SAE") || wq_ieq(out->type, tl, "WPA3")) out->security = WQ_SEC_WPA3;
        else err = WQ_ERR_UNSUPPORTED_SECURITY;     // WEP, WPA2-EAP, ...
    }
    if (err == WQ_OK && hlen) {
        if (wq_ieq(hval, hlen, "true")) out->hidden = true;
        else if (!wq_ieq(hval, hlen, "false")) err = WQ_ERR_HIDDEN;
    }
    if (err == WQ_OK && (seen & F_R)) {
        unsigned long r = 0;
        if (rlen == 0 || rlen > 8) err = WQ_ERR_TRANSITION;
        for (size_t k = 0; err == WQ_OK && k < rlen; k++) {
            if (!wq_is_hex(rval[k])) err = WQ_ERR_TRANSITION;
            else r = (r << 4) | (unsigned long)(rval[k] <= '9' ? rval[k] - '0' : (wq_lower(rval[k]) - 'a' + 10));
        }
        // Bit 0: WPA3-Personal only. Bits 1-3 (SAE-PK, WPA3-Enterprise, Enhanced Open) are not supported.
        if (err == WQ_OK && (r & ~1ul)) err = WQ_ERR_TRANSITION;
        if (err == WQ_OK && r == 1) {
            if (out->security == WQ_SEC_OPEN) err = WQ_ERR_TRANSITION;
            else { out->transition_disable = true; out->security = WQ_SEC_WPA3; }
        }
    }
    // Password
    if (err == WQ_OK) {
        if (out->security == WQ_SEC_OPEN) {
            if (out->pass_len) err = WQ_ERR_OPEN_WITH_PASSWORD;
        } else if (!out->pass_len) {
            err = WQ_ERR_PASSWORD_MISSING;
        } else if (out->pass_len == 64 && !pass_quoted) {
            for (size_t k = 0; k < 64 && err == WQ_OK; k++) {
                if (!wq_is_hex(out->pass[k])) err = WQ_ERR_PASSWORD;
            }
            // SAE has no raw-PSK form: a 64-hex "password" for WPA3 would silently mean something else.
            if (err == WQ_OK && out->security == WQ_SEC_WPA3) err = WQ_ERR_PASSWORD;
            out->pass_is_psk = err == WQ_OK;
        } else if (out->pass_len < 8 || out->pass_len > 63) {
            err = WQ_ERR_PASSWORD;
        } else {
            for (size_t k = 0; k < out->pass_len; k++) {
                if ((unsigned char)out->pass[k] < 0x20 || (unsigned char)out->pass[k] > 0x7E) err = WQ_ERR_PASSWORD;
            }
        }
    }
    if (err != WQ_OK) {
        memset(out, 0, sizeof(*out));
    }
    return err;
}

// --- Scan visibility ---

// Security advertised by a scanned access point (mapped from wifi_auth_mode_t by the firmware).
typedef enum {
    WQ_AP_OPEN, WQ_AP_WEP, WQ_AP_WPA, WQ_AP_WPA2, WQ_AP_WPA_WPA2, WQ_AP_WPA3, WQ_AP_WPA2_WPA3,
    WQ_AP_ENTERPRISE, WQ_AP_OWE, WQ_AP_OTHER,
} wq_ap_sec_t;

typedef struct {
    char ssid[WQ_SSID_MAX + 1];
    int rssi, channel;
    wq_ap_sec_t sec;
} wq_net_t;

typedef enum {
    WQ_VIS_UNKNOWN = 0,     // Not scanned yet
    WQ_VIS_VISIBLE,
    WQ_VIS_NOT_SEEN,        // Scan succeeded, SSID not listed (out of range, other band, or hidden)
    WQ_VIS_SCAN_FAILED,
} wq_vis_state_t;

typedef struct {
    wq_vis_state_t state;
    int matches;            // Access points with this SSID
    int rssi, channel;      // Strongest match
    wq_ap_sec_t ap_sec;
    bool sec_ok;            // The strongest match accepts the QR security
} wq_visibility_t;

/** Can a station configured for `sec` (minimum) join an AP advertising `ap`? */
static inline bool wq_sec_compatible(wq_security_t sec, wq_ap_sec_t ap)
{
    switch (sec) {
    case WQ_SEC_OPEN: return ap == WQ_AP_OPEN;
    case WQ_SEC_WPA:  return ap == WQ_AP_WPA || ap == WQ_AP_WPA2 || ap == WQ_AP_WPA_WPA2 ||
                             ap == WQ_AP_WPA3 || ap == WQ_AP_WPA2_WPA3;
    case WQ_SEC_WPA2: return ap == WQ_AP_WPA2 || ap == WQ_AP_WPA_WPA2 || ap == WQ_AP_WPA3 || ap == WQ_AP_WPA2_WPA3;
    case WQ_SEC_WPA3: return ap == WQ_AP_WPA3 || ap == WQ_AP_WPA2_WPA3;
    }
    return false;
}

static inline const char *wq_ap_sec_name(wq_ap_sec_t s)
{
    static const char *const names[] = {"open", "wep", "wpa", "wpa2", "wpa/wpa2", "wpa3", "wpa2/wpa3",
                                        "enterprise", "owe", "other"};
    return (unsigned)s < sizeof(names) / sizeof(names[0]) ? names[s] : "other";
}

static inline const char *wq_vis_name(wq_vis_state_t s)
{
    switch (s) {
    case WQ_VIS_VISIBLE:     return "visible";
    case WQ_VIS_NOT_SEEN:    return "not_seen";
    case WQ_VIS_SCAN_FAILED: return "scan_failed";
    default:                 return "unknown";
    }
}

/** n < 0: the scan failed. Exact (case-sensitive, byte-wise) SSID match. */
static inline wq_visibility_t wq_visibility(const wq_net_t *nets, int n, const char *ssid, wq_security_t sec)
{
    wq_visibility_t v = {.state = n < 0 ? WQ_VIS_SCAN_FAILED : WQ_VIS_NOT_SEEN, .rssi = -128};
    for (int i = 0; i < n; i++) {
        if (strcmp(nets[i].ssid, ssid) != 0) continue;
        v.matches++;
        if (v.state != WQ_VIS_VISIBLE || nets[i].rssi > v.rssi) {
            v.state = WQ_VIS_VISIBLE;
            v.rssi = nets[i].rssi;
            v.channel = nets[i].channel;
            v.ap_sec = nets[i].sec;
            v.sec_ok = wq_sec_compatible(sec, nets[i].sec);
        }
    }
    if (v.state != WQ_VIS_VISIBLE) v.rssi = 0;
    return v;
}

// --- Profile slot choice ---

typedef struct {
    bool valid;
    char ssid[WQ_SSID_MAX + 1];
} wq_slot_info_t;

typedef enum {
    WQ_SLOT_OK = 0,
    WQ_SLOT_FULL,               // No free slot: choose a slot to overwrite (with overwrite=true)
    WQ_SLOT_NEED_OVERWRITE,     // Requested slot holds another network: confirm with overwrite=true
    WQ_SLOT_DUPLICATE,          // SSID already saved in another slot: update that one instead
    WQ_SLOT_BAD,                // Slot out of range
} wq_slot_err_t;

static inline const char *wq_slot_err_text(wq_slot_err_t e)
{
    switch (e) {
    case WQ_SLOT_OK:             return "ok";
    case WQ_SLOT_FULL:           return "all profile slots are used: choose a slot to overwrite";
    case WQ_SLOT_NEED_OVERWRITE: return "slot holds another network: confirm overwrite";
    case WQ_SLOT_DUPLICATE:      return "this SSID is already saved in another slot (that profile is updated instead)";
    case WQ_SLOT_BAD:            return "invalid slot";
    }
    return "error";
}

/** First valid slot with exactly this SSID, or -1. */
static inline int wq_find_ssid(const wq_slot_info_t *slots, int n, const char *ssid)
{
    for (int i = 0; i < n; i++) {
        if (slots[i].valid && strcmp(slots[i].ssid, ssid) == 0) return i;
    }
    return -1;
}

/**
 * Choose the slot for a QR network. requested < 0 = automatic. An already saved SSID is updated
 * in place (its static IP settings are kept by the caller); saving it into a second slot is
 * refused. Otherwise the first free slot is used; overwriting another network needs an explicit
 * slot and overwrite=true.
 */
static inline wq_slot_err_t wq_pick_slot(const wq_slot_info_t *slots, int n, const char *ssid, int requested,
                                         bool overwrite, int *slot_out, bool *update_out)
{
    *slot_out = -1;
    *update_out = false;
    if (requested >= n) return WQ_SLOT_BAD;
    int existing = wq_find_ssid(slots, n, ssid);
    if (existing >= 0) {
        if (requested >= 0 && requested != existing) return WQ_SLOT_DUPLICATE;
        *slot_out = existing;
        *update_out = true;
        return WQ_SLOT_OK;
    }
    if (requested >= 0) {
        if (slots[requested].valid && !overwrite) return WQ_SLOT_NEED_OVERWRITE;
        *slot_out = requested;
        return WQ_SLOT_OK;
    }
    for (int i = 0; i < n; i++) {
        if (!slots[i].valid) {
            *slot_out = i;
            return WQ_SLOT_OK;
        }
    }
    return WQ_SLOT_FULL;
}

// --- Connect / rollback transaction ---
//
// The firmware worker (wifi_prov.c) owns one transaction. Every connect it issues gets a new
// attempt generation from wifi.c; events from older attempts are stale and ignored, so a late
// failure of a previous attempt can never trigger a rollback. Success means a usable IP address
// (DHCP lease, or the profile's static address applied after association), not association.

typedef enum { WQT_IDLE, WQT_CONNECTING, WQT_ROLLBACK, WQT_DONE } wqt_phase_t;

typedef enum {
    WQT_EV_ASSOCIATED,      // Associated with the target SSID (attempt-tagged)
    WQT_EV_GOT_IP,          // Usable IP configuration (attempt-tagged)
    WQT_EV_DISCONNECTED,    // Attempt failed / link lost (attempt-tagged, with fail kind)
    WQT_EV_TIMEOUT,         // Phase deadline passed (attempt-tagged)
    WQT_EV_START_FAILED,    // The connect request itself failed
    WQT_EV_CANCEL,          // User cancel: ignored once the rollback runs
    WQT_EV_SUPERSEDED,      // The user chose another connection meanwhile: stop, no rollback
} wqt_event_type_t;

typedef enum {
    WQT_FAIL_NONE, WQT_FAIL_AUTH, WQT_FAIL_NO_AP, WQT_FAIL_ASSOC, WQT_FAIL_TIMEOUT, WQT_FAIL_NO_IP,
    WQT_FAIL_START, WQT_FAIL_CANCELLED,
} wqt_fail_t;

typedef enum {
    WQT_ACT_NONE,
    WQT_ACT_CONNECT_NEW,    // (Re)connect to the QR network
    WQT_ACT_CONNECT_PRIOR,  // Restore the snapshot connection
    WQT_ACT_FINISH,         // Done: read the result
} wqt_action_t;

typedef enum {
    WQT_RES_NONE,
    WQT_RES_CONNECTED,
    WQT_RES_FAILED_RESTORED,        // New network failed, previous connection restored
    WQT_RES_FAILED_RESTORE_FAILED,  // New network failed, previous connection could not be restored
    WQT_RES_FAILED_NO_PRIOR,        // New network failed, there was no previous connection
    WQT_RES_CANCELLED_RESTORED,
    WQT_RES_CANCELLED_RESTORE_FAILED,
    WQT_RES_CANCELLED,              // Cancelled, there was no previous connection
    WQT_RES_SUPERSEDED,
} wqt_result_t;

typedef struct {
    wqt_event_type_t type;
    uint32_t attempt;
    wqt_fail_t fail;        // WQT_EV_DISCONNECTED
} wqt_event_t;

typedef struct {
    wqt_phase_t phase;
    bool has_prior;
    int max_tries;          // Connects per target (a retry only follows non-auth failures)
    int tries;
    uint32_t attempt;       // Generation of the current connect, 0 = none issued yet
    bool assoc;
    bool cancelled;
    wqt_fail_t new_fail, prior_fail;
    wqt_result_t result;
} wqt_t;

static inline const char *wqt_fail_name(wqt_fail_t f)
{
    static const char *const names[] = {"none", "auth", "no_ap", "assoc", "timeout", "no_ip", "start", "cancelled"};
    return (unsigned)f < sizeof(names) / sizeof(names[0]) ? names[f] : "none";
}

static inline const char *wqt_result_name(wqt_result_t r)
{
    static const char *const names[] = {"none", "connected", "failed_restored", "failed_restore_failed",
                                        "failed_no_prior", "cancelled_restored", "cancelled_restore_failed",
                                        "cancelled", "superseded"};
    return (unsigned)r < sizeof(names) / sizeof(names[0]) ? names[r] : "none";
}

static inline wqt_action_t wqt_start(wqt_t *t, bool has_prior, int max_tries)
{
    memset(t, 0, sizeof(*t));
    t->phase = WQT_CONNECTING;
    t->has_prior = has_prior;
    t->max_tries = max_tries < 1 ? 1 : max_tries;
    t->tries = 1;
    return WQT_ACT_CONNECT_NEW;
}

/** Record the generation of the connect just issued for the current phase. */
static inline void wqt_attempt_started(wqt_t *t, uint32_t attempt)
{
    t->attempt = attempt;
    t->assoc = false;
}

static inline wqt_action_t wqt_finish(wqt_t *t, wqt_result_t r)
{
    t->phase = WQT_DONE;
    t->result = r;
    return WQT_ACT_FINISH;
}

static inline wqt_action_t wqt_new_failed(wqt_t *t, wqt_fail_t f)
{
    t->new_fail = f;
    if (!t->has_prior) return wqt_finish(t, t->cancelled ? WQT_RES_CANCELLED : WQT_RES_FAILED_NO_PRIOR);
    t->phase = WQT_ROLLBACK;
    t->tries = 1;
    t->attempt = 0;
    t->assoc = false;
    return WQT_ACT_CONNECT_PRIOR;
}

static inline wqt_action_t wqt_prior_failed(wqt_t *t, wqt_fail_t f)
{
    t->prior_fail = f;
    return wqt_finish(t, t->cancelled ? WQT_RES_CANCELLED_RESTORE_FAILED : WQT_RES_FAILED_RESTORE_FAILED);
}

static inline wqt_action_t wqt_event(wqt_t *t, wqt_event_t ev)
{
    if (t->phase != WQT_CONNECTING && t->phase != WQT_ROLLBACK) return WQT_ACT_NONE;
    bool rollback = t->phase == WQT_ROLLBACK;
    switch (ev.type) {
    case WQT_EV_SUPERSEDED:
        return wqt_finish(t, WQT_RES_SUPERSEDED);
    case WQT_EV_CANCEL:
        if (rollback) return WQT_ACT_NONE;  // Restoring the previous connection is not cancellable
        t->cancelled = true;
        return wqt_new_failed(t, WQT_FAIL_CANCELLED);
    case WQT_EV_START_FAILED:
        return rollback ? wqt_prior_failed(t, WQT_FAIL_START) : wqt_new_failed(t, WQT_FAIL_START);
    default:
        break;
    }
    if (t->attempt == 0 || ev.attempt != t->attempt) return WQT_ACT_NONE;  // Stale event
    switch (ev.type) {
    case WQT_EV_ASSOCIATED:
        t->assoc = true;
        return WQT_ACT_NONE;
    case WQT_EV_GOT_IP:
        return wqt_finish(t, rollback ? (t->cancelled ? WQT_RES_CANCELLED_RESTORED : WQT_RES_FAILED_RESTORED)
                                      : WQT_RES_CONNECTED);
    case WQT_EV_TIMEOUT: {
        wqt_fail_t f = t->assoc ? WQT_FAIL_NO_IP : WQT_FAIL_TIMEOUT;
        return rollback ? wqt_prior_failed(t, f) : wqt_new_failed(t, f);
    }
    case WQT_EV_DISCONNECTED: {
        wqt_fail_t f = ev.fail == WQT_FAIL_NONE ? WQT_FAIL_ASSOC : ev.fail;
        if (f != WQT_FAIL_AUTH && t->tries < t->max_tries) {
            t->tries++;
            t->attempt = 0;     // Events of the failed attempt are stale from now on
            return rollback ? WQT_ACT_CONNECT_PRIOR : WQT_ACT_CONNECT_NEW;
        }
        return rollback ? wqt_prior_failed(t, f) : wqt_new_failed(t, f);
    }
    default:
        return WQT_ACT_NONE;
    }
}
