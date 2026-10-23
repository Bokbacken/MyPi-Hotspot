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
  const PAGES = ["overview", "wifi", "access-point", "traffic", "ethernet", "system", "camera", "optimize", "debug"];
  const PAGE_ALIASES = { diagnostics: "debug", status: "overview", clients: "access-point", opt: "optimize", photo: "camera" };

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
  // live in the radio settings (/api/radio). v0.9.4: TCP window, LwIP core and NAPT settings are back.
  // v0.9.5: 5 GHz width (bw_5g 40|20, 2.4 GHz always HT20) and SDIO clock (sdio_mhz 40|50, auto-fallback).
  const OPT_FIELDS = ["wifi_tx_power_dbm", "ps_mode", "ap_beacon_ms", "country",
    "tcp_wnd_kb", "lwip_core", "napt_max", "napt_tcp_to", "bw_5g", "sdio_mhz"];
  const OPT_PS_NAMES = ["Off", "Min modem", "Max modem"];
  const OPT_CORE_NAMES = ["Auto (no pin)", "Core 0", "Core 1"];
  const OPT_NAPT_MAX_ENTRIES = { 5: 512, 10: 1024, 20: 2048 };
  const OPT_NAPT_TCP_TO_NAMES = ["30 min", "1 h", "2 h"];
  const OPT_BAND_NAMES = ["Both (2.4 + 5 GHz)", "2.4 GHz only", "5 GHz only"];
  // Same bits as RADIO_PROTO_* in main/wifi.h (ax allowed again since v0.9.3, known C5 fw issue).
  const PROTO_BITS = { a: 16, b: 1, g: 2, n: 4, ac: 32, ax: 64 };
  const PROTO_ALL = 0x77;
  const PROTO_2G = 0x47;   // b/g/n/ax
  const PROTO_5G = 0x74;   // a/n/ac/ax

  function optDefaults() {
    return { wifi_tx_power_dbm: 0, ps_mode: 0, ap_beacon_ms: 100, country: "SE",
      tcp_wnd_kb: 32, lwip_core: 0, napt_max: 5, napt_tcp_to: 2, bw_5g: 40, sdio_mhz: 40 };
  }

  // AP beacon interval, country, TCP window, LwIP core, NAPT max sessions, 5 GHz width and SDIO clock are only read at boot
  // (same rule as opt_config_reboot_required()); the NAPT TCP timeout applies immediately.
  function optRebootRequired(active, saved) {
    return !!active && !!saved &&
      (Number(active.ap_beacon_ms) !== Number(saved.ap_beacon_ms) ||
       String(active.country) !== String(saved.country) ||
       Number(active.tcp_wnd_kb) !== Number(saved.tcp_wnd_kb) ||
       Number(active.lwip_core) !== Number(saved.lwip_core) ||
       Number(active.napt_max) !== Number(saved.napt_max) ||
       Number(active.bw_5g) !== Number(saved.bw_5g) ||
       Number(active.sdio_mhz) !== Number(saved.sdio_mhz));
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
    ["opt-country", "country"], ["opt-tcp-wnd", "tcp_wnd_kb"], ["opt-lwip-core", "lwip_core"],
    ["opt-napt-max", "napt_max"], ["opt-napt-tcp-to", "napt_tcp_to"], ["opt-bw-5g", "bw_5g"], ["opt-sdio", "sdio_mhz"]];

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
      "opt-tcp-wnd-st": "active " + a.tcp_wnd_kb + " KB" + pend("tcp_wnd_kb"),
      "opt-lwip-core-st": "active " + (OPT_CORE_NAMES[a.lwip_core] || "?") + pend("lwip_core"),
      "opt-napt-max-st": "active " + (OPT_NAPT_MAX_ENTRIES[a.napt_max] || "?") + " sessions" + pend("napt_max"),
      "opt-napt-tcp-to-st": "active " + (OPT_NAPT_TCP_TO_NAMES[a.napt_tcp_to] || "?"),
      "opt-bw-5g-st": "active 5 GHz HT" + a.bw_5g + (Number(a.bw_5g) === 40 ? " (11n, ac/ax off on 5 GHz)" : "") +
        " (2.4 GHz always HT20)" +
        (Number(a.bw_5g) !== Number(d.bw_5g) ? " - saved HT" + d.bw_5g + ", reboot pending" : ""),
      "opt-sdio-st": sdioStatus(d),
      "opt-emerg-st": (d.emergency_active ? "SET - emergency mode was entered after a C5 failure" : "clear") +
        (d.emergency_mode_now ? " (emergency mode active now)" : "") + "; auto-clears on successful boot",
    };
    if (r) {
      const ra = r.active || r, rs = r.saved || r;
      st["opt-band-st"] = "active " + (OPT_BAND_NAMES[ra.band_mode] || "?") +
        (ra.band_mode !== rs.band_mode ? " - saved " + (OPT_BAND_NAMES[rs.band_mode] || "?") + ", reboot pending" : "");
      st["opt-proto-st"] = "active " + protoHint(ra.proto, ra.band_mode) +
        (ra.proto !== rs.proto ? " - saved " + protoLabel(rs.proto) + ", reboot pending" : "");
      if (r.info && r.info.sta_width && Number(r.sta_channel) > 14) st["opt-bw-5g-st"] += "; STA link now " + r.info.sta_width;
    }
    return st;
  }

  /** SDIO row: clock in use this boot (real bus clock if known), fallback notice, pending change. */
  function sdioStatus(d) {
    const mhz = d.sdio_active_mhz !== undefined ? Number(d.sdio_active_mhz) : Number((d.active || d).sdio_mhz);
    let t = "active " + mhz + " MHz";
    if (d.sdio_real_khz) t += " (bus " + (Number(d.sdio_real_khz) / 1000) + " MHz)";
    if (d.sdio_fell_back) t += " - fell back from 50 MHz after a C5 failure";
    else if (d.sdio_mhz !== undefined && Number(d.sdio_mhz) !== mhz) t += " - saved " + d.sdio_mhz + " MHz, reboot pending";
    return t;
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

  // Camera page (v0.10.0): status + saved photos from /api/camera/status, preview = one JPEG at a time
  // from /api/camera/frame.jpg while the page is visible (the firmware stops the stream when idle),
  // shutter = POST /api/camera/capture. Errors name the failing stage (camera, capture, storage).
  const CAMERA_PREVIEW_MS = 300, CAMERA_RETRY_MS = 3000;
  const CAMERA_STAGES = { camera: "Camera", capture: "Capture", storage: "Storage" };
  const CAMERA_PHOTO_RE = /^IMG\d{5}\.JPG$/;
  const CAMERA_NUMBERS = ["preview_quality", "photo_quality", "idle_stop_s", "max_photos", "store_kb"];
  const CAMERA_BOOLEANS = ["hflip", "vflip", "live_preview"];
  const cameraForms = new WeakMap();

  function cameraSettingsFill(doc, settings, caps) {
    for (const key of CAMERA_NUMBERS) {
      const el = doc.getElementById("cam-" + key);
      if (el) el.value = String(settings[key]);
    }
    for (const key of CAMERA_BOOLEANS) {
      const el = doc.getElementById("cam-" + key);
      if (el) {
        el.checked = !!settings[key];
        if (key !== "live_preview") el.disabled = !caps[key];
      }
    }
    const select = doc.getElementById("cam-resolution");
    if (select) {
      select.replaceChildren();
      for (const mode of caps.modes || []) {
        const option = doc.createElement("option");
        option.value = `${mode.width}x${mode.height}`;
        option.textContent = option.value;
        select.appendChild(option);
      }
      select.value = `${settings.width}x${settings.height}`;
      select.disabled = (caps.modes || []).length < 2;
    }
    camSet(doc, "cam-mode-note", (caps.modes || []).length < 2 ?
      "Only one driver-supported sensor mode is enabled." : "Driver-supported modes only.");
  }

  async function cameraSettingsLoad(document, fetchFn) {
    const doc = document || root.document, f = fetchFn || root.fetch.bind(root);
    const form = doc.getElementById("cam-settings");
    try {
      const response = await f("/api/camera/settings", { cache: "no-store" });
      const data = await response.json();
      if (!response.ok || !data.settings || !data.capabilities) throw new Error();
      cameraForms.set(doc, data);
      cameraSettingsFill(doc, data.settings, data.capabilities);
      if (form) form.disabled = false;
      return data;
    } catch (e) {
      if (form) form.disabled = true;
      camSet(doc, "cam-settings-msg", "Camera settings unavailable", true);
      return null;
    }
  }

  async function cameraSettingsSave(document, fetchFn) {
    const doc = document || root.document, f = fetchFn || root.fetch.bind(root);
    if (!cameraForms.has(doc)) return null;
    const settings = {};
    for (const key of CAMERA_NUMBERS) {
      const input = doc.getElementById("cam-" + key);
      if (!input.value.trim() || (input.checkValidity && !input.checkValidity())) {
        camSet(doc, "cam-settings-msg", "Check the setting ranges", true);
        return null;
      }
      settings[key] = Number(input.value);
      if (!Number.isInteger(settings[key])) return null;
    }
    for (const key of CAMERA_BOOLEANS) settings[key] = doc.getElementById("cam-" + key).checked;
    const [width, height] = doc.getElementById("cam-resolution").value.split("x").map(Number);
    settings.width = width;
    settings.height = height;
    try {
      const response = await f("/api/camera/settings", {
        method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(settings),
      });
      const result = await response.json();
      if (!response.ok || !result.ok) throw new Error();
      await cameraSettingsLoad(doc, f);
      await cameraLoad(doc, f);
      camSet(doc, "cam-settings-msg", "Saved and applied", false);
      if (cameraVisible(doc) && cameraLiveEnabled(doc)) cameraPreview(doc, f);
      return result;
    } catch (e) {
      camSet(doc, "cam-settings-msg", "Could not save camera settings; check supported values and retry", true);
      return null;
    }
  }

  function cameraSettingsReset(document) {
    const doc = document || root.document, data = cameraForms.get(doc);
    if (!data) return;
    cameraSettingsFill(doc, data.defaults, data.capabilities);
    camSet(doc, "cam-settings-msg", "Defaults loaded; Save and apply to keep them");
    cameraLiveChanged(doc);
  }

  function cameraLiveEnabled(doc) {
    const input = doc.getElementById("cam-live_preview");
    return !input || input.checked;
  }

  function cameraLiveChanged(document) {
    const doc = document || root.document;
    if (cameraLiveEnabled(doc) && cameraVisible(doc)) cameraPreview(doc);
  }

  // Wi-Fi QR import (v0.11.0). The MyPi decodes the QR code itself (POST /api/wifiqr/scan, quirc on
  // the device - no browser decoder, works over plain HTTP) and keeps the password server-side; this
  // page shows SSID, security, visibility and the password length only, and polls the status so a
  // brief loss of the management connection while the uplink switches is tolerated. Credentials are
  // never put in URLs, logs or browser storage.
  const WIFIQR_POLL_MS = 700, WIFIQR_RETRY_MS = 2000;
  const WIFIQR_SECURITY = {
    open: "Open (no password)", wpa: "WPA/WPA2/WPA3 personal", wpa2: "WPA2/WPA3 personal (WPA2 minimum)",
    wpa3: "WPA3 personal (SAE) only",
  };
  const wifiQrLoops = new WeakSet();
  const wifiQrSlotKeys = new WeakMap();

  function htmlEsc(value) {
    return String(value).replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[c]);
  }

  function wifiQrVisibilityText(st) {
    const v = st && st.visibility, c = st && st.candidate;
    if (!v) return "";
    if (v.state === "visible") {
      let text = `Visible (${v.rssi} dBm, channel ${v.channel}, ${v.ap_security})`;
      if (v.matches > 1) text += `, ${v.matches} access points`;
      if (!v.security_ok) text += " - its security does not match the QR code";
      return text;
    }
    if (v.state === "not_seen") {
      return c && c.hidden ? "Not seen: hidden network (does not broadcast its name); connecting can still work" :
        "Not seen in the scan (out of range, other band, or hidden)";
    }
    if (v.state === "scan_failed") return "Network scan failed: use Scan networks again";
    return "Not scanned yet";
  }

  function wifiQrPriorText(p) {
    if (!p) return "";
    if (p.manual_off) return "None (Wi-Fi uplink switched off)";
    if (!p.connected) return "None (not connected)";
    const source = p.source === "default" ? "built-in default" : `profile ${p.slot}`;
    return `"${p.ssid}" (${source}, ${p.dhcp ? "DHCP" : "static IP"}${p.ip ? ", " + p.ip : ""})`;
  }

  function wifiQrInfoRows(st) {
    const c = st && st.candidate;
    if (!c) return "";
    const row = (k, v) => `<tr><td>${htmlEsc(k)}</td><td>${htmlEsc(v)}</td></tr>`;
    const pass = c.security === "open" ? "none" :
      `hidden (${c.password_len} ${c.psk ? "hex digits, raw PSK" : "characters"})`;
    let rows = row("Network (SSID)", c.ssid) + row("Security", WIFIQR_SECURITY[c.security] || c.security) +
      row("Password", pass) + row("Hidden network", c.hidden ? "yes" : "no");
    if (c.ignored) rows += row("Ignored QR fields", c.ignored);
    if (c.existing_slot >= 0) rows += row("Saved profile", `already in slot ${c.existing_slot}: it is updated`);
    return rows + row("Current connection", wifiQrPriorText(st.prior)) + row("Visibility", wifiQrVisibilityText(st));
  }

  function wifiQrSlotOptions(st) {
    const c = st && st.candidate;
    const options = [{ value: "-1", text: c && c.existing_slot >= 0 ?
      `Automatic (update slot ${c.existing_slot})` : "Automatic (first free slot)" }];
    for (const s of (st && st.slots) || []) {
      options.push({ value: String(s.slot),
        text: `Slot ${s.slot}: ${s.valid ? s.ssid + (s.held ? " (saved only)" : "") : "(empty)"}` });
    }
    return options;
  }

  function wifiQrRender(doc, st) {
    if (!st) return;
    const review = st.state === "review" && !!st.candidate && !st.busy;
    camSet(doc, "qr-msg", st.message || "", st.state === "error" ||
      (st.result && st.state === "done" && !["connected", "saved"].includes(st.result.outcome)));
    camSet(doc, "qr-progress", st.busy ? (st.progress || "Working...") : "");
    const panel = doc.getElementById("qr-review");
    if (panel) panel.hidden = !st.candidate;
    const info = doc.getElementById("qr-info");
    if (info) info.innerHTML = wifiQrInfoRows(st);
    for (const id of ["qr-save", "qr-connect", "qr-rescan"]) {
      const el = doc.getElementById(id);
      if (el) el.disabled = !review;
    }
    const read = doc.getElementById("qr-read");
    if (read) read.disabled = !!st.busy;
    const cancel = doc.getElementById("qr-cancel");
    if (cancel) cancel.hidden = !st.cancellable;
    const select = doc.getElementById("qr-slot");
    const key = st.candidate ? `${st.seq}:${JSON.stringify(st.slots || [])}` : "";
    if (select && key && wifiQrSlotKeys.get(doc) !== key && doc.createElement) {
      wifiQrSlotKeys.set(doc, key);
      const keep = select.value;
      select.replaceChildren();
      for (const o of wifiQrSlotOptions(st)) {
        const option = doc.createElement("option");
        option.value = o.value;
        option.textContent = o.text;
        select.appendChild(option);
      }
      select.value = keep && wifiQrSlotOptions(st).some((o) => o.value === keep) ? keep : "-1";
    }
  }

  async function wifiQrPoll(document, fetchFn) {
    const doc = document || root.document, f = fetchFn || root.fetch.bind(root);
    let st = null;
    try {
      const r = await f("/api/wifiqr/status", { cache: "no-store" });
      st = await r.json();
    } catch (e) {
      st = null;
    }
    if (!st) {
      camSet(doc, "qr-progress", "Waiting for the MyPi (the network connection may be switching)...", true);
      return null;
    }
    wifiQrRender(doc, st);
    return st;
  }

  // Poll until the current step finishes (status retained on the device across connection drops).
  async function wifiQrFollow(document, fetchFn, sleep) {
    const doc = document || root.document, f = fetchFn || root.fetch.bind(root);
    const wait = sleep || ((ms) => new Promise((resolve) => root.setTimeout(resolve, ms)));
    if (wifiQrLoops.has(doc)) return null;
    wifiQrLoops.add(doc);
    try {
      for (;;) {
        const st = await wifiQrPoll(doc, f);
        if ((st && !st.busy) || doc.hidden) return st;
        await wait(st ? WIFIQR_POLL_MS : WIFIQR_RETRY_MS);
      }
    } finally {
      wifiQrLoops.delete(doc);
    }
  }

  async function wifiQrPost(doc, f, url, body) {
    try {
      const r = await f(url, { method: "POST", headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body || {}) });
      const j = await r.json();
      if (!j.ok) camSet(doc, "qr-msg", j.error || "Request failed", true);
      return j;
    } catch (e) {
      camSet(doc, "qr-msg", "Device unreachable", true);
      return null;
    }
  }

  async function wifiQrAction(doc, f, url, body, sleep) {
    const j = await wifiQrPost(doc, f, url, body);
    if (j && j.ok) return wifiQrFollow(doc, f, sleep);
    return null;
  }

  function wifiQrScan(document, fetchFn, sleep) {
    const doc = document || root.document, f = fetchFn || root.fetch.bind(root);
    camSet(doc, "qr-msg", "Looking for a QR code in the MyPi camera image...", false);
    return wifiQrAction(doc, f, "/api/wifiqr/scan", {}, sleep);
  }

  function wifiQrSubmitText(document, fetchFn, sleep) {
    const doc = document || root.document, f = fetchFn || root.fetch.bind(root);
    const input = doc.getElementById("qr-payload");
    const payload = input ? input.value : "";
    if (input) input.value = "";
    if (!payload.trim()) {
      camSet(doc, "qr-msg", "Paste the WIFI: text first", true);
      return Promise.resolve(null);
    }
    return wifiQrAction(doc, f, "/api/wifiqr/payload", { payload }, sleep);
  }

  function wifiQrRescan(document, fetchFn, sleep) {
    const doc = document || root.document, f = fetchFn || root.fetch.bind(root);
    return wifiQrAction(doc, f, "/api/wifiqr/rescan", {}, sleep);
  }

  function wifiQrCommit(connect, document, fetchFn, sleep) {
    const doc = document || root.document, f = fetchFn || root.fetch.bind(root);
    const select = doc.getElementById("qr-slot"), overwrite = doc.getElementById("qr-overwrite");
    const slot = select && select.value !== "" ? Number(select.value) : -1;
    return wifiQrAction(doc, f, "/api/wifiqr/commit",
      { connect: !!connect, slot: Number.isInteger(slot) ? slot : -1, overwrite: !!(overwrite && overwrite.checked) }, sleep);
  }

  function wifiQrCancel(document, fetchFn, sleep) {
    const doc = document || root.document, f = fetchFn || root.fetch.bind(root);
    return wifiQrAction(doc, f, "/api/wifiqr/cancel", {}, sleep);
  }

  async function wifiQrDiscard(document, fetchFn) {
    const doc = document || root.document, f = fetchFn || root.fetch.bind(root);
    const j = await wifiQrPost(doc, f, "/api/wifiqr/discard", {});
    if (j && j.ok) await wifiQrPoll(doc, f);
    return j;
  }

  // One fresh high-quality frame (independent of the preview settings) to aim the MyPi camera.
  async function wifiQrAim(document, fetchFn) {
    const doc = document || root.document, f = fetchFn || root.fetch.bind(root);
    try {
      const r = await f(`/api/camera/frame.jpg?qr=1&t=${Date.now()}`, { cache: "no-store" });
      if (!r.ok) throw new Error((r.text && (await r.text())) || `HTTP ${r.status}`);
      const blob = await r.blob(), img = doc.getElementById("qr-img");
      if (!img || !root.URL || !root.URL.createObjectURL) return true;
      const old = img.dataset && img.dataset.url;
      const url = root.URL.createObjectURL(blob);
      img.src = url;
      if (img.dataset) img.dataset.url = url;
      if (old) root.URL.revokeObjectURL(old);
      return true;
    } catch (e) {
      camSet(doc, "qr-msg", `Camera image unavailable: ${e && e.message ? e.message : e}`, true);
      return false;
    }
  }

  // Leaving the page clears local inputs/images only; a decoded candidate stays on the device (it
  // expires after 10 minutes) so navigation does not interrupt a running step.
  function wifiQrClear(document) {
    const doc = document || root.document;
    const input = doc.getElementById("qr-payload");
    if (input) input.value = "";
    const img = doc.getElementById("qr-img");
    if (img && img.dataset && img.dataset.url) {
      root.URL.revokeObjectURL(img.dataset.url);
      delete img.dataset.url;
      img.removeAttribute("src");
    }
  }

  function cameraUsable(s) {
    return !!(s && s.enabled && ["ready", "streaming", "error"].includes(s.state));
  }

  function cameraStatusText(s) {
    if (!s) return "Device unreachable";
    if (!s.enabled) return "Camera support is disabled in this firmware (menuconfig: MyPi-Hotspot > Camera)";
    if (s.state === "starting") return "Camera starting...";
    if (s.state === "not_detected") return "Camera not detected" + (s.error ? `: ${s.error}` : "");
    const base = `${s.sensor || "Camera"} ${s.width}x${s.height}`;
    if (s.state === "error") return `${base} - error: ${s.error || "unknown"} (retrying)`;
    return `${base} - ${s.state === "streaming" ? "streaming" : "ready"}`;
  }

  // v0.11.0: photos live in PSRAM only (volatile: cleared on reboot), never on flash.
  function cameraStorageText(s) {
    const st = s && s.storage;
    if (!st || !st.available) return "PSRAM unavailable: photos cannot be kept";
    const kb = (bytes) => Math.ceil((bytes || 0) / 1024);
    return `PSRAM (cleared on reboot) - ${st.count} of max ${st.max_photos} photos, ${kb(st.used_bytes)} KB used of ` +
      `${kb(st.budget_bytes)} KB budget, ${st.psram_free_kb} KB PSRAM free`;
  }

  function cameraResultText(j) {
    if (!j) return "Capture failed: device unreachable";
    if (j.ok) return `Saved ${j.name} (${Math.ceil((j.size || 0) / 1024)} KB)`;
    return `${CAMERA_STAGES[j.stage] || "Camera"} error: ${j.error || "unknown"}`;
  }

  function cameraPhotoRows(photos) {
    const rows = (photos || []).filter((p) => p && CAMERA_PHOTO_RE.test(p.name)).map((p) => {
      const url = `/api/camera/photo?name=${p.name}`;
      return `<tr><td>${p.name}</td><td>${Math.ceil((p.size || 0) / 1024)} KB</td>` +
        `<td><a href='${url}' target='_blank'>view</a> <a href='${url}&dl=1'>download</a> ` +
        `<button type='button' onclick="MyPiSerial.cameraDelete('${p.name}')">delete</button></td></tr>`;
    });
    return rows.length ? "<tr><th>Photo</th><th>Size</th><th></th></tr>" + rows.join("") : "<tr><td>No photos saved yet</td></tr>";
  }

  function camSet(doc, id, text, bad) {
    const el = doc.getElementById(id);
    if (!el) return null;
    el.textContent = text;
    if (bad !== undefined && el.classList) {
      el.classList.toggle("bad", !!bad);
      el.classList.toggle("ok", !bad);
    }
    return el;
  }

  async function cameraLoad(document, fetchFn) {
    const doc = document || root.document, f = fetchFn || root.fetch.bind(root);
    let s = null;
    try {
      s = await (await f("/api/camera/status")).json();
    } catch (e) {
      s = null;
    }
    const usable = cameraUsable(s);
    camSet(doc, "cam-st", cameraStatusText(s), !usable || s.state === "error");
    const store = camSet(doc, "cam-store", cameraStorageText(s), !(s && s.storage && s.storage.available));
    if (store && !s) store.textContent = "";
    const btn = doc.getElementById("cam-shutter");
    if (btn) btn.disabled = !usable;
    const table = doc.getElementById("cam-photos");
    if (table) table.innerHTML = s ? cameraPhotoRows(s.photos) : "";
    return s;
  }

  async function cameraCapture(document, fetchFn) {
    const doc = document || root.document, f = fetchFn || root.fetch.bind(root);
    const btn = doc.getElementById("cam-shutter");
    if (btn) btn.disabled = true;
    camSet(doc, "cam-msg", "Capturing...");
    let j = null;
    try {
      j = await (await f("/api/camera/capture", { method: "POST" })).json();
    } catch (e) {
      j = null;
    }
    camSet(doc, "cam-msg", cameraResultText(j), !(j && j.ok));
    await cameraLoad(doc, f);
    return j;
  }

  async function cameraDelete(name, document, fetchFn) {
    const doc = document || root.document, f = fetchFn || root.fetch.bind(root);
    if (!CAMERA_PHOTO_RE.test(String(name))) return null;
    if (typeof root.confirm === "function" && !root.confirm(`Delete ${name}?`)) return null;
    let j = null;
    try {
      j = await (await f(`/api/camera/delete?name=${name}`, { method: "POST" })).json();
    } catch (e) {
      j = null;
    }
    camSet(doc, "cam-msg", j && j.ok ? `Deleted ${name}` : `Delete failed: ${(j && j.error) || "device unreachable"}`, !(j && j.ok));
    await cameraLoad(doc, f);
    return j;
  }

  function cameraVisible(doc) {
    const section = doc.querySelector && doc.querySelector("section.page[data-page='camera']");
    return !!section && !section.hidden && !doc.hidden;
  }

  // One preview frame into #cam-img. Throws with the firmware's reason text on failure.
  async function cameraFrame(doc, f) {
    const r = await f(`/api/camera/frame.jpg?t=${Date.now()}`, { cache: "no-store" });
    if (!r.ok) throw new Error((await r.text()) || `HTTP ${r.status}`);
    const blob = await r.blob();
    const img = doc.getElementById("cam-img");
    if (!img || !root.URL || !root.URL.createObjectURL) return;
    const old = img.dataset && img.dataset.url;
    const url = root.URL.createObjectURL(blob);
    img.src = url;
    if (img.dataset) img.dataset.url = url;
    if (old) root.URL.revokeObjectURL(old);
  }

  let cameraLoopRunning = false;

  // Sequential preview loop (never more than one request in flight); ends when the page is hidden.
  async function cameraPreview(document, fetchFn) {
    const doc = document || root.document, f = fetchFn || root.fetch.bind(root);
    if (cameraLoopRunning) return;
    cameraLoopRunning = true;
    let failed = false;
    try {
      while (cameraVisible(doc) && cameraLiveEnabled(doc)) {
        let delay = CAMERA_PREVIEW_MS;
        try {
          await cameraFrame(doc, f);
          const err = doc.getElementById("cam-err");
          if (err) err.hidden = true;
          if (failed) await cameraLoad(doc, f);
          failed = false;
        } catch (e) {
          const err = camSet(doc, "cam-err", `Preview unavailable: ${e && e.message ? e.message : e}`);
          if (err) err.hidden = false;
          if (!failed) await cameraLoad(doc, f);
          failed = true;
          delay = CAMERA_RETRY_MS;
        }
        await new Promise((resolve) => root.setTimeout(resolve, delay));
      }
    } finally {
      cameraLoopRunning = false;
    }
  }

  let cameraOpening = false, cameraOpenTimer;

  async function cameraOpen(document) {
    if (typeof root.fetch !== "function" || !cameraVisible(document)) return;
    root.clearTimeout(cameraOpenTimer);
    if (cameraOpening) return;
    cameraOpening = true;
    let settings;
    try {
      [, settings] = await Promise.all([cameraLoad(document), cameraSettingsLoad(document)]);
    } finally {
      cameraOpening = false;
    }
    if (settings) cameraPreview(document);
    else if (cameraVisible(document)) {
      camSet(document, "cam-settings-msg", "Camera settings unavailable; retrying before starting preview", true);
      cameraOpenTimer = root.setTimeout(() => cameraOpen(document), CAMERA_RETRY_MS);
    }
  }

  function start(document) {
    drawDividers(document);
    addPasswordToggles(document);
    const isShell = !!showPage(document, root.location && root.location.hash);
    if (isShell) {
      root.addEventListener("hashchange", () => {
        const page = showPage(document, root.location.hash);
        if (page !== "wifi") wifiQrClear(document);
        else if (typeof root.fetch === "function") wifiQrFollow(document);
        if (page === "optimize" && typeof root.fetch === "function") optLoad(document);
        if (page === "camera") cameraOpen(document);
      });
      if (document.addEventListener) document.addEventListener("visibilitychange", () => {
        if (document.hidden) wifiQrClear(document);
        else if (pageFromHash(root.location && root.location.hash) === "wifi" && typeof root.fetch === "function") {
          wifiQrFollow(document);
        }
        cameraOpen(document);
      });
      cameraOpen(document);
      if (pageFromHash(root.location && root.location.hash) === "wifi" && typeof root.fetch === "function") {
        wifiQrFollow(document);
      }
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
    OPT_FIELDS, OPT_SELECTS, optDefaults, optRebootRequired, optFillForm, optReadForm, optStatus, sdioStatus, optLoad, optSave, optClearEmergency,
    PROTO_BITS, protoMaskFromChecks, protoSetChecks, protoFilterForBand, protoLabel, protoHint, radioFillForm, radioReadForm,
    radioRebootRequired, cameraUsable, cameraStatusText, cameraStorageText, cameraResultText, cameraPhotoRows,
    cameraLoad, cameraCapture, cameraDelete, cameraVisible, cameraFrame, cameraPreview,
    cameraSettingsFill, cameraSettingsLoad, cameraSettingsSave, cameraSettingsReset, cameraLiveChanged,
    cameraLiveEnabled, cameraOpen, htmlEsc, wifiQrVisibilityText, wifiQrPriorText, wifiQrInfoRows, wifiQrSlotOptions,
    wifiQrRender, wifiQrPoll, wifiQrFollow, wifiQrScan, wifiQrSubmitText, wifiQrRescan, wifiQrCommit, wifiQrCancel,
    wifiQrDiscard, wifiQrAim, wifiQrClear };
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
