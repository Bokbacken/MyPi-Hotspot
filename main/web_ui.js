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

  // --- Optimize page (v0.9.0, trimmed v0.9.3): GET/POST /api/opt + /api/radio, POST /api/clear_emergency ---
  // Same field names and defaults as main/opt_config_core.h. Band and the shared STA/AP protocol mask
  // live in the radio settings (/api/radio); everything else that used to be here now runs on defaults.
  const OPT_FIELDS = ["wifi_tx_power_dbm", "ps_mode", "ap_beacon_ms", "country"];
  const OPT_PS_NAMES = ["Off", "Min modem", "Max modem"];
  const OPT_BAND_NAMES = ["Both (2.4 + 5 GHz)", "2.4 GHz only", "5 GHz only"];
  // Same bits as RADIO_PROTO_* in main/wifi.h (ax allowed again since v0.9.3, known C5 fw issue).
  const PROTO_BITS = { a: 16, b: 1, g: 2, n: 4, ac: 32, ax: 64 };
  const PROTO_ALL = 0x77;
  const PROTO_2G = 0x47;   // b/g/n/ax
  const PROTO_5G = 0x74;   // a/n/ac/ax

  function optDefaults() {
    return { wifi_tx_power_dbm: 0, ps_mode: 0, ap_beacon_ms: 100, country: "SE" };
  }

  // AP beacon interval and country are only read at boot (same rule as opt_config_reboot_required()).
  function optRebootRequired(active, saved) {
    return !!active && !!saved &&
      (Number(active.ap_beacon_ms) !== Number(saved.ap_beacon_ms) ||
       String(active.country) !== String(saved.country));
  }

  /** Protocol bitmask from the a/b/g/n/ac/ax checkboxes (0 = none checked = default). */
  function protoMaskFromChecks(document) {
    let m = 0;
    for (const [k, bit] of Object.entries(PROTO_BITS)) {
      const e = document.getElementById("opt-proto-" + k);
      if (e && e.checked) m |= bit;
    }
    return m & PROTO_ALL;
  }

  function protoSetChecks(document, mask) {
    for (const [k, bit] of Object.entries(PROTO_BITS)) {
      const e = document.getElementById("opt-proto-" + k);
      if (e) e.checked = (Number(mask) & bit) !== 0;
    }
  }

  /** The part of a mask that the selected band can use (0 = both bands, 1 = 2.4 GHz, 2 = 5 GHz). */
  function protoFilterForBand(mask, band) {
    const m = Number(mask) & PROTO_ALL;
    return Number(band) === 1 ? m & PROTO_2G : Number(band) === 2 ? m & PROTO_5G : m;
  }

  function protoLabel(mask) {
    const m = Number(mask) & PROTO_ALL;
    if (!m) return "default (b/g/n + a/n/ac)";
    return Object.entries(PROTO_BITS).filter(([, bit]) => m & bit).map(([k]) => k).join("/");
  }

  /** Status text for the protocol row; warns about ax and about a band left without protocols. */
  function protoHint(mask, band) {
    const m = Number(mask) & PROTO_ALL;
    const parts = [protoLabel(m)];
    if (m && Number(band) !== 2 && !(m & PROTO_2G)) parts.push("no 2.4 GHz protocol checked - 2.4 GHz uses default b/g/n");
    if (m && Number(band) !== 1 && !(m & PROTO_5G)) parts.push("no 5 GHz protocol checked - 5 GHz uses default a/n/ac");
    if (m & PROTO_BITS.ax) parts.push("ax: known C5 issue, upload may stall");
    return parts.join("; ");
  }

  const OPT_SELECTS = [["opt-tx", "wifi_tx_power_dbm"], ["opt-ps", "ps_mode"], ["opt-beacon", "ap_beacon_ms"],
    ["opt-country", "country"]];

  function optFillForm(document, d) {
    for (const [id, k] of OPT_SELECTS) {
      const e = document.getElementById(id);
      if (e && d[k] !== undefined) e.value = String(d[k]);
    }
  }

  /** Only the controls that have a value end up in the POST body (missing keys keep the saved value). */
  function optReadForm(document) {
    const body = {};
    for (const [id, k] of OPT_SELECTS) {
      const e = document.getElementById(id);
      if (e && e.value !== "") body[k] = k === "country" ? String(e.value) : Number(e.value);
    }
    return body;
  }

  /** Radio part of the form (band + shared protocol mask) from a GET /api/radio reply. */
  function radioFillForm(document, r) {
    const b = document.getElementById("opt-band");
    if (b && r && r.band_mode !== undefined) b.value = String(r.band_mode);
    if (r && r.proto !== undefined) protoSetChecks(document, r.proto);
  }

  /** POST /api/radio body: the band and one protocol mask that STA and AP share. */
  function radioReadForm(document) {
    const body = { proto: protoMaskFromChecks(document) };
    const b = document.getElementById("opt-band");
    if (b && b.value !== "") body.band_mode = Number(b.value);
    return body;
  }

  /** Right-hand status text per row, from a GET /api/opt reply (and optionally GET /api/radio). */
  function optStatus(d, r) {
    const a = d.active || d;
    const pend = (k) => (a[k] !== d[k] ? " - saved " + d[k] + ", reboot pending" : "");
    const st = {
      "opt-tx-st": a.wifi_tx_power_dbm ? "cap " + a.wifi_tx_power_dbm + " dBm" : "driver default (max)",
      "opt-ps-st": (OPT_PS_NAMES[a.ps_mode] || "?") + " - Recommended: Off in AP+STA mode",
      "opt-beacon-st": "active " + a.ap_beacon_ms + " ms" + pend("ap_beacon_ms"),
      "opt-country-st": "active " + a.country + pend("country"),
      "opt-emerg-st": (d.emergency_active ? "SET - emergency mode was entered after a C5 failure" : "clear") +
        (d.emergency_mode_now ? " (emergency mode active now)" : "") + "; auto-clears on successful boot",
    };
    if (r) {
      const ra = r.active || r, rs = r.saved || r;
      st["opt-band-st"] = "active " + (OPT_BAND_NAMES[ra.band_mode] || "?") +
        (ra.band_mode !== rs.band_mode ? " - saved " + (OPT_BAND_NAMES[rs.band_mode] || "?") + ", reboot pending" : "");
      st["opt-proto-st"] = "active " + protoHint(ra.proto, ra.band_mode) +
        (ra.proto !== rs.proto ? " - saved " + protoLabel(rs.proto) + ", reboot pending" : "");
    }
    return st;
  }

  function radioRebootRequired(r) {
    if (!r) return false;
    const ra = r.active || r, rs = r.saved || r;
    return ra.band_mode !== rs.band_mode || ra.proto !== rs.proto;
  }

  function optShow(document, d, r) {
    for (const [id, text] of Object.entries(optStatus(d, r))) {
      const el = document.getElementById(id);
      if (el) el.textContent = text;
    }
    const st = document.getElementById("opt-emerg-st");
    if (st && st.classList) st.classList.toggle("bad", !!d.emergency_active);
    const btn = document.getElementById("opt-emerg-btn");
    if (btn) btn.disabled = !d.emergency_active;
    optBanner(document, !!d.reboot_required || radioRebootRequired(r));
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
      let r = null;
      try { r = await (await f("/api/radio")).json(); } catch (e) { r = null; }
      optFillForm(doc, d);
      if (r) radioFillForm(doc, r);
      optShow(doc, d, r);
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
      const jr = doc.getElementById("opt-band") ? await optPost(f, "/api/radio", radioReadForm(doc)) : { ok: true };
      if (!jr.ok) {
        optMsg(doc, "ERROR (radio) " + (jr.error || ""));
        return jr;
      }
      const j = await optPost(f, "/api/opt", optReadForm(doc));
      optMsg(doc, j.ok ? "Saved" + (j.reboot_required ? " - reboot required" : " - band/protocol changes apply after reboot")
        : "ERROR " + (j.error || ""));
      if (j.ok) await optLoad(doc, f);
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
    OPT_FIELDS, optDefaults, optRebootRequired, optFillForm, optReadForm, optStatus, optLoad, optSave, optClearEmergency,
    PROTO_BITS, protoMaskFromChecks, protoSetChecks, protoFilterForBand, protoLabel, protoHint, radioFillForm, radioReadForm,
    radioRebootRequired };
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
