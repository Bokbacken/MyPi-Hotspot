"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const {
  frame8n1, waveformPath, pageFromHash, showPage, rssiStep, rssiColor, staSignal,
} = require("../main/web_ui.js");

assert.deepEqual(frame8n1("H"), [0, 0, 0, 0, 1, 0, 0, 1, 0, 1]);

const text = "Hotspot";
const allBits = frame8n1(text);
assert.equal(allBits.length, text.length * 10);
for (let i = 0; i < text.length; i++) {
  const byte = text.charCodeAt(i);
  assert.deepEqual(allBits.slice(i * 10, (i + 1) * 10), [
    0, ...Array.from({ length: 8 }, (_, bit) => (byte >> bit) & 1), 1,
  ]);
}

const waveform = waveformPath(frame8n1("H"), 7, 12, 23);
assert.equal(waveform.width, 70);
assert.equal(waveform.path.startsWith("M0 23"), true);
assert.equal(waveform.path.endsWith("H70"), true);
assert.equal((waveform.path.match(/ V/g) || []).length, 5);

// Hash routing: unknown/empty hashes fall back to the first (most used) page.
assert.equal(pageFromHash(""), "overview");
assert.equal(pageFromHash("#wifi"), "wifi");
assert.equal(pageFromHash("#DEBUG"), "debug");
assert.equal(pageFromHash("#diagnostics"), "debug");
assert.equal(pageFromHash("#nope"), "overview");
assert.equal(pageFromHash("#optimize"), "optimize");

function fakeElement(attrs) {
  const classes = new Set();
  return {
    hidden: false,
    dataset: attrs.dataset || {},
    attrs: { ...attrs.attrs },
    getAttribute(name) { return this.attrs[name]; },
    setAttribute(name, value) { this.attrs[name] = value; },
    removeAttribute(name) { delete this.attrs[name]; },
    classList: { toggle(name, on) { if (on) classes.add(name); else classes.delete(name); }, has: (n) => classes.has(n) },
  };
}
const pages = ["overview", "wifi", "access-point", "traffic", "ethernet", "system", "optimize", "debug"];
const sections = pages.map((page) => fakeElement({ dataset: { page } }));
const links = pages.map((page) => fakeElement({ attrs: { href: `/#${page}` } }));
const fakeDoc = {
  querySelectorAll(selector) { return selector.startsWith("section") ? sections : links; },
};
assert.equal(showPage(fakeDoc, "#traffic"), "traffic");
assert.deepEqual(sections.map((s) => s.hidden), pages.map((p) => p !== "traffic"));
assert.equal(links[3].classList.has("active"), true);
assert.equal(links[3].getAttribute("aria-current"), "page");
assert.equal(links[0].classList.has("active"), false);
assert.equal(showPage({ querySelectorAll: () => [] }, "#wifi"), null);

// The shell in web.c contains exactly these pages, in this order, plus matching nav buttons.
const webC = fs.readFileSync(path.join(__dirname, "../main/web.c"), "utf8");
assert.deepEqual([...webC.matchAll(/data-page='([a-z-]+)'/g)].map((m) => m[1]), pages);
assert.deepEqual([...webC.matchAll(/<a href='\/#([a-z-]+)'>/g)].map((m) => m[1]), pages);

// RSSI gradient: 20 steps red (<= -80) to green (>= -40), same as the LCD.
assert.equal(rssiStep(-90), 0);
assert.equal(rssiStep(-80), 0);
assert.equal(rssiStep(-40), 19);
assert.equal(rssiStep(-30), 19);
assert.equal(new Set(Array.from({ length: 41 }, (_, i) => rssiStep(-80 + i))).size, 20);
assert.equal(rssiColor(-85), "rgb(255,0,0)");
assert.equal(rssiColor(-35), "rgb(0,255,0)");
assert.deepEqual(staSignal({ wifi_state: "connected", rssi: -56 }), { text: "-56 dBm", color: rssiColor(-56) });
assert.equal(staSignal({ wifi_state: "disconnected", rssi: -56 }).text, "\u2014");
assert.equal(staSignal({ wifi_state: "connected", rssi: 0 }).text, "\u2014");
assert.equal(staSignal(null).text, "\u2014");

// Header polling: only on the visible main shell, never on standalone pages such as /update.
const { shouldPollStatus } = require("../main/web_ui.js");
assert.equal(shouldPollStatus(true, false), true);
assert.equal(shouldPollStatus(true, true), false);
assert.equal(shouldPollStatus(false, false), false);

// Password Show/Hide toggle (v0.8.8): one button per password field, flips the type both ways.
const { togglePassword, addPasswordToggles } = require("../main/web_ui.js");
assert.equal((webC.match(/type='password'/g) || []).length >= 2, true);
// v0.8.9: both profile APIs return the stored password and the edit forms fill it in.
assert.equal((webC.match(/cJSON_AddStringToObject\(o, "pass", p\.valid \? p\.pass : ""\)/g) || []).length, 2);
assert.ok(webC.includes("$('spw').value=x.pass||''") && webC.includes("$('apw').value=x.pass||''"));
assert.ok(/if\(FL\)\{FL=0;edS\(/.test(webC), "forms must be filled on first page load");
assert.ok(!/\$\('[as]pw'\)\.value=''/.test(webC), "password fields must not be blanked");
function fakeInput(id) {
  return { id, type: "password", dataset: {}, after: null,
    insertAdjacentElement(where, el) { assert.equal(where, "afterend"); this.after = el; } };
}
function fakeButton() {
  const b = { attrs: {}, listeners: {}, textContent: "", title: "",
    setAttribute(n, v) { this.attrs[n] = v; }, getAttribute(n) { return this.attrs[n]; },
    addEventListener(t, fn) { this.listeners[t] = fn; }, click() { this.listeners.click(); } };
  return b;
}
const pwInputs = [fakeInput("spw"), fakeInput("apw")];
const pwDoc = {
  querySelectorAll(sel) { assert.equal(sel, "input[type=password]"); return pwInputs.filter((i) => i.type === "password"); },
  createElement(tag) { assert.equal(tag, "button"); return fakeButton(); },
};
assert.equal(addPasswordToggles(pwDoc), 2);
for (const input of pwInputs) {
  const btn = input.after;
  assert.ok(btn && btn.type === "button" && btn.className === "pw-toggle");
  assert.equal(btn.getAttribute("aria-controls"), input.id);
  btn.click();
  assert.equal(input.type, "text");
  assert.equal(btn.getAttribute("aria-pressed"), "true");
  assert.match(btn.textContent, /Hide/);
  btn.click();
  assert.equal(input.type, "password");
  assert.match(btn.textContent, /Show/);
}
assert.equal(addPasswordToggles(pwDoc), 2);           // Idempotent: no second button
assert.equal(pwInputs.every((i) => i.dataset.pwToggle === "1"), true);
const lone = fakeInput(""), loneBtn = fakeButton();
assert.equal(togglePassword(lone, loneBtn), true);
assert.equal(togglePassword(lone, loneBtn), false);


// --- Optimize page (v0.9.0, trimmed v0.9.3) ------------------------------------------------
(async () => {
  const ui = require("../main/web_ui.js");
  const core = fs.readFileSync(path.join(__dirname, "../main/opt_config_core.h"), "utf8");
  const def = (n) => core.match(new RegExp(`#define OPT_DEFAULT_${n}\\s+(\\w+)`))[1];
  // JS defaults == firmware defaults (opt_config_core.h).
  assert.deepEqual(ui.optDefaults(), {
    wifi_tx_power_dbm: +def("TX_DBM"), ps_mode: +def("PS_MODE"), ap_beacon_ms: +def("BEACON_MS"),
    country: core.match(/#define OPT_DEFAULT_COUNTRY\s+"(\w+)"/)[1],
    tcp_wnd_kb: +def("TCP_WND_KB"), lwip_core: +def("LWIP_CORE"), napt_max: +def("NAPT_MAX"), napt_tcp_to: +def("NAPT_TCP_TO"),
    bw_5g: +def("BW_5G"), sdio_mhz: +def("SDIO_MHZ"),
  });
  assert.deepEqual(ui.OPT_FIELDS, ["wifi_tx_power_dbm", "ps_mode", "ap_beacon_ms", "country",
    "tcp_wnd_kb", "lwip_core", "napt_max", "napt_tcp_to", "bw_5g", "sdio_mhz"]);
  // Removed settings: old NVS keys are erased at boot; the v0.9.4 restored keys must not be in that list.
  const obsolete = core.match(/OPT_OBSOLETE_KEYS\[\] = \{([\s\S]*?)\};/)[1];
  for (const k of ["tcp_kb", "napt", "log_lvl"]) assert.ok(obsolete.includes(`"${k}"`), `obsolete key ${k}`);
  for (const k of ["tcp_wnd_kb", "lwip_core", "napt_max", "napt_tcp_to", "bw_5g", "sdio_mhz"]) assert.ok(!obsolete.includes(`"${k}"`), `restored key ${k}`);
  const optC = fs.readFileSync(path.join(__dirname, "../main/opt_config.c"), "utf8");
  assert.ok(/nvs_erase_key\(h, OPT_OBSOLETE_KEYS\[i\]\)/.test(optC));
  assert.ok(/__wrap_sys_thread_new/.test(optC) && /xTaskCreatePinnedToCore/.test(optC), "LwIP core pinning restored");
  assert.ok(/mypi_tcp_wnd = opt_tcp_wnd_bytes\(c->tcp_wnd_kb\)/.test(optC), "TCP window applied at boot");

  // web.c: GET /api/opt emits exactly these field names (+ read-only emergency_active), POST replies
  // reboot_required from opt_config_save(), the endpoints are registered.
  const addOpt = webC.match(/static void add_opt_cfg[\s\S]*?\n\}/)[0];
  assert.deepEqual([...addOpt.matchAll(/To\w+\(o, "(\w+)"/g)].map((m) => m[1]), ui.OPT_FIELDS);
  assert.ok(/AddBoolToObject\(j, "emergency_active", opt_emergency_flag\(\)\)/.test(webC));
  assert.ok(/opt_config_save\(&c, &reboot\)/.test(webC) && /AddBoolToObject\(r, "reboot_required", reboot\)/.test(webC));
  for (const [uri, m] of [["/api/opt", "HTTP_GET"], ["/api/opt", "HTTP_POST"], ["/api/radio", "HTTP_GET"], ["/api/radio", "HTTP_POST"],
    ["/api/clear_emergency", "HTTP_POST"], ["/api/reboot", "HTTP_POST"]]) {
    assert.ok(new RegExp(`\\.uri = "${uri}",\\s*\\.method = ${m}`).test(webC), `${m} ${uri} registered`);
  }
  assert.ok(/opt_emergency_set\(false\)/.test(webC.match(/api_clear_emergency_post[\s\S]*?\n\}/)[0]));
  const optPost = webC.match(/static esp_err_t api_opt_post[\s\S]*?\n\}/)[0];
  assert.ok(!/sdio_freq_mhz"|log_level"|"napt"/.test(optPost), "removed fields not accepted");
  for (const k of ["tcp_wnd_kb", "lwip_core", "napt_max", "napt_tcp_to", "bw_5g", "sdio_mhz"]) assert.ok(optPost.includes(`"${k}"`), `POST accepts ${k}`);
  // /api/radio: one shared protocol mask + band; GET reports the last STA channel.
  const radioPost = webC.match(/static esp_err_t api_radio_post[\s\S]*?\n\}/)[0];
  assert.ok(/"proto"/.test(radioPost) && /"band_mode"/.test(radioPost) && /opt_proto_merge/.test(radioPost));
  assert.ok(!/"sta_bw"|"ap_bw"|"tx_power_dbm"|"ap_dtim"/.test(radioPost), "width/TX/DTIM not settable");
  assert.ok(/"last_sta_channel", opt_sta_last_ch\(\)/.test(webC));
  // Reboot-required rule mirrors opt_config_reboot_required(): beacon, country, TCP window, LwIP core, NAPT size.
  const rule = core.match(/opt_config_reboot_required\([^)]*\)\s*\{([\s\S]*?)\n\}/)[1];
  assert.ok(/active->ap_beacon_ms != saved->ap_beacon_ms/.test(rule) && /strncmp\(active->country, saved->country/.test(rule) &&
    /tcp_wnd_kb/.test(rule) && /lwip_core/.test(rule) && /napt_max/.test(rule) &&
    /bw_5g/.test(rule) && /sdio_mhz/.test(rule) && !/ps_mode|tx_power|napt_tcp_to/.test(rule));
  const d0 = ui.optDefaults();
  assert.equal(ui.optRebootRequired(d0, { ...d0, ap_beacon_ms: 200 }), true);
  assert.equal(ui.optRebootRequired(d0, { ...d0, country: "US" }), true);
  assert.equal(ui.optRebootRequired(d0, { ...d0, wifi_tx_power_dbm: 14, ps_mode: 2, napt_tcp_to: 0 }), false);
  for (const [k, v] of [["tcp_wnd_kb", 128], ["lwip_core", 1], ["napt_max", 20], ["bw_5g", 20], ["sdio_mhz", 50]]) {
    assert.equal(ui.optRebootRequired(d0, { ...d0, [k]: v }), true, k);
  }

  // Shared protocol selector helpers (bits == RADIO_PROTO_* in wifi.h).
  const wifiH = fs.readFileSync(path.join(__dirname, "../main/wifi.h"), "utf8");
  for (const [k, n] of [["a", "A"], ["b", "B"], ["g", "G"], ["n", "N"], ["ac", "AC"], ["ax", "AX"]]) {
    const m = wifiH.match(new RegExp(`#define RADIO_PROTO_${n}\\s+(0x[0-9a-fA-F]+|\\d+)`));
    assert.ok(m, `RADIO_PROTO_${n}`);
    assert.equal(ui.PROTO_BITS[k], Number(m[1]), k);
  }
  assert.ok(/#define RADIO_PROTO_ALLOWED\s+0x77/.test(wifiH), "ax allowed (v0.9.3)");
  assert.equal(ui.protoLabel(0), "default (b/g/n + a/n/ac)");
  assert.equal(ui.protoLabel(0x24), "n/ac");
  assert.equal(ui.protoLabel(0x77), "a/b/g/n/ac/ax");
  assert.equal(ui.protoFilterForBand(0x77, 1), 0x47);
  assert.equal(ui.protoFilterForBand(0x77, 2), 0x74);
  assert.equal(ui.protoFilterForBand(0x177, 0), 0x77);
  assert.match(ui.protoHint(0x64, 0), /ax: known C5 issue/);
  assert.match(ui.protoHint(0x30, 0), /no 2\.4 GHz protocol checked/);
  assert.ok(!/no 2\.4 GHz/.test(ui.protoHint(0x30, 2)));
  assert.match(ui.protoHint(0x03, 0), /no 5 GHz protocol checked/);

  // Optimize page HTML: band + shared protocol checkboxes + TX/PS/beacon/country; old controls gone.
  const optHtml = webC.match(/data-page='optimize'[\s\S]*?<\/section>/)[0];
  for (const id of ["opt-band", "opt-tx", "opt-ps", "opt-beacon", "opt-country"]) assert.ok(optHtml.includes(`<select id='${id}'`), id);
  for (const k of ["a", "b", "g", "n", "ac", "ax"]) assert.ok(optHtml.includes(`<input type='checkbox' id='opt-proto-${k}'>`), k);
  // v0.9.4: TCP / LwIP / NAT section below the radio items, dropdowns only.
  assert.ok(optHtml.indexOf("<h3>TCP / LwIP / NAT</h3>") > optHtml.indexOf("<select id='opt-country'"));
  for (const id of ["opt-tcp-wnd", "opt-lwip-core", "opt-napt-max", "opt-napt-tcp-to"]) assert.ok(optHtml.includes(`<select id='${id}'`), id);
  assert.ok(/32 KB \(default\)[\s\S]*64 KB[\s\S]*128 KB/.test(optHtml));
  assert.ok(/512 \(default\)[\s\S]*1024[\s\S]*2048/.test(optHtml) && /2 h \(default\)[\s\S]*1 h[\s\S]*30 min/.test(optHtml));
  // v0.9.5: 5 GHz width + SDIO clock dropdowns in the radio grid (above the TCP section), fallback note.
  for (const id of ["opt-bw-5g", "opt-sdio"]) {
    assert.ok(optHtml.includes(`<select id='${id}'`), id);
    assert.ok(optHtml.indexOf(`<select id='${id}'`) < optHtml.indexOf("<h3>TCP / LwIP / NAT</h3>"), `${id} in radio grid`);
  }
  assert.ok(/<option value='40'>HT40 \(default\)<\/option>\s*"?\s*"?<option value='20'>HT20<\/option>/.test(optHtml));
  assert.ok(/<option value='40'>40 MHz \(default\)<\/option>\s*"?\s*"?<option value='50'>50 MHz<\/option>/.test(optHtml));
  assert.ok(/50 MHz automatically falls back to 40 MHz/.test(optHtml));
  assert.ok(!/SDIO is fixed at 40|width is automatic/i.test(optHtml), "old footer text gone");
  for (const gone of ["opt-napt'", "opt-log", "id='rsb'", "id='rab'",
    "ap-dtim", "ht-warn", "opt-emerg"]) {
    assert.ok(!optHtml.includes(gone), `removed from Optimize: ${gone}`);
  }
  assert.ok(/<option value='8'>8 dBm<\/option>[\s\S]*<option value='21'>21 dBm \(max\)/.test(optHtml));
  assert.ok(!/<option value='[2-7]'>[2-7] dBm/.test(optHtml), "TX power starts at 8 dBm");
  assert.ok(/Off \(best throughput\)[\s\S]*Min \(balanced\)[\s\S]*Max \(lowest power\)/.test(optHtml));
  assert.ok(/100 ms \(default\)[\s\S]*200 ms[\s\S]*300 ms/.test(optHtml));
  assert.ok(/SE \(Sweden\)[\s\S]*EU \(Europe\)[\s\S]*US \(United States\)[\s\S]*JP \(Japan\)/.test(optHtml));
  // Emergency status/clear lives on the Debug page now.
  const dbgHtml = webC.match(/data-page='debug'[\s\S]*?<\/section>/)[0];
  assert.ok(dbgHtml.includes("id='opt-emerg-st'") && dbgHtml.includes("id='opt-emerg-btn'"));
  const sysHtml = webC.match(/data-page='system'[\s\S]*?<\/section>/)[0];
  assert.ok(!/id='rsb'|id='rab'|id='band-mode'|Save radio/.test(sysHtml), "radio controls not on System");
  // Firmware: 2.4 GHz HT20 locked, PS/country/beacon wired, AP channel persistence, CSA.
  const wifiC = fs.readFileSync(path.join(__dirname, "../main/wifi.c"), "utf8");
  assert.ok(/esp_wifi_set_ps\(/.test(wifiC) && /esp_wifi_set_country\(/.test(wifiC) && /\.ghz_2g = WIFI_BW20/.test(wifiC));
  assert.ok(/beacon_interval = opt_ap_beacon_ms\(\)/.test(wifiC));
  // v0.9.5: apply_bw() gets the Optimize 5 GHz width; RADIO_BW_AUTO is no longer passed.
  assert.equal([...wifiC.matchAll(/apply_bw\([^;]*opt_bw_5g\(\)\)/g)].length >= 2, true, "apply_bw uses opt_bw_5g()");
  assert.ok(!/apply_bw\([^;]*RADIO_BW_AUTO\)/.test(wifiC));
  // SDIO: stored value drives the boot clock (trial/fallback kept); the 40 MHz HS cap is lifted via a link wrap.
  assert.ok(/opt_sdio_boot_mhz\(c\.sdio_mhz,/.test(optC) && !/opt_sdio_boot_mhz\(OPT_DEFAULT_SDIO_MHZ/.test(optC));
  assert.ok(/clock_freq_khz = opt_sdio_begin\(\)/.test(wifiC), "ESP-Hosted SDIO clock comes from Optimize");
  const mainCm0 = fs.readFileSync(path.join(__dirname, "../main/CMakeLists.txt"), "utf8");
  assert.ok(/-Wl,--wrap=sdmmc_io_enable_hs_mode/.test(mainCm0) && /sdio_clk\.c/.test(mainCm0));
  const clkC = fs.readFileSync(path.join(__dirname, "../main/sdio_clk.c"), "utf8");
  assert.ok(/__wrap_sdmmc_io_enable_hs_mode/.test(clkC) && /__real_sdmmc_io_enable_hs_mode/.test(clkC));
  assert.ok(/csa_count = 3/.test(wifiC) && /opt_ap_channel_pick\(/.test(wifiC) && /opt_sta_last_ch_store\(/.test(wifiC));
  assert.ok(/esp_wifi_get_channel\(/.test(wifiC.match(/static void refresh_ap_channel[\s\S]*?\n\}/)[0]));
  // lwIP: NAPT size/timeout and the TCP window come from variables, tcpip thread wrapper restored.
  const tune = fs.readFileSync(path.join(__dirname, "../main/lwip_tune.h"), "utf8");
  assert.ok(/#define IP_NAPT_MAX\s+\(mypi_napt_max_entries\)/.test(tune) && /#define IP_NAPT_TIMEOUT_MS_TCP\s+\(mypi_napt_tcp_timeout_ms\)/.test(tune));
  assert.ok(/#undef CONFIG_LWIP_TCP_WND_DEFAULT/.test(tune) && /#define CONFIG_LWIP_TCP_WND_DEFAULT \(mypi_tcp_wnd \? mypi_tcp_wnd : 131072\)/.test(tune));
  const mainCm = fs.readFileSync(path.join(__dirname, "../main/CMakeLists.txt"), "utf8");
  assert.ok(/-include\$\{CMAKE_CURRENT_LIST_DIR\}\/lwip_tune\.h/.test(mainCm) && /-Wl,--wrap=sys_thread_new/.test(mainCm));
  const sdk = fs.readFileSync(path.join(__dirname, "../sdkconfig.defaults"), "utf8");
  assert.ok(/^CONFIG_LWIP_TCP_MSS=1460$/m.test(sdk) && /^CONFIG_LWIP_TCP_WND_DEFAULT=32768$/m.test(sdk));
  // 128 KB needs window scaling and a recvmbox >= 131072/1460 + 2 = 92.
  assert.ok(/^CONFIG_LWIP_WND_SCALE=y$/m.test(sdk) && /^CONFIG_LWIP_TCP_RCV_SCALE=2$/m.test(sdk));
  assert.ok(+sdk.match(/^CONFIG_LWIP_TCP_RECVMBOX_SIZE=(\d+)$/m)[1] >= 92);

  // Optimize section: checkboxes, dropdowns and buttons only - no text input.
  const inputs = [...optHtml.matchAll(/<input[^>]*>/g)].map((m) => m[0]);
  assert.equal(inputs.length, 6);
  assert.ok(inputs.every((i) => /type='(radio|checkbox)'/.test(i)), "only radio/checkbox inputs");
  assert.ok(!/<textarea/.test(optHtml) && /Some changes require reboot/.test(optHtml) && /Reboot now/.test(optHtml));
  assert.ok(/Apply &amp; Save/.test(optHtml));

  // Mock device with the firmware's /api/opt + /api/radio semantics, driving the real page logic.
  const dev = { active: ui.optDefaults(), saved: ui.optDefaults(), emergency: false, calls: [],
    ra: { band_mode: 0, proto: 0 }, rs: { band_mode: 0, proto: 0 } };
  const reply = (o) => ({ json: async () => o });
  const fetchFn = async (url, opt) => {
    dev.calls.push([url, opt && opt.method, opt && opt.body]);
    if (url === "/api/opt" && !opt) {
      return reply({ ...dev.saved, emergency_active: dev.emergency, active: { ...dev.active },
        reboot_required: ui.optRebootRequired(dev.active, dev.saved), sdio_active_mhz: dev.active.sdio_mhz,
        sdio_fell_back: !!dev.fellBack, sdio_real_khz: dev.active.sdio_mhz * 1000 });
    }
    if (url === "/api/opt") {
      const b = JSON.parse(opt.body);
      for (const k of Object.keys(b)) assert.ok(ui.OPT_FIELDS.includes(k), `unknown field ${k}`);
      dev.saved = { ...dev.saved, ...b };
      for (const k of ["wifi_tx_power_dbm", "ps_mode", "napt_tcp_to"]) dev.active[k] = dev.saved[k];   // runtime fields
      return reply({ ok: true, reboot_required: ui.optRebootRequired(dev.active, dev.saved) });
    }
    if (url === "/api/radio" && !opt) {
      return reply({ ...dev.rs, sta_proto: dev.rs.proto, ap_proto: dev.rs.proto, active: { ...dev.ra }, saved: { ...dev.rs },
        last_sta_channel: 44, sta_channel: 44, info: { sta_width: "HT40" } });
    }
    if (url === "/api/radio") {
      const b = JSON.parse(opt.body);
      assert.deepEqual(Object.keys(b).sort(), ["band_mode", "proto"]);
      dev.rs = { band_mode: b.band_mode, proto: b.proto };
      return reply({ ok: true, msg: "saved - reboot to apply" });
    }
    if (url === "/api/clear_emergency") { dev.emergency = false; return reply({ ok: true, emergency_active: false }); }
    throw new Error("unexpected " + url);
  };
  const el = (extra) => ({ value: "", checked: false, textContent: "", hidden: false, disabled: false,
    classList: { set: new Set(), toggle(n, on) { if (on) this.set.add(n); else this.set.delete(n); } }, ...extra });
  const ids = {};
  for (const id of ["opt-band", "opt-tx", "opt-ps", "opt-beacon", "opt-country", "opt-banner", "opt-msg", "opt-emerg-btn",
    "opt-bw-5g", "opt-sdio", "opt-bw-5g-st", "opt-sdio-st", "opt-tcp-wnd", "opt-lwip-core", "opt-napt-max", "opt-napt-tcp-to",
    "opt-tcp-wnd-st", "opt-lwip-core-st", "opt-napt-max-st", "opt-napt-tcp-to-st",
    "opt-band-st", "opt-proto-st", "opt-tx-st", "opt-ps-st", "opt-beacon-st", "opt-country-st", "opt-emerg-st", "opt-form",
    "opt-proto-a", "opt-proto-b", "opt-proto-g", "opt-proto-n", "opt-proto-ac", "opt-proto-ax"]) ids[id] = el();
  ids["opt-banner"].hidden = true;
  const doc = { getElementById: (id) => ids[id] || null, querySelectorAll: () => [] };

  // GET: defaults land in the controls.
  const g = await ui.optLoad(doc, fetchFn);
  for (const k of ui.OPT_FIELDS) assert.ok(k in g, `GET /api/opt has ${k}`);
  assert.equal(g.emergency_active, false);
  assert.equal(ids["opt-band"].value, "0");
  assert.equal(ui.protoMaskFromChecks(doc), 0);
  assert.equal(ids["opt-tx"].value, "0");
  assert.equal(ids["opt-ps"].value, "0");
  assert.equal(ids["opt-beacon"].value, "100");
  assert.equal(ids["opt-country"].value, "SE");
  assert.match(ids["opt-ps-st"].textContent, /Recommended: Off in AP\+STA mode/);
  assert.equal(ids["opt-tcp-wnd"].value, "32");
  assert.equal(ids["opt-napt-max"].value, "5");
  assert.equal(ids["opt-tcp-wnd-st"].textContent, "active 32 KB");
  assert.equal(ids["opt-napt-max-st"].textContent, "active 512 sessions");
  assert.equal(ids["opt-napt-tcp-to-st"].textContent, "active 2 h");
  assert.match(ids["opt-proto-st"].textContent, /default/);
  assert.equal(ids["opt-banner"].hidden, true);
  assert.equal(ids["opt-emerg-btn"].disabled, true);
  assert.deepEqual(ui.optReadForm(doc), ui.optDefaults());

  // Shared protocol: n + ac on 5 GHz only -> one mask posted to /api/radio, reboot banner.
  ids["opt-proto-n"].checked = true; ids["opt-proto-ac"].checked = true; ids["opt-band"].value = "2";
  assert.deepEqual(ui.radioReadForm(doc), { proto: 0x24, band_mode: 2 });
  const p0 = await ui.optSave(doc, fetchFn);
  assert.deepEqual(p0, { ok: true, reboot_required: false });
  assert.deepEqual(dev.rs, { band_mode: 2, proto: 0x24 });
  assert.equal(ids["opt-banner"].hidden, false);
  assert.match(ids["opt-proto-st"].textContent, /saved n\/ac, reboot pending/);
  assert.match(ids["opt-band-st"].textContent, /5 GHz only, reboot pending/);
  assert.equal(ids["opt-proto-n"].checked && ids["opt-proto-ac"].checked && !ids["opt-proto-ax"].checked, true);
  dev.ra = { ...dev.rs };                         // "reboot"
  await ui.optLoad(doc, fetchFn);
  assert.equal(ids["opt-banner"].hidden, true);
  // ax can be selected (known C5 issue shown in the status).
  ids["opt-proto-ax"].checked = true;
  await ui.optSave(doc, fetchFn);
  assert.equal(dev.rs.proto, 0x64);
  dev.ra = { ...dev.rs };
  await ui.optLoad(doc, fetchFn);
  assert.match(ids["opt-proto-st"].textContent, /ax: known C5 issue/);

  // TX power and STA power save apply at runtime; country change needs a reboot.
  ids["opt-tx"].value = "14"; ids["opt-ps"].value = "1";
  assert.deepEqual(await ui.optSave(doc, fetchFn), { ok: true, reboot_required: false });
  assert.equal(dev.active.ps_mode, 1);
  assert.equal(dev.active.wifi_tx_power_dbm, 14);
  assert.match(ids["opt-tx-st"].textContent, /cap 14 dBm/);
  ids["opt-country"].value = "JP";
  assert.deepEqual(await ui.optSave(doc, fetchFn), { ok: true, reboot_required: true });
  assert.match(ids["opt-country-st"].textContent, /reboot pending/);
  assert.equal(ids["opt-banner"].hidden, false);
  ids["opt-country"].value = "SE";
  assert.deepEqual(await ui.optSave(doc, fetchFn), { ok: true, reboot_required: false });
  assert.equal(ids["opt-banner"].hidden, true);

  // Emergency flag: shown as SET with an enabled Clear button; Clear posts /api/clear_emergency.
  dev.emergency = true;
  await ui.optLoad(doc, fetchFn);
  assert.match(ids["opt-emerg-st"].textContent, /^SET/);
  assert.equal(ids["opt-emerg-btn"].disabled, false);
  const c = await ui.optClearEmergency(doc, fetchFn);
  assert.deepEqual(c, { ok: true, emergency_active: false });
  assert.equal(dev.calls.filter((x) => x[0] === "/api/clear_emergency" && x[1] === "POST").length, 1);
  assert.match(ids["opt-emerg-st"].textContent, /^clear/);
  assert.equal(ids["opt-emerg-btn"].disabled, true);

  // v0.9.4: TCP window 128 KB -> reboot pending; NAPT TCP timeout applies at once.
  ids["opt-tcp-wnd"].value = "128"; ids["opt-napt-tcp-to"].value = "0";
  const p9 = await ui.optSave(doc, fetchFn);
  assert.equal(p9.reboot_required, true);
  assert.equal(dev.saved.tcp_wnd_kb, 128);
  assert.equal(dev.active.napt_tcp_to, 0);
  assert.match(ids["opt-tcp-wnd-st"].textContent, /active 32 KB - saved 128, reboot pending/);
  assert.equal(ids["opt-napt-tcp-to-st"].textContent, "active 30 min");
  ids["opt-tcp-wnd"].value = "32"; ids["opt-napt-tcp-to"].value = "2";
  await ui.optSave(doc, fetchFn);
  dev.active = { ...dev.saved };

  // v0.9.5: 5 GHz width and SDIO clock - defaults in the form, both need a reboot.
  await ui.optLoad(doc, fetchFn);
  assert.equal(ids["opt-bw-5g"].value, "40");
  assert.equal(ids["opt-sdio"].value, "40");
  assert.equal(ids["opt-bw-5g-st"].textContent, "active 5 GHz HT40 (2.4 GHz always HT20); STA link now HT40");
  assert.equal(ids["opt-sdio-st"].textContent, "active 40 MHz (bus 40 MHz)");
  assert.deepEqual(ui.OPT_SELECTS.slice(-2), [["opt-bw-5g", "bw_5g"], ["opt-sdio", "sdio_mhz"]]);
  ids["opt-bw-5g"].value = "20"; ids["opt-sdio"].value = "50";
  assert.equal(ui.optReadForm(doc).bw_5g, 20);
  assert.equal(ui.optReadForm(doc).sdio_mhz, 50);
  const p10 = await ui.optSave(doc, fetchFn);
  assert.equal(p10.reboot_required, true);
  assert.equal(dev.saved.bw_5g, 20);
  assert.equal(dev.saved.sdio_mhz, 50);
  assert.match(ids["opt-bw-5g-st"].textContent, /active 5 GHz HT40 .* - saved HT20, reboot pending/);
  assert.match(ids["opt-sdio-st"].textContent, /^active 40 MHz \(bus 40 MHz\) - saved 50 MHz, reboot pending$/);
  assert.equal(ids["opt-banner"].hidden, false);
  // "Reboot" with 50 MHz that works.
  dev.active = { ...dev.saved };
  await ui.optLoad(doc, fetchFn);
  assert.equal(ids["opt-sdio-st"].textContent, "active 50 MHz (bus 50 MHz)");
  assert.match(ids["opt-bw-5g-st"].textContent, /^active 5 GHz HT20/);
  assert.equal(ids["opt-banner"].hidden, true);
  // "Reboot" where the C5 failed at 50 MHz: firmware rewrote the key to 40 and reports the fallback.
  dev.active.sdio_mhz = 40; dev.saved.sdio_mhz = 40; dev.fellBack = true;
  await ui.optLoad(doc, fetchFn);
  assert.equal(ids["opt-sdio"].value, "40");
  assert.equal(ids["opt-sdio-st"].textContent, "active 40 MHz (bus 40 MHz) - fell back from 50 MHz after a C5 failure");
  assert.equal(ui.sdioStatus({ sdio_mhz: 50, active: { sdio_mhz: 40 } }), "active 40 MHz - saved 50 MHz, reboot pending");

  console.log("Web UI serial, routing, signal and Optimize tests passed");
})().catch((e) => { console.error(e); process.exit(1); });
