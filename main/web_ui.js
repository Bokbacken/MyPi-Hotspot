(function (root) {
  "use strict";

  function frame8n1(text) {
    const bytes = new TextEncoder().encode(String(text));
    const bits = [];
    for (const byte of bytes) {
      bits.push(0);
      for (let bit = 0; bit < 8; bit++) bits.push((byte >> bit) & 1);
      bits.push(1);
    }
    return bits;
  }

  function waveformPath(bits, bitWidth, highY, lowY) {
    if (!bits.length) return { path: "", width: 0 };
    const width = bitWidth || 10;
    const high = highY === undefined ? 4 : highY;
    const low = lowY === undefined ? 20 : lowY;
    let y = bits[0] ? high : low;
    let path = `M0 ${y}`;
    for (let i = 0; i < bits.length; i++) {
      const nextY = bits[i] ? high : low;
      if (i && nextY !== y) path += ` V${nextY}`;
      path += ` H${(i + 1) * width}`;
      y = nextY;
    }
    return { path, width: bits.length * width };
  }

  function drawDividers(document) {
    for (const svg of document.querySelectorAll(".mypi-divider svg[data-text]")) {
      const text = svg.getAttribute("data-text");
      const bits = frame8n1(text);
      const geometry = waveformPath(bits, 10, 4, 20);
      svg.setAttribute("viewBox", `0 0 ${geometry.width} 24`);
      svg.setAttribute("preserveAspectRatio", "none");
      svg.setAttribute("role", "img");
      svg.setAttribute("aria-label", `${text}, framed as 8N1 UART data`);
      svg.dataset.bitCount = String(bits.length);
      const path = document.createElementNS("http://www.w3.org/2000/svg", "path");
      path.setAttribute("d", geometry.path);
      svg.replaceChildren(path);
    }
  }

  // Dashboard pages: one HTML shell, sections selected by location.hash (no extra URI handlers).
  const PAGES = ["overview", "wifi", "access-point", "traffic", "ethernet", "system", "optimize", "debug"];
  const PAGE_ALIASES = { diagnostics: "debug", status: "overview", clients: "access-point", opt: "optimize" };

  function pageFromHash(hash) {
    const key = String(hash || "").replace(/^#/, "").toLowerCase();
    const page = PAGE_ALIASES[key] || key;
    return PAGES.includes(page) ? page : PAGES[0];
  }

  function showPage(document, hash) {
    const sections = document.querySelectorAll("section.page[data-page]");
    if (!sections.length) return null;   // Standalone pages (/update, /c5update, /pins)
    const page = pageFromHash(hash);
    for (const section of sections) section.hidden = section.dataset.page !== page;
    for (const link of document.querySelectorAll(".mypi-nav a")) {
      const active = link.getAttribute("href") === `/#${page}`;
      link.classList.toggle("active", active);
      if (active) link.setAttribute("aria-current", "page");
      else link.removeAttribute("aria-current");
    }
    return page;
  }

  // STA RSSI: same 20-step red (<= -80 dBm) to green (>= -40 dBm) gradient as the LCD icon.
  function rssiStep(rssi) {
    const step = Math.trunc(((rssi + 80) * 19) / 40);
    return Math.min(19, Math.max(0, step));
  }

  function rssiColor(rssi) {
    const step = rssiStep(rssi);
    return `rgb(${Math.trunc((255 * (19 - step)) / 19)},${Math.trunc((255 * step) / 19)},0)`;
  }

  function staSignal(status) {
    const state = String((status && status.wifi_state) || "");
    const up = state === "connected" || state.startsWith("associated");
    const rssi = Number(status && status.rssi);
    if (!up || !Number.isFinite(rssi) || rssi >= 0) return { text: "\u2014", color: "" };
    return { text: `${Math.round(rssi)} dBm`, color: rssiColor(rssi) };
  }

  async function updateWifi(document, fetchFn) {
    const box = document.getElementById("mypi-wifi");
    const value = document.getElementById("mypi-dbm");
    if (!box || !value) return;
    let signal = { text: "\u2014", color: "" };
    try {
      signal = staSignal(await (await fetchFn("/api/status")).json());
    } catch (e) {
      // Keep the placeholder while the device is unreachable.
    }
    value.textContent = signal.text;
    box.style.color = signal.color;
  }

  // Only the main page shell refreshes the header RSSI periodically. Standalone pages
  // (/update, /c5update, /pins) read it once: the single-task HTTP server is busy with
  // the upload during OTA, and extra requests would only queue up behind it.
  function shouldPollStatus(isShell, hidden) {
    return !!isShell && !hidden;
  }

  // Show/hide toggle (v0.8.8): flips one input between type="password" and type="text".
  function togglePassword(input, button) {
    const show = input.type === "password";
    input.type = show ? "text" : "password";
    button.textContent = show ? "\u{1F512} Hide" : "\u{1F441} Show";
    button.setAttribute("aria-pressed", show ? "true" : "false");
    button.title = show ? "Hide password" : "Show password";
    return show;
  }

  // Adds a small Show/Hide button right after every password field on the page.
  function addPasswordToggles(document) {
    const inputs = Array.from(document.querySelectorAll("input[type=password]"));
    for (const input of inputs) {
      if (input.dataset.pwToggle) continue;
      input.dataset.pwToggle = "1";
      const button = document.createElement("button");
      button.type = "button";
      button.className = "pw-toggle";
      button.textContent = "\u{1F441} Show";
      button.title = "Show password";
      button.setAttribute("aria-pressed", "false");
      if (input.id) button.setAttribute("aria-controls", input.id);
      button.addEventListener("click", () => togglePassword(input, button));
      input.insertAdjacentElement("afterend", button);
    }
    return inputs.length;
  }

  // --- Optimize page (v0.9.0, C5 radio fields v0.9.1): GET/POST /api/opt, POST /api/clear_emergency ---
  // Same field names and defaults as main/opt_config_core.h.
  const OPT_FIELDS = ["sdio_freq_mhz", "wifi_tx_power_dbm", "napt", "log_level", "ps_mode", "ap_beacon_ms", "country"];
  const OPT_PS_NAMES = ["Off", "Min modem", "Max modem"];
  const OPT_LOG_NAMES = ["Error", "Warning", "Info", "Debug"];

  function optDefaults() {
    return { sdio_freq_mhz: 40, wifi_tx_power_dbm: 0, napt: true, log_level: 2, ps_mode: 0, ap_beacon_ms: 100, country: "SE" };
  }

  // SDIO clock, AP beacon interval and country are only read at boot (same rule as opt_config_reboot_required()).
  function optRebootRequired(active, saved) {
    return !!active && !!saved &&
      (Number(active.sdio_freq_mhz) !== Number(saved.sdio_freq_mhz) ||
       Number(active.ap_beacon_ms) !== Number(saved.ap_beacon_ms) ||
       String(active.country) !== String(saved.country));
  }

  function optSetRadio(document, name, value) {
    for (const r of document.querySelectorAll(`input[name=${name}]`)) r.checked = String(r.value) === String(value);
  }

  function optGetRadio(document, name) {
    for (const r of document.querySelectorAll(`input[name=${name}]`)) if (r.checked) return Number(r.value);
    return undefined;
  }

  function optFillForm(document, d) {
    optSetRadio(document, "opt-sdio", d.sdio_freq_mhz);
    const tx = document.getElementById("opt-tx");
    if (tx) tx.value = String(d.wifi_tx_power_dbm);
    for (const [id, k] of [["opt-ps", "ps_mode"], ["opt-beacon", "ap_beacon_ms"], ["opt-country", "country"]]) {
      const e = document.getElementById(id);
      if (e && d[k] !== undefined) e.value = String(d[k]);
    }
    const napt = document.getElementById("opt-napt");
    if (napt) napt.checked = !!d.napt;
    const log = document.getElementById("opt-log");
    if (log) log.value = String(d.log_level);
  }

  /** Only the controls that have a value end up in the POST body (missing keys keep the saved value). */
  function optReadForm(document) {
    const body = {};
    const sdio = optGetRadio(document, "opt-sdio");
    if (sdio !== undefined) body.sdio_freq_mhz = sdio;
    const tx = document.getElementById("opt-tx");
    if (tx && tx.value !== "") body.wifi_tx_power_dbm = Number(tx.value);
    const napt = document.getElementById("opt-napt");
    if (napt) body.napt = !!napt.checked;
    const log = document.getElementById("opt-log");
    if (log && log.value !== "") body.log_level = Number(log.value);
    const ps = document.getElementById("opt-ps");
    if (ps && ps.value !== "") body.ps_mode = Number(ps.value);
    const bc = document.getElementById("opt-beacon");
    if (bc && bc.value !== "") body.ap_beacon_ms = Number(bc.value);
    const cc = document.getElementById("opt-country");
    if (cc && cc.value !== "") body.country = String(cc.value);
    return body;
  }

  /** Right-hand status text per row, from a GET /api/opt reply. */
  function optStatus(d) {
    const a = d.active || d;
    const pend = (k) => (a[k] !== d[k] ? " - saved " + d[k] + ", reboot pending" : "");
    return {
      "opt-sdio-st": "active " + (d.sdio_active_mhz || a.sdio_freq_mhz) + " MHz" +
        (d.sdio_fell_back ? " (auto-fallback from 50 MHz)" : "") + pend("sdio_freq_mhz"),
      "opt-tx-st": a.wifi_tx_power_dbm ? "cap " + a.wifi_tx_power_dbm + " dBm" : "radio setting (Max TX power above)",
      "opt-ps-st": (OPT_PS_NAMES[a.ps_mode] || "?") + " - Recommended: Off in AP+STA mode",
      "opt-beacon-st": "active " + a.ap_beacon_ms + " ms" + pend("ap_beacon_ms"),
      "opt-country-st": "active " + a.country + pend("country"),
      "opt-napt-st": a.napt ? "on (per AP profile NAT flag)" : "off for all AP profiles",
      "opt-log-st": OPT_LOG_NAMES[a.log_level] || "?",
      "opt-emerg-st": (d.emergency_active ? "SET - emergency mode was entered after a C5 failure" : "clear") +
        (d.emergency_mode_now ? " (emergency mode active now)" : "") + "; auto-clears on successful boot",
    };
  }

  function optShow(document, d) {
    for (const [id, text] of Object.entries(optStatus(d))) {
      const el = document.getElementById(id);
      if (el) el.textContent = text;
    }
    const st = document.getElementById("opt-emerg-st");
    if (st && st.classList) st.classList.toggle("bad", !!d.emergency_active);
    const btn = document.getElementById("opt-emerg-btn");
    if (btn) btn.disabled = !d.emergency_active;
    optBanner(document, !!d.reboot_required);
  }

  function optBanner(document, on) {
    const b = document.getElementById("opt-banner");
    if (b) b.hidden = !on;
  }

  function optMsg(document, text) {
    const m = document.getElementById("opt-msg");
    if (m) m.textContent = text;
  }

  async function optLoad(document, fetchFn) {
    const doc = document || root.document, f = fetchFn || root.fetch.bind(root);
    try {
      const d = await (await f("/api/opt")).json();
      optFillForm(doc, d);
      optShow(doc, d);
      return d;
    } catch (e) {
      optMsg(doc, "Device unreachable");
      return null;
    }
  }

  async function optPost(f, url, body) {
    const r = await f(url, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body || {}) });
    return r.json();
  }

  async function optSave(document, fetchFn) {
    const doc = document || root.document, f = fetchFn || root.fetch.bind(root);
    try {
      const j = await optPost(f, "/api/opt", optReadForm(doc));
      optMsg(doc, j.ok ? "Saved" + (j.reboot_required ? " - reboot required" : " and applied") : "ERROR " + (j.error || ""));
      if (j.ok) {
        optBanner(doc, !!j.reboot_required);
        await optLoad(doc, f);
      }
      return j;
    } catch (e) {
      optMsg(doc, "Save failed: " + e);
      return null;
    }
  }

  async function optClearEmergency(document, fetchFn) {
    const doc = document || root.document, f = fetchFn || root.fetch.bind(root);
    try {
      const j = await optPost(f, "/api/clear_emergency");
      optMsg(doc, j.ok ? "Emergency flag cleared" : "ERROR " + (j.error || ""));
      if (j.ok) await optLoad(doc, f);
      return j;
    } catch (e) {
      optMsg(doc, "Clear failed: " + e);
      return null;
    }
  }

  function start(document) {
    drawDividers(document);
    addPasswordToggles(document);
    const isShell = !!showPage(document, root.location && root.location.hash);
    if (isShell) {
      root.addEventListener("hashchange", () => {
        if (showPage(document, root.location.hash) === "optimize" && typeof root.fetch === "function") optLoad(document);
      });
    }
    if (typeof root.fetch === "function") {
      updateWifi(document, root.fetch.bind(root));
      if (document.getElementById && document.getElementById("opt-form")) optLoad(document);
      if (isShell) {
        root.setInterval(() => {
          if (shouldPollStatus(isShell, document.hidden)) updateWifi(document, root.fetch.bind(root));
        }, 5000);
      }
    }
  }

  const api = { frame8n1, waveformPath, drawDividers, pageFromHash, showPage, rssiStep, rssiColor, staSignal, shouldPollStatus, togglePassword, addPasswordToggles,
    OPT_FIELDS, optDefaults, optRebootRequired, optFillForm, optReadForm, optStatus, optLoad, optSave, optClearEmergency };
  root.MyPiSerial = api;
  if (typeof module !== "undefined" && module.exports) module.exports = api;
  if (root.document) {
    if (root.document.readyState === "loading") {
      root.document.addEventListener("DOMContentLoaded", () => start(root.document), { once: true });
    } else {
      start(root.document);
    }
  }
})(globalThis);
