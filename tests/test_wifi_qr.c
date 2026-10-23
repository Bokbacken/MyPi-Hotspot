// Host test for Wi-Fi QR provisioning: WIFI: parser, visibility, slot choice, the connect/rollback
// state machine, and end-to-end decoding of rendered QR images with the vendored quirc.
//
//   cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I main -I components/quirc/lib
//      -DQUIRC_FLOAT_TYPE=float -DQUIRC_USE_TGMATH -Wno-unused-parameter -Wno-sign-compare (quirc)
//      tests/test_wifi_qr.c main/qr_decode.c
//      components/quirc/lib/*.c -lm -o /tmp/mypi-wifi-qr && /tmp/mypi-wifi-qr

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qr_decode.h"
#include "wifi_qr_core.h"
#include "wifi_qr_fixture.h"

static wq_err_t parse(const char *s, wq_wifi_t *w) { return wq_parse(s, strlen(s), w); }

static void expect_err(const char *s, wq_err_t e)
{
    wq_wifi_t w;
    wq_err_t got = parse(s, &w);
    if (got != e) {
        fprintf(stderr, "payload %s: got %d (%s), want %d\n", s, got, wq_err_text(got), e);
        assert(0);
    }
    if (e != WQ_OK) {
        // A rejected code never leaves credentials behind.
        static const wq_wifi_t zero;
        assert(memcmp(&w, &zero, sizeof(w)) == 0);
    }
}

static void test_parse_ok(void)
{
    wq_wifi_t w;
    assert(parse("WIFI:S:Home;T:WPA;P:secret123;;", &w) == WQ_OK);
    assert(strcmp(w.ssid, "Home") == 0 && w.ssid_len == 4 && strcmp(w.pass, "secret123") == 0);
    assert(w.security == WQ_SEC_WPA && !w.hidden && !w.transition_disable && !w.pass_is_psk);
    assert(strcmp(w.type, "WPA") == 0 && w.ignored[0] == '\0');

    // Any field order, escapes, lower-case prefix/keys, hidden flag, missing final ';'.
    assert(parse("wifi:p:a\\;b\\,c\\:d\\\\e\\\"f;t:wpa2;h:TRUE;s:Net\\;1", &w) == WQ_OK);
    assert(strcmp(w.ssid, "Net;1") == 0 && strcmp(w.pass, "a;b,c:d\\e\"f") == 0);
    assert(w.security == WQ_SEC_WPA2 && w.hidden);

    // Open networks.
    assert(parse("WIFI:S:Cafe;T:nopass;;", &w) == WQ_OK && w.security == WQ_SEC_OPEN && w.pass_len == 0);
    assert(parse("WIFI:S:Cafe;T:nopass;P:;;", &w) == WQ_OK && w.security == WQ_SEC_OPEN);
    assert(parse("WIFI:S:Cafe;;", &w) == WQ_OK && w.security == WQ_SEC_OPEN);
    assert(parse("WIFI:S:Cafe;T:;;", &w) == WQ_OK && w.security == WQ_SEC_OPEN);
    // No T but a password: generic WPA personal (never open).
    assert(parse("WIFI:S:Cafe;P:password;;", &w) == WQ_OK && w.security == WQ_SEC_WPA);

    // WPA3: SAE/WPA3 types and R bit 0 (transition disable) on WPA.
    assert(parse("WIFI:T:SAE;S:W3;P:password;;", &w) == WQ_OK && w.security == WQ_SEC_WPA3);
    assert(parse("WIFI:T:WPA3;S:W3;P:password;;", &w) == WQ_OK && w.security == WQ_SEC_WPA3);
    assert(parse("WIFI:T:WPA;S:W3;P:password;R:1;;", &w) == WQ_OK && w.security == WQ_SEC_WPA3);
    assert(w.transition_disable);
    assert(parse("WIFI:T:WPA;S:W3;P:password;R:0;;", &w) == WQ_OK && w.security == WQ_SEC_WPA);

    // Quotes are stripped; escaped quotes are kept.
    assert(parse("WIFI:S:\"My Net\";T:WPA;P:\"pass word\";;", &w) == WQ_OK);
    assert(strcmp(w.ssid, "My Net") == 0 && strcmp(w.pass, "pass word") == 0);
    assert(parse("WIFI:S:\\\"q\\\";;", &w) == WQ_OK && strcmp(w.ssid, "\"q\"") == 0);

    // Byte-length boundaries: SSID 32 bytes (UTF-8 counts bytes), password 63 / 64 hex.
    char buf[256];
    char ssid32[33], pw63[64], hex64[65];
    memset(ssid32, 'x', 32); ssid32[32] = 0;
    memset(pw63, 'p', 63); pw63[63] = 0;
    for (int i = 0; i < 64; i++) hex64[i] = "0123456789abcdef"[i % 16];
    hex64[64] = 0;
    snprintf(buf, sizeof(buf), "WIFI:S:%s;T:WPA;P:%s;;", ssid32, pw63);
    assert(parse(buf, &w) == WQ_OK && w.ssid_len == 32 && w.pass_len == 63);
    snprintf(buf, sizeof(buf), "WIFI:S:x;T:WPA;P:%s;;", hex64);
    assert(parse(buf, &w) == WQ_OK && w.pass_is_psk && w.pass_len == 64);
    // 16 x "é" = 32 bytes.
    strcpy(buf, "WIFI:S:");
    for (int i = 0; i < 16; i++) strcat(buf, "\xC3\xA9");
    strcat(buf, ";;");
    assert(parse(buf, &w) == WQ_OK && w.ssid_len == 32);
    assert(parse("WIFI:S:x;T:WPA;P:12345678;;", &w) == WQ_OK);

    // Unknown fields are ignored and reported; trailing whitespace after ";;" is fine.
    assert(parse("WIFI:S:x;T:WPA;P:12345678;V:1;X:\\;y;;\r\n", &w) == WQ_OK);
    assert(strcmp(w.ignored, "V,X") == 0);
}

static void test_parse_errors(void)
{
    expect_err("", WQ_ERR_NOT_WIFI);
    expect_err("https://example.com", WQ_ERR_NOT_WIFI);
    expect_err("WIFI", WQ_ERR_NOT_WIFI);
    expect_err("MECARD:N:x;;", WQ_ERR_NOT_WIFI);

    char big[WQ_PAYLOAD_MAX + 16];
    memset(big, 'a', sizeof(big) - 1);
    big[sizeof(big) - 1] = 0;
    memcpy(big, "WIFI:S:", 7);
    expect_err(big, WQ_ERR_TOO_LONG);

    expect_err("WIFI:", WQ_ERR_SSID);
    expect_err("WIFI:T:WPA;P:12345678;;", WQ_ERR_SSID);
    expect_err("WIFI:S:;;", WQ_ERR_SSID);
    expect_err("WIFI:S:\"\";;", WQ_ERR_SSID);
    expect_err("WIFI:S:123456789012345678901234567890123;;", WQ_ERR_SSID);   // 33 bytes
    expect_err("WIFI:S:a\tb;;", WQ_ERR_SSID);
    expect_err("WIFI:S:\xC3;;", WQ_ERR_SSID);           // Truncated UTF-8
    expect_err("WIFI:S:\xC0\xAF;;", WQ_ERR_SSID);       // Overlong
    expect_err("WIFI:S:\xED\xA0\x80;;", WQ_ERR_SSID);   // Surrogate

    expect_err("WIFI:S:x;;junk", WQ_ERR_MALFORMED);
    expect_err("WIFI:S:x\\n;;", WQ_ERR_MALFORMED);      // Unsupported escape
    expect_err("WIFI:S:x\\", WQ_ERR_MALFORMED);         // Dangling escape
    expect_err("WIFI:Sx;;", WQ_ERR_MALFORMED);
    expect_err("WIFI::x;;", WQ_ERR_MALFORMED);
    expect_err("WIFI:S-1:x;;", WQ_ERR_MALFORMED);
    expect_err("WIFI:S:a;S:b;;", WQ_ERR_DUPLICATE_FIELD);
    expect_err("WIFI:S:a;P:12345678;P:12345678;T:WPA;;", WQ_ERR_DUPLICATE_FIELD);
    wq_wifi_t w;
    assert(wq_parse("WIFI:S:a\0b;;", 12, &w) == WQ_ERR_MALFORMED);

    expect_err("WIFI:S:x;T:WEP;P:12345;;", WQ_ERR_UNSUPPORTED_SECURITY);
    expect_err("WIFI:S:x;T:WPA2-EAP;P:12345678;;", WQ_ERR_UNSUPPORTED_SECURITY);
    expect_err("WIFI:S:x;T:WPA;E:PEAP;I:user;P:12345678;;", WQ_ERR_UNSUPPORTED_SECURITY);
    expect_err("WIFI:S:x;T:WPA;PH2:MSCHAPV2;P:12345678;;", WQ_ERR_UNSUPPORTED_SECURITY);
    expect_err("WIFI:S:x;T:WPA4;P:12345678;;", WQ_ERR_UNSUPPORTED_SECURITY);
    expect_err("WIFI:S:x;T:SAE;P:12345678;K:MDkwEwYHKoZIzj0CAQ;;", WQ_ERR_SAE_PK);

    expect_err("WIFI:S:x;T:WPA;;", WQ_ERR_PASSWORD_MISSING);
    expect_err("WIFI:S:x;T:WPA;P:;;", WQ_ERR_PASSWORD_MISSING);
    expect_err("WIFI:S:x;T:nopass;P:12345678;;", WQ_ERR_OPEN_WITH_PASSWORD);
    expect_err("WIFI:S:x;T:WPA;P:1234567;;", WQ_ERR_PASSWORD);          // 7 characters
    expect_err("WIFI:S:x;T:WPA;P:pass\xC3\xA9word;;", WQ_ERR_PASSWORD); // Not ASCII
    char buf[256], pw64[65];
    memset(pw64, 'g', 64); pw64[64] = 0;            // 64 characters, not hex
    snprintf(buf, sizeof(buf), "WIFI:S:x;T:WPA;P:%s;;", pw64);
    expect_err(buf, WQ_ERR_PASSWORD);
    memset(pw64, 'a', 64);                          // Quoted 64 hex = 64-character passphrase
    snprintf(buf, sizeof(buf), "WIFI:S:x;T:WPA;P:\"%s\";;", pw64);
    expect_err(buf, WQ_ERR_PASSWORD);
    snprintf(buf, sizeof(buf), "WIFI:S:x;T:SAE;P:%s;;", pw64);  // SAE has no raw PSK
    expect_err(buf, WQ_ERR_PASSWORD);
    snprintf(buf, sizeof(buf), "WIFI:S:x;T:WPA;P:%sa;;", pw64); // 65
    expect_err(buf, WQ_ERR_PASSWORD);

    expect_err("WIFI:S:x;H:yes;;", WQ_ERR_HIDDEN);
    expect_err("WIFI:S:x;T:WPA;P:12345678;R:z;;", WQ_ERR_TRANSITION);
    expect_err("WIFI:S:x;T:WPA;P:12345678;R:2;;", WQ_ERR_TRANSITION);   // SAE-PK only
    expect_err("WIFI:S:x;T:WPA;P:12345678;R:;;", WQ_ERR_TRANSITION);
    expect_err("WIFI:S:x;T:nopass;R:1;;", WQ_ERR_TRANSITION);
}

static void test_visibility(void)
{
    wq_net_t nets[] = {
        {"Other", -40, 1, WQ_AP_WPA2},
        {"Home", -70, 6, WQ_AP_WPA2},
        {"Home", -55, 36, WQ_AP_WPA2_WPA3},
        {"home", -30, 11, WQ_AP_OPEN},
    };
    wq_visibility_t v = wq_visibility(nets, 4, "Home", WQ_SEC_WPA3);
    assert(v.state == WQ_VIS_VISIBLE && v.matches == 2 && v.rssi == -55 && v.channel == 36 && v.sec_ok);
    v = wq_visibility(nets, 2, "Home", WQ_SEC_WPA3);
    assert(v.state == WQ_VIS_VISIBLE && v.rssi == -70 && !v.sec_ok);   // WPA3 required, AP WPA2 only
    v = wq_visibility(nets, 4, "Missing", WQ_SEC_WPA);
    assert(v.state == WQ_VIS_NOT_SEEN && v.matches == 0 && v.rssi == 0);
    v = wq_visibility(NULL, -1, "Home", WQ_SEC_WPA);
    assert(v.state == WQ_VIS_SCAN_FAILED);
    v = wq_visibility(nets, 0, "Home", WQ_SEC_WPA);
    assert(v.state == WQ_VIS_NOT_SEEN);
    assert(strcmp(wq_vis_name(WQ_VIS_NOT_SEEN), "not_seen") == 0);

    assert(wq_sec_compatible(WQ_SEC_OPEN, WQ_AP_OPEN) && !wq_sec_compatible(WQ_SEC_OPEN, WQ_AP_WPA2));
    assert(wq_sec_compatible(WQ_SEC_WPA, WQ_AP_WPA) && wq_sec_compatible(WQ_SEC_WPA, WQ_AP_WPA3));
    assert(!wq_sec_compatible(WQ_SEC_WPA, WQ_AP_OPEN) && !wq_sec_compatible(WQ_SEC_WPA, WQ_AP_WEP));
    assert(!wq_sec_compatible(WQ_SEC_WPA, WQ_AP_ENTERPRISE));
    assert(!wq_sec_compatible(WQ_SEC_WPA2, WQ_AP_WPA) && wq_sec_compatible(WQ_SEC_WPA2, WQ_AP_WPA_WPA2));
    assert(!wq_sec_compatible(WQ_SEC_WPA3, WQ_AP_WPA2) && wq_sec_compatible(WQ_SEC_WPA3, WQ_AP_WPA2_WPA3));
}

static void test_slots(void)
{
    wq_slot_info_t s[4] = {{true, "A"}, {false, ""}, {true, "Home"}, {false, ""}};
    int slot;
    bool update;
    assert(wq_pick_slot(s, 4, "New", -1, false, &slot, &update) == WQ_SLOT_OK && slot == 1 && !update);
    assert(wq_pick_slot(s, 4, "New", 3, false, &slot, &update) == WQ_SLOT_OK && slot == 3);
    assert(wq_pick_slot(s, 4, "New", 0, false, &slot, &update) == WQ_SLOT_NEED_OVERWRITE && slot == -1);
    assert(wq_pick_slot(s, 4, "New", 0, true, &slot, &update) == WQ_SLOT_OK && slot == 0 && !update);
    assert(wq_pick_slot(s, 4, "New", 4, true, &slot, &update) == WQ_SLOT_BAD);
    // Duplicate SSID: update in place, never a second copy.
    assert(wq_pick_slot(s, 4, "Home", -1, false, &slot, &update) == WQ_SLOT_OK && slot == 2 && update);
    assert(wq_pick_slot(s, 4, "Home", 2, false, &slot, &update) == WQ_SLOT_OK && slot == 2 && update);
    assert(wq_pick_slot(s, 4, "Home", 1, true, &slot, &update) == WQ_SLOT_DUPLICATE);
    assert(wq_pick_slot(s, 4, "home", -1, false, &slot, &update) == WQ_SLOT_OK && slot == 1);   // Case-sensitive
    // Full: an explicit slot and overwrite are required.
    wq_slot_info_t full[2] = {{true, "A"}, {true, "B"}};
    assert(wq_pick_slot(full, 2, "C", -1, true, &slot, &update) == WQ_SLOT_FULL);
    assert(wq_pick_slot(full, 2, "C", 1, false, &slot, &update) == WQ_SLOT_NEED_OVERWRITE);
    assert(wq_pick_slot(full, 2, "C", 1, true, &slot, &update) == WQ_SLOT_OK && slot == 1);
    assert(wq_pick_slot(full, 2, "B", -1, false, &slot, &update) == WQ_SLOT_OK && slot == 1 && update);
}

static wqt_event_t ev(wqt_event_type_t type, uint32_t attempt, wqt_fail_t fail)
{
    return (wqt_event_t){.type = type, .attempt = attempt, .fail = fail};
}

static void test_txn(void)
{
    wqt_t t;
    // Success: association alone is not success; the IP is.
    assert(wqt_start(&t, true, 2) == WQT_ACT_CONNECT_NEW);
    wqt_attempt_started(&t, 10);
    assert(wqt_event(&t, ev(WQT_EV_ASSOCIATED, 10, 0)) == WQT_ACT_NONE && t.phase == WQT_CONNECTING);
    assert(wqt_event(&t, ev(WQT_EV_GOT_IP, 9, 0)) == WQT_ACT_NONE);         // Stale attempt
    assert(wqt_event(&t, ev(WQT_EV_GOT_IP, 10, 0)) == WQT_ACT_FINISH && t.result == WQT_RES_CONNECTED);
    assert(wqt_event(&t, ev(WQT_EV_DISCONNECTED, 10, WQT_FAIL_NO_AP)) == WQT_ACT_NONE);  // Done: ignored

    // Events before the first connect is issued are ignored.
    wqt_start(&t, true, 2);
    assert(wqt_event(&t, ev(WQT_EV_DISCONNECTED, 0, WQT_FAIL_NO_AP)) == WQT_ACT_NONE);

    // Wrong password: no retry, rollback to the previous connection, which succeeds.
    wqt_start(&t, true, 2);
    wqt_attempt_started(&t, 1);
    assert(wqt_event(&t, ev(WQT_EV_DISCONNECTED, 1, WQT_FAIL_AUTH)) == WQT_ACT_CONNECT_PRIOR);
    assert(t.phase == WQT_ROLLBACK && t.new_fail == WQT_FAIL_AUTH);
    assert(wqt_event(&t, ev(WQT_EV_GOT_IP, 1, 0)) == WQT_ACT_NONE);         // Late event of the new network
    wqt_attempt_started(&t, 2);
    assert(wqt_event(&t, ev(WQT_EV_CANCEL, 0, 0)) == WQT_ACT_NONE);         // Rollback not cancellable
    assert(wqt_event(&t, ev(WQT_EV_GOT_IP, 2, 0)) == WQT_ACT_FINISH && t.result == WQT_RES_FAILED_RESTORED);

    // AP not found: one retry, then rollback; the rollback fails twice -> restore failed.
    wqt_start(&t, true, 2);
    wqt_attempt_started(&t, 5);
    assert(wqt_event(&t, ev(WQT_EV_DISCONNECTED, 5, WQT_FAIL_NO_AP)) == WQT_ACT_CONNECT_NEW && t.tries == 2);
    assert(wqt_event(&t, ev(WQT_EV_DISCONNECTED, 5, WQT_FAIL_NO_AP)) == WQT_ACT_NONE);   // Duplicate, stale
    wqt_attempt_started(&t, 6);
    assert(wqt_event(&t, ev(WQT_EV_DISCONNECTED, 6, WQT_FAIL_NO_AP)) == WQT_ACT_CONNECT_PRIOR);
    wqt_attempt_started(&t, 7);
    assert(wqt_event(&t, ev(WQT_EV_DISCONNECTED, 7, WQT_FAIL_ASSOC)) == WQT_ACT_CONNECT_PRIOR);
    wqt_attempt_started(&t, 8);
    assert(wqt_event(&t, ev(WQT_EV_TIMEOUT, 8, 0)) == WQT_ACT_FINISH);
    assert(t.result == WQT_RES_FAILED_RESTORE_FAILED && t.new_fail == WQT_FAIL_NO_AP && t.prior_fail == WQT_FAIL_TIMEOUT);

    // Associated but no DHCP lease before the deadline: an IP failure, not a timeout.
    wqt_start(&t, false, 2);
    wqt_attempt_started(&t, 3);
    wqt_event(&t, ev(WQT_EV_ASSOCIATED, 3, 0));
    assert(wqt_event(&t, ev(WQT_EV_TIMEOUT, 3, 0)) == WQT_ACT_FINISH);
    assert(t.result == WQT_RES_FAILED_NO_PRIOR && t.new_fail == WQT_FAIL_NO_IP);

    // No previous connection: failure is reported, nothing to restore.
    wqt_start(&t, false, 1);
    wqt_attempt_started(&t, 4);
    assert(wqt_event(&t, ev(WQT_EV_DISCONNECTED, 4, WQT_FAIL_NO_AP)) == WQT_ACT_FINISH);
    assert(t.result == WQT_RES_FAILED_NO_PRIOR);

    // Connect request rejected by the driver.
    wqt_start(&t, true, 2);
    assert(wqt_event(&t, ev(WQT_EV_START_FAILED, 0, 0)) == WQT_ACT_CONNECT_PRIOR && t.new_fail == WQT_FAIL_START);
    assert(wqt_event(&t, ev(WQT_EV_START_FAILED, 0, 0)) == WQT_ACT_FINISH);
    assert(t.result == WQT_RES_FAILED_RESTORE_FAILED && t.prior_fail == WQT_FAIL_START);

    // Cancel during the attempt: restore the previous connection.
    wqt_start(&t, true, 2);
    wqt_attempt_started(&t, 11);
    assert(wqt_event(&t, ev(WQT_EV_CANCEL, 0, 0)) == WQT_ACT_CONNECT_PRIOR);
    wqt_attempt_started(&t, 12);
    assert(wqt_event(&t, ev(WQT_EV_GOT_IP, 11, 0)) == WQT_ACT_NONE);
    assert(wqt_event(&t, ev(WQT_EV_GOT_IP, 12, 0)) == WQT_ACT_FINISH && t.result == WQT_RES_CANCELLED_RESTORED);
    wqt_start(&t, false, 2);
    wqt_attempt_started(&t, 13);
    assert(wqt_event(&t, ev(WQT_EV_CANCEL, 0, 0)) == WQT_ACT_FINISH && t.result == WQT_RES_CANCELLED);

    // The user picked another network meanwhile: stop without any rollback, in either phase.
    wqt_start(&t, true, 2);
    wqt_attempt_started(&t, 20);
    assert(wqt_event(&t, ev(WQT_EV_SUPERSEDED, 0, 0)) == WQT_ACT_FINISH && t.result == WQT_RES_SUPERSEDED);
    assert(wqt_event(&t, ev(WQT_EV_DISCONNECTED, 20, WQT_FAIL_NO_AP)) == WQT_ACT_NONE);
    wqt_start(&t, true, 2);
    wqt_attempt_started(&t, 21);
    wqt_event(&t, ev(WQT_EV_DISCONNECTED, 21, WQT_FAIL_AUTH));
    wqt_attempt_started(&t, 22);
    assert(wqt_event(&t, ev(WQT_EV_SUPERSEDED, 0, 0)) == WQT_ACT_FINISH && t.result == WQT_RES_SUPERSEDED);

    assert(strcmp(wqt_result_name(WQT_RES_FAILED_RESTORE_FAILED), "failed_restore_failed") == 0);
    assert(strcmp(wqt_fail_name(WQT_FAIL_NO_IP), "no_ip") == 0);
}

// --- End to end: render module matrices into a grayscale camera-like image and decode ---

static void render(uint8_t *img, int w, int h, const char *const *rows, int n, int x0, int y0, int scale)
{
    for (int y = 0; y < n * scale; y++) {
        for (int x = 0; x < n * scale; x++) {
            if (y0 + y < 0 || y0 + y >= h || x0 + x < 0 || x0 + x >= w) continue;
            img[(y0 + y) * w + x0 + x] = rows[y / scale][x / scale] == '#' ? 40 : 210;
        }
    }
}

static struct quirc *image(int w, int h)
{
    struct quirc *q = quirc_new();
    assert(q && quirc_resize(q, w, h) == 0);
    uint8_t *img = quirc_begin(q, NULL, NULL);
    // Uneven lighting, like a camera frame.
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) img[y * w + x] = (uint8_t)(180 + (x * 40) / w - (y * 20) / h);
    }
    return q;
}

static void test_decode(void)
{
    const int w = 320, h = 240;
    struct quirc *q = image(w, h);
    uint8_t *img = quirc_begin(q, NULL, NULL);
    render(img, w, h, WIFI_QR, WIFI_QR_SIZE, 60, 40, 4);
    quirc_end(q);
    qr_scan_result_t r;
    assert(qr_scan(q, &r));
    assert(r.codes == 1 && r.decoded == 1 && r.wifi == 1 && r.other == 0 && !r.too_long);
    wq_wifi_t wifi;
    assert(wq_parse(r.payload, r.payload_len, &wifi) == WQ_OK);
    assert(strcmp(wifi.ssid, "Caf\xC3\xA9 ;Net") == 0 && strcmp(wifi.pass, "pa:ss\\word 1") == 0);
    assert(wifi.security == WQ_SEC_WPA && wifi.hidden);
    quirc_destroy(q);

    // A non-Wi-Fi code is decoded but reported as "other" (no payload, nothing to apply).
    q = image(w, h);
    img = quirc_begin(q, NULL, NULL);
    render(img, w, h, URL_QR, URL_QR_SIZE, 100, 60, 5);
    quirc_end(q);
    assert(qr_scan(q, &r) && r.decoded == 1 && r.wifi == 0 && r.other == 1 && r.payload_len == 0);
    quirc_destroy(q);

    // Both codes in one frame: the Wi-Fi one is still found.
    q = image(w, h);
    img = quirc_begin(q, NULL, NULL);
    render(img, w, h, WIFI_QR, WIFI_QR_SIZE, 10, 50, 4);
    render(img, w, h, URL_QR, URL_QR_SIZE, 180, 60, 4);
    quirc_end(q);
    assert(qr_scan(q, &r) && r.decoded == 2 && r.wifi == 1 && r.other == 1);
    quirc_destroy(q);

    // Mirrored image (e.g. hflip on): decoded through quirc_flip.
    q = image(w, h);
    img = quirc_begin(q, NULL, NULL);
    render(img, w, h, WIFI_QR, WIFI_QR_SIZE, 60, 40, 4);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w / 2; x++) {
            uint8_t t = img[y * w + x];
            img[y * w + x] = img[y * w + w - 1 - x];
            img[y * w + w - 1 - x] = t;
        }
    }
    quirc_end(q);
    assert(qr_scan(q, &r) && r.wifi == 1);
    assert(wq_parse(r.payload, r.payload_len, &wifi) == WQ_OK && wifi.ssid_len == strlen("Caf\xC3\xA9 ;Net"));
    quirc_destroy(q);

    // Nothing in view.
    q = image(w, h);
    quirc_end(q);
    assert(qr_scan(q, &r) && r.codes == 0 && r.wifi == 0);
    quirc_destroy(q);
}

int main(void)
{
    test_parse_ok();
    test_parse_errors();
    test_visibility();
    test_slots();
    test_txn();
    test_decode();
    puts("wifi qr tests passed");
    return 0;
}
