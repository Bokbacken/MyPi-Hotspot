#include "wifi_prov.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "camera.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "profiles.h"
#include "qr_decode.h"
#include "quirc.h"
#include "wifi.h"
#include "wifi_qr_core.h"

static const char *TAG = "wifiqr";

#define CAND_TTL_US     (10LL * 60 * 1000000)   // A decoded candidate is wiped after 10 minutes
#define ATTEMPT_MS      25000                   // Per connect attempt: association + IP address
#define MAX_TRIES       2                       // Per network (authentication failures: 1)
#define SCAN_FRAMES     4                       // Camera frames tried per scan request
#define SCAN_MAX        32
#define WORKER_STACK    16384                   // quirc_decode() alone needs ~10 KB

typedef enum {
    ST_IDLE, ST_DECODING, ST_SCANNING, ST_REVIEW, ST_SAVING, ST_CONNECTING, ST_ROLLBACK, ST_DONE, ST_ERROR,
} st_t;

static const char *const ST_NAMES[] = {"idle", "decoding", "scanning", "review", "saving", "connecting",
                                       "restoring", "done", "error"};

typedef enum { JOB_CAMERA, JOB_TEXT, JOB_RESCAN, JOB_COMMIT } job_t;

static struct {
    st_t state;
    bool busy;
    uint32_t seq;
    char msg[256];
    char progress[128];
    const char *source;             // "camera" / "text"
    int codes, decoded, wifi_codes, other;
    // Pending candidate (holds the password: never logged, never reported)
    bool have;
    wq_wifi_t cand;
    int64_t cand_us;
    wifi_sta_snapshot_t prior;      // Snapshot taken right after decoding (shown in the review)
    wq_visibility_t vis;
    // Last commit
    bool have_result;
    bool res_connect, res_update, res_overwrote, res_reverted, res_held;
    int res_slot;
    wqt_result_t res;
    wqt_fail_t new_fail, prior_fail;
    uint8_t new_reason, prior_reason;
    char res_ssid[WQ_SSID_MAX + 1], res_prior_ssid[WQ_SSID_MAX + 1], res_ip[16];
    bool cancel_req;
    // Job parameters
    job_t job;
    bool job_connect, job_overwrite;
    int job_slot;
    char text[WQ_PAYLOAD_MAX + 1];
    size_t text_len;
} s;

static SemaphoreHandle_t s_mx;
static StaticSemaphore_t s_mx_buf;
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;

static void lock(void)
{
    if (!s_mx) {
        portENTER_CRITICAL(&s_init_mux);
        if (!s_mx) s_mx = xSemaphoreCreateMutexStatic(&s_mx_buf);
        portEXIT_CRITICAL(&s_init_mux);
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
}
static void unlock(void) { xSemaphoreGive(s_mx); }

static void wipe(void *p, size_t n)
{
    volatile uint8_t *v = p;
    while (n--) *v++ = 0;
}

/** Caller holds the lock. */
static void clear_candidate(void)
{
    wipe(&s.cand, sizeof(s.cand));
    wipe(&s.prior, sizeof(s.prior));
    memset(&s.vis, 0, sizeof(s.vis));
    s.have = false;
}

/** Caller holds the lock. */
static void expire_candidate(void)
{
    if (s.have && !s.busy && esp_timer_get_time() - s.cand_us > CAND_TTL_US) {
        clear_candidate();
        s.state = ST_IDLE;
        strlcpy(s.msg, "The decoded Wi-Fi code expired after 10 minutes and was discarded.", sizeof(s.msg));
    }
}

static void set_state(st_t st, const char *msg)
{
    lock();
    s.state = st;
    if (msg) strlcpy(s.msg, msg, sizeof(s.msg));
    unlock();
}

/** what: "connect" / "restore" / "assoc" (literal formats only). */
static void set_progress(const char *what, const char *ssid, int a, int b)
{
    lock();
    if (strcmp(what, "restore") == 0) {
        snprintf(s.progress, sizeof(s.progress), "Restoring the previous connection to \"%s\" (attempt %d/%d)...",
                 ssid, a, b);
    } else if (strcmp(what, "assoc") == 0) {
        snprintf(s.progress, sizeof(s.progress), "Associated with \"%s\", waiting for an IP address (attempt %d/%d)...",
                 ssid, a, b);
    } else {
        snprintf(s.progress, sizeof(s.progress), "Connecting to \"%s\" (attempt %d/%d)...", ssid, a, b);
    }
    unlock();
}

static wq_ap_sec_t map_auth(int auth)
{
    switch (auth) {
    case WIFI_AUTH_OPEN:            return WQ_AP_OPEN;
    case WIFI_AUTH_WEP:             return WQ_AP_WEP;
    case WIFI_AUTH_WPA_PSK:         return WQ_AP_WPA;
    case WIFI_AUTH_WPA2_PSK:        return WQ_AP_WPA2;
    case WIFI_AUTH_WPA_WPA2_PSK:    return WQ_AP_WPA_WPA2;
    case WIFI_AUTH_WPA3_PSK:        return WQ_AP_WPA3;
    case WIFI_AUTH_WPA2_WPA3_PSK:   return WQ_AP_WPA2_WPA3;
    case WIFI_AUTH_WPA2_ENTERPRISE:
    case WIFI_AUTH_WPA3_ENT_192:    return WQ_AP_ENTERPRISE;
    case WIFI_AUTH_OWE:             return WQ_AP_OWE;
    default:                        return WQ_AP_OTHER;
    }
}

static wqt_fail_t map_reason(uint8_t r)
{
    switch (r) {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
    case WIFI_REASON_802_1X_AUTH_FAILED:
        return WQT_FAIL_AUTH;
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return WQT_FAIL_NO_AP;
    default:
        return WQT_FAIL_ASSOC;
    }
}

static const char *fail_text(wqt_fail_t f)
{
    switch (f) {
    case WQT_FAIL_AUTH:      return "authentication failed (wrong password or security type?)";
    case WQT_FAIL_NO_AP:     return "network not found (out of range, other band, or security mismatch)";
    case WQT_FAIL_ASSOC:     return "association failed";
    case WQT_FAIL_TIMEOUT:   return "timed out";
    case WQT_FAIL_NO_IP:     return "associated but got no IP address";
    case WQT_FAIL_START:     return "the connect request failed";
    case WQT_FAIL_CANCELLED: return "cancelled";
    default:                 return "failed";
    }
}

// --- Decoding ---

static uint8_t *gray_alloc(int w, int h, void *ctx)
{
    struct quirc *q = ctx;
    if (quirc_resize(q, w, h) < 0) return NULL;
    return quirc_begin(q, NULL, NULL);
}

/** Returns false with err set if the camera or memory failed; true if frames were examined. */
static bool decode_camera(qr_scan_result_t *r, char *err, size_t errlen)
{
    struct quirc *q = quirc_new();
    if (!q) {
        strlcpy(err, "Out of memory for the QR decoder", errlen);
        return false;
    }
    bool ok = true;
    for (int i = 0; i < SCAN_FRAMES; i++) {
        int w = 0, h = 0;
        esp_err_t e = camera_snapshot_gray(gray_alloc, q, &w, &h);
        if (e != ESP_OK) {
            camera_status_t st;
            camera_get_status(&st);
            snprintf(err, errlen, "Camera image unavailable: %s",
                     e == ESP_ERR_NO_MEM ? "out of memory for the QR image" :
                     st.last_error[0] ? st.last_error : esp_err_to_name(e));
            ok = false;
            break;
        }
        quirc_end(q);
        if (!qr_scan(q, r)) {
            strlcpy(err, "Out of memory for the QR decoder", errlen);
            ok = false;
            break;
        }
        if (r->decoded > 0 || r->too_long) break;
    }
    quirc_destroy(q);
    return ok;
}

static void scan_visibility(void)
{
    char ssid[WQ_SSID_MAX + 1];
    wq_security_t sec;
    lock();
    strlcpy(ssid, s.cand.ssid, sizeof(ssid));
    sec = s.cand.security;
    unlock();
    wifi_scan_entry_t *e = calloc(SCAN_MAX, sizeof(*e));
    wq_net_t *nets = calloc(SCAN_MAX, sizeof(*nets));
    int n = (e && nets) ? wifi_scan(e, SCAN_MAX) : -1;
    for (int i = 0; i < n; i++) {
        strlcpy(nets[i].ssid, e[i].ssid, sizeof(nets[i].ssid));
        nets[i].rssi = e[i].rssi;
        nets[i].channel = e[i].channel;
        nets[i].sec = map_auth(e[i].auth);
    }
    wq_visibility_t v = wq_visibility(nets, n, ssid, sec);
    free(e);
    free(nets);
    lock();
    s.vis = v;
    unlock();
}

static void review_message(void)
{
    lock();
    const char *vis;
    switch (s.vis.state) {
    case WQ_VIS_VISIBLE:
        vis = s.vis.sec_ok ? "The network is visible." :
              "The network is visible, but its security does not match the code; connecting will likely fail.";
        break;
    case WQ_VIS_NOT_SEEN:
        vis = s.cand.hidden ? "Hidden network: it does not appear in scans; connecting is still possible." :
                              "The network was not seen in the scan (out of range, other band, or hidden).";
        break;
    case WQ_VIS_SCAN_FAILED: vis = "The network scan failed; you can still save the profile or try to connect."; break;
    default:                 vis = ""; break;
    }
    snprintf(s.msg, sizeof(s.msg), "Wi-Fi code read. %s Choose \"Add as profile\" or \"Add and connect\".", vis);
    s.state = ST_REVIEW;
    unlock();
}

static void accept_payload(const char *payload, size_t len)
{
    wq_wifi_t w;
    wq_err_t e = wq_parse(payload, len, &w);
    if (e != WQ_OK) {
        char m[160];
        snprintf(m, sizeof(m), e == WQ_ERR_NOT_WIFI ? "%s: nothing was changed." : "Wi-Fi code rejected: %s.",
                 wq_err_text(e));
        set_state(ST_ERROR, m);
        return;
    }
    wifi_sta_snapshot_t snap;
    wifi_sta_snapshot(&snap);
    lock();
    s.cand = w;
    s.have = true;
    s.cand_us = esp_timer_get_time();
    s.prior = snap;
    s.state = ST_SCANNING;
    strlcpy(s.msg, "Wi-Fi code read; scanning for networks...", sizeof(s.msg));
    unlock();
    wipe(&w, sizeof(w));
    wipe(&snap, sizeof(snap));
    ESP_LOGI(TAG, "Wi-Fi QR code accepted for review");    // never log SSID/password details
    scan_visibility();
    review_message();
}

static void job_camera(void)
{
    qr_scan_result_t *r = calloc(1, sizeof(*r));
    char err[128] = "";
    if (!r) {
        set_state(ST_ERROR, "Out of memory");
        return;
    }
    bool ok = decode_camera(r, err, sizeof(err));
    lock();
    s.codes = r->codes;
    s.decoded = r->decoded;
    s.wifi_codes = r->wifi;
    s.other = r->other;
    unlock();
    if (!ok) {
        set_state(ST_ERROR, err);
    } else if (r->too_long) {
        set_state(ST_ERROR, "Wi-Fi code rejected: text too long. Nothing was changed.");
    } else if (r->wifi > 1) {
        set_state(ST_ERROR, "Several different Wi-Fi QR codes are in view: show only one. Nothing was changed.");
    } else if (r->wifi == 1) {
        accept_payload(r->payload, r->payload_len);
    } else if (r->other > 0) {
        set_state(ST_ERROR, "A QR code was read, but it is not a Wi-Fi code. Nothing was changed.");
    } else if (r->codes > 0) {
        set_state(ST_ERROR, "A QR code was found but could not be read: hold it steady, fill more of the image, "
                            "improve the light, and scan again.");
    } else {
        set_state(ST_ERROR, "No QR code found: aim the MyPi camera at the code and scan again.");
    }
    wipe(r, sizeof(*r));
    free(r);
}

// --- Commit ---

static void back_to_review(const char *msg)
{
    set_state(ST_REVIEW, msg);
}

static void finish_commit(const char *msg)
{
    lock();
    clear_candidate();
    s.state = ST_DONE;
    s.progress[0] = '\0';
    strlcpy(s.msg, msg, sizeof(s.msg));
    unlock();
}

/** Add-and-connect: bounded attempts on the new network, then rollback to the snapshot. */
static void connect_flow(uint32_t txn, const sta_profile_t *pp, const sta_profile_t *old, const wifi_sta_snapshot_t *sp,
                         int slot, bool update)
{
    sta_profile_t p = *pp;
    wifi_sta_snapshot_t snap = *sp;
    char m[256];
    bool has_prior = snap.connected && snap.profile.valid && !snap.manual_off;
    wqt_t t;
    wqt_action_t act = wqt_start(&t, has_prior, MAX_TRIES);
    int64_t deadline = 0;
    bool cancel_sent = false;
    set_state(ST_CONNECTING, NULL);
    while (act != WQT_ACT_FINISH) {
        if (act == WQT_ACT_CONNECT_NEW || act == WQT_ACT_CONNECT_PRIOR) {
            bool prior = act == WQT_ACT_CONNECT_PRIOR;
            set_state(prior ? ST_ROLLBACK : ST_CONNECTING, NULL);
            set_progress(prior ? "restore" : "connect", prior ? snap.profile.ssid : p.ssid, t.tries, t.max_tries);
            uint32_t attempt = 0;
            esp_err_t e = wifi_txn_connect(txn, prior ? &snap.profile : &p, prior ? snap.slot : slot, &attempt);
            if (e != ESP_OK) {
                act = wqt_event(&t, (wqt_event_t){.type = e == ESP_ERR_INVALID_STATE ? WQT_EV_SUPERSEDED
                                                                                      : WQT_EV_START_FAILED});
                continue;
            }
            wqt_attempt_started(&t, attempt);
            deadline = esp_timer_get_time() + (int64_t)ATTEMPT_MS * 1000;
            act = WQT_ACT_NONE;
        }
        if (!cancel_sent) {
            lock();
            bool cancel = s.cancel_req;
            unlock();
            if (cancel) {
                cancel_sent = true;
                act = wqt_event(&t, (wqt_event_t){.type = WQT_EV_CANCEL});
                if (act != WQT_ACT_NONE) continue;
            }
        }
        int64_t now = esp_timer_get_time();
        if (now >= deadline) {
            act = wqt_event(&t, (wqt_event_t){.type = WQT_EV_TIMEOUT, .attempt = t.attempt});
            continue;
        }
        int64_t left_ms = (deadline - now) / 1000;
        wifi_txn_event_t ev;
        if (!wifi_txn_next_event(txn, &ev, left_ms < 250 ? (uint32_t)left_ms + 1 : 250)) continue;
        bool rollback = t.phase == WQT_ROLLBACK;
        wqt_event_t we = {.attempt = ev.attempt};
        switch (ev.type) {
        case WIFI_TXN_EV_ASSOCIATED:
            we.type = WQT_EV_ASSOCIATED;
            if (ev.attempt == t.attempt) {
                set_progress("assoc", rollback ? snap.profile.ssid : p.ssid, t.tries, t.max_tries);
            }
            break;
        case WIFI_TXN_EV_GOT_IP:
            we.type = WQT_EV_GOT_IP;
            break;
        case WIFI_TXN_EV_DISCONNECTED:
            we.type = WQT_EV_DISCONNECTED;
            we.fail = map_reason(ev.reason);
            if (t.attempt && ev.attempt == t.attempt) {
                lock();
                if (rollback) s.prior_reason = ev.reason;
                else s.new_reason = ev.reason;
                unlock();
            }
            break;
        default:
            we.type = WQT_EV_SUPERSEDED;
            break;
        }
        act = wqt_event(&t, we);
    }

    int pinned = snap.pinned_slot;
    bool manual_off = snap.manual_off;
    bool reverted = false, held = false;
    if (t.result == WQT_RES_CONNECTED) {
        pinned = slot;
        manual_off = false;
        wifi_autoselect_hold(slot, false);
    } else if (t.result != WQT_RES_SUPERSEDED) {
        if (update) {
            reverted = sta_profile_save(slot, old) == ESP_OK;   // keep the working credentials
        } else {
            wifi_autoselect_hold(slot, true);
            held = true;
        }
    }
    // Keep the link only if it is the new network or the restored one; otherwise stop a still
    // running attempt (e.g. after a cancel) and let the normal reconnect logic take over.
    bool keep = t.result == WQT_RES_CONNECTED || t.result == WQT_RES_FAILED_RESTORED ||
                t.result == WQT_RES_CANCELLED_RESTORED;
    if (t.result != WQT_RES_SUPERSEDED) wifi_txn_end(txn, pinned, manual_off, keep);
    wifi_sta_snapshot_t now_snap;
    wifi_sta_snapshot(&now_snap);
    lock();
    s.res = t.result;
    s.new_fail = t.new_fail;
    s.prior_fail = t.prior_fail;
    s.res_reverted = reverted;
    s.res_held = held;
    strlcpy(s.res_ip, now_snap.connected ? now_snap.ip : "", sizeof(s.res_ip));
    unlock();
    ESP_LOGI(TAG, "Wi-Fi QR connect result: %s (new: %s, previous: %s)", wqt_result_name(t.result),
             wqt_fail_name(t.new_fail), wqt_fail_name(t.prior_fail));

    char prof[120] = "";
    if (reverted) snprintf(prof, sizeof(prof), " Profile %d was restored to its previous settings.", slot);
    else if (held) snprintf(prof, sizeof(prof), " The profile stays saved in slot %d but is not auto-selected until you "
                            "connect to it.", slot);
    switch (t.result) {
    case WQT_RES_CONNECTED:
        snprintf(m, sizeof(m), "Connected to \"%s\" (IP %s). Saved in profile slot %d.", p.ssid,
                 now_snap.connected ? now_snap.ip : "?", slot);
        break;
    case WQT_RES_FAILED_RESTORED:
        snprintf(m, sizeof(m), "Could not connect to \"%s\": %s. The previous connection to \"%s\" was restored.%s",
                 p.ssid, fail_text(t.new_fail), snap.profile.ssid, prof);
        break;
    case WQT_RES_FAILED_RESTORE_FAILED:
        snprintf(m, sizeof(m), "Could not connect to \"%s\": %s. Restoring \"%s\" also failed: %s; automatic "
                 "reconnect continues.%.100s", p.ssid, fail_text(t.new_fail), snap.profile.ssid,
                 fail_text(t.prior_fail), prof);
        break;
    case WQT_RES_FAILED_NO_PRIOR:
        snprintf(m, sizeof(m), "Could not connect to \"%s\": %s. There was no previous connection to restore.%s",
                 p.ssid, fail_text(t.new_fail), prof);
        break;
    case WQT_RES_CANCELLED_RESTORED:
        snprintf(m, sizeof(m), "Cancelled. The previous connection to \"%s\" was restored.%s", snap.profile.ssid, prof);
        break;
    case WQT_RES_CANCELLED_RESTORE_FAILED:
        snprintf(m, sizeof(m), "Cancelled. Restoring \"%s\" failed: %s; automatic reconnect continues.%s",
                 snap.profile.ssid, fail_text(t.prior_fail), prof);
        break;
    case WQT_RES_CANCELLED:
        snprintf(m, sizeof(m), "Cancelled. There was no previous connection to restore.%s", prof);
        break;
    default:
        snprintf(m, sizeof(m), "Stopped: another connection change was requested meanwhile. The profile stays saved "
                 "in slot %d.", slot);
        break;
    }
    finish_commit(m);
    wipe(&now_snap, sizeof(now_snap));
    wipe(&p, sizeof(p));
    wipe(&snap, sizeof(snap));
}

static void job_commit(void)
{
    wq_wifi_t c;
    lock();
    c = s.cand;
    bool connect = s.job_connect, overwrite = s.job_overwrite;
    int req = s.job_slot;
    unlock();

    wq_slot_info_t slots[STA_PROFILE_MAX];
    sta_profile_t tmp;
    for (int i = 0; i < STA_PROFILE_MAX; i++) {
        sta_profile_get(i, &tmp);
        slots[i].valid = tmp.valid;
        strlcpy(slots[i].ssid, tmp.valid ? tmp.ssid : "", sizeof(slots[i].ssid));
    }
    wipe(&tmp, sizeof(tmp));
    int slot = -1;
    bool update = false;
    wq_slot_err_t se = wq_pick_slot(slots, STA_PROFILE_MAX, c.ssid, req, overwrite, &slot, &update);
    if (se != WQ_SLOT_OK) {
        char m[160];
        snprintf(m, sizeof(m), "Not saved: %s.", wq_slot_err_text(se));
        back_to_review(m);
        wipe(&c, sizeof(c));
        return;
    }
    // A transaction pauses automatic reconnect/selection before anything changes.
    uint32_t txn = 0;
    if (connect && !(txn = wifi_txn_begin())) {
        back_to_review("Not saved: Wi-Fi is not ready (not started, emergency mode, or another connection change "
                       "is running).");
        wipe(&c, sizeof(c));
        return;
    }
    wifi_sta_snapshot_t snap;      // The exact prior connection, taken right before the change
    wifi_sta_snapshot(&snap);
    bool overwrote = !update && slots[slot].valid;
    if (overwrote && (slot == snap.slot || slot == snap.pinned_slot)) {
        if (txn) wifi_txn_end(txn, snap.pinned_slot, snap.manual_off, true);
        char m[160];
        snprintf(m, sizeof(m), "Not saved: slot %d holds the network currently in use or selected; choose another "
                 "slot.", slot);
        back_to_review(m);
        wipe(&c, sizeof(c));
        wipe(&snap, sizeof(snap));
        return;
    }
    sta_profile_t old, p;
    sta_profile_get(slot, &old);
    if (update) {
        p = old;            // keep the DHCP / static IP settings of the saved network
    } else {
        memset(&p, 0, sizeof(p));
        p.dhcp = true;
    }
    strlcpy(p.ssid, c.ssid, sizeof(p.ssid));
    strlcpy(p.pass, c.pass, sizeof(p.pass));
    p.security = (uint8_t)c.security;
    p.valid = true;
    wipe(&c, sizeof(c));
    lock();
    s.have_result = true;
    s.res_connect = connect;
    s.res_slot = slot;
    s.res_update = update;
    s.res_overwrote = overwrote;
    s.res_reverted = s.res_held = false;
    s.res = WQT_RES_NONE;
    s.new_fail = s.prior_fail = WQT_FAIL_NONE;
    s.new_reason = s.prior_reason = 0;
    strlcpy(s.res_ssid, p.ssid, sizeof(s.res_ssid));
    strlcpy(s.res_prior_ssid, snap.connected && snap.profile.valid ? snap.profile.ssid : "", sizeof(s.res_prior_ssid));
    s.res_ip[0] = '\0';
    unlock();

    esp_err_t err = sta_profile_save(slot, &p);
    if (err != ESP_OK) {
        if (txn) wifi_txn_end(txn, snap.pinned_slot, snap.manual_off, true);
        lock();
        s.have_result = false;
        unlock();
        char m[160];
        snprintf(m, sizeof(m), "Saving the profile failed (%s); nothing was changed.", esp_err_to_name(err));
        back_to_review(m);
    } else if (!connect) {
        ESP_LOGI(TAG, "Wi-Fi QR network saved in STA profile %d (not connecting)", slot);
        if (!update) wifi_autoselect_hold(slot, true);
        lock();
        s.res_held = !update;
        unlock();
        char m[256];
        snprintf(m, sizeof(m), "Saved \"%s\" in profile slot %d without connecting.%s", p.ssid, slot,
                 update ? " The existing profile was updated." :
                 " It is not auto-selected until you connect to it (or use Connect auto).");
        finish_commit(m);
    } else {
        ESP_LOGI(TAG, "Wi-Fi QR network saved in STA profile %d, connecting", slot);
        connect_flow(txn, &p, &old, &snap, slot, update);
    }
    wipe(&p, sizeof(p));
    wipe(&old, sizeof(old));
    wipe(&snap, sizeof(snap));
}

// --- Worker / jobs ---

static void worker(void *arg)
{
    (void)arg;
    lock();
    job_t job = s.job;
    unlock();
    switch (job) {
    case JOB_CAMERA:
        job_camera();
        break;
    case JOB_TEXT: {
        char *t = malloc(WQ_PAYLOAD_MAX + 1);
        size_t len = 0;
        lock();
        if (t) {
            len = s.text_len;
            memcpy(t, s.text, len);
            t[len] = '\0';
        }
        wipe(s.text, sizeof(s.text));
        s.text_len = 0;
        unlock();
        if (t) {
            accept_payload(t, len);
            wipe(t, WQ_PAYLOAD_MAX + 1);
            free(t);
        } else {
            set_state(ST_ERROR, "Out of memory");
        }
        break;
    }
    case JOB_RESCAN:
        scan_visibility();
        review_message();
        break;
    case JOB_COMMIT:
        job_commit();
        break;
    }
    lock();
    s.busy = false;
    s.cancel_req = false;
    unlock();
    vTaskDelete(NULL);
}

static esp_err_t start_job(job_t job, const char *text, size_t len, bool connect, int slot, bool overwrite)
{
    lock();
    expire_candidate();
    if (s.busy) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    st_t prev = s.state;
    if (job == JOB_RESCAN || job == JOB_COMMIT) {
        if (!s.have || s.state != ST_REVIEW) {
            unlock();
            return ESP_ERR_NOT_FOUND;
        }
        s.state = job == JOB_RESCAN ? ST_SCANNING : ST_SAVING;
        strlcpy(s.msg, job == JOB_RESCAN ? "Scanning for networks..." : "Saving...", sizeof(s.msg));
    } else {
        clear_candidate();
        s.have_result = false;
        s.codes = s.decoded = s.wifi_codes = s.other = 0;
        s.source = job == JOB_CAMERA ? "camera" : "text";
        s.state = ST_DECODING;
        strlcpy(s.msg, job == JOB_CAMERA ? "Looking for a QR code in the camera image..." : "Checking the text...",
                sizeof(s.msg));
        if (job == JOB_TEXT) {
            memcpy(s.text, text, len);
            s.text[len] = '\0';
            s.text_len = len;
        }
    }
    s.progress[0] = '\0';
    s.job = job;
    s.job_connect = connect;
    s.job_slot = slot;
    s.job_overwrite = overwrite;
    s.cancel_req = false;
    s.busy = true;
    s.seq++;
    unlock();
    if (xTaskCreate(worker, "wifi_qr", WORKER_STACK, NULL, 4, NULL) != pdPASS) {
        lock();
        s.busy = false;
        wipe(s.text, sizeof(s.text));
        s.text_len = 0;
        s.state = (job == JOB_RESCAN || job == JOB_COMMIT) ? prev : ST_ERROR;
        strlcpy(s.msg, "Out of memory: could not start the Wi-Fi QR worker.", sizeof(s.msg));
        unlock();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t wifi_prov_scan_camera(void)
{
    return start_job(JOB_CAMERA, NULL, 0, false, -1, false);
}

esp_err_t wifi_prov_submit_text(const char *payload, size_t len)
{
    if (!payload || len == 0 || len > WQ_PAYLOAD_MAX) return ESP_ERR_INVALID_SIZE;
    return start_job(JOB_TEXT, payload, len, false, -1, false);
}

esp_err_t wifi_prov_rescan(void)
{
    return start_job(JOB_RESCAN, NULL, 0, false, -1, false);
}

esp_err_t wifi_prov_commit(bool connect, int slot, bool overwrite)
{
    if (slot < -1 || slot >= STA_PROFILE_MAX) return ESP_ERR_INVALID_ARG;
    return start_job(JOB_COMMIT, NULL, 0, connect, slot, overwrite);
}

esp_err_t wifi_prov_cancel(void)
{
    lock();
    esp_err_t err = ESP_OK;
    if (s.busy) {
        if (s.state == ST_CONNECTING) s.cancel_req = true;
        else err = ESP_ERR_INVALID_STATE;   // decoding/scanning finish by themselves; restoring is not cancellable
    } else {
        clear_candidate();
        s.state = ST_IDLE;
        strlcpy(s.msg, "Discarded.", sizeof(s.msg));
    }
    unlock();
    return err;
}

esp_err_t wifi_prov_discard(void)
{
    lock();
    if (s.busy) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    clear_candidate();
    s.have_result = false;
    s.state = ST_IDLE;
    s.msg[0] = '\0';
    s.progress[0] = '\0';
    s.codes = s.decoded = s.wifi_codes = s.other = 0;
    unlock();
    return ESP_OK;
}

cJSON *wifi_prov_status_json(void)
{
    sta_profile_t prof[STA_PROFILE_MAX];
    for (int i = 0; i < STA_PROFILE_MAX; i++) sta_profile_get(i, &prof[i]);
    uint32_t held = wifi_autoselect_held();
    cJSON *j = cJSON_CreateObject();
    lock();
    expire_candidate();
    cJSON_AddStringToObject(j, "state", ST_NAMES[s.state]);
    cJSON_AddBoolToObject(j, "busy", s.busy);
    cJSON_AddNumberToObject(j, "seq", s.seq);
    cJSON_AddStringToObject(j, "message", s.msg);
    cJSON_AddStringToObject(j, "progress", s.progress);
    cJSON_AddBoolToObject(j, "cancellable", s.busy && s.state == ST_CONNECTING);
    if (s.source) {
        cJSON *d = cJSON_AddObjectToObject(j, "decode");
        cJSON_AddStringToObject(d, "source", s.source);
        cJSON_AddNumberToObject(d, "codes", s.codes);
        cJSON_AddNumberToObject(d, "decoded", s.decoded);
        cJSON_AddNumberToObject(d, "wifi", s.wifi_codes);
        cJSON_AddNumberToObject(d, "other", s.other);
    }
    if (s.have) {
        cJSON *c = cJSON_AddObjectToObject(j, "candidate");
        cJSON_AddStringToObject(c, "ssid", s.cand.ssid);
        cJSON_AddStringToObject(c, "security", wq_security_name(s.cand.security));
        cJSON_AddStringToObject(c, "type", s.cand.type);
        cJSON_AddBoolToObject(c, "hidden", s.cand.hidden);
        cJSON_AddNumberToObject(c, "password_len", (double)s.cand.pass_len);
        cJSON_AddBoolToObject(c, "psk", s.cand.pass_is_psk);
        cJSON_AddBoolToObject(c, "wpa3_required", s.cand.transition_disable);
        cJSON_AddStringToObject(c, "ignored", s.cand.ignored);
        int64_t left = (CAND_TTL_US - (esp_timer_get_time() - s.cand_us)) / 1000000;
        cJSON_AddNumberToObject(c, "expires_s", left > 0 ? (double)left : 0);
        int existing = -1;
        for (int i = 0; i < STA_PROFILE_MAX && existing < 0; i++) {
            if (prof[i].valid && strcmp(prof[i].ssid, s.cand.ssid) == 0) existing = i;
        }
        cJSON_AddNumberToObject(c, "existing_slot", existing);

        cJSON *p = cJSON_AddObjectToObject(j, "prior");
        cJSON_AddBoolToObject(p, "connected", s.prior.connected);
        cJSON_AddStringToObject(p, "ssid", s.prior.profile.valid ? s.prior.profile.ssid : "");
        cJSON_AddNumberToObject(p, "slot", s.prior.slot);
        cJSON_AddStringToObject(p, "source", !s.prior.profile.valid ? "none" : s.prior.slot >= 0 ? "profile" : "default");
        cJSON_AddBoolToObject(p, "dhcp", !s.prior.profile.valid || s.prior.profile.dhcp);
        cJSON_AddStringToObject(p, "ip", s.prior.ip);
        cJSON_AddNumberToObject(p, "pinned_slot", s.prior.pinned_slot);
        cJSON_AddBoolToObject(p, "manual_off", s.prior.manual_off);

        cJSON *v = cJSON_AddObjectToObject(j, "visibility");
        cJSON_AddStringToObject(v, "state", wq_vis_name(s.vis.state));
        cJSON_AddNumberToObject(v, "matches", s.vis.matches);
        if (s.vis.state == WQ_VIS_VISIBLE) {
            cJSON_AddNumberToObject(v, "rssi", s.vis.rssi);
            cJSON_AddNumberToObject(v, "channel", s.vis.channel);
            cJSON_AddStringToObject(v, "ap_security", wq_ap_sec_name(s.vis.ap_sec));
            cJSON_AddBoolToObject(v, "security_ok", s.vis.sec_ok);
        }
    }
    if (s.have_result) {
        cJSON *r = cJSON_AddObjectToObject(j, "result");
        cJSON_AddStringToObject(r, "action", s.res_connect ? "connect" : "save");
        cJSON_AddStringToObject(r, "outcome", s.res_connect ? wqt_result_name(s.res) : "saved");
        cJSON_AddStringToObject(r, "ssid", s.res_ssid);
        cJSON_AddNumberToObject(r, "slot", s.res_slot);
        cJSON_AddBoolToObject(r, "updated", s.res_update);
        cJSON_AddBoolToObject(r, "overwrote", s.res_overwrote);
        cJSON_AddBoolToObject(r, "reverted", s.res_reverted);
        cJSON_AddBoolToObject(r, "held", s.res_held);
        cJSON_AddStringToObject(r, "new_failure", wqt_fail_name(s.new_fail));
        cJSON_AddNumberToObject(r, "new_reason", s.new_reason);
        cJSON_AddStringToObject(r, "previous_ssid", s.res_prior_ssid);
        cJSON_AddStringToObject(r, "previous_failure", wqt_fail_name(s.prior_fail));
        cJSON_AddNumberToObject(r, "previous_reason", s.prior_reason);
        cJSON_AddStringToObject(r, "ip", s.res_ip);
    }
    unlock();
    cJSON *sl = cJSON_AddArrayToObject(j, "slots");
    for (int i = 0; i < STA_PROFILE_MAX; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "slot", i);
        cJSON_AddBoolToObject(o, "valid", prof[i].valid);
        cJSON_AddStringToObject(o, "ssid", prof[i].valid ? prof[i].ssid : "");
        cJSON_AddBoolToObject(o, "held", prof[i].valid && (held & (1u << i)));
        cJSON_AddItemToArray(sl, o);
    }
    wipe(prof, sizeof(prof));
    return j;
}
