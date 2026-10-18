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


// --- Optimize page (v0.9.0) ---------------------------------------------------------------
(async () => {
  const ui = require("../main/web_ui.js");
  const core = fs.readFileSync(path.join(__dirname, "../main/opt_config_core.h"), "utf8");
  const def = (n) => core.match(new RegExp(`#define OPT_DEFAULT_${n}\\s+(\\w+)`))[1];
  // JS defaults == firmware defaults (opt_config_core.h).
  assert.deepEqual(ui.optDefaults(), {
    sdio_freq_mhz: +def("SDIO_MHZ"), wifi_tx_power_dbm: +def("TX_DBM"), tcp_window_kb: +def("TCP_KB"),
    napt: def("NAPT") === "true", log_level: +def("LOG_LVL"),
  });
  assert.deepEqual(ui.OPT_FIELDS, ["sdio_freq_mhz", "wifi_tx_power_dbm", "tcp_window_kb", "napt", "log_level"]);

  // web.c: GET /api/opt emits exactly these field names (+ read-only emergency_active), POST replies
  // reboot_required from opt_config_save(), the three endpoints are registered.
  const addOpt = webC.match(/static void add_opt_cfg[\s\S]*?\n\}/)[0];
  assert.deepEqual([...addOpt.matchAll(/To\w+\(o, "(\w+)"/g)].map((m) => m[1]), ui.OPT_FIELDS);
  assert.ok(/AddBoolToObject\(j, "emergency_active", opt_emergency_flag\(\)\)/.test(webC));
  assert.ok(/opt_config_save\(&c, &reboot\)/.test(webC) && /AddBoolToObject\(r, "reboot_required", reboot\)/.test(webC));
  for (const [uri, m] of [["/api/opt", "HTTP_GET"], ["/api/opt", "HTTP_POST"], ["/api/clear_emergency", "HTTP_POST"], ["/api/reboot", "HTTP_POST"]]) {
    assert.ok(new RegExp(`\\.uri = "${uri}",\\s*\\.method = ${m}`).test(webC), `${m} ${uri} registered`);
  }
  assert.ok(/opt_emergency_set\(false\)/.test(webC.match(/api_clear_emergency_post[\s\S]*?\n\}/)[0]));
  // Reboot-required rule mirrors opt_config_reboot_required(): SDIO clock and TCP window only.
  assert.ok(/active->sdio_freq_mhz != saved->sdio_freq_mhz \|\| active->tcp_window_kb != saved->tcp_window_kb/.test(core));
  const d0 = ui.optDefaults();
  assert.equal(ui.optRebootRequired(d0, { ...d0, sdio_freq_mhz: 50 }), true);
  assert.equal(ui.optRebootRequired(d0, { ...d0, tcp_window_kb: 128 }), true);
  assert.equal(ui.optRebootRequired(d0, { ...d0, napt: false, log_level: 3, wifi_tx_power_dbm: 14 }), false);

  // Optimize section: checkboxes, radio buttons, dropdowns and buttons only - no text input.
  const optHtml = webC.match(/data-page='optimize'[\s\S]*?<\/section>/)[0];
  const inputs = [...optHtml.matchAll(/<input[^>]*>/g)].map((m) => m[0]);
  assert.ok(inputs.length >= 6);
  assert.ok(inputs.every((i) => /type='(radio|checkbox)'/.test(i)), "only radio/checkbox inputs");
  assert.ok(!/<textarea/.test(optHtml) && /Some changes require reboot/.test(optHtml) && /Reboot now/.test(optHtml));
  assert.ok(/Apply &amp; Save/.test(optHtml));

  // Mock device with the firmware's /api/opt semantics, driving the real page logic.
  const dev = { active: ui.optDefaults(), saved: ui.optDefaults(), emergency: false, calls: [] };
  const reply = (o) => ({ json: async () => o });
  const fetchFn = async (url, opt) => {
    dev.calls.push([url, opt && opt.method, opt && opt.body]);
    if (url === "/api/opt" && !opt) {
      return reply({ ...dev.saved, emergency_active: dev.emergency, active: { ...dev.active },
        reboot_required: ui.optRebootRequired(dev.active, dev.saved), sdio_active_mhz: dev.active.sdio_freq_mhz });
    }
    if (url === "/api/opt") {
      const b = JSON.parse(opt.body);
      for (const k of Object.keys(b)) assert.ok(ui.OPT_FIELDS.includes(k), `unknown field ${k}`);
      dev.saved = { ...dev.saved, ...b };
      for (const k of ["wifi_tx_power_dbm", "napt", "log_level"]) dev.active[k] = dev.saved[k];   // runtime fields
      return reply({ ok: true, reboot_required: ui.optRebootRequired(dev.active, dev.saved) });
    }
    if (url === "/api/clear_emergency") { dev.emergency = false; return reply({ ok: true, emergency_active: false }); }
    throw new Error("unexpected " + url);
  };
  const el = (extra) => ({ value: "", checked: false, textContent: "", hidden: false, disabled: false,
    classList: { set: new Set(), toggle(n, on) { if (on) this.set.add(n); else this.set.delete(n); } }, ...extra });
  const radios = { "opt-sdio": [el({ value: "40" }), el({ value: "50" })],
    "opt-tcp": [el({ value: "32" }), el({ value: "64" }), el({ value: "128" })] };
  const ids = {};
  for (const id of ["opt-tx", "opt-napt", "opt-log", "opt-banner", "opt-msg", "opt-emerg-btn", "opt-sdio-st",
    "opt-tx-st", "opt-tcp-st", "opt-napt-st", "opt-log-st", "opt-emerg-st", "opt-form"]) ids[id] = el();
  ids["opt-banner"].hidden = true;
  const doc = { getElementById: (id) => ids[id] || null,
    querySelectorAll: (sel) => radios[sel.match(/name=([\w-]+)/)[1]] || [] };

  // GET /api/opt: defaults land in the controls.
  const g = await ui.optLoad(doc, fetchFn);
  for (const k of ui.OPT_FIELDS) assert.ok(k in g, `GET /api/opt has ${k}`);
  assert.equal(g.emergency_active, false);
  assert.equal(radios["opt-sdio"][0].checked, true);
  assert.equal(radios["opt-tcp"][1].checked, true);
  assert.equal(ids["opt-tx"].value, "0");
  assert.equal(ids["opt-napt"].checked, true);
  assert.equal(ids["opt-log"].value, "2");
  assert.equal(ids["opt-banner"].hidden, true);
  assert.equal(ids["opt-emerg-btn"].disabled, true);
  assert.deepEqual(ui.optReadForm(doc), ui.optDefaults());

  // POST /api/opt with sdio_freq_mhz=50 -> reboot_required:true and the yellow banner.
  radios["opt-sdio"][0].checked = false; radios["opt-sdio"][1].checked = true;
  const p1 = await ui.optSave(doc, fetchFn);
  assert.equal(JSON.parse(dev.calls.find((c) => c[1] === "POST")[2]).sdio_freq_mhz, 50);
  assert.deepEqual(p1, { ok: true, reboot_required: true });
  assert.equal(ids["opt-banner"].hidden, false);
  assert.match(ids["opt-sdio-st"].textContent, /reboot pending/);
  // Runtime-only change (log level) after reverting SDIO -> no reboot needed, banner hidden.
  radios["opt-sdio"][1].checked = false; radios["opt-sdio"][0].checked = true; ids["opt-log"].value = "3";
  const p2 = await ui.optSave(doc, fetchFn);
  assert.deepEqual(p2, { ok: true, reboot_required: false });
  assert.equal(ids["opt-banner"].hidden, true);
  assert.equal(dev.active.log_level, 3);

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

  console.log("Web UI serial, routing, signal and Optimize tests passed");
})().catch((e) => { console.error(e); process.exit(1); });
