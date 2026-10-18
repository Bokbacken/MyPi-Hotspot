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
const pages = ["overview", "wifi", "access-point", "traffic", "ethernet", "system", "debug"];
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

console.log("Web UI serial, routing and signal tests passed");
